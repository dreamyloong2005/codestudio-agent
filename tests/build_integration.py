"""Exercise isolated builds through the CLI, model adapters and stdio service.

Usage: python tests/build_integration.py path/to/codestudio-agent.exe
The adjacent build-fixture.exe must be built with a static CRT. All model
responses come from an ephemeral loopback fixture; no live model is used.
"""
import copy
import ctypes
import hashlib
import json
import os
import pathlib
import queue
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

EXE = pathlib.Path(sys.argv.pop(1)).resolve()
FIXTURE = EXE.parent / "build-fixture.exe"
TOKEN = "isolated-build-credential-never-emit"
SECRET = "isolated-build-host-environment-secret"
SOURCE = "original workspace source\n"
APIS = ("chat-completions", "responses", "messages")
REGISTRY_READ_SID = "S-1-15-3-1024-1065365936-1281604716-3511738428-1654721687-432734479-3232135806-4053264122-3456934681"


def model_reply(api, arguments=None):
    if api == "chat-completions":
        message = {"role": "assistant", "content": None if arguments is not None else "Build receipt inspected"}
        if arguments is not None:
            message["tool_calls"] = [{"id": "build-once", "type": "function", "function": {
                "name": "build_workspace", "arguments": json.dumps(arguments)}}]
        return {"choices": [{"finish_reason": "tool_calls" if arguments is not None else "stop", "message": message}]}
    if api == "responses":
        output = ([{"type": "function_call", "id": "function_1", "call_id": "build-once",
                    "name": "build_workspace", "arguments": json.dumps(arguments), "status": "completed"}]
                  if arguments is not None else [{"id": "message_1", "type": "message", "role": "assistant",
                    "status": "completed", "content": [{"type": "output_text", "text": "Build receipt inspected", "annotations": []}]}])
        return {"id": "response_1", "status": "completed", "output": output,
                "usage": {"input_tokens": 7, "output_tokens": 3, "total_tokens": 10}}
    content = ([{"type": "tool_use", "id": "build-once", "name": "build_workspace", "input": arguments}]
               if arguments is not None else [{"type": "text", "text": "Build receipt inspected"}])
    return {"type": "message", "id": "message_1", "role": "assistant", "model": "fixture",
            "content": content, "stop_reason": "tool_use" if arguments is not None else "end_turn",
            "stop_sequence": None, "usage": {"input_tokens": 7, "output_tokens": 3}}




class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with self.server.guard:
            self.server.requests.append({"path": self.path, "body": body})
            response = self.server.responses.pop(0) if self.server.responses else None
            before_reply = self.server.before_reply
            self.server.before_reply = None
        if before_reply is not None:
            before_reply()
        data = json.dumps(response or {"error": "unexpected model request"}).encode()
        try:
            self.send_response(200 if response else 500)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
        except (ConnectionError, OSError):
            pass


class Client:
    """A bounded protocol reader with independent stderr draining."""
    def __init__(self, workspace, env):
        self.process = subprocess.Popen([str(EXE), "serve", "--workspace", str(workspace)],
            env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.queue = queue.Queue()
        self.pending = []
        self.raw = bytearray()
        self.stderr = bytearray()
        self.next_id = 1
        self.stdout_thread = threading.Thread(target=self.read_stdout, daemon=True)
        self.stderr_thread = threading.Thread(target=self.read_stderr, daemon=True)
        self.stdout_thread.start()
        self.stderr_thread.start()

    def read_stdout(self):
        try:
            for line in self.process.stdout:
                self.raw.extend(line)
                try:
                    frame = json.loads(line)
                    if not isinstance(frame, dict) or frame.get("jsonrpc") != "2.0":
                        raise AssertionError(f"Non-protocol stdout: {line[:200]!r}")
                    self.queue.put(frame)
                except Exception as error:
                    self.queue.put(error)
        finally:
            self.queue.put(EOFError("Service stdout closed"))

    def read_stderr(self):
        while block := self.process.stderr.read(4096):
            self.stderr.extend(block)

    def receive(self, predicate, timeout=20):
        for index, frame in enumerate(self.pending):
            if predicate(frame):
                return self.pending.pop(index)
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"Service response missing; stderr={bytes(self.stderr)!r}")
            try:
                frame = self.queue.get(timeout=remaining)
            except queue.Empty as error:
                raise AssertionError(f"Service timed out; stderr={bytes(self.stderr)!r}") from error
            if isinstance(frame, Exception):
                raise AssertionError(f"{frame}; stderr={bytes(self.stderr)!r}")
            if predicate(frame):
                return frame
            self.pending.append(frame)

    def rpc(self, method, params=None, error=False):
        request_id = self.next_id
        self.next_id += 1
        data = {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}}
        self.process.stdin.write((json.dumps(data) + "\n").encode())
        self.process.stdin.flush()
        reply = self.receive(lambda frame: frame.get("id") == request_id)
        if error:
            if "error" not in reply:
                raise AssertionError(f"Expected RPC rejection: {reply!r}")
            return reply["error"]
        if "error" in reply:
            raise AssertionError(f"Unexpected RPC rejection: {reply!r}")
        return reply["result"]

    def start(self, manifest):
        result = self.rpc("tasks/start", {"mode": "build", "options": {"build_config": str(manifest)}})
        if result.get("status") != "accepted":
            raise AssertionError(f"Unexpected operation receipt: {result!r}")
        return result["operation_id"]

    def finished(self, operation):
        return self.receive(lambda frame: frame.get("method") == "operation/finished" and
            frame.get("params", {}).get("operation_id") == operation)["params"]

    def wait_exit(self):
        status = self.process.wait(timeout=15)
        self.stdout_thread.join(timeout=2)
        self.stderr_thread.join(timeout=2)
        return status

    def close(self):
        if self.process.poll() is None:
            try:
                if not self.process.stdin.closed:
                    self.rpc("shutdown")
                self.wait_exit()
            except (AssertionError, OSError, subprocess.TimeoutExpired):
                self.process.kill()
                self.wait_exit()
        else:
            self.wait_exit()
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            if stream and not stream.closed:
                try:
                    stream.close()
                except BrokenPipeError:
                    pass


if os.name == "nt":
    from ctypes import wintypes

    class ProcessEntry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                    ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
                    ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]

    KERNEL = ctypes.WinDLL("kernel32", use_last_error=True)
    KERNEL.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    KERNEL.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    KERNEL.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    KERNEL.Process32FirstW.restype = wintypes.BOOL
    KERNEL.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    KERNEL.Process32NextW.restype = wintypes.BOOL
    KERNEL.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    KERNEL.OpenProcess.restype = wintypes.HANDLE
    KERNEL.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    KERNEL.QueryFullProcessImageNameW.restype = wintypes.BOOL
    KERNEL.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    KERNEL.WaitForSingleObject.restype = wintypes.DWORD
    KERNEL.CloseHandle.argtypes = [wintypes.HANDLE]
    KERNEL.CloseHandle.restype = wintypes.BOOL


def fixture_descendants(parent_pid):
    snapshot = KERNEL.CreateToolhelp32Snapshot(2, 0)  # TH32CS_SNAPPROCESS
    if snapshot == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    entries = []
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        available = KERNEL.Process32FirstW(snapshot, ctypes.byref(entry))
        while available:
            entries.append((entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile))
            available = KERNEL.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        KERNEL.CloseHandle(snapshot)
    descendants = {parent_pid}
    while True:
        expanded = descendants | {pid for pid, parent, _ in entries if parent in descendants}
        if expanded == descendants:
            break
        descendants = expanded
    return [pid for pid, _, name in entries if pid in descendants and name.lower() == FIXTURE.name.lower()]


@unittest.skipUnless(os.name == "nt", "The workspace build backend requires Windows LPAC")
class BuildTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(FIXTURE.is_file(), f"Build the static-CRT fixture first: {FIXTURE}")
        # Windows TEMP can contain an 8.3 user alias; use the actual long path
        # because the build grant intentionally rejects filesystem aliases.
        self.temp = tempfile.TemporaryDirectory(prefix="codestudio-build-", dir=pathlib.Path(tempfile.gettempdir()).resolve())
        self.workspace = pathlib.Path(self.temp.name) / "workspace with spaces"
        self.workspace.mkdir()
        (self.workspace / "input.txt").write_text(SOURCE, encoding="utf-8", newline="")
        self.outside = pathlib.Path(self.temp.name) / "outside-secret.txt"
        self.outside.write_text("host only", encoding="utf-8")
        self.outside_new = pathlib.Path(self.temp.name) / "must-not-create.txt"
        self.database = self.workspace / ".codestudio" / "state.sqlite3"
        self.manifest = self.workspace / "build.json"
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("CODESTUDIO_")}
        self.env.update(CODESTUDIO_API_KEY=TOKEN, CODESTUDIO_BUILD_TEST_SECRET=SECRET)
        self.env["CODESTUDIO_SANDBOX_REGISTRY"] = str(pathlib.Path(self.temp.name) / "sandbox registry")
        self.clients = []
        self.handles = []
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.server.guard = threading.Lock()
        self.server.responses = []
        self.server.requests = []
        self.server.before_reply = None
        self.worker = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        self.worker.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}/v1"
        # The exact endpoint supplied to every isolated generation is listening
        # and reachable from the host. A refused/timed-out port cannot pass as
        # evidence that the sandbox blocked networking.
        with socket.create_connection(("127.0.0.1", self.server.server_port), timeout=2):
            pass

    def tearDown(self):
        for client in self.clients:
            client.close()
        for handle in self.handles:
            KERNEL.CloseHandle(handle)
        self.server.shutdown()
        self.server.server_close()
        self.worker.join(timeout=5)
        # Close SQLite and process handles before removing Windows files.
        if self.database.exists():
            with closing(sqlite3.connect(self.database)) as database:
                persisted = "\n".join(database.iterdump())
            self.assertNotIn(TOKEN, persisted)
            self.assertNotIn(SECRET, persisted)
        self.temp.cleanup()

    def recipe(self, mode="write", timeout=10000):
        return {"version": 2, "inputs": ["input.txt"],
                "mounts": [{"name": "fixture", "source": str(FIXTURE.parent), "inputs": [FIXTURE.name]}],
                "steps": [{"executable": "${fixture}/" + FIXTURE.name, "arguments": [mode, json.dumps({
                    "outside_file": str(self.outside), "outside_new": str(self.outside_new),
                    "loopback_port": self.server.server_port})], "cwd": "."}],
                "outputs": ["out"], "artifact_directory": "artifacts",
                "limits": {"timeout_ms": timeout, "processes": 8, "memory_mb": 256, "cpu_percent": 80}}

    def save(self, value):
        self.manifest.write_text(json.dumps(value, indent=2), encoding="utf-8", newline="")

    def invoke(self, *args, expected=0):
        process = subprocess.run([str(EXE), *map(str, args)], env=self.env, capture_output=True,
                                 encoding="utf-8", timeout=45)
        self.assertEqual(process.returncode, expected, process.stdout + process.stderr)
        self.assertNotIn(TOKEN, process.stdout + process.stderr)
        self.assertNotIn(SECRET, process.stdout + process.stderr)
        return json.loads(process.stdout)

    def build(self, expected=0):
        return self.invoke("build", "--workspace", self.workspace, "--build-config", self.manifest, expected=expected)

    def show(self, task):
        return self.invoke("show", task, "--workspace", self.workspace)

    def action(self, task):
        actions = self.show(task)["actions"]
        self.assertEqual(len(actions), 1)
        self.assertEqual(actions[0]["tool"], "build_workspace")
        return actions[0]

    def reclaim_crash_stage(self, stage, task):
        # Select only the exact resource observed in this test's own child tree.
        # The private registry is never shared with historical crash evidence.
        records = self.invoke("sandbox-resources")["items"]
        matches = [item for item in records if any(resource["kind"] == "staging" and
                   pathlib.Path(resource["path"]).resolve() == stage.resolve()
                   for resource in item["resources"])]
        self.assertEqual(len(matches), 1, records)
        record = matches[0]
        self.assertEqual(record["task_id"], task)
        with closing(sqlite3.connect(self.database)) as database:
            before = "\n".join(database.iterdump())
        preview = self.invoke("sandbox-reclaim", "--id", record["id"], "--dry-run")
        self.assertEqual(preview["state"], "reclaimable", preview)
        self.assertTrue(stage.is_dir())
        reclaimed = self.invoke("sandbox-reclaim", "--id", record["id"])
        self.assertEqual(reclaimed["state"], "reclaimed", reclaimed)
        self.assertFalse(stage.exists())
        self.assertEqual(self.invoke("sandbox-reclaim", "--id", record["id"])["state"], "absent")
        with closing(sqlite3.connect(self.database)) as database:
            self.assertEqual("\n".join(database.iterdump()), before,
                             "Resource cleanup must not change unknown effects, checkpoints or task events")
        print(f"Reclaimed owned crash resource: {record['id']} ({stage})", file=sys.stderr)

    def bundles(self):
        root = self.workspace / "artifacts"
        return sorted(root.iterdir()) if root.exists() else []

    def requests(self):
        with self.server.guard:
            return copy.deepcopy(self.server.requests)

    def replies(self, *values):
        with self.server.guard:
            self.server.responses = list(values)

    def model_args(self, api, *extra):
        return ["run", "--workspace", self.workspace, "--prompt", "Build the explicitly granted workspace",
                "--base-url", self.url, "--model", "build-fixture", "--api", api,
                "--allow-http-loopback", "--no-stream", "--build-config", self.manifest, *extra]

    def resume_args(self, task, *extra):
        return ["resume", task, "--workspace", self.workspace, "--allow-http-loopback", *extra]

    def client(self):
        client = Client(self.workspace, self.env)
        self.clients.append(client)
        self.assertEqual(client.rpc("initialize", {"protocol_version": 2})["protocol_version"], 2)
        return client

    def verify_artifacts(self, action):
        self.assertEqual((action["status"], action["effect"]), ("succeeded", "applied"), action["result"])
        receipt = action["result"]
        self.assertTrue(receipt["ok"])
        output = receipt["output"]
        self.assertEqual(output["backend"], "windows-lpac-build-v2")
        self.assertFalse(output["timed_out"])
        self.assertFalse(output["cancelled"])
        for step in output["steps"]:
            self.assertEqual(step["exit_code"], 0)
        artifacts = output["artifacts"]
        bundle = pathlib.Path(artifacts["directory"])
        self.assertEqual(bundle.parent, self.workspace / "artifacts")
        self.assertEqual(pathlib.Path(artifacts["manifest"]), bundle / "manifest.json")
        manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["files"], artifacts["files"])
        self.assertEqual(artifacts["file_count"], len(artifacts["files"]))
        total = 0
        for record in artifacts["files"]:
            self.assertEqual(record["artifact_path"], "files/" + record["path"])
            data = (bundle / record["artifact_path"]).read_bytes()
            self.assertEqual(record["bytes"], len(data))
            self.assertEqual(record["sha256"], hashlib.sha256(data).hexdigest())
            total += len(data)
        self.assertEqual(artifacts["bytes"], total)
        return bundle

    def verify_identity(self, evidence):
        self.assertTrue(evidence["appcontainer"])
        self.assertTrue(evidence["lpac"])
        self.assertTrue(evidence["appcontainer_sid"].startswith("S-1-15-2-"))
        self.assertEqual(evidence["capabilities"], [{"sid": REGISTRY_READ_SID, "attributes": 4}])
        self.assertEqual(evidence["integrity_rid"], 0x1000)  # SECURITY_MANDATORY_LOW_RID
        self.assertEqual(evidence["job_query_error"], 0)
        self.assertEqual(evidence["job_active_process_limit"], 8)
        self.assertEqual(evidence["job_limit_flags"] & 0x2208, 0x2208)  # KILL_ON_JOB_CLOSE | JOB_MEMORY | ACTIVE_PROCESS
        self.assertEqual(evidence["job_limit_flags"] & 0x1800, 0)  # No BREAKAWAY_OK or SILENT_BREAKAWAY_OK
        self.assertEqual(evidence["job_memory_bytes"], 256 * 1024 * 1024)
        self.assertEqual(evidence["job_cpu_query_error"], 0)
        self.assertEqual(evidence["job_cpu_flags"], 0x5)  # ENABLE | HARD_CAP
        self.assertEqual(evidence["job_cpu_rate"], 8000)
        self.assertEqual(evidence["breakaway_error"], 5)
        self.assertEqual(evidence["network_wsa_startup"], 0, "Winsock failure is not a connection denial")
        if evidence["network_socket_created"]:
            self.assertEqual(evidence["network_socket_error"], 0)
            self.assertTrue(evidence["network_loopback_attempted"])
            self.assertEqual(evidence["network_loopback_error"], 10013)  # WSAEACCES at connect
        else:
            self.assertEqual(evidence["network_socket_error"], 10013)  # WSAEACCES before connect
            self.assertFalse(evidence["network_loopback_attempted"])
            self.assertIsNone(evidence["network_loopback_error"])
        self.assertFalse(evidence["api_key_present"])
        self.assertFalse(evidence["secret_present"])
        for field in ("outside_read", "outside_write", "outside_create", "mount_create", "network_afd_open"):
            self.assertEqual(evidence[field], 5, f"Expected access denied for {field}: {evidence}")
        self.assertFalse(pathlib.Path(evidence["cwd"]).exists(), "Private workspace was not removed")
        self.assertFalse(pathlib.Path(evidence["image"]).exists(), "Private tool mount was not removed")
        self.assertEqual(self.outside.read_text(encoding="utf-8"), "host only")
        self.assertFalse(self.outside_new.exists())

    def test_direct_build_requires_explicit_recipe_and_rejects_extra_grants(self):
        self.save(self.recipe())
        self.invoke("build", "--workspace", self.workspace, expected=2)
        for extra in (["--allow-write"], ["--allow-process", FIXTURE], ["--sandbox", "none"],
                      ["--base-url", self.url], ["--model", "fixture"], ["--mcp-config", self.manifest]):
            with self.subTest(extra=extra):
                self.invoke("build", "--workspace", self.workspace, "--build-config", self.manifest, *extra, expected=2)
        self.assertEqual(self.bundles(), [])
        self.assertEqual(self.requests(), [])

    def test_manifest_configuration_rejection_happens_without_execution(self):
        changes = [{"version": 99}, {"inputs": ["../outside-secret.txt"]}, {"outputs": ["../escape"]},
                   {"artifact_directory": "."}, {"artifact_directory": "../escape"},
                   {"environment": {"CODESTUDIO_API_KEY": "forbidden"}}, {"limits": {"processes": 0}},
                   {"network": "unrestricted"},
                   {"steps": [{"executable": str(FIXTURE), "arguments": []}]},
                   {"steps": [{"executable": "${missing}/tool.exe", "arguments": []}]}]
        for change in changes:
            with self.subTest(change=change):
                recipe = self.recipe()
                recipe.update(change)
                self.save(recipe)
                self.invoke("build", "--workspace", self.workspace, "--build-config", self.manifest, expected=2)
        self.assertEqual(self.bundles(), [])
        self.assertEqual((self.workspace / "input.txt").read_text(), SOURCE)

    def test_generic_v2_recipe_has_no_xmake_defaults_and_accepts_explicit_tool_environment(self):
        self.env.update(XMAKE_GLOBALDIR="host-xmake-global", XMAKE_PKG_INSTALLDIR="host-xmake-packages",
                        XMAKE_PROGRAM_DIR="host-xmake-program", CARGO_HOME="host-cargo-home")
        recipe = self.recipe()
        recipe.update(version=2, environment={"CARGO_HOME": "${home}/cargo"})
        self.save(recipe)
        bundle = self.verify_artifacts(self.action(self.build()["task_id"]))
        evidence = json.loads((bundle / "files/out/identity-0.json").read_text())
        self.verify_identity(evidence)
        environment = evidence["build_environment"]
        self.assertEqual(pathlib.PureWindowsPath(environment["CARGO_HOME"]),
                         pathlib.PureWindowsPath(environment["HOME"]) / "cargo")
        for name in ("XMAKE_GLOBALDIR", "XMAKE_PKG_INSTALLDIR", "XMAKE_PROGRAM_DIR"):
            self.assertIsNone(environment[name], f"Generic executor inherited or injected {name}")
        self.assertEqual((self.workspace / "input.txt").read_text(), SOURCE)
        self.assertEqual(self.requests(), [])

    def test_unknown_manifest_fields_rejected_before_dispatch(self):
        self.assertEqual(self.invoke("doctor")["build_manifest_versions"], [2])
        for value in (None, {}, {"nested": "unexpected"}):
            with self.subTest(value=value):
                invalid = self.recipe()
                invalid["unrecognized_plan_field"] = value
                self.save(invalid)
                rejected = self.build(expected=2)
                self.assertIn("Unknown build manifest field", json.dumps(rejected))
                self.invoke(*self.model_args("responses"), expected=2)
        self.assertEqual(self.bundles(), [])
        self.assertEqual(self.requests(), [])
        self.assertEqual((self.workspace / "input.txt").read_text(), SOURCE)
        if self.database.exists():
            with closing(sqlite3.connect(self.database)) as database:
                self.assertEqual(database.execute("SELECT count(*) FROM actions").fetchone()[0], 0)

    def test_unsupported_manifest_versions_rejected_across_cli_model_and_service(self):
        client = self.client()
        mcp_fixture = EXE.parent / "mcp-fixture.exe"
        self.assertTrue(mcp_fixture.is_file())
        marker = pathlib.Path(self.temp.name) / "mcp.started"
        mcp_manifest = pathlib.Path(self.temp.name) / "mcp.json"
        mcp_manifest.write_text(json.dumps({"version": 2, "servers": [{"id": "echo",
            "command": str(mcp_fixture), "args": ["normal", str(marker)],
            "tools": ["echo"], "timeout_ms": 1000}]}), encoding="utf-8")
        for version in (0, 1, 3, True, "2"):
            with self.subTest(version=version):
                unsupported = self.recipe()
                unsupported["version"] = version
                self.save(unsupported)
                frozen = self.manifest.read_bytes()
                for arguments in (["build", "--workspace", self.workspace, "--build-config", self.manifest],
                                  self.model_args("responses", "--mcp-config", mcp_manifest, "--allow-mcp", "echo")):
                    rejected = self.invoke(*arguments, expected=2)
                    self.assertIn("Unsupported build manifest version", json.dumps(rejected))
                for mode in ("build", "run"):
                    options = {"build_config": str(self.manifest)}
                    if mode == "run":
                        options.update(prompt="Reject this unsupported plan", base_url=self.url, model="build-fixture",
                                       api="responses", allow_http_loopback=True, stream=False,
                                       mcp_config=str(mcp_manifest), allow_mcp=["echo"])
                    accepted = client.rpc("tasks/start", {"mode": mode, "options": options})
                    finished = client.finished(accepted["operation_id"])
                    self.assertEqual(finished["exit_code"], 2)
                    self.assertIn("Unsupported build manifest version", finished["error"]["message"])
                self.assertEqual(self.manifest.read_bytes(), frozen, "Unsupported input must not be rewritten")
        self.assertEqual(self.bundles(), [])
        self.assertFalse((self.workspace / "out").exists())
        self.assertFalse(marker.exists(), "Unsupported plan must be rejected before granted MCP startup")
        self.assertEqual(self.requests(), [])
        self.assertEqual((self.workspace / "input.txt").read_text(), SOURCE)
        if self.database.exists():
            with closing(sqlite3.connect(self.database)) as database:
                for table in ("tasks", "actions", "task_checkpoints", "events", "build_proposal_decisions"):
                    self.assertEqual(database.execute(f"SELECT count(*) FROM {table}").fetchone()[0], 0, table)

    def test_v2_environment_cannot_override_host_identity_or_supply_credentials(self):
        for name in ("HOME", "TEMP", "USERPROFILE", "SYSTEMROOT", "COMSPEC",
                     "CODESTUDIO_API_KEY", "AWS_SECRET_ACCESS_KEY", "ACCESS_TOKEN",
                     "DATABASE_PASSWORD", "cargo_home", "BAD=NAME"):
            with self.subTest(name=name):
                recipe = self.recipe()
                recipe.update(version=2, environment={name: "forbidden-value"})
                self.save(recipe)
                self.build(expected=2)
        self.assertEqual(self.bundles(), [])
        self.assertEqual(self.requests(), [])
        self.assertEqual((self.workspace / "input.txt").read_text(), SOURCE)

    def test_artifact_hashes_and_private_source_changes_never_overwrite_workspace(self):
        self.save(self.recipe())
        original_manifest = self.manifest.read_bytes()
        (self.workspace / "out").mkdir()
        sentinel = self.workspace / "out" / "payload.txt"
        sentinel.write_text("existing workspace output", encoding="utf-8")
        first = self.build()
        self.assertEqual(first["status"], "completed")
        self.assertEqual(self.show(first["task_id"])["provider"], "isolated-build")
        first_action = self.action(first["task_id"])
        self.assertEqual(first["build"], first_action["result"], "CLI must expose the persisted build receipt")
        first_bundle = self.verify_artifacts(first_action)
        payload = first_bundle / "files" / "out" / "payload.txt"
        self.assertEqual(payload.read_bytes(), (SOURCE + "\nprivate build output\n").encode())
        first_bytes = {str(p.relative_to(first_bundle)): p.read_bytes() for p in first_bundle.rglob("*") if p.is_file()}
        self.verify_identity(json.loads((first_bundle / "files/out/identity-0.json").read_text()))
        second_bundle = self.verify_artifacts(self.action(self.build()["task_id"]))
        self.assertNotEqual(first_bundle, second_bundle)
        self.assertEqual(first_bytes, {str(p.relative_to(first_bundle)): p.read_bytes() for p in first_bundle.rglob("*") if p.is_file()})
        self.assertEqual((self.workspace / "input.txt").read_text(), SOURCE)
        self.assertEqual(sentinel.read_text(), "existing workspace output")
        self.assertEqual(self.manifest.read_bytes(), original_manifest)
        self.assertEqual(self.requests(), [])
        with closing(sqlite3.connect(self.database)) as database:
            binding = json.loads(database.execute("SELECT binding FROM task_checkpoints WHERE task_id=?",
                                                  (first["task_id"],)).fetchone()[0])
        normalized = json.loads(original_manifest)
        self.assertEqual(normalized["version"], 2)
        self.assertEqual(binding["configuration"]["build"], {
            "version": 2, "path": str(self.manifest), "manifest": original_manifest.decode(),
            "backend": "windows-lpac-build-v2", "network": "none", "export": "new-atomic-bundle",
            "recipe": normalized})
        environment = json.loads((first_bundle / "files/out/identity-0.json").read_text())["build_environment"]
        self.assertIsNone(environment["XMAKE_GLOBALDIR"])
        self.assertIsNone(environment["XMAKE_PKG_INSTALLDIR"])

    def test_custom_state_journal_is_excluded_from_toolchain_mounts(self):
        state_directory = pathlib.Path(self.temp.name) / "external state"
        state_directory.mkdir()
        self.database = state_directory / "ledger.sqlite3"
        journal = state_directory / "ledger.sqlite3-journal"
        (state_directory / "ordinary.txt").write_text("explicitly granted mount input", encoding="utf-8")
        recipe = self.recipe()
        recipe["mounts"].append({"name": "state_store", "source": str(state_directory), "inputs": ["."]})
        recipe["steps"][0]["arguments"].append("${state_store}/ledger.sqlite3-journal")
        self.save(recipe)
        # Introduce the sidecar after Store has opened its WAL database, before
        # the model tool call starts source/mount import. This avoids SQLite's
        # own startup handling of a preexisting rollback journal.
        with self.server.guard:
            self.server.before_reply = lambda: journal.write_text("host-only rollback journal", encoding="utf-8")
        self.replies(model_reply("chat-completions", {}))
        first = self.invoke(*self.model_args("chat-completions", "--state", self.database, "--max-turns", "1"), expected=1)
        self.assertEqual(first["status"], "exhausted")
        self.assertEqual(journal.read_text(), "host-only rollback journal")
        detail = self.invoke("show", first["task_id"], "--workspace", self.workspace, "--state", self.database)
        self.assertEqual(len(detail["actions"]), 1)
        bundle = self.verify_artifacts(detail["actions"][0])
        evidence = json.loads((bundle / "files/out/identity-0.json").read_text())
        self.assertFalse(evidence["staged_file_exists"], "SQLite rollback journal reached an isolated toolchain mount")
        self.assertEqual(evidence["staged_file_error"], 2)  # ERROR_FILE_NOT_FOUND, not merely denied access.
        self.assertEqual(detail["actions"][0]["result"]["output"]["imported"]["mount_files"], 2)
        self.verify_identity(evidence)

    def test_child_and_grandchild_keep_lpac_job_and_denied_boundaries(self):
        self.save(self.recipe("tree"))
        bundle = self.verify_artifacts(self.action(self.build()["task_id"]))
        identities = [json.loads((bundle / f"files/out/identity-{generation}.json").read_text()) for generation in range(3)]
        self.assertEqual(len({item["pid"] for item in identities}), 3)
        self.assertEqual(len({item["appcontainer_sid"] for item in identities}), 1,
                         "Descendants changed AppContainer identity")
        for generation, evidence in enumerate(identities):
            self.assertEqual(evidence["generation"], generation)
            self.verify_identity(evidence)

    def test_failed_step_publishes_nothing_and_stops_following_steps(self):
        recipe = self.recipe()
        failed = copy.deepcopy(recipe["steps"][0])
        failed["arguments"][0] = "fail"
        recipe["steps"] += [failed, copy.deepcopy(recipe["steps"][0])]
        self.save(recipe)
        result = self.action(self.build(expected=1)["task_id"])["result"]
        self.assertFalse(result["ok"])
        self.assertEqual(result["effect"], "none")
        self.assertNotIn("artifacts", result["output"])
        self.assertEqual([step["exit_code"] for step in result["output"]["steps"]], [0, 17])
        self.assertEqual(self.bundles(), [])
        self.assertEqual((self.workspace / "input.txt").read_text(), SOURCE)

    def test_missing_declared_output_does_not_publish_partial_bundle(self):
        recipe = self.recipe()
        recipe["outputs"] += ["missing-output"]
        self.save(recipe)
        result = self.action(self.build(expected=1)["task_id"])["result"]
        self.assertFalse(result["ok"])
        self.assertEqual(result["effect"], "none")
        self.assertNotIn("artifacts", result["output"])
        self.assertEqual(self.bundles(), [])

    def test_timeout_reaps_tree_and_never_exports(self):
        self.save(self.recipe("tree-sleep", timeout=3000))
        result = self.action(self.build(expected=1)["task_id"])["result"]
        self.assertFalse(result["ok"])
        self.assertEqual(result["effect"], "none")
        self.assertTrue(result["output"]["timed_out"])
        self.assertNotIn("artifacts", result["output"])
        evidence = [json.loads(line) for step in result["output"]["steps"] for line in step["output"].splitlines()]
        self.assertEqual(len(evidence), 3)
        for item in evidence:
            self.verify_identity(item)
        self.assertEqual(self.bundles(), [])

    def test_service_build_mode_runs_and_rejects_model_or_process_overrides(self):
        self.save(self.recipe())
        client = self.client()
        for extra in ({"model": "forbidden"}, {"allow_write": True}, {"sandbox": "none"}):
            with self.subTest(extra=extra):
                client.rpc("tasks/start", {"mode": "build", "options": {"build_config": str(self.manifest), **extra}}, error=True)
        finished = client.finished(client.start(self.manifest))
        self.assertEqual(finished["exit_code"], 0)
        action = self.action(finished["result"]["task_id"])
        self.assertEqual(finished["result"]["build"], action["result"], "Service must expose the persisted build receipt")
        self.verify_artifacts(action)
        self.assertNotIn(TOKEN.encode(), bytes(client.raw) + bytes(client.stderr))

    def live_tree(self, client):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            # Surface an early backend rejection immediately instead of waiting
            # the full descendant-start deadline with an opaque timeout.
            while True:
                try:
                    frame = client.queue.get_nowait()
                except queue.Empty:
                    break
                if isinstance(frame, Exception):
                    self.fail(f"Service stopped before descendants started: {frame}")
                client.pending.append(frame)
            for frame in client.pending:
                if frame.get("method") == "operation/finished":
                    self.fail(f"Build finished before its descendant tree was observed: {frame['params']!r}")
            pids = fixture_descendants(client.process.pid)
            if len(pids) == 3:
                evidence = []
                for pid in pids:
                    handle = KERNEL.OpenProcess(0x00100000 | 0x1000, False, pid)  # SYNCHRONIZE | QUERY_LIMITED_INFORMATION
                    self.assertTrue(handle, f"Unable to inspect fixture child {pid}")
                    self.handles.append(handle)
                    image = ctypes.create_unicode_buffer(32768)
                    size = wintypes.DWORD(len(image))
                    self.assertTrue(KERNEL.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(size)))
                    evidence.append((handle, pathlib.Path(image.value)))
                return evidence
            if client.process.poll() is not None:
                self.fail(f"Service exited before spawning descendants: {bytes(client.stderr)!r}")
            time.sleep(0.02)
        self.fail(f"Expected a live parent/child/grandchild tree; pending={client.pending!r}")

    def assert_tree_cleaned(self, evidence):
        for handle, image in evidence:
            self.assertEqual(KERNEL.WaitForSingleObject(handle, 5000), 0, f"Build descendant still running: {image}")
            self.assertFalse(image.exists(), f"Private mount leaked after build completion: {image}")
        self.assertEqual(self.bundles(), [])
        self.assertEqual((self.workspace / "input.txt").read_text(), SOURCE)

    def test_service_cancellation_reaps_parent_child_and_grandchild(self):
        self.save(self.recipe("tree-sleep", timeout=20000))
        client = self.client()
        operation = client.start(self.manifest)
        evidence = self.live_tree(client)
        self.assertTrue(client.rpc("operations/cancel", {"operation_id": operation})["cancel_requested"])
        finished = client.finished(operation)
        self.assertEqual(finished["exit_code"], 130)
        self.assertEqual(finished["result"]["status"], "cancelled")
        receipt = self.action(finished["result"]["task_id"])["result"]
        self.assertEqual(receipt["effect"], "none")
        self.assertTrue(receipt["output"]["cancelled"])
        self.assert_tree_cleaned(evidence)

    def test_service_stdin_eof_cancels_and_cleans_live_build_tree(self):
        self.save(self.recipe("tree-sleep", timeout=20000))
        client = self.client()
        client.start(self.manifest)
        evidence = self.live_tree(client)
        client.process.stdin.close()
        self.assertEqual(client.wait_exit(), 0)
        self.assert_tree_cleaned(evidence)
        with closing(sqlite3.connect(self.database)) as database:
            tasks = database.execute("SELECT status FROM tasks").fetchall()
            actions = database.execute("SELECT status,effect FROM actions").fetchall()
        self.assertEqual(tasks, [("cancelled",)])
        self.assertEqual(actions, [("failed", "none")])

    def test_host_crash_releases_build_drive_and_reaps_descendants(self):
        self.save(self.recipe("tree-sleep", timeout=20000))
        process = subprocess.Popen([str(EXE), "build", "--workspace", str(self.workspace),
            "--build-config", str(self.manifest)], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        # Reuse the descendant observer without creating a service: direct CLI
        # output stays bounded and is collected once this owned host exits.
        observer = type("DirectBuildObserver", (), {})()
        observer.process, observer.queue = process, queue.Queue()
        observer.pending, observer.stderr = [], bytearray()
        KERNEL.QueryDosDeviceW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
        KERNEL.QueryDosDeviceW.restype = wintypes.DWORD
        KERNEL.GetProcessId.argtypes = [wintypes.HANDLE]
        KERNEL.GetProcessId.restype = wintypes.DWORD

        def mapping(drive):
            data = ctypes.create_unicode_buffer(32768)
            count = KERNEL.QueryDosDeviceW(drive, data, len(data))
            if not count:
                error = ctypes.get_last_error()
                if error == 2:  # ERROR_FILE_NOT_FOUND: the temporary link is gone.
                    return ()
                raise ctypes.WinError(error)
            return tuple(item for item in data[:count].split("\0") if item)

        try:
            evidence = self.live_tree(observer)
            physical_image = evidence[0][1].resolve(strict=True)
            self.assertEqual(physical_image.parent.name, "fixture")
            self.assertEqual(physical_image.parent.parent.name, "bin")
            stage = physical_image.parents[2]
            self.assertTrue(stage.name.startswith("codestudio-sandbox-"))
            # The measured executable is inside this unique private staging
            # tree. Read its three generated identities before killing the host.
            identities = []
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                try:
                    identities = [json.loads((stage / f"work/workspace/out/identity-{generation}.json").read_text())
                                  for generation in range(3)]
                    break
                except (FileNotFoundError, json.JSONDecodeError):
                    time.sleep(0.02)
            self.assertEqual(len(identities), 3, "All descendant identities must exist before the simulated crash")
            self.assertEqual({item["pid"] for item in identities}, {KERNEL.GetProcessId(handle) for handle, _ in evidence})
            drives = {pathlib.PureWindowsPath(item["image"]).drive for item in identities}
            self.assertEqual(len(drives), 1)
            drive = drives.pop()
            self.assertEqual(len(drive), 2)
            self.assertEqual(drive[1], ":")
            before = mapping(drive)  # Query only this task's observed drive.
            self.assertTrue(before)
            self.assertTrue(any(target.casefold().endswith(stage.name.casefold()) for target in before),
                            f"Observed drive does not point at this private stage: {drive} {before}")
            with closing(sqlite3.connect(self.database)) as database:
                rows = database.execute("SELECT task_id,status,effect FROM actions").fetchall()
            self.assertEqual(len(rows), 1)
            task, status, effect = rows[0]
            self.assertEqual((status, effect), ("dispatched", "unknown"))
            process.kill()  # Terminate only the CLI process created by this test.
            out, err = process.communicate(timeout=10)
            self.assertNotEqual(process.returncode, 0)
            self.assertNotIn(TOKEN.encode(), out + err)
            self.assertNotIn(SECRET.encode(), out + err)
            for handle, image in evidence:
                self.assertEqual(KERNEL.WaitForSingleObject(handle, 5000), 0,
                                 f"Host crash left a build descendant running: {image}")
            deadline = time.monotonic() + 5
            after = mapping(drive)
            while set(before).intersection(after) and time.monotonic() < deadline:
                time.sleep(0.02)
                after = mapping(drive)
            # A concurrent build may reuse the free letter; its different target
            # is permitted, while retaining this crashed task's mapping is not.
            self.assertFalse(set(before).intersection(after), f"Crashed build drive mapping survived: {drive} {after}")
            recovered = self.invoke("recover", "--workspace", self.workspace)
            self.assertFalse(recovered["tools_executed"])
            self.assertEqual(len(recovered["recovered"]), 1)
            self.assertEqual(recovered["recovered"][0]["task_id"], task)
            self.assertEqual(recovered["recovered"][0]["unknown_actions"], 1)
            action = self.action(task)
            self.assertEqual((action["status"], action["effect"]), ("unknown", "unknown"))
            self.invoke("resume", task, "--workspace", self.workspace, "--build-config", self.manifest, expected=2)
            self.assertEqual(self.bundles(), [])
            self.assertEqual((self.workspace / "input.txt").read_text(), SOURCE)
            self.assertEqual(self.requests(), [])
            self.reclaim_crash_stage(stage, task)
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)

    def test_model_cannot_override_granted_build_arguments(self):
        self.save(self.recipe())
        for api in APIS:
            with self.subTest(api=api):
                self.replies(model_reply(api, {"command": "escape", "outputs": ["input.txt"]}), model_reply(api))
                result = self.invoke(*self.model_args(api), expected=1)
                self.assertEqual(result["status"], "needs_attention")
                action = self.action(result["task_id"])
                self.assertEqual((action["status"], action["effect"]), ("denied", "none"))
                self.assertIn("empty", action["result"]["error"])
        self.assertEqual(self.bundles(), [])

    def test_all_provider_schemas_resume_binding_and_exactly_once_export(self):
        self.save(self.recipe())
        frozen_manifest = self.manifest.read_bytes()
        for api in APIS:
            with self.subTest(api=api):
                self.replies(model_reply(api, {}))
                first = self.invoke(*self.model_args(api, "--max-turns", "1"), expected=1)
                self.assertEqual(first["status"], "exhausted")
                task = first["task_id"]
                action = self.action(task)
                bundle = self.verify_artifacts(action)
                before = self.bundles()
                exported = {str(p.relative_to(bundle)): p.read_bytes() for p in bundle.rglob("*") if p.is_file()}
                requests = self.requests()
                tool = next(tool for tool in requests[-1]["body"]["tools"]
                            if tool.get("name", tool.get("function", {}).get("name")) == "build_workspace")
                parameters = tool["function"]["parameters"] if api == "chat-completions" else tool[
                    "input_schema" if api == "messages" else "parameters"]
                self.assertEqual(parameters, {"type": "object", "properties": {}, "required": [], "additionalProperties": False})
                self.invoke(*self.resume_args(task, "--max-turns", "2"), expected=2)
                self.manifest.write_bytes(frozen_manifest + b"\n")  # Semantically identical is still a changed grant.
                self.invoke(*self.resume_args(task, "--build-config", self.manifest, "--max-turns", "2"), expected=2)
                self.manifest.write_bytes(frozen_manifest)
                self.invoke(*self.resume_args(task, "--build-config", self.manifest, "--allow-write", "--max-turns", "2"), expected=2)
                self.assertEqual(self.requests(), requests, "Rejected bindings reached the provider")
                self.replies(model_reply(api))
                final = self.invoke(*self.resume_args(task, "--build-config", self.manifest, "--max-turns", "2"))
                self.assertEqual((final["task_id"], final["status"]), (task, "completed"))
                self.assertEqual(self.action(task), action, "Resume changed the original build receipt")
                self.assertEqual(self.bundles(), before, "Resume exported another artifact bundle")
                self.assertEqual(exported, {str(p.relative_to(bundle)): p.read_bytes() for p in bundle.rglob("*") if p.is_file()})
                self.assertEqual(len(self.requests()), len(requests) + 1)



    def test_v2_resume_binds_recipe_format_and_normalized_plan_without_reexport(self):
        recipe = self.recipe()
        recipe.update(version=2, environment={"CARGO_HOME": "${home}/cargo"})
        self.save(recipe)
        original_manifest = self.manifest.read_bytes()
        api = "responses"
        self.replies(model_reply(api, {}))
        first = self.invoke(*self.model_args(api, "--max-turns", "1"), expected=1)
        self.assertEqual(first["status"], "exhausted")
        task = first["task_id"]
        action = self.action(task)
        bundle = self.verify_artifacts(action)
        exported = {str(p.relative_to(bundle)): p.read_bytes() for p in bundle.rglob("*") if p.is_file()}
        before = self.bundles()
        requests = self.requests()
        with closing(sqlite3.connect(self.database)) as database:
            original_binding = database.execute("SELECT binding FROM task_checkpoints WHERE task_id=?",
                                                (task,)).fetchone()[0]
        binding = json.loads(original_binding)
        build_binding = binding["configuration"]["build"]
        self.assertEqual(build_binding["version"], 2)
        self.assertNotIn("plugin", build_binding)
        self.assertEqual(build_binding["recipe"]["version"], 2)
        self.assertEqual(build_binding["recipe"]["steps"], recipe["steps"])
        self.assertEqual(build_binding["recipe"]["environment"], recipe["environment"])
        # Change the format version or normalized plan while the
        # authorized source manifest remains byte-for-byte identical. A raw
        # manifest-only binding would incorrectly accept both resumptions.
        for changed_field in ("version", "recipe"):
            with self.subTest(changed_field=changed_field):
                changed = copy.deepcopy(binding)
                if changed_field == "version":
                    changed["configuration"]["build"]["version"] = 999
                else:
                    changed["configuration"]["build"]["recipe"]["environment"]["CARGO_HOME"] = "${home}/changed"
                with closing(sqlite3.connect(self.database)) as database:
                    database.execute("UPDATE task_checkpoints SET binding=? WHERE task_id=?",
                                     (json.dumps(changed), task))
                    database.commit()
                try:
                    self.invoke(*self.resume_args(task, "--build-config", self.manifest, "--max-turns", "2"), expected=2)
                    self.assertEqual(self.requests(), requests)
                    self.assertEqual(self.bundles(), before)
                finally:
                    with closing(sqlite3.connect(self.database)) as database:
                        database.execute("UPDATE task_checkpoints SET binding=? WHERE task_id=?",
                                         (original_binding, task))
                        database.commit()
        changed_recipe = copy.deepcopy(recipe)
        changed_recipe["environment"]["CARGO_HOME"] = "${home}/another-cargo"
        self.save(changed_recipe)
        self.invoke(*self.resume_args(task, "--build-config", self.manifest, "--max-turns", "2"), expected=2)
        self.manifest.write_bytes(original_manifest)
        self.assertEqual(self.requests(), requests)
        self.replies(model_reply(api))
        final = self.invoke(*self.resume_args(task, "--build-config", self.manifest, "--max-turns", "2"))
        self.assertEqual((final["task_id"], final["status"]), (task, "completed"))
        self.assertEqual(self.action(task), action)
        self.assertEqual(self.bundles(), before)
        self.assertEqual(exported, {str(p.relative_to(bundle)): p.read_bytes() for p in bundle.rglob("*") if p.is_file()})
        self.assertEqual(len(self.requests()), len(requests) + 1)


if __name__ == "__main__":
    unittest.main()
