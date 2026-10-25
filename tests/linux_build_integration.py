"""Linux isolated builds, approvals, C++26 and process-tree lifetime regression.

Run under scripts/linux_test_delegation.py with the built CLI path. Uses only
installed ELF tools and a loopback model fixture; no downloads or live models.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

EXE = Path(sys.argv.pop(1)).resolve()
FIXTURE = EXE.parent / "build-fixture"
TOKEN = "linux-build-credential-never-emit"
SECRET = "linux-build-host-secret-never-emit"
SOURCE = "original workspace source\n"


def model_reply(arguments=None):
    output = ([{"type": "function_call", "id": "function_once", "call_id": "build-once",
                "name": "propose_build", "arguments": json.dumps(arguments), "status": "completed"}]
              if arguments is not None else [{"type": "message", "id": "message_once", "role": "assistant",
                "status": "completed", "content": [{"type": "output_text", "text": "Build receipt inspected", "annotations": []}]}])
    return {"id": "response_fixture", "status": "completed", "output": output,
            "usage": {"input_tokens": 7, "output_tokens": 3, "total_tokens": 10}}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with self.server.guard:
            self.server.requests.append(body)
            response = self.server.responses.pop(0) if self.server.responses else None
        payload = json.dumps(response or {"error": "unexpected model request"}).encode()
        self.send_response(200 if response else 500)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


class Service:
    def __init__(self, workspace, env):
        self.process = subprocess.Popen([str(EXE), "serve", "--workspace", str(workspace)], env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.queue, self.pending, self.stderr = queue.Queue(), [], bytearray()
        self.next_id = 0
        self.readers = [threading.Thread(target=self.read_stdout, daemon=True),
                        threading.Thread(target=self.read_stderr, daemon=True)]
        for reader in self.readers:
            reader.start()
        self.rpc("initialize", {"protocol_version": 2})

    def read_stdout(self):
        try:
            for line in self.process.stdout:
                self.queue.put(json.loads(line))
        except Exception as error:
            self.queue.put(error)
        finally:
            self.queue.put(EOFError("Service stdout closed"))

    def read_stderr(self):
        while data := self.process.stderr.read(4096):
            self.stderr.extend(data)

    def receive(self, predicate):
        for index, frame in enumerate(self.pending):
            if predicate(frame):
                return self.pending.pop(index)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                frame = self.queue.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if isinstance(frame, Exception):
                raise AssertionError(f"{frame}; stderr={bytes(self.stderr)!r}")
            if predicate(frame):
                return frame
            self.pending.append(frame)
        raise AssertionError(f"Service reply timed out: {bytes(self.stderr)!r}")

    def rpc(self, method, params=None):
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}}
        self.process.stdin.write((json.dumps(request) + "\n").encode())
        self.process.stdin.flush()
        result = self.receive(lambda frame: frame.get("id") == self.next_id)
        if "error" in result:
            raise AssertionError(result)
        return result["result"]

    def finished(self, operation):
        return self.receive(lambda frame: frame.get("method") == "operation/finished" and
            frame.get("params", {}).get("operation_id") == operation)["params"]

    def close(self):
        if self.process.poll() is None:
            if not self.process.stdin.closed:
                self.process.stdin.close()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        for reader in self.readers:
            reader.join(timeout=2)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            if not stream.closed:
                stream.close()


def process_identity(pid):
    try:
        data = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return data[19], data[0], int(data[1])  # start ticks, state, parent PID
    except (FileNotFoundError, ProcessLookupError):
        return None


def fixture_descendants(parent):
    processes = {}
    for item in Path("/proc").iterdir():
        if item.name.isdigit() and (identity := process_identity(item.name)):
            processes[int(item.name)] = identity
    descendants = {parent}
    while True:
        expanded = descendants | {pid for pid, (_, _, owner) in processes.items() if owner in descendants}
        if expanded == descendants:
            break
        descendants = expanded
    result = []
    for pid in descendants - {parent}:
        try:
            if Path(os.readlink(f"/proc/{pid}/exe")).name == FIXTURE.name:
                result.append((pid, processes[pid][0]))
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    return result


def group_populated(group):
    try:
        return "populated 1" in (group / "cgroup.events").read_text()
    except FileNotFoundError:
        return False


@unittest.skipUnless(sys.platform == "linux", "Linux namespace backend required")
class LinuxBuildTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(FIXTURE.is_file(), f"Build fixture missing: {FIXTURE}")
        self.temp = tempfile.TemporaryDirectory(prefix="codestudio-linux-build-")
        self.root = Path(self.temp.name).resolve()
        self.workspace = self.root / "workspace with spaces"
        self.workspace.mkdir()
        (self.workspace / "input.txt").write_text(SOURCE)
        self.outside = self.root / "outside-secret"
        self.outside.write_text("host only")
        self.manifest = self.workspace / "build.json"
        self.database = self.workspace / ".codestudio/state.sqlite3"
        self.env = {name: value for name, value in os.environ.items()
                    if not name.startswith("CODESTUDIO_") or name == "CODESTUDIO_CGROUP_ROOT"}
        self.env.update(CODESTUDIO_API_KEY=TOKEN, CODESTUDIO_BUILD_TEST_SECRET=SECRET)
        self.services = []
        self.crash_staging, self.crash_groups = [], []
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.guard = threading.Lock()
        self.server.requests, self.server.responses = [], []
        self.worker = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        self.worker.start()

    def tearDown(self):
        for service in self.services:
            service.close()
        self.server.shutdown()
        self.server.server_close()
        self.worker.join(timeout=3)
        try:
            if self.database.exists():
                with closing(sqlite3.connect(self.database)) as database:
                    persisted = "\n".join(database.iterdump())
                self.assertNotIn(TOKEN, persisted)
                self.assertNotIn(SECRET, persisted)
        finally:
            # Crash tests record exact owned paths while the sandbox is live.
            # Remove retained evidence only after proving its cgroup is empty.
            for group, stage in zip(self.crash_groups, self.crash_staging):
                if not group_populated(group):
                    if group.exists():
                        group.rmdir()
                    if stage.exists():
                        shutil.rmtree(stage)
            self.temp.cleanup()

    def invoke(self, *args, expected=0):
        result = subprocess.run([str(EXE), *map(str, args)], env=self.env, capture_output=True,
                                text=True, encoding="utf-8", timeout=150)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertNotIn(TOKEN, result.stdout + result.stderr)
        self.assertNotIn(SECRET, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def recipe(self, mode="write", timeout=15000):
        return {"version": 2, "inputs": ["input.txt"],
            "mounts": [{"name": "fixture", "source": str(FIXTURE.parent), "inputs": [FIXTURE.name]}],
            "steps": [{"executable": "${fixture}/" + FIXTURE.name, "arguments": [mode, json.dumps({
                "outside_file": str(self.outside), "outside_new": str(self.root / "must-not-create"),
                "loopback_port": self.server.server_port})]}],
            "outputs": ["out"], "artifact_directory": "artifacts",
            "limits": {"timeout_ms": timeout, "processes": 8, "memory_mb": 256, "cpu_percent": 80}}

    def save(self, recipe):
        self.manifest.write_text(json.dumps(recipe))

    def show(self, task):
        return self.invoke("show", task, "--workspace", self.workspace)

    def build(self, recipe, expected=0):
        self.save(recipe)
        result = self.invoke("build", "--workspace", self.workspace, "--build-config", self.manifest, expected=expected)
        return self.show(result["task_id"])["actions"][0]

    def bundles(self):
        directory = self.workspace / "artifacts"
        return sorted(directory.iterdir()) if directory.exists() else []

    def verify_identity(self, value):
        self.assertTrue(value["linux_namespace"], value)
        self.assertEqual((value["no_new_privileges"], value["seccomp_mode"]), (1, 2))
        for name, identity in value["namespaces"].items():
            self.assertNotEqual(identity, os.readlink(f"/proc/self/ns/{name}"))
        self.assertFalse(value["api_key_present"])
        self.assertFalse(value["secret_present"])
        self.assertEqual(value["cwd"], "/work/workspace")
        self.assertEqual(value["build_environment"]["HOME"], "/work/home")
        for name in ("XMAKE_GLOBALDIR", "XMAKE_PKG_INSTALLDIR", "XMAKE_PROGRAM_DIR"):
            self.assertIsNone(value["build_environment"][name])
        for name in ("mount_create", "outside_read", "outside_write", "outside_create"):
            self.assertNotEqual(value[name], 0, (name, value))
        if value["network_socket_created"]:
            self.assertTrue(value["network_loopback_attempted"])
            self.assertNotEqual(value["network_loopback_error"], 0)
        else:
            self.assertNotEqual(value["network_socket_error"], 0)

    def verify_artifacts(self, action):
        self.assertEqual((action["status"], action["effect"]), ("succeeded", "applied"), action)
        result = action["result"]
        self.assertTrue(result["ok"], result)
        output = result["output"]
        self.assertEqual(output["backend"], "linux-namespace-build-v2")
        self.assertTrue(all(step["exit_code"] == 0 for step in output["steps"]), output)
        artifact = output["artifacts"]
        bundle = Path(artifact["directory"])
        self.assertEqual(bundle.parent, self.workspace / "artifacts")
        self.assertEqual(json.loads((bundle / "manifest.json").read_text()), artifact)
        total = 0
        for item in artifact["files"]:
            data = (bundle / item["artifact_path"]).read_bytes()
            self.assertEqual(item["artifact_path"], "files/" + item["path"])
            self.assertEqual(item["sha256"], hashlib.sha256(data).hexdigest())
            self.assertEqual(item["bytes"], len(data))
            total += len(data)
        self.assertEqual((artifact["file_count"], artifact["bytes"]), (len(artifact["files"]), total))
        self.assertEqual((self.workspace / "input.txt").read_text(), SOURCE)
        self.assertFalse((self.workspace / "out").exists())
        return bundle

    def test_build_export_boundaries_and_distinct_bundles(self):
        bundle = self.verify_artifacts(self.build(self.recipe()))
        self.assertEqual((bundle / "files/out/payload.txt").read_text(), SOURCE + "\nprivate build output\n")
        self.verify_identity(json.loads((bundle / "files/out/identity-0.json").read_text()))
        other = self.verify_artifacts(self.build(self.recipe()))
        self.assertNotEqual(bundle, other)
        self.assertEqual(self.outside.read_text(), "host only")
        self.assertFalse((self.root / "must-not-create").exists())

    def test_failed_steps_and_missing_outputs_publish_nothing(self):
        recipe = self.recipe()
        failed = copy.deepcopy(recipe["steps"][0])
        failed["arguments"][0] = "fail"
        recipe["steps"] += [failed, copy.deepcopy(recipe["steps"][0])]
        result = self.build(recipe, expected=1)["result"]
        self.assertEqual([step["exit_code"] for step in result["output"]["steps"]], [0, 17])
        self.assertEqual(result["effect"], "none")
        self.assertNotIn("artifacts", result["output"])
        recipe = self.recipe()
        recipe["outputs"].append("missing-output")
        result = self.build(recipe, expected=1)["result"]
        self.assertFalse(result["ok"])
        self.assertEqual(result["effect"], "none")
        self.assertEqual(self.bundles(), [])

    def propose(self):
        with self.server.guard:
            self.server.responses = [model_reply({"reason": "Compile the requested workspace", "manifest": self.recipe()})]
        result = self.invoke("run", "--workspace", self.workspace, "--prompt", "Propose a build",
            "--base-url", f"http://127.0.0.1:{self.server.server_port}/v1", "--model", "fixture", "--api", "responses",
            "--allow-http-loopback", "--no-stream", "--max-turns", "2", "--allow-build-proposals", expected=3)
        self.assertEqual(result["status"], "waiting_approval")
        action = self.show(result["task_id"])["actions"][0]
        self.assertEqual((action["tool"], action["effect"]), ("propose_build", "none"))
        self.assertEqual(self.bundles(), [])
        return result["task_id"], action["id"], action["result"]["output"]["digest"]

    def decide(self, command, task, action, digest, expected=0):
        return self.invoke(command, task, "--action", action, "--digest", digest,
                           "--workspace", self.workspace, expected=expected)

    def test_approval_executes_once_and_resume_consumes_receipt_once(self):
        task, action, digest = self.propose()
        approved = self.decide("build-approve", task, action, digest)
        self.verify_artifacts(approved["execution"]["actions"][0])
        bundles = self.bundles()
        repeated = self.decide("build-approve", task, action, digest)
        self.assertEqual(approved["execution"], repeated["execution"])
        self.assertEqual(self.bundles(), bundles)
        with self.server.guard:
            self.server.responses = [model_reply()]
        resumed = self.invoke("resume", task, "--workspace", self.workspace,
                              "--allow-http-loopback", "--allow-build-proposals")
        self.assertEqual(resumed["status"], "completed")
        consumed = [event for event in self.show(task)["events"] if event["kind"] == "build.proposal.consumed"]
        self.assertEqual(len(consumed), 1)
        self.assertEqual(self.bundles(), bundles)
        with self.server.guard:
            self.assertEqual(len(self.server.requests), 2)

    def test_rejection_never_builds(self):
        task, action, digest = self.propose()
        rejected = self.decide("build-reject", task, action, digest)
        self.assertIsNone(rejected["execution"])
        self.decide("build-approve", task, action, digest, expected=2)
        self.assertEqual(self.bundles(), [])

    def live_tree(self, process):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            evidence = fixture_descendants(process.pid)
            if len(evidence) == 3:
                try:
                    pid = evidence[0][0]
                    directory = Path(f"/proc/{pid}/cwd")
                    identities = [json.loads((directory / f"out/identity-{index}.json").read_text()) for index in range(3)]
                    for identity in identities:
                        self.verify_identity(identity)
                    groups = [line.removeprefix("0::") for line in Path(f"/proc/{pid}/cgroup").read_text().splitlines()
                              if line.startswith("0::")]
                    self.assertEqual(len(groups), 1)
                    group = Path("/sys/fs/cgroup") / groups[0].lstrip("/")
                    self.assertEqual((group / "pids.max").read_text().strip(), "8")
                    self.assertEqual((group / "memory.max").read_text().strip(), str(256 * 1024 * 1024))
                    self.assertEqual((group / "cpu.max").read_text().strip(), "80000 100000")
                    # /proc/<pid>/cwd can stringify to /work/workspace in the
                    # child's mount namespace. Bind the owned staging name to
                    # the observed cgroup and verify its inode via the magic
                    # /proc link instead of resolving that link lexically.
                    self.assertTrue(group.name.startswith("codestudio-isolation-"), group)
                    stage = Path(tempfile.gettempdir()).resolve() / group.name
                    self.assertEqual(stage.stat().st_uid, os.geteuid())
                    self.assertTrue(os.path.samefile(directory, stage / "work/workspace"), stage)
                    return evidence, group, stage
                except (FileNotFoundError, json.JSONDecodeError):
                    pass  # Descendants have exec'd but may still be writing evidence.
            if process.poll() is not None:
                self.fail(f"Host exited before descendant tree appeared: {process.returncode}")
            time.sleep(0.02)
        self.fail("Expected three live fixture generations")

    def assert_tree_stopped(self, evidence, group):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            alive = [pid for pid, started in evidence if (identity := process_identity(pid)) and
                     identity[0] == started and identity[1] != "Z"]
            populated = group_populated(group)
            if not alive and not populated:
                break
            time.sleep(0.02)
        else:
            self.fail(f"Sandbox descendant tree survived termination: {alive}, {group}")
        self.assertEqual(self.bundles(), [])
        self.assertEqual((self.workspace / "input.txt").read_text(), SOURCE)

    def test_cancel_and_stdin_eof_stop_descendants_without_export(self):
        for mode in ("cancel", "eof"):
            with self.subTest(mode=mode):
                self.save(self.recipe("tree-sleep", timeout=30000))
                service = Service(self.workspace, self.env)
                self.services.append(service)
                try:
                    operation = service.rpc("tasks/start", {"mode": "build", "options": {"build_config": str(self.manifest)}})["operation_id"]
                    evidence, group, stage = self.live_tree(service.process)
                    if mode == "cancel":
                        self.assertTrue(service.rpc("operations/cancel", {"operation_id": operation})["cancel_requested"])
                        finished = service.finished(operation)
                        self.assertEqual(finished["exit_code"], 130)
                        self.assertEqual(finished["result"]["status"], "cancelled")
                    else:
                        service.process.stdin.close()
                        self.assertEqual(service.process.wait(timeout=15), 0)
                    self.assert_tree_stopped(evidence, group)
                    self.assertFalse(stage.exists(), "Normal shutdown leaked private staging")
                finally:
                    service.close()

    def test_timeout_stops_live_descendants_without_export(self):
        self.save(self.recipe("tree-sleep", timeout=6000))
        service = Service(self.workspace, self.env)
        self.services.append(service)
        operation = service.rpc("tasks/start", {"mode": "build", "options": {"build_config": str(self.manifest)}})["operation_id"]
        evidence, group, stage = self.live_tree(service.process)
        finished = service.finished(operation)
        self.assertEqual(finished["exit_code"], 1)
        receipt = self.show(finished["result"]["task_id"])["actions"][0]["result"]
        self.assertTrue(receipt["output"]["timed_out"])
        self.assertEqual(receipt["effect"], "none")
        self.assert_tree_stopped(evidence, group)
        self.assertFalse(stage.exists())

    def test_host_crash_kills_descendants_and_recovery_never_replays(self):
        self.save(self.recipe("tree-sleep", timeout=30000))
        process = subprocess.Popen([str(EXE), "build", "--workspace", str(self.workspace), "--build-config", str(self.manifest)],
                                   env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            evidence, group, stage = self.live_tree(process)
            self.crash_groups.append(group)
            self.crash_staging.append(stage)
            process.kill()
            process.communicate(timeout=10)
            self.assert_tree_stopped(evidence, group)
            self.assertFalse(self.invoke("recover", "--workspace", self.workspace)["tools_executed"])
            with closing(sqlite3.connect(self.database)) as database:
                actions = database.execute("SELECT status,effect FROM actions").fetchall()
            self.assertEqual(actions, [("unknown", "unknown")])
            self.assertEqual(self.bundles(), [])
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)

    def test_real_cpp26_compile_assemble_link_run_and_hash(self):
        compiler = shutil.which("g++")
        self.assertIsNotNone(compiler, "Install GCC with C++26 pack indexing support")
        # Discovery queries do not compile or execute the project on the host.
        cc1plus = Path(subprocess.check_output([compiler, "-print-prog-name=cc1plus"], text=True).strip()).resolve()
        toolchain = self.root / "explicit-toolchain"
        toolchain.mkdir()
        for name, source in (("cc1plus", cc1plus), ("as", shutil.which("as")), ("ld", shutil.which("ld"))):
            self.assertIsNotNone(source, f"Missing installed {name}")
            shutil.copyfile(Path(source).resolve(), toolchain / name)
            (toolchain / name).chmod(0o700)
        source = ('template <auto... values> constexpr auto first = values...[0];\n'
                  'static_assert(first<81, 9, 0> == 81);\n'
                  'extern "C" [[gnu::noreturn]] void _start() {\n'
                  '  const long status = first<81, 9, 0> == 81 ? 0 : 1;\n'
                  '  asm volatile("syscall" : : "a"(60L), "D"(status) : "rcx", "r11", "memory");\n'
                  '  __builtin_unreachable();\n}\n')
        (self.workspace / "main.cpp").write_text(source)
        recipe = self.recipe("prepare", timeout=120000)
        recipe["inputs"].append("main.cpp")
        recipe["mounts"].append({"name": "compiler", "source": str(toolchain), "inputs": ["cc1plus", "as", "ld"]})
        recipe["limits"].update(memory_mb=2048, processes=16)
        recipe["steps"] += [
            {"executable": "${compiler}/cc1plus", "arguments": ["-quiet", "-std=c++26", "-ffreestanding", "-fno-exceptions",
                "-fno-rtti", "-fno-stack-protector", "-fno-pie", "-o", "out/main.s", "main.cpp"]},
            {"executable": "${compiler}/as", "arguments": ["-o", "out/main.o", "out/main.s"]},
            {"executable": "${compiler}/ld", "arguments": ["-static", "-e", "_start", "-o", "out/program", "out/main.o"]},
            {"executable": "${workspace}/out/program", "arguments": []}]
        recipe["outputs"] = ["out/program"]
        action = self.build(recipe)
        self.assertEqual(len(action["result"]["output"]["steps"]), 5)
        bundle = self.verify_artifacts(action)
        program = bundle / "files/out/program"
        self.assertEqual(program.read_bytes()[:4], b"\x7fELF")
        self.assertTrue(program.stat().st_mode & 0o100)
        self.assertEqual((self.workspace / "main.cpp").read_text(), source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
