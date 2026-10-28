"""Exercise the real CLI and SQLite receipts against a local model HTTP fixture."""
import json
import os
import pathlib
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
PROCESS = EXE.parent / ("process-fixture.exe" if os.name == "nt" else "process-fixture")
TOKEN = "cli-test-credential-never-persist"


def final(text="Verified fixture result"):
    return {"choices": [{"finish_reason": "stop", "message": {"role": "assistant", "content": text}}]}


def call(name, arguments, call_id="call_1"):
    return {"choices": [{"finish_reason": "tool_calls", "message": {
        "role": "assistant", "content": None, "tool_calls": [{"id": call_id, "type": "function",
        "function": {"name": name, "arguments": json.dumps(arguments)}}]}}]}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with self.server.guard:
            self.server.requests.append(data)
            response = self.server.responses.pop(0) if self.server.responses else final("Unexpected extra turn")
        body = json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (ConnectionResetError, BrokenPipeError):
            pass


class CliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.server.guard = threading.Lock()
        cls.server.requests = []
        cls.server.responses = []
        cls.worker = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.worker.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.worker.join()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="codestudio-cli-")
        self.workspace = pathlib.Path(self.temp.name) / "workspace with spaces"
        self.workspace.mkdir()
        self.database = self.workspace / ".codestudio" / "state.sqlite3"
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("CODESTUDIO_")}
        self.env["CODESTUDIO_API_KEY"] = TOKEN
        with self.server.guard:
            self.server.responses = []
            self.server.requests = []

    def tearDown(self):
        self.temp.cleanup()

    def invoke(self, *args, expected=0):
        process = subprocess.run([str(EXE), *map(str, args)], env=self.env, capture_output=True,
                                 text=True, encoding="utf-8", timeout=15)
        self.assertEqual(process.returncode, expected, process.stdout + process.stderr)
        self.assertNotIn(TOKEN, process.stdout + process.stderr)
        return json.loads(process.stdout)

    def run_args(self, *extra):
        return ["run", "--workspace", str(self.workspace), "--prompt", "Test a bounded engineering task",
                "--base-url", f"http://127.0.0.1:{self.server.server_port}/v1", "--model", "local-fixture",
                "--allow-http-loopback", "--no-stream", *map(str, extra)]

    def set_responses(self, *responses):
        with self.server.guard:
            self.server.responses = list(responses)

    def test_doctor_declares_native_capabilities_for_the_current_platform(self):
        doctor = self.invoke("doctor")
        native_isolation = os.name == "nt" or sys.platform.startswith("linux")
        native_integration = native_isolation or sys.platform == "darwin"
        for capability in ("background_scheduler", "worker_supervision", "login_autostart",
                           "supervised_autostart", "supervisor_diagnostics"):
            self.assertEqual(doctor[capability], native_integration)
        self.assertEqual(doctor["mcp_oauth"]["available"], native_integration)
        self.assertEqual(doctor["os_sandbox"], native_isolation)
        self.assertEqual(doctor["workspace_build"]["available"], native_isolation)
        self.assertEqual(doctor["sandbox_resources"]["available"], native_isolation)
        self.assertTrue(doctor["build_proposals"])
        self.assertTrue(doctor["approval_continuation"])
        self.assertFalse(self.database.exists())

    @unittest.skipIf(os.name == "nt" or sys.platform.startswith("linux"),
                     "Checks unavailable native integrations on other platforms")
    def test_unsupported_native_integrations_fail_without_host_fallback(self):
        for sandbox in ("appcontainer", "linux-namespace"):
            with self.subTest(sandbox=sandbox):
                error = self.invoke(*self.run_args("--sandbox", sandbox), expected=2)
                self.assertIn("isolation was not disabled", error["error"])
                self.assertFalse((self.workspace / "CODESTUDIO_DEMO.txt").exists())
        error = self.invoke("sandbox-resources", expected=2)
        self.assertIn("unavailable", error["error"].lower())
        self.assertFalse(self.database.exists())

    @unittest.skipIf(os.name == "nt" or sys.platform.startswith("linux") or sys.platform == "darwin",
                     "Checks unavailable native integrations on other platforms")
    def test_unsupported_background_and_credentials_fail_without_fallback(self):
        for command in ("scheduler-start", "scheduler-supervise", "scheduler-autostart-enable"):
            with self.subTest(command=command):
                error = self.invoke(command, "--workspace", self.workspace, expected=2)
                self.assertIn("unsupported", error["error"].lower())
        # The default environment secret must not substitute for a requested
        # system credential when no native store exists.
        status = self.invoke("credential-status", "--credential", "system:macos-fixture", expected=2)
        self.assertEqual((status["backend"], status["state"]), ("unsupported", "unavailable"))
        self.invoke(*self.run_args("--credential", "system:macos-fixture"), expected=2)
        self.assertEqual(self.server.requests, [])

    @unittest.skipIf(os.name == "nt" or sys.platform.startswith("linux") or sys.platform == "darwin",
                     "Checks OAuth's unsupported native store and lock")
    def test_unsupported_oauth_login_does_not_contact_server(self):
        manifest = self.workspace / "oauth.json"
        origin = f"http://127.0.0.1:{self.server.server_port}"
        manifest.write_text(json.dumps({"version": 2, "servers": [{
            "id": "oauth", "transport": "streamable-http", "url": origin + "/mcp",
            "allow_http_loopback": True, "tools": ["echo"], "timeout_ms": 1000,
            "oauth": {"issuer": origin + "/issuer", "client_id": "fixture-client",
                      "credential": "system:macos-oauth-fixture", "scopes": ["tools.read"]}}]}))
        for command in ("mcp-auth-login", "mcp-auth-status", "mcp-auth-logout"):
            with self.subTest(command=command):
                error = self.invoke(command, "--mcp-config", manifest, "--server", "oauth", expected=2)
                self.assertIn("unsupported", error["error"].lower())
        self.assertEqual(self.server.requests, [])
        self.assertFalse(self.database.exists())

    @unittest.skipUnless(sys.platform == "darwin", "macOS proposal state without an isolation backend")
    def test_macos_proposal_rejection_continues_but_approval_cannot_execute(self):
        arguments = {"reason": "Review a build without running it", "manifest": {
            "version": 2, "inputs": ["missing-source.cpp"],
            "mounts": [{"name": "compiler", "source": str(self.workspace / "missing-toolchain"),
                        "inputs": ["compiler"]}],
            "steps": [{"executable": "${compiler}/compiler", "arguments": ["missing-source.cpp"]}],
            "outputs": ["out/program"], "artifact_directory": "artifacts"}}
        self.set_responses(call("propose_build", arguments), final("The rejected build was not run"))
        result = self.invoke(*self.run_args("--allow-build-proposals"), expected=3)
        self.assertEqual(result["status"], "waiting_approval")
        task = result["task_id"]
        source = self.invoke("show", task, "--workspace", self.workspace)
        action = source["actions"][0]
        selection = [task, "--action", action["id"], "--workspace", self.workspace]
        reviewed = self.invoke("build-review", *selection)
        self.assertEqual(reviewed["proposal"]["backend"], "unsupported")
        self.assertIsNone(reviewed["decision"])
        digest = reviewed["proposal"]["digest"]
        with closing(sqlite3.connect(self.database)) as database:
            before = "\n".join(database.iterdump())
        error = self.invoke("build-approve", *selection, "--digest", digest, expected=2)
        self.assertIn("no backend", error["error"])
        with closing(sqlite3.connect(self.database)) as database:
            self.assertEqual("\n".join(database.iterdump()), before)
        self.assertEqual(self.invoke("build-review", *selection), reviewed)
        self.assertEqual(len(self.server.requests), 1)
        rejected = self.invoke("build-reject", *selection, "--digest", digest)
        self.assertIsNone(rejected["execution"])
        self.assertEqual(rejected["proposal"]["decision"]["decision"], "rejected")
        again = self.invoke("build-reject", *selection, "--digest", digest)
        self.assertEqual(again["proposal"]["decision"], rejected["proposal"]["decision"])
        resumed = self.invoke("resume", task, "--workspace", self.workspace,
                              "--allow-http-loopback", "--allow-build-proposals")
        self.assertEqual(resumed["status"], "completed")
        self.assertEqual(len(self.server.requests), 2)
        self.assertEqual(len(self.invoke("history", "--workspace", self.workspace)), 1)
        self.assertFalse((self.workspace / "artifacts").exists())
        self.assertFalse((self.workspace / "out").exists())

    def test_offline_demo_history_and_readback(self):
        result = self.invoke("demo", "--workspace", self.workspace)
        self.assertEqual(result["provider"], "offline-demo")
        self.assertEqual((self.workspace / "CODESTUDIO_DEMO.txt").read_bytes(), b"Hello from CodeStudio-Agent!\n")
        rows = self.invoke("history", "--workspace", self.workspace)
        self.assertEqual(rows[0]["id"], result["task_id"])
        detail = self.invoke("show", result["task_id"], "--workspace", self.workspace)
        self.assertEqual([item["status"] for item in detail["actions"]], ["succeeded", "succeeded"])
        self.assertGreater(len(detail["events"]), 5)

    def test_complete_live_protocol_tool_loop(self):
        self.set_responses(call("write_file", {"path": "out.txt", "content": "hello"}, "write"),
                           call("read_file", {"path": "out.txt"}, "read"), final())
        result = self.invoke(*self.run_args("--allow-write"))
        self.assertEqual(result["status"], "completed")
        self.assertEqual((self.workspace / "out.txt").read_text(), "hello")
        self.assertEqual(len(self.server.requests), 3)
        tool_message = self.server.requests[2]["messages"][-1]
        self.assertEqual(json.loads(tool_message["content"])["output"]["content"], "hello")

    def test_denied_write_cannot_be_reported_success(self):
        self.set_responses(call("write_file", {"path": "denied", "content": "bad"}), final("All good"))
        result = self.invoke(*self.run_args(), expected=1)
        self.assertEqual(result["status"], "needs_attention")
        self.assertFalse((self.workspace / "denied").exists())

    def test_exact_process_grant_completes_and_hides_api_keys(self):
        self.set_responses(call("run_process", {"executable": str(PROCESS), "arguments": ["environment"]}), final())
        result = self.invoke(*self.run_args("--allow-process", PROCESS))
        self.assertEqual(result["status"], "completed")
        output = json.loads(self.server.requests[1]["messages"][-1]["content"])["output"]["output"]
        self.assertEqual(output.replace("\r\n", "\n"), "absent\nabsent\n")

    def test_state_database_read_is_denied(self):
        self.set_responses(call("read_file", {"path": ".codestudio/state.sqlite3"}), final())
        result = self.invoke(*self.run_args(), expected=1)
        self.assertEqual(result["status"], "needs_attention")

    def test_turn_budget(self):
        (self.workspace / "x.txt").write_text("x")
        self.set_responses(call("read_file", {"path": "x.txt"}))
        result = self.invoke(*self.run_args("--max-turns", "1"), expected=1)
        self.assertEqual(result["status"], "exhausted")

    def test_secret_in_tool_id_and_final_is_redacted_from_storage(self):
        (self.workspace / "x.txt").write_text("x")
        self.set_responses(call("read_file", {"path": "x.txt"}, TOKEN), final(TOKEN))
        result = self.invoke(*self.run_args())
        detail = self.invoke("show", result["task_id"], "--workspace", self.workspace)
        self.assertNotIn(TOKEN, json.dumps(detail))
        with closing(sqlite3.connect(self.database)) as db:
            dump = "\n".join(db.iterdump())
        self.assertNotIn(TOKEN, dump)

    def test_crash_keeps_uncertain_action_and_recovery_does_not_execute(self):
        self.set_responses(call("run_process", {"executable": str(PROCESS), "arguments": ["sleep", "10000"]}))
        process = subprocess.Popen([str(EXE), *self.run_args("--allow-process", PROCESS, "--process-timeout-ms", "15000")],
                                   env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        found = False
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline and process.poll() is None:
                if self.database.exists():
                    try:
                        with closing(sqlite3.connect(self.database)) as db:
                            found = bool(db.execute("SELECT count(*) FROM actions WHERE status='dispatched'").fetchone()[0])
                    except sqlite3.OperationalError:
                        pass
                if found:
                    break
                time.sleep(0.02)
            self.assertTrue(found, "no durable dispatched intent before effect")
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)
        blocked = self.invoke(*self.run_args(), expected=2)
        self.assertIn("recover", blocked["error"])
        recovered = self.invoke("recover", "--workspace", self.workspace)
        self.assertFalse(recovered["tools_executed"])
        self.assertEqual(recovered["recovered"][0]["unknown_actions"], 1)
        task_id = recovered["recovered"][0]["task_id"]
        detail = self.invoke("show", task_id, "--workspace", self.workspace)
        self.assertEqual(detail["status"], "interrupted")
        self.assertEqual(detail["actions"][0]["effect"], "unknown")

    def test_invalid_cli_arguments_do_not_start_model(self):
        result = self.invoke(*self.run_args("--max-turns", "0"), expected=2)
        self.assertIn("max-turns", result["error"])
        self.assertEqual(self.server.requests, [])

    def test_recovery_guard_checks_beyond_displayed_history(self):
        self.invoke("demo", "--workspace", self.workspace)
        with closing(sqlite3.connect(self.database)) as db:
            db.execute("INSERT INTO tasks(id,prompt,workspace,provider,status) VALUES(?,?,?,?,?)",
                       ("old-running", "interrupted task", str(self.workspace), "fixture", "running"))
            db.executemany("INSERT INTO tasks(id,prompt,workspace,provider,status) VALUES(?,?,?,?,?)",
                           [(f"newer-{i}", "finished task", str(self.workspace), "fixture", "completed")
                            for i in range(101)])
            db.commit()
        result = self.invoke(*self.run_args(), expected=2)
        self.assertIn("recover", result["error"])
        self.assertEqual(self.server.requests, [])
        recovered = self.invoke("recover", "--workspace", self.workspace)
        self.assertEqual(recovered["recovered"][0]["task_id"], "old-running")

    def test_unsupported_schema_refusal_preserves_database_without_migration(self):
        for version in (1, 2, 3, 4, 5, 999):
            with self.subTest(version=version):
                database = self.workspace / f"unsupported-state-{version}.sqlite3"
                with closing(sqlite3.connect(database)) as db:
                    db.execute("CREATE TABLE preserved(value TEXT)")
                    db.execute("INSERT INTO preserved VALUES('must remain unchanged')")
                    db.execute(f"PRAGMA user_version={version}")
                    db.commit()
                    self.assertEqual(db.execute("PRAGMA journal_mode").fetchone()[0], "delete")
                original = database.read_bytes()
                commands = (["history", "--workspace", self.workspace, "--state", database],
                            ["show", "unreadable", "--workspace", self.workspace, "--state", database],
                            self.run_args("--state", database))
                for command in commands:
                    result = self.invoke(*command, expected=2)
                    self.assertIn("Unsupported state schema", result["error"])
                    self.assertEqual(database.read_bytes(), original)
                with closing(sqlite3.connect(database)) as db:
                    self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], version)
                    self.assertEqual(db.execute("PRAGMA journal_mode").fetchone()[0], "delete")
                    self.assertEqual(db.execute("SELECT * FROM preserved").fetchall(), [("must remain unchanged",)])
                self.assertFalse(pathlib.Path(str(database) + "-wal").exists())
                self.assertFalse(pathlib.Path(str(database) + "-shm").exists())
        self.assertEqual(self.server.requests, [])


if __name__ == "__main__":
    unittest.main()
