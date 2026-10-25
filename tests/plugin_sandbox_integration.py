"""Real locked multi-file MCP packages inside the native OS sandbox.

Every binary/package/model reply is an owned fixture. No installed third-party
plugin, user credential, external network service or permanent sandbox remains.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import resume_integration as fixture

_arguments = sys.argv[:]
sys.argv[:] = [_arguments[0], str(fixture.EXE)]
try:
    import service_integration as service_fixture
finally:
    sys.argv[:] = _arguments

WINDOWS = os.name == "nt"
MODE = "appcontainer" if WINDOWS else "linux-namespace"
IMAGE = fixture.EXE.parent / ("plugin-sandbox-fixture.exe" if WINDOWS else "plugin-sandbox-fixture")
LIBRARY = fixture.EXE.parent / ("plugin-sandbox-library.dll" if WINDOWS else "libplugin-sandbox-library.so")
DEPENDENCY = fixture.EXE.parent / ("plugin-sandbox-dependency.dll" if WINDOWS else "libplugin-sandbox-dependency.so")


class PluginSandboxTests(unittest.TestCase):
    run_args = fixture.ResumeTests.run_args
    resume_args = fixture.ResumeTests.resume_args
    setup_responses = fixture.ResumeTests.setup_responses
    requests = fixture.ResumeTests.requests
    sql_rows = fixture.ResumeTests.sql_rows
    sql_execute = fixture.ResumeTests.sql_execute
    wait_for = fixture.ResumeTests.wait_for
    start = fixture.ResumeTests.start
    kill = staticmethod(fixture.ResumeTests.kill)

    def setUp(self):
        fixture.ResumeTests.setUp(self)
        self.workspace = self.workspace.resolve()
        self.database = self.workspace / ".codestudio/state.sqlite3"
        self.assertTrue(IMAGE.is_file(), "Build the native plugin-sandbox-fixture target")
        self.assertTrue(LIBRARY.is_file(), "Build the native plugin-sandbox-library target")
        self.assertTrue(DEPENDENCY.is_file(), "Build the native plugin-sandbox-dependency target")
        self.source = self.workspace / "plugin source"
        (self.source / "deps").mkdir(parents=True)
        (self.source / "configuration").mkdir()
        shutil.copyfile(IMAGE, self.source / IMAGE.name)
        shutil.copyfile(LIBRARY, self.source / "deps" / LIBRARY.name)
        # LPAC's dependency search uses the application directory. Keep the
        # explicitly loaded library nested, with its Windows import beside EXE.
        self.dependency_relative = Path(DEPENDENCY.name) if WINDOWS else Path("deps") / DEPENDENCY.name
        shutil.copyfile(DEPENDENCY, self.source / self.dependency_relative)
        if not WINDOWS:
            (self.source / IMAGE.name).chmod(0o755)
            (self.source / "deps" / LIBRARY.name).chmod(0o755)
            (self.source / "deps" / DEPENDENCY.name).chmod(0o755)
        self.config = {"value": "nested package configuration"}
        self.descriptor = {"format": 2, "id": "isolated-fixture", "version": "1.0.0",
                           "platform": "windows" if WINDOWS else "linux",
                           "arch": "x64" if WINDOWS else "x86_64", "entrypoint": IMAGE.name,
                           "args": ["${plugin}/configuration/settings.json", "${plugin}/deps/" + LIBRARY.name],
                           "tools": ["probe", "spawn", "hang", "hang_child"]}
        self.bundle = self.workspace / "fixture.csplugin"
        self.store = self.workspace / "plugin store"
        self.lock = self.workspace / "fixture.lock.json"
        self.manifest = self.workspace / "mcp.json"
        self.outside = self.workspace / "private.txt"
        self.outside.write_text("host-private-sentinel", encoding="utf-8")
        self.forbidden = self.workspace / "must-not-exist.txt"
        self.env["CODESTUDIO_PLUGIN_TEST_SECRET"] = "do-not-inherit-plugin-secret"
        self.clients = []
        self.before_resources = self.resources()

    def tearDown(self):
        errors = []
        for client in self.clients:
            try:
                client.close()
                self.assertNotIn(fixture.TOKEN.encode(), bytes(client.raw) + bytes(client.stderr))
            except Exception as error:
                errors.append(error)
        for process in self.children:
            try:
                self.kill(process)
            except Exception as error:
                errors.append(error)
        self.children.clear()
        try:
            self.assert_resources_released()
        except Exception as error:
            errors.append(error)
        try:
            fixture.ResumeTests.tearDown(self)
        except Exception as error:
            errors.append(error)
        if errors:
            for error in errors[1:]:
                errors[0].add_note("Additional cleanup error: " + str(error))
            raise errors[0]

    def resources(self):
        roots = [Path(tempfile.gettempdir())]
        prefix = "codestudio-sandbox-" if WINDOWS else "codestudio-isolation-"
        if WINDOWS:
            roots.append(Path(os.environ["LOCALAPPDATA"]) / "Packages")
        elif os.environ.get("CODESTUDIO_CGROUP_ROOT"):
            roots.append(Path(os.environ["CODESTUDIO_CGROUP_ROOT"]))
        return {entry for root in roots if root.exists() for entry in root.glob(prefix + "*")
                if re.fullmatch(re.escape(prefix) + r"[0-9a-f]{32}", entry.name)}

    def assert_resources_released(self):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            remaining = self.resources() - self.before_resources
            if not remaining:
                return
            time.sleep(0.02)
        self.assertEqual(remaining, set(), "Owned staging/profile/cgroup resources were not released")

    def capture(self, *args):
        result = subprocess.run([str(fixture.EXE), *map(str, args)], cwd=self.workspace,
                                env=self.env, capture_output=True, timeout=30)
        for secret in (fixture.TOKEN, "do-not-inherit-plugin-secret"):
            self.assertNotIn(secret.encode(), result.stdout + result.stderr)
        return result

    def invoke(self, *args, expected=0):
        result = self.capture(*args)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def prepare(self, **limits):
        (self.source / "configuration/settings.json").write_text(json.dumps(self.config), encoding="utf-8")
        (self.source / "plugin.json").write_text(json.dumps(self.descriptor), encoding="utf-8")
        self.invoke("plugin-pack", "--source", self.source, "--output", self.bundle)
        digest = hashlib.sha256(self.bundle.read_bytes()).hexdigest()
        installed = self.invoke("plugin-install", "--bundle", self.bundle, "--sha256", digest, "--store", self.store)
        self.installed = Path(installed["directory"])
        self.invoke("plugin-lock", "--directory", self.installed, "--output", self.lock)
        self.configure(limits=limits or None)
        self.original_files = self.inventory(self.installed)

    def configure(self, limits=None, **overrides):
        server = {"id": "plugin", "plugin_lock": str(self.lock), "tools": self.descriptor["tools"], "timeout_ms": 3000}
        if limits is not None:
            server["sandbox_limits"] = limits
        server.update(overrides)
        self.manifest.write_text(json.dumps({"version": 2, "servers": [server]}), encoding="utf-8")
        return server

    def grants(self, mode=MODE, granted=True):
        return ["--mcp-config", self.manifest, *(["--allow-mcp", "plugin"] if granted else []), "--sandbox", mode]

    def discover(self, expected=0, mode=MODE):
        return self.invoke("mcp-tools", "--workspace", self.workspace, *self.grants(mode), expected=expected)

    def inventory(self, root):
        return {p.relative_to(root).as_posix(): (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mode)
                for p in root.rglob("*") if p.is_file()}

    def probe_arguments(self):
        return {"outside_file": str(self.outside), "outside_new": str(self.forbidden), "port": self.server.server_port}

    def tool(self, name, arguments=None, expected=0):
        self.setup_responses({"body": fixture.reply(tool=("mcp_plugin_" + name, arguments or {}))},
                             {"body": fixture.reply()})
        result = self.invoke(*self.run_args(*self.grants()), expected=expected)
        detail = self.invoke("show", result["task_id"], "--workspace", self.workspace)
        if expected:
            return result, detail
        self.assertEqual(result["status"], "completed")
        self.assertEqual(detail["actions"][0]["status"], "succeeded")
        return detail["actions"][0]["result"]["output"]["structuredContent"]

    def assert_probe(self, report):
        self.assertEqual(report["config_value"], "nested package configuration")
        self.assertEqual(report["library_value"], 7331)
        self.assertEqual(report["scratch_value"], "private scratch")
        self.assertFalse(report["scratch_existed"])
        self.assertFalse(report["secret_present"])
        for field in ("outside_read", "outside_write", "outside_create", "package_write", "package_create", "image_write"):
            self.assertNotEqual(report[field], 0, (field, report))
        self.assertNotIn(str(self.installed), report["config_path"])
        self.assertNotIn(str(self.installed), report["library_path"])
        self.assertNotEqual(report["cwd"], str(self.workspace))
        if WINDOWS:
            self.assertTrue(report["appcontainer"])
            self.assertEqual(report["outside_read"], 5)
            network = report["network"]
            self.assertTrue(network.get("startup", 0) or network.get("socket_error", 0) or network.get("connect_error", 0), network)
            self.assertFalse(Path(report["cwd"]).exists())
        else:
            self.assertEqual((report["seccomp"], report["no_new_privileges"]), (2, 1))
            self.assertEqual(report["cwd"], "/work")
            self.assertTrue(report["config_path"].startswith("/tools/"))
            self.assertEqual(report["network"]["socket_error"], 1)
        self.assertFalse(self.forbidden.exists())
        self.assertEqual(self.outside.read_text(), "host-private-sentinel")
        self.assertEqual(self.inventory(self.installed), self.original_files)
        self.assert_resources_released()

    def test_private_library_and_nested_config_run_with_readonly_package_and_private_scratch(self):
        self.prepare()
        self.assert_probe(self.tool("probe", self.probe_arguments()))

    def test_each_run_gets_fresh_scratch_and_preserves_locked_source(self):
        self.prepare()
        first = self.tool("probe", self.probe_arguments())
        self.assert_probe(first)
        second = self.tool("probe", self.probe_arguments())
        self.assert_probe(second)
        if WINDOWS:
            self.assertNotEqual(first["cwd"], second["cwd"])

    def test_default_process_limit_denies_child_creation(self):
        self.prepare()
        result = self.tool("spawn")
        self.assertFalse(result["child_created"], result)
        self.assertNotEqual(result["spawn_error"], 0)
        self.assertEqual(self.inventory(self.installed), self.original_files)

    def test_explicit_child_process_grant_executes_only_inside_owned_lifetime(self):
        self.prepare(processes=2)
        result = self.tool("spawn")
        self.assertTrue(result["child_created"], result)
        self.assertEqual(result["child_exit"], 0)
        self.assertTrue(result["child_marker"].startswith("child "), result)
        if WINDOWS:
            self.assertFalse(Path(result["marker"]).exists())
        self.assert_resources_released()

    def test_explicit_memory_cpu_and_process_limits_reach_native_job_or_cgroup(self):
        self.prepare(processes=2, memory_mb=192, cpu_percent=35)
        release = threading.Event()
        self.releases.append(release)
        self.setup_responses({"body": fixture.reply(tool=("mcp_plugin_probe", self.probe_arguments()))},
                             {"body": fixture.reply(), "release": release})
        process = self.start(*self.run_args(*self.grants()))
        self.wait_for(lambda: len(self.requests()) == 2, process)
        action = json.loads(self.sql_rows("SELECT result FROM actions")[0][0])
        report = action["output"]["structuredContent"]
        if WINDOWS:
            self.assertEqual((report["process_limit"], report["memory_limit"], report["cpu_percent"]), (2, 192 * 1024 * 1024, 35))
        else:
            groups = [p for p in self.resources() - self.before_resources if str(p).startswith("/sys/fs/cgroup/")]
            self.assertEqual(len(groups), 1, groups)
            self.assertEqual((groups[0] / "pids.max").read_text().strip(), "4")
            self.assertEqual((groups[0] / "memory.max").read_text().strip(), str(192 * 1024 * 1024))
            self.assertEqual((groups[0] / "cpu.max").read_text().strip(), "35000 100000")
        release.set()
        out, err = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, out + err)
        self.assert_probe(report)

    def test_tool_timeout_preserves_unknown_effect_and_blocks_resume(self):
        self.prepare()
        self.configure(timeout_ms=300)
        started = time.monotonic()
        result, detail = self.tool("hang", expected=1)
        self.assertLess(time.monotonic() - started, 10)
        self.assertEqual(result["status"], "needs_attention")
        self.assertEqual((detail["actions"][0]["status"], detail["actions"][0]["effect"]), ("unknown", "unknown"))
        count = len(self.requests())
        self.invoke(*self.resume_args(result["task_id"], *self.grants()), expected=2)
        self.assertEqual(len(self.requests()), count)
        self.assert_resources_released()

    def test_initialization_timeout_cleans_complete_package_without_fallback(self):
        self.config["hang_initialize"] = True
        self.prepare()
        self.configure(timeout_ms=300)
        started = time.monotonic()
        self.discover(expected=2)
        self.assertLess(time.monotonic() - started, 10)
        self.assertFalse(self.database.exists())
        self.assert_resources_released()

    def test_invalid_limits_refuse_before_model_and_task_dispatch(self):
        self.prepare()
        for limits in ({"processes": 0}, {"processes": 33}, {"memory_mb": 63}, {"memory_mb": 4097},
                       {"cpu_percent": 0}, {"cpu_percent": 101}, {"processes": True}, {"network": True}):
            with self.subTest(limits=limits):
                self.configure(limits=limits)
                self.invoke(*self.run_args(*self.grants()), expected=2)
                self.assertEqual(self.requests(), [])
                self.assertEqual(self.sql_rows("SELECT count(*) FROM tasks"), [(0,)])
                self.assert_resources_released()

    def test_limits_require_a_locked_package_and_explicit_isolation(self):
        self.prepare(processes=2)
        self.discover(expected=2, mode="none")
        server = {"id": "plugin", "command": str(IMAGE), "args": [], "tools": ["probe"], "sandbox_limits": {"processes": 2}}
        self.manifest.write_text(json.dumps({"version": 2, "servers": [server]}), encoding="utf-8")
        self.discover(expected=2)
        self.assertFalse(self.database.exists())
        self.assert_resources_released()

    def test_ungranted_package_does_not_launch_or_expose_tools(self):
        self.prepare()
        self.setup_responses({"body": fixture.reply()})
        result = self.invoke(*self.run_args(*self.grants(granted=False)))
        self.assertEqual(result["status"], "completed")
        tools = self.requests()[0]["body"]["tools"]
        self.assertFalse(any(item["function"]["name"].startswith("mcp_") for item in tools))
        self.assert_resources_released()

    def test_modified_private_library_is_refused_before_initialization(self):
        self.prepare()
        with (self.installed / "deps" / LIBRARY.name).open("ab") as output:
            output.write(b"tampered")
        self.discover(expected=2)
        self.assertFalse(self.database.exists())
        self.assert_resources_released()

    def test_linked_package_file_is_refused_without_touching_target(self):
        self.prepare()
        target = self.installed / "configuration/settings.json"
        target.unlink()
        os.link(self.outside, target)
        self.discover(expected=2)
        self.assertEqual(self.outside.read_text(), "host-private-sentinel")
        self.assertFalse(self.database.exists())
        self.assert_resources_released()

    def test_invalid_library_cannot_silently_fall_back_to_unisolated_process(self):
        (self.source / "deps" / LIBRARY.name).write_bytes(b"not-a-loadable-library")
        self.prepare()
        self.discover(expected=2)
        self.assertFalse(self.database.exists())
        self.assert_resources_released()

    def test_missing_transitive_dependency_refuses_without_host_loader_fallback(self):
        (self.source / self.dependency_relative).unlink()
        self.prepare()
        self.discover(expected=2)
        self.assertFalse(self.database.exists())
        self.assert_resources_released()

    def test_changed_process_limits_block_resume_until_original_binding_is_restored(self):
        self.prepare(processes=1, memory_mb=192, cpu_percent=35)
        self.setup_responses({"status": 503})
        task = self.invoke(*self.run_args(*self.grants()), expected=1)["task_id"]
        original = self.manifest.read_bytes()
        self.configure(limits={"processes": 2, "memory_mb": 192, "cpu_percent": 35})
        self.invoke(*self.resume_args(task, *self.grants()), expected=2)
        self.assertEqual(len(self.requests()), 1)
        self.assert_resources_released()
        self.manifest.write_bytes(original)
        self.setup_responses({"body": fixture.reply()})
        result = self.invoke(*self.resume_args(task, *self.grants()))
        self.assertEqual((result["task_id"], result["status"]), (task, "completed"))

    def test_missing_or_changed_package_execution_binding_cannot_resume(self):
        self.prepare()
        self.setup_responses({"status": 503})
        task = self.invoke(*self.run_args(*self.grants()), expected=1)["task_id"]
        original = self.sql_rows("SELECT binding FROM task_checkpoints WHERE task_id=?", (task,))[0][0]
        for missing in (True, False):
            with self.subTest(missing=missing):
                binding = json.loads(original)
                plugin = binding["configuration"]["mcp"]["plugins"]["plugin"]
                self.assertEqual(plugin["execution"]["kind"], "isolated-process-package")
                if missing:
                    del plugin["execution"]
                else:
                    plugin["execution"]["memory_bytes"] *= 2
                self.sql_execute("UPDATE task_checkpoints SET binding=? WHERE task_id=?", (json.dumps(binding), task))
                self.invoke(*self.resume_args(task, *self.grants()), expected=2)
                self.assertEqual(len(self.requests()), 1)
                self.assert_resources_released()
        self.sql_execute("UPDATE task_checkpoints SET binding=? WHERE task_id=?", (original, task))

    def test_service_cancel_terminates_granted_child_and_removes_package_resources(self):
        self.prepare(processes=2)
        self.configure(limits={"processes": 2}, timeout_ms=15000)
        self.setup_responses({"body": fixture.reply(tool=("mcp_plugin_hang_child", {}))})
        client = service_fixture.Client(self.workspace, self.env)
        self.clients.append(client)
        client.initialize()
        operation = client.start({"prompt": "Exercise packaged child cancellation", "base_url": self.url,
                                  "model": "fixture", "allow_http_loopback": True, "stream": False,
                                  "mcp_config": str(self.manifest), "allow_mcp": ["plugin"], "sandbox": MODE})
        def child_started():
            return [p / "work/child.started" for p in self.resources() - self.before_resources
                    if not str(p).startswith("/sys/fs/cgroup/") and (p / "work/child.started").is_file()]
        self.wait_for(child_started, client.process)
        self.assertEqual(self.sql_rows("SELECT status FROM actions"), [("dispatched",)])
        client.rpc("operations/cancel", {"operation_id": operation})
        done = client.finished(operation)
        self.assertNotEqual(done["exit_code"], 0)
        self.assertEqual(done["result"]["status"], "needs_attention")
        self.assertEqual(self.sql_rows("SELECT status,effect FROM actions"), [("unknown", "unknown")])
        self.assert_resources_released()
        self.assertEqual(self.inventory(self.installed), self.original_files)


if __name__ == "__main__":
    unittest.main()
