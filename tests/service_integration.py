"""Exercise the real stdio task service, shared CLI, SQLite and model fixtures.

No local inference engine or cloud account is used. Each test owns its child
processes, temporary workspace and an ephemeral HTTP fixture.
"""
import copy
import json
import os
import pathlib
import queue
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
TOKEN = "service-test-credential-never-emit"


def final(text="Verified service result"):
    return {"choices": [{"finish_reason": "stop", "message": {"role": "assistant", "content": text}}]}


def call(name, arguments, call_id="service-call"):
    return {"choices": [{"finish_reason": "tool_calls", "message": {
        "role": "assistant", "content": None, "tool_calls": [{"id": call_id, "type": "function",
        "function": {"name": name, "arguments": json.dumps(arguments)}}]}}]}


def stream_parts(fragments):
    def frame(delta, finish=None):
        return ("data: " + json.dumps({"choices": [{"index": 0, "delta": delta,
            "finish_reason": finish}]}, ensure_ascii=False) + "\n\n").encode()
    return [frame({"role": "assistant"}), *[frame({"content": part}) for part in fragments],
            frame({}, "stop"), b"data: [DONE]\n\n"]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with self.server.guard:
            self.server.requests.append({"path": self.path, "headers": dict(self.headers), "body": body})
            spec = self.server.responses.pop(0) if self.server.responses else {"status": 500}
        parts = spec.get("parts", [json.dumps(spec.get("body", {"error": "unexpected request"})).encode()])
        try:
            if "before_headers" in spec:
                spec["before_headers"].wait(10)
            self.send_response(spec.get("status", 200))
            self.send_header("Content-Type", spec.get("content_type", "application/json"))
            self.send_header("Content-Length", str(sum(map(len, parts))))
            self.send_header("Connection", "close")
            self.end_headers()
            for index, part in enumerate(parts):
                if index == spec.get("pause_at"):
                    spec["release"].wait(10)
                # Actual byte fragmentation exercises UTF-8 and SSE transport.
                for offset in range(0, len(part), 13):
                    self.wfile.write(part[offset:offset + 13])
                    self.wfile.flush()
        except (ConnectionError, OSError):
            pass


class Client:
    """One protocol client; reader threads keep both child pipes drained."""
    def __init__(self, workspace, env, state=None):
        args = [str(EXE), "serve", "--workspace", str(workspace)]
        if state is not None:
            args += ["--state", str(state)]
        self.process = subprocess.Popen(args, env=env, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.queue = queue.Queue()
        self.pending = []
        self.frames = []
        self.raw = bytearray()
        self.stderr = bytearray()
        self.next_id = 1
        self.stdout_thread = threading.Thread(target=self._read_stdout, daemon=True)
        self.stderr_thread = threading.Thread(target=self._read_stderr, daemon=True)
        self.stdout_thread.start()
        self.stderr_thread.start()

    def _read_stdout(self):
        try:
            for line in self.process.stdout:
                self.raw.extend(line)
                try:
                    if len(line) > 4 * 1024 * 1024 + 2:
                        raise AssertionError("Service emitted an oversized protocol frame")
                    frame = json.loads(line.decode("utf-8"))
                    if not isinstance(frame, dict) or frame.get("jsonrpc") != "2.0":
                        raise AssertionError(f"Non-protocol stdout: {line[:300]!r}")
                    self.frames.append(frame)
                    self.queue.put(frame)
                except Exception as error:
                    self.queue.put(error)
        finally:
            self.queue.put(EOFError("Service stdout closed"))

    def _read_stderr(self):
        while chunk := self.process.stderr.read(4096):
            self.stderr.extend(chunk)

    def send_raw(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def send(self, method, params=None):
        request_id = self.next_id
        self.next_id += 1
        self.send_raw(json.dumps({"jsonrpc": "2.0", "id": request_id, "method": method,
            "params": {} if params is None else params}, ensure_ascii=False) + "\n")
        return request_id

    def receive(self, predicate, timeout=8):
        for index, frame in enumerate(self.pending):
            if predicate(frame):
                return self.pending.pop(index)
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"Missing service response; pending={self.pending!r}; stderr={bytes(self.stderr)!r}")
            try:
                frame = self.queue.get(timeout=remaining)
            except queue.Empty as error:
                raise AssertionError(f"Service response timed out; stderr={bytes(self.stderr)!r}") from error
            if isinstance(frame, Exception):
                raise AssertionError(f"{frame}; stdout={bytes(self.raw)!r}; stderr={bytes(self.stderr)!r}")
            if predicate(frame):
                return frame
            self.pending.append(frame)

    def rpc(self, method, params=None, error=False):
        request_id = self.send(method, params)
        reply = self.receive(lambda frame: frame.get("id") == request_id)
        if error:
            if "error" not in reply:
                raise AssertionError(f"Expected RPC error: {reply!r}")
            return reply["error"]
        if "error" in reply:
            raise AssertionError(f"Unexpected RPC error: {reply!r}")
        return reply["result"]

    def initialize(self):
        return self.rpc("initialize", {"protocol_version": 2})

    def start(self, options=None, mode="run"):
        result = self.rpc("tasks/start", {"mode": mode, "options": options or {}})
        if result.get("status") != "accepted" or not result.get("operation_id"):
            raise AssertionError(f"Invalid start receipt: {result!r}")
        return result["operation_id"]

    def finished(self, operation):
        return self.receive(lambda frame: frame.get("method") == "operation/finished" and
            frame.get("params", {}).get("operation_id") == operation)["params"]

    def wait_exit(self, timeout=8):
        code = self.process.wait(timeout=timeout)
        self.stdout_thread.join(timeout=2)
        self.stderr_thread.join(timeout=2)
        return code

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
                    pass  # The child may reject an oversized input before EOF.


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="codestudio-service-")
        self.workspace = pathlib.Path(self.temp.name) / "workspace with spaces"
        self.workspace.mkdir()
        self.database = self.workspace / ".codestudio" / "state.sqlite3"
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("CODESTUDIO_")}
        if sys.platform.startswith("linux"):
            # RPC autostart fixtures must never use the real user's unit files.
            self.env["XDG_CONFIG_HOME"] = str(pathlib.Path(self.temp.name) / "private configuration")
        if sys.platform.startswith("linux") and "CODESTUDIO_CGROUP_ROOT" in os.environ:
            self.env["CODESTUDIO_CGROUP_ROOT"] = os.environ["CODESTUDIO_CGROUP_ROOT"]
        self.env["CODESTUDIO_API_KEY"] = TOKEN
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.server.guard = threading.Lock()
        self.server.responses = []
        self.server.requests = []
        self.http_thread = threading.Thread(target=self.server.serve_forever,
            kwargs={"poll_interval": 0.02}, daemon=True)
        self.http_thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}/v1"
        self.clients = []
        self.raw_children = []
        self.releases = []

    def tearDown(self):
        for release in self.releases:
            release.set()
        for client in self.clients:
            client.close()
            self.assertNotIn(TOKEN.encode(), bytes(client.raw) + bytes(client.stderr))
        for process in self.raw_children:
            if process.poll() is None:
                process.kill()
            out, err = process.communicate(timeout=5)
            self.assertNotIn(TOKEN.encode(), (out or b"") + (err or b""))
        self.server.shutdown()
        self.server.server_close()
        self.http_thread.join(timeout=5)
        self.temp.cleanup()

    def client(self, initialize=True, state=None):
        client = Client(self.workspace, self.env, state)
        self.clients.append(client)
        if initialize:
            client.initialize()
        return client

    def raw_client(self):
        """A client without background readers, for output-loss/backpressure."""
        process = subprocess.Popen([str(EXE), "serve", "--workspace", str(self.workspace)],
            env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.raw_children.append(process)
        self.raw_send(process, 1, "initialize", {"protocol_version": 2})
        initialized = self.raw_receive(process)
        self.assertEqual(initialized["result"]["protocol_version"], 2)
        return process

    @staticmethod
    def raw_send(process, request_id, method, params=None):
        body = json.dumps({"jsonrpc": "2.0", "id": request_id, "method": method,
            "params": {} if params is None else params}) + "\n"
        process.stdin.write(body.encode())
        process.stdin.flush()

    def raw_receive(self, process):
        # Read exactly one line. Join before closing stdout so no Python stream
        # lock is held while the output-loss test closes the parent pipe handle.
        received = queue.Queue()
        reader = threading.Thread(target=lambda: received.put(process.stdout.readline()), daemon=True)
        reader.start()
        try:
            line = received.get(timeout=5)
        except queue.Empty:
            process.kill()
            reader.join(timeout=2)
            self.fail("Raw service client did not receive a response")
        reader.join(timeout=2)
        self.assertFalse(reader.is_alive())
        self.assertNotIn(TOKEN.encode(), line)
        frame = json.loads(line)
        self.assertEqual(frame["jsonrpc"], "2.0")
        return frame

    def responses(self, *responses):
        with self.server.guard:
            self.server.responses = list(responses)

    def requests(self):
        with self.server.guard:
            return copy.deepcopy(self.server.requests)

    def options(self, **overrides):
        return {"prompt": "Exercise the task service", "base_url": self.url, "model": "service-fixture",
            "api": "chat-completions", "allow_http_loopback": True, "stream": False,
            "timeout_ms": 10000, **overrides}

    def gate(self):
        release = threading.Event()
        self.releases.append(release)
        return release

    def wait_for(self, condition, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                result = condition()
                if result:
                    return result
            except sqlite3.OperationalError:
                pass
            time.sleep(0.02)
        self.fail("Expected live task boundary was not reached")

    def sql(self, statement, parameters=()):
        if not self.database.exists():
            return []
        with closing(sqlite3.connect(self.database)) as database:
            return database.execute(statement, parameters).fetchall()

    def cli(self, *args, expected=0):
        result = subprocess.run([str(EXE), *map(str, args)], env=self.env,
            capture_output=True, encoding="utf-8", timeout=12)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertNotIn(TOKEN, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def demo(self, client):
        done = client.finished(client.start(mode="demo"))
        self.assertEqual(done["exit_code"], 0, done)
        self.assertEqual(done["result"]["status"], "completed")
        return done["result"]["task_id"]

    def test_initialize_and_read_only_queries_do_not_create_state(self):
        client = self.client(initialize=False)
        client.rpc("tasks/list", error=True)
        client.rpc("initialize", {"protocol_version": 1}, error=True)
        client.rpc("initialize", {"protocol_version": 999}, error=True)
        initialized = client.initialize()
        self.assertEqual(initialized["protocol_version"], 2)
        self.assertEqual(pathlib.Path(initialized["workspace"]), self.workspace.resolve())
        self.assertEqual(pathlib.Path(initialized["state_database"]), self.database.resolve())
        self.assertIsInstance(initialized["capabilities"], dict)
        self.assertTrue(initialized["capabilities"]["approval_continuation"])
        self.assertTrue(initialized["capabilities"]["automatic_resume"])
        for capability in ("background_scheduler", "mcp_oauth"):
            self.assertEqual(initialized["capabilities"][capability],
                             os.name == "nt" or sys.platform.startswith("linux") or sys.platform == "darwin")
        self.assertEqual(initialized["capabilities"]["supervisor_diagnostics"],
                         os.name == "nt" or sys.platform.startswith("linux") or sys.platform == "darwin")
        self.assertEqual(client.rpc("tasks/list")["items"], [])
        self.assertEqual(client.rpc("events/list")["items"], [])
        self.assertEqual(client.rpc("schedules/list")["items"], [])
        scheduler = client.rpc("scheduler/status")
        self.assertIsNone(scheduler["supervisor"]["record"])
        diagnostics = client.rpc("scheduler/supervisor/status")
        self.assertIsNone(diagnostics["record"])
        self.assertEqual(diagnostics["observation"]["state"], "not_checked")
        self.assertGreater(diagnostics["observation"]["observed_at_ms"], 0)
        self.assertFalse(diagnostics["worker_matches"])
        self.assertEqual(client.rpc("scheduler/supervisor/events"),
                         {"items": [], "next_after": 0, "has_more": False})
        self.assertEqual(client.rpc("scheduler/supervisor/events", {"after": 9223372036854775807}),
                         {"items": [], "next_after": 9223372036854775807, "has_more": False})
        client.rpc("schedules/get", {"job_id": "missing"}, error=True)
        client.rpc("tasks/get", {"task_id": "missing"}, error=True)
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_approval_continuation_uses_resume_option_validation_before_dispatch(self):
        client = self.client()
        params = {"task_id": "missing-task", "action_id": "missing-action", "digest": "a" * 64}
        invalid_options = (
            None, [], "resume", True,
            {"workspace": str(self.workspace)}, {"state": str(self.database)},
            {"api_key": "not-a-credential"}, {"prompt": "replace the original prompt"},
            {"unknown": True}, {"stream": True}, {"stream": "false"},
            {"allow_write": 1}, {"allow_http_loopback": "true"}, {"allow_build_proposals": None},
            {"sandbox": "invalid"}, {"api": "invalid"}, {"model": ""},
            {"max_turns": 0}, {"max_turns": 101}, {"max_turns": 1.5},
            {"timeout_ms": 99}, {"process_timeout_ms": 300001}, {"max_output_tokens": 0},
            {"allow_process": "tool.exe"}, {"allow_mcp": [None]}, {"allow_mcp": ["echo"] * 129},
        )
        for options in invalid_options:
            for method in ("tasks/resume", "schedules/create", "builds/approve", "builds/reject"):
                with self.subTest(method=method, options=options):
                    request = ({"task_id": params["task_id"], "options": options}
                               if method in ("tasks/resume", "schedules/create") else {**params, "continue_options": options})
                    rejection = client.rpc(method, request, error=True)
                    self.assertEqual(rejection["code"], -32602, rejection)
        rejection = client.rpc("builds/review", {
            "task_id": params["task_id"], "action_id": params["action_id"], "continue_options": {}}, error=True)
        self.assertEqual(rejection["code"], -32602, rejection)
        self.assertFalse(client.rpc("operations/status")["active"])
        self.assertEqual(self.requests(), [])
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_unsupported_state_schema_is_rejected_without_migration(self):
        for version in (1, 2, 3, 4, 5, 6, 999):
            with self.subTest(version=version):
                database = self.workspace / f"unsupported-state-{version}.sqlite3"
                with closing(sqlite3.connect(database)) as db:
                    db.execute("CREATE TABLE preserved(value TEXT)")
                    db.execute("INSERT INTO preserved VALUES('must remain unchanged')")
                    db.execute(f"PRAGMA user_version={version}")
                    db.commit()
                original = database.read_bytes()
                request = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                      "params": {"protocol_version": 2}}) + "\n"
                process = subprocess.run([str(EXE), "serve", "--workspace", str(self.workspace),
                                          "--state", str(database)], input=request, env=self.env,
                                         capture_output=True, encoding="utf-8", timeout=8)
                self.assertEqual(process.returncode, 2, process.stdout + process.stderr)
                self.assertIn("Unsupported state schema", process.stderr)
                self.assertEqual(process.stdout, "")
                self.assertEqual(database.read_bytes(), original)
                with closing(sqlite3.connect(database)) as db:
                    self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], version)
                    self.assertEqual(db.execute("PRAGMA journal_mode").fetchone()[0], "delete")
                    self.assertEqual(db.execute("SELECT * FROM preserved").fetchall(), [("must remain unchanged",)])
                self.assertFalse(pathlib.Path(str(database) + "-wal").exists())
                self.assertFalse(pathlib.Path(str(database) + "-shm").exists())
        self.assertEqual(self.requests(), [])

    def test_cron_preview_and_queries_are_read_only(self):
        client = self.client(initialize=False)
        capabilities = client.initialize()["capabilities"]
        self.assertTrue(capabilities["cron"])
        self.assertEqual(capabilities["login_autostart"], os.name == "nt" or sys.platform.startswith("linux") or sys.platform == "darwin")
        self.assertEqual(capabilities["supervised_autostart"], os.name == "nt" or sys.platform.startswith("linux") or sys.platform == "darwin")
        self.assertEqual(client.rpc("crons/list")["items"], [])
        client.rpc("crons/get", {"cron_id": "missing"}, error=True)
        preview = client.rpc("crons/preview", {"expression": "*/15 * * * *",
            "timezone": "UTC", "after": 0, "count": 3})
        self.assertEqual([item["at_ms"] for item in preview["occurrences"]], [900000, 1800000, 2700000])
        self.assertEqual((preview["misfire"], preview["overlap"]), ("coalesce", "skip"))
        if os.name == "nt":
            status = client.rpc("scheduler/autostart/status")
            self.assertFalse(status["registered"])
            self.assertFalse(status["started"])
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / ".codestudio").exists())
        self.assertEqual(self.requests(), [])

    def test_cron_async_creation_survives_eof_and_controls_work_during_foreground_task(self):
        client = self.client()
        accepted = client.rpc("crons/create", {"name": "Daily check", "expression": "0 9 * * 1-5",
            "timezone": "UTC", "max_runs": 2, "options": self.options(max_turns=3)})
        self.assertEqual(accepted["status"], "accepted")
        done = client.finished(accepted["operation_id"])
        self.assertEqual(done["exit_code"], 0, done)
        cron = done["result"]
        self.assertEqual((cron["status"], cron["run_count"], cron["max_runs"]), ("active", 0, 2))
        self.assertEqual(client.rpc("crons/get", {"cron_id": cron["id"]}), cron)
        self.assertEqual(client.rpc("crons/list")["items"], [cron])
        self.assertEqual(client.rpc("tasks/list")["items"], [])
        self.assertEqual(client.rpc("schedules/list")["items"], [])
        self.assertEqual(self.requests(), [])
        client.process.stdin.close()
        self.assertEqual(client.wait_exit(), 0)
        client = self.client()
        release = self.gate()
        self.responses({"body": final(), "before_headers": release})
        operation = client.start(self.options())
        self.wait_for(lambda: len(self.requests()) == 1)
        self.assertEqual(client.rpc("crons/pause", {"cron_id": cron["id"]})["status"], "paused")
        resumed = client.rpc("crons/resume", {"cron_id": cron["id"]})
        self.assertEqual(resumed["status"], "active")
        self.assertGreater(resumed["next_due_at_ms"], int(time.time() * 1000))
        self.assertEqual(client.rpc("crons/cancel", {"cron_id": cron["id"]})["status"], "cancelled")
        client.rpc("crons/resume", {"cron_id": cron["id"]}, error=True)
        client.rpc("operations/cancel", {"operation_id": operation})
        self.assertEqual(client.finished(operation)["exit_code"], 130)

    def test_eof_cancels_cron_mcp_discovery_before_enrollment(self):
        fixture = EXE.parent / ("mcp-fixture.exe" if os.name == "nt" else "mcp-fixture")
        marker = self.workspace / "enrollment.started"
        manifest = self.workspace / "enrollment-mcp.json"
        manifest.write_text(json.dumps({"version": 2, "servers": [{"id": "echo",
            "command": str(fixture), "args": ["hang-initialize", str(marker)],
            "tools": ["echo"], "timeout_ms": 30000}]}), encoding="utf-8")
        client = self.client()
        accepted = client.rpc("crons/create", {"expression": "* * * * *", "timezone": "UTC",
            "options": self.options(mcp_config=str(manifest), allow_mcp=["echo"])})
        self.assertEqual(accepted["status"], "accepted")
        self.wait_for(marker.exists)
        self.assertTrue(client.rpc("operations/status")["active"])
        self.assertEqual(client.rpc("crons/list")["items"], [])
        client.process.stdin.close()
        self.assertEqual(client.wait_exit(timeout=5), 0)
        self.assertEqual(self.sql("SELECT count(*) FROM cron_schedules"), [(0,)])
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(0,)])
        self.assertEqual(self.requests(), [])
        self.assertEqual(self.cli("demo", "--workspace", self.workspace)["status"], "completed")

    def test_cron_and_autostart_validation_has_no_effects(self):
        client = self.client()
        create = {"expression": "* * * * *", "timezone": "UTC", "options": self.options()}
        for key, value in (("expression", None), ("max_runs", -1), ("max_runs", True),
                           ("max_runs", 1000001), ("max_attempts", 0), ("deadline_ms", 0),
                           ("retry_delay_ms", 99), ("name", ""), ("workspace", str(self.workspace))):
            with self.subTest(key=key, value=value):
                self.assertEqual(client.rpc("crons/create", {**create, key: value}, error=True)["code"], -32602)
        for method, params in (("crons/list", {"limit": 0}), ("crons/get", {"cron_id": ""}),
                ("crons/pause", {"cron_id": "missing", "force": True}),
                ("crons/preview", {"expression": "* * * * *", "count": 0}),
                ("scheduler/autostart/enable", {"poll_ms": 19}),
                ("scheduler/autostart/enable", {"poll_ms": 5001}),
                ("scheduler/autostart/enable", {"poll_ms": True}),
                ("scheduler/autostart/enable", {"password": "not-accepted"}),
                ("scheduler/autostart/disable", {"force": True}),
                ("scheduler/autostart/status", {"poll_ms": 100})):
            with self.subTest(method=method, params=params):
                self.assertEqual(client.rpc(method, params, error=True)["code"], -32602)
        for expression in ("invalid", "0 0 31 2 *"):
            accepted = client.rpc("crons/create", {**create, "expression": expression})
            self.assertEqual(client.finished(accepted["operation_id"])["exit_code"], 2)
        self.assertEqual(self.requests(), [])
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_supervised_autostart_invalid_parameters_have_no_effects(self):
        client = self.client()
        before = None
        if os.name == "nt" or sys.platform.startswith("linux") or sys.platform == "darwin":
            before = client.rpc("scheduler/autostart/status")
            self.assertFalse(before["registered"])
        invalid = [
            {"supervise": 1}, {"supervise": "true"}, {"supervise": None},
            {"max_restarts": 3}, {"restart_delay_ms": 1000},
            {"supervise": False, "max_restarts": 3},
            {"supervise": False, "restart_delay_ms": 1000},
            {"supervise": True, "max_restarts": -1},
            {"supervise": True, "max_restarts": 101},
            {"supervise": True, "max_restarts": True},
            {"supervise": True, "max_restarts": 3.0},
            {"supervise": True, "max_restarts": "3"},
            {"supervise": True, "max_restarts": None},
            {"supervise": True, "restart_delay_ms": 99},
            {"supervise": True, "restart_delay_ms": 60001},
            {"supervise": True, "restart_delay_ms": False},
            {"supervise": True, "restart_delay_ms": 1000.0},
            {"supervise": True, "restart_delay_ms": "1000"},
            {"supervise": True, "restart_delay_ms": None},
            {"supervise": True, "poll_ms": 19},
            {"supervise": True, "poll_ms": 5001},
            {"supervise": True, "poll_ms": 100.0},
            {"supervise": True, "once": True},
            {"supervise": True, "workspace": str(self.workspace)},
        ]
        try:
            for params in invalid:
                with self.subTest(method="enable", params=params):
                    error = client.rpc("scheduler/autostart/enable", params, error=True)
                    self.assertEqual(error["code"], -32602)
                    self.assertEqual(error["data"]["outcome"], "not_started")
            for action in ("status", "disable"):
                for params in ({"supervise": True}, {"supervise": False}, {"max_restarts": 3},
                               {"restart_delay_ms": 1000}, {"poll_ms": 100}):
                    with self.subTest(method=action, params=params):
                        error = client.rpc("scheduler/autostart/" + action, params, error=True)
                        self.assertEqual(error["code"], -32602)
                        self.assertEqual(error["data"]["outcome"], "not_started")
            if before is not None:
                self.assertEqual(client.rpc("scheduler/autostart/status"), before)
        finally:
            # Also clean up if a regression accidentally accepts invalid input.
            if before is not None and before["available"]:
                cleanup = self.client()
                self.assertFalse(cleanup.rpc("scheduler/autostart/disable")["registered"])
        self.assertFalse(client.rpc("operations/status")["active"])
        self.assertEqual(self.requests(), [])
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / ".codestudio").exists())
        if sys.platform.startswith("linux"):
            self.assertFalse(pathlib.Path(self.env["XDG_CONFIG_HOME"]).exists())

    def test_scheduler_parameters_are_rejected_without_creating_state(self):
        client = self.client()
        invalid = [
            ("schedules/create", {"task_id": "missing", "delay_ms": -1}),
            ("schedules/create", {"task_id": "missing", "delay_ms": 604800001}),
            ("schedules/create", {"task_id": "missing", "deadline_ms": 0}),
            ("schedules/create", {"task_id": "missing", "deadline_ms": 604800001}),
            ("schedules/create", {"task_id": "missing", "max_attempts": 0}),
            ("schedules/create", {"task_id": "missing", "max_attempts": 101}),
            ("schedules/create", {"task_id": "missing", "retry_delay_ms": 99}),
            ("schedules/create", {"task_id": "missing", "retry_delay_ms": 60001}),
            ("schedules/create", {"task_id": "missing", "delay_ms": True}),
            ("schedules/create", {"task_id": "missing", "deadline_ms": 1.5}),
            ("schedules/create", {"task_id": "missing", "prompt": "replace"}),
            ("schedules/create", {"task_id": "missing", "api_key": "not-a-credential"}),
            ("schedules/create", {"task_id": "missing", "workspace": str(self.workspace)}),
            ("schedules/create", {"task_id": "missing", "state": str(self.database)}),
            ("schedules/list", {"before": -1}), ("schedules/list", {"limit": 0}),
            ("schedules/list", {"limit": 101}), ("schedules/list", {"after": 0}),
            ("schedules/get", {"job_id": ""}), ("schedules/get", {"job_id": None}),
            ("schedules/cancel", {"job_id": "missing", "force": True}),
            ("scheduler/start", {"poll_ms": 19}), ("scheduler/start", {"poll_ms": 5001}),
            ("scheduler/start", {"poll_ms": "20"}), ("scheduler/start", {"poll_ms": True}),
            ("scheduler/start", {"owner": "client-owner"}),
            ("scheduler/start", {"supervise": 1}),
            ("scheduler/start", {"max_restarts": 1}),
            ("scheduler/start", {"supervise": False, "restart_delay_ms": 100}),
            ("scheduler/start", {"supervise": True, "max_restarts": -1}),
            ("scheduler/start", {"supervise": True, "max_restarts": 101}),
            ("scheduler/start", {"supervise": True, "max_restarts": True}),
            ("scheduler/start", {"supervise": True, "restart_delay_ms": 99}),
            ("scheduler/start", {"supervise": True, "restart_delay_ms": 60001}),
            ("scheduler/start", {"supervise": True, "restart_delay_ms": 100.0}),
            ("scheduler/status", {"workspace": str(self.workspace)}),
            ("scheduler/stop", {"force": True}),
        ]
        for method, params in invalid:
            with self.subTest(method=method, params=params):
                rejection = client.rpc(method, params, error=True)
                self.assertEqual(rejection["code"], -32602, rejection)
        self.assertFalse(client.rpc("operations/status")["active"])
        self.assertEqual(self.requests(), [])
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_supervisor_diagnostic_parameters_are_rejected_without_state_or_worker_effects(self):
        client = self.client()
        cases = [("scheduler/supervisor/status", params) for params in (
            {"workspace": str(self.workspace)}, {"state": str(self.database)}, {"poll_ms": 100},
            {"supervise": True}, {"after": 0}, {"limit": 1})]
        cases += [("scheduler/supervisor/events", params) for params in (
            {"after": -1}, {"after": 9223372036854775808}, {"after": True}, {"after": 1.0},
            {"after": "0"}, {"limit": 0}, {"limit": 101}, {"limit": False}, {"limit": 1.0},
            {"limit": "1"}, {"run_id": "injected"}, {"workspace": str(self.workspace)},
            {"state": str(self.database)}, {"supervise": True})]
        for method, params in cases:
            with self.subTest(method=method, params=params):
                error = client.rpc(method, params, error=True)
                self.assertEqual(error["code"], -32602, error)
                self.assertFalse(self.database.exists())
                self.assertFalse((self.workspace / ".codestudio").exists())
        self.assertFalse(client.rpc("operations/status")["active"])
        self.assertEqual(self.requests(), [])

    def test_schedule_survives_client_eof_and_can_be_cancelled_during_foreground_work(self):
        release = self.gate()
        self.responses({"body": final(), "before_headers": release})
        client = self.client()
        operation = client.start(self.options())
        self.wait_for(lambda: len(self.requests()) == 1)
        client.rpc("operations/cancel", {"operation_id": operation})
        cancelled = client.finished(operation)
        self.assertEqual(cancelled["exit_code"], 130, cancelled)
        task_id = cancelled["result"]["task_id"]
        release.set()
        created = client.rpc("schedules/create", {"task_id": task_id,
            "options": {"allow_http_loopback": True}, "delay_ms": 60000,
            "deadline_ms": 120000, "max_attempts": 2, "retry_delay_ms": 100})
        job_id = created["id"]
        self.assertEqual(created["task_id"], task_id)
        self.assertEqual(created["status"], "queued")
        self.assertEqual(len(self.requests()), 1, "Queue enrollment must not call the provider")
        self.assertEqual(client.rpc("schedules/get", {"job_id": job_id})["status"], "queued")
        client.process.stdin.close()
        self.assertEqual(client.wait_exit(), 0)

        replacement = self.client()
        self.assertEqual(replacement.rpc("schedules/get", {"job_id": job_id})["status"], "queued")
        self.assertIn(job_id, [job["id"] for job in replacement.rpc("schedules/list")["items"]])
        pending = self.gate()
        self.responses({"body": final(), "before_headers": pending})
        active = replacement.start(self.options())
        self.wait_for(lambda: len(self.requests()) == 2)
        self.assertTrue(replacement.rpc("operations/status")["active"])
        self.assertIsInstance(replacement.rpc("scheduler/status"), dict)
        replacement.rpc("schedules/cancel", {"job_id": job_id})
        self.assertEqual(replacement.rpc("schedules/get", {"job_id": job_id})["status"], "cancelled")
        self.assertTrue(replacement.rpc("operations/status")["active"], "Queue cancellation must not cancel foreground work")
        replacement.rpc("operations/cancel", {"operation_id": active})
        self.assertEqual(replacement.finished(active)["exit_code"], 130)
        self.assertEqual(len(self.requests()), 2)

    def test_demo_and_cli_share_receipts_and_summarized_task_detail(self):
        client = self.client()
        task_id = self.demo(client)
        self.assertEqual((self.workspace / "CODESTUDIO_DEMO.txt").read_bytes(), b"Hello from CodeStudio-Agent!\n")
        detail = client.rpc("tasks/get", {"task_id": task_id})
        self.assertEqual((detail["status"], detail["action_count"]), ("completed", 2))
        self.assertNotIn("messages", detail["checkpoint"])
        self.assertNotIn("state", detail["checkpoint"])
        self.assertNotIn("binding", detail["checkpoint"])
        self.assertEqual(self.cli("history", "--workspace", self.workspace)[0]["id"], task_id)
        cli_detail = self.cli("show", task_id, "--workspace", self.workspace)
        self.assertEqual(len(cli_detail["actions"]), 2)
        client.rpc("tasks/resume", {"task_id": task_id, "options": {}}, error=False)
        rejected = client.receive(lambda frame: frame.get("method") == "operation/finished")["params"]
        self.assertEqual(rejected["exit_code"], 2)
        self.assertIn("error", rejected)
        self.assertEqual(self.requests(), [])

    def test_task_and_event_cursor_pages_do_not_skip_or_repeat_rows(self):
        client = self.client()
        oldest = self.demo(client)
        newest = self.demo(client)
        first = client.rpc("tasks/list", {"limit": 1})
        self.assertEqual([row["id"] for row in first["items"]], [newest])
        self.assertTrue(first["has_more"])
        second = client.rpc("tasks/list", {"before": first["next_before"], "limit": 1})
        self.assertEqual([row["id"] for row in second["items"]], [oldest])
        self.assertFalse(second["has_more"])
        after, rows = 0, []
        for _ in range(100):
            page = client.rpc("events/list", {"after": after, "limit": 2})
            self.assertLessEqual(len(page["items"]), 2)
            rows.extend(page["items"])
            after = page["next_after"]
            if not page["has_more"]:
                break
        else:
            self.fail("Event pagination did not terminate")
        expected = self.sql("SELECT seq,task_id,kind FROM events ORDER BY seq")
        self.assertEqual([(row["seq"], row["task_id"], row["kind"]) for row in rows], expected)
        self.assertEqual(client.rpc("events/list", {"after": after})["items"], [])
        self.assertEqual(len({row["seq"] for row in rows}), len(rows))

    def test_subscription_delivers_committed_events_and_replays_after_cursor(self):
        client = self.client()
        self.assertEqual(client.rpc("events/subscribe", {"after": 0})["after"], 0)
        task_id = self.demo(client)
        received = []
        while not any(row["kind"] == "task.finished" and row["task_id"] == task_id for row in received):
            received.extend(client.receive(lambda frame: frame.get("method") == "events/batch")["params"]["items"])
        seqs = [row["seq"] for row in received]
        self.assertEqual(seqs, sorted(set(seqs)))
        self.assertEqual(seqs, [row[0] for row in self.sql("SELECT seq FROM events ORDER BY seq")])
        cursor = seqs[-1]
        client.rpc("events/unsubscribe")
        next_task = self.demo(client)
        replay = self.client()
        replay.rpc("events/subscribe", {"after": cursor})
        replayed = []
        while not any(row["kind"] == "task.finished" and row["task_id"] == next_task for row in replayed):
            replayed.extend(replay.receive(lambda frame: frame.get("method") == "events/batch")["params"]["items"])
        self.assertTrue(all(row["seq"] > cursor for row in replayed))
        self.assertEqual([row["seq"] for row in replayed],
            [row[0] for row in self.sql("SELECT seq FROM events WHERE seq>? ORDER BY seq", (cursor,))])


    def test_streaming_text_arrives_before_terminal_and_redacts_split_credentials(self):
        release = self.gate()
        fragments = ["Result: ", TOKEN[:7], TOKEN[7:19], TOKEN[19:], " verified 世界\n"]
        parts = stream_parts(fragments)
        self.responses({"parts": parts, "content_type": "text/event-stream",
            "pause_at": len(parts) - 2, "release": release})
        client = self.client()
        operation = client.start(self.options(stream=True))
        text = ""
        while "verified 世界\n" not in text:
            notification = client.receive(lambda frame: frame.get("method") == "operation/text" and
                frame["params"]["operation_id"] == operation, timeout=3)["params"]
            self.assertTrue(notification["provisional"])
            text += notification["text"]
        self.assertEqual(text, "Result: [REDACTED] verified 世界\n")
        self.assertFalse(any(frame.get("method") == "operation/finished" for frame in client.frames))
        self.assertTrue(client.rpc("operations/status")["active"])
        release.set()
        done = client.finished(operation)
        self.assertEqual(done["exit_code"], 0, done)
        self.assertNotIn(TOKEN, json.dumps(done))
        with closing(sqlite3.connect(self.database)) as database:
            self.assertNotIn(TOKEN, "\n".join(database.iterdump()))

    def test_active_task_remains_queryable_and_cancellation_does_not_authorize_other_work(self):
        release = self.gate()
        self.responses({"body": final(), "before_headers": release})
        client = self.client()
        operation = client.start(self.options())
        self.wait_for(lambda: len(self.requests()) == 1)
        status = client.rpc("operations/status")
        self.assertTrue(status["active"])
        self.assertEqual(status["operation_id"], operation)
        self.assertEqual(client.rpc("tasks/list")["items"][0]["status"], "running")
        client.rpc("tasks/start", {"mode": "demo", "options": {}}, error=True)
        client.rpc("tasks/recover", error=True)
        client.rpc("operations/cancel", {"operation_id": "wrong-operation"}, error=True)
        self.assertFalse((self.workspace / "CODESTUDIO_DEMO.txt").exists())
        cancelled = client.rpc("operations/cancel", {"operation_id": operation})
        self.assertTrue(cancelled["cancel_requested"])
        done = client.finished(operation)
        self.assertEqual((done["exit_code"], done["result"]["status"]), (130, "cancelled"))
        self.assertFalse(client.rpc("operations/status")["active"])
        self.demo(client)

    def test_resume_requires_original_grants_and_reuses_completed_tool_receipt(self):
        self.responses({"body": call("write_file", {"path": "once.txt", "content": "first write"})})
        client = self.client()
        first = client.finished(client.start(self.options(allow_write=True, max_turns=1)))
        self.assertEqual(first["result"]["status"], "exhausted")
        task_id = first["result"]["task_id"]
        target = self.workspace / "once.txt"
        target.write_text("changed after receipt", encoding="utf-8")
        rejected = client.rpc("tasks/resume", {"task_id": task_id,
            "options": {"max_turns": 2, "allow_http_loopback": True}})
        failure = client.finished(rejected["operation_id"])
        self.assertEqual(failure["exit_code"], 2)
        self.assertIn("error", failure)
        self.assertEqual(len(self.requests()), 1)
        self.responses({"body": final()})
        resumed = client.rpc("tasks/resume", {"task_id": task_id,
            "options": {"max_turns": 2, "allow_http_loopback": True, "allow_write": True}})
        done = client.finished(resumed["operation_id"])
        self.assertEqual((done["exit_code"], done["result"]["task_id"], done["result"]["turns"]), (0, task_id, 2))
        self.assertEqual(target.read_text(), "changed after receipt")
        self.assertEqual(client.rpc("tasks/get", {"task_id": task_id})["action_count"], 1)
        self.assertEqual(len(self.requests()), 2)

    def test_crashed_process_requires_recover_and_unknown_effects_never_resume(self):
        options = self.options(allow_process=[str(PROCESS)], process_timeout_ms=15000)
        self.responses({"body": call("run_process", {"executable": str(PROCESS), "arguments": ["sleep", "10000"]})})
        client = self.client()
        client.start(options)
        self.wait_for(lambda: self.sql("SELECT id FROM actions WHERE status='dispatched'"))
        client.process.kill()
        client.wait_exit()
        task_id = self.sql("SELECT id FROM tasks")[0][0]
        recovery = self.client()
        blocked = recovery.rpc("tasks/start", {"mode": "demo", "options": {}})
        self.assertEqual(recovery.finished(blocked["operation_id"])["exit_code"], 2)
        self.assertFalse((self.workspace / "CODESTUDIO_DEMO.txt").exists())
        report = recovery.rpc("tasks/recover")
        self.assertFalse(report["tools_executed"])
        self.assertEqual(report["recovered"][0]["unknown_actions"], 1)
        self.assertEqual(self.sql("SELECT status,effect FROM actions"), [("unknown", "unknown")])
        resumed = recovery.rpc("tasks/resume", {"task_id": task_id, "options": {
            "allow_process": [str(PROCESS)], "process_timeout_ms": 15000, "allow_http_loopback": True}})
        self.assertEqual(recovery.finished(resumed["operation_id"])["exit_code"], 2)
        self.assertEqual(len(self.requests()), 1)

    def test_malformed_requests_and_duplicate_keys_have_no_effects(self):
        client = self.client()
        invalid = [
            "{broken\n",
            '[{"jsonrpc":"2.0","id":1,"method":"tasks/start","params":{"mode":"demo"}}]\n',
            '{"jsonrpc":"2.0","id":91,"method":"tasks/start","method":"tasks/list","params":{"mode":"demo"}}\n',
            '{"jsonrpc":"2.0","id":true,"method":"tasks/start","params":{"mode":"demo"}}\n',
            '{"jsonrpc":"2.0","id":92,"method":"tasks/start","params":[]}\n',
            '{"jsonrpc":"1.0","id":93,"method":"tasks/start","params":{"mode":"demo"}}\n',
        ]
        for raw in invalid:
            with self.subTest(raw=raw):
                client.send_raw(raw)
                frame = client.receive(lambda item: "error" in item)
                self.assertIsInstance(frame["error"]["code"], int)
        self.assertEqual(client.rpc("unknown/method", error=True)["code"], -32601)
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / "CODESTUDIO_DEMO.txt").exists())

    def test_option_validation_pins_workspace_state_and_credentials(self):
        client = self.client()
        outside = pathlib.Path(self.temp.name) / "outside"
        for options in [
            {"workspace": str(outside)}, {"state": str(outside / "state.sqlite3")},
            {"api_key": TOKEN}, {"unknown": True}, {"allow_write": "yes"},
            {"max_turns": True}, {"allow_process": str(PROCESS)},
            {"allow_mcp": [123]}, {"stream": "false"}, {"sandbox": "disabled"},
        ]:
            with self.subTest(options=options):
                client.rpc("tasks/start", {"mode": "run", "options": {**self.options(), **options}}, error=True)
        for params in [{"limit": 0}, {"limit": 101}, {"before": -1}, {"limit": True}]:
            client.rpc("tasks/list", params, error=True)
        client.rpc("events/list", {"after": -1}, error=True)
        client.rpc("tasks/start", {"mode": "demo", "options": {"allow_write": True}}, error=True)
        client.rpc("tasks/resume", {"task_id": "missing", "options": {"stream": True}}, error=True)
        self.assertFalse(self.database.exists())
        self.assertFalse(outside.exists())
        self.assertEqual(self.requests(), [])

    def test_explicit_state_path_is_shared_with_the_cli(self):
        database = pathlib.Path(self.temp.name) / "custom state" / "tasks.sqlite3"
        client = self.client(initialize=False, state=database)
        self.assertEqual(pathlib.Path(client.initialize()["state_database"]), database.resolve())
        self.assertFalse(database.exists())
        task_id = self.demo(client)
        self.assertTrue(database.is_file())
        self.assertFalse(self.database.exists())
        self.assertEqual(self.cli("history", "--workspace", self.workspace, "--state", database)[0]["id"], task_id)

    def test_external_cli_obeys_the_same_active_workspace_lock(self):
        release = self.gate()
        self.responses({"body": final(), "before_headers": release})
        client = self.client()
        operation = client.start(self.options())
        self.wait_for(lambda: len(self.requests()) == 1)
        conflict = self.cli("demo", "--workspace", self.workspace, expected=2)
        self.assertIn("error", conflict)
        self.assertFalse((self.workspace / "CODESTUDIO_DEMO.txt").exists())
        client.rpc("operations/cancel", {"operation_id": operation})
        self.assertEqual(client.finished(operation)["result"]["status"], "cancelled")
        self.assertEqual(self.cli("demo", "--workspace", self.workspace)["status"], "completed")

    def test_shutdown_cancels_active_task_and_flushes_protocol_response(self):
        release = self.gate()
        self.responses({"body": final(), "before_headers": release})
        client = self.client()
        client.start(self.options())
        self.wait_for(lambda: len(self.requests()) == 1)
        self.assertTrue(client.rpc("shutdown")["stopping"])
        client.wait_exit()
        self.assertEqual(self.sql("SELECT status FROM tasks"), [("cancelled",)])
        self.assertEqual(self.cli("history", "--workspace", self.workspace)[0]["status"], "cancelled")

    def test_eof_cancels_active_task_and_releases_runner_locks(self):
        release = self.gate()
        self.responses({"body": final(), "before_headers": release})
        client = self.client()
        client.start(self.options())
        self.wait_for(lambda: len(self.requests()) == 1)
        client.process.stdin.close()
        client.wait_exit()
        self.assertEqual(self.sql("SELECT status FROM tasks"), [("cancelled",)])
        self.assertEqual(self.cli("demo", "--workspace", self.workspace)["status"], "completed")

    def test_oversized_input_frame_returns_error_and_exits_without_effects(self):
        client = self.client()
        # No newline is needed to trigger the bounded input buffer. The extra
        # byte distinguishes the limit from a valid exactly-sized frame.
        payload = b"x" * (1024 * 1024 + 1) + b"\n"
        try:
            client.send_raw(payload)
        except BrokenPipeError:
            # A prompt rejection may close input before the parent's final
            # bytes are written; the error receipt and exit remain mandatory.
            pass
        rejection = client.receive(lambda frame: "error" in frame)
        self.assertEqual(rejection["error"]["code"], -32600)
        self.assertIn("limit", rejection["error"]["message"].lower())
        client.wait_exit()
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_client_notifications_cannot_start_tasks_or_shutdown_the_service(self):
        client = self.client()
        for method, params in (("tasks/start", {"mode": "demo", "options": {}}), ("shutdown", {}),
                               ("schedules/create", {"task_id": "missing", "options": {}}),
                               ("schedules/cancel", {"job_id": "missing"}),
                               ("scheduler/start", {}), ("scheduler/stop", {}),
                               ("crons/create", {"expression": "* * * * *", "options": self.options()}),
                               ("crons/pause", {"cron_id": "missing"}), ("crons/cancel", {"cron_id": "missing"}),
                               ("scheduler/autostart/enable", {}), ("scheduler/autostart/disable", {})):
            client.send_raw(json.dumps({"jsonrpc": "2.0", "method": method, "params": params}) + "\n")
        # A later correlated response is a processing barrier for both ignored
        # notifications, not a timing assumption about their side effects.
        status = client.rpc("operations/status")
        self.assertFalse(status["active"])
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / "CODESTUDIO_DEMO.txt").exists())
        self.assertFalse(any(frame.get("method") == "operation/finished" for frame in client.frames))

    def test_closed_output_cancels_active_task_and_releases_locks(self):
        release = self.gate()
        self.responses({"body": final(), "before_headers": release})
        process = self.raw_client()
        self.raw_send(process, 2, "tasks/start", {"mode": "run", "options": self.options()})
        accepted = self.raw_receive(process)
        self.assertEqual(accepted["result"]["status"], "accepted")
        self.wait_for(lambda: len(self.requests()) == 1)
        process.stdout.close()
        process.stdout = None
        # A response write observes the lost output endpoint immediately, even
        # while the model has no text/event to emit of its own.
        self.raw_send(process, 3, "operations/status")
        self.assertNotEqual(process.wait(timeout=8), 0)
        self.assertEqual(self.sql("SELECT status FROM tasks"), [("cancelled",)])
        self.assertEqual(self.cli("demo", "--workspace", self.workspace)["status"], "completed")

    def test_unread_output_hits_write_deadline_without_waiting_for_input_eof(self):
        process = self.raw_client()
        # More than a default Windows or POSIX pipe can hold, but far below the
        # service queue/input limits. stdin deliberately remains open.
        requests = b"".join((json.dumps({"jsonrpc": "2.0", "id": index,
            "method": "operations/status", "params": {}}) + "\n").encode() for index in range(2, 2050))
        completed = threading.Event()
        failures = []

        def flood():
            try:
                process.stdin.write(requests)
                process.stdin.flush()
            except (BrokenPipeError, OSError) as error:
                failures.append(error)
            finally:
                completed.set()

        writer = threading.Thread(target=flood, daemon=True)
        writer.start()
        try:
            self.assertTrue(completed.wait(3), "Service stopped draining client requests before its output deadline")
            self.assertEqual(failures, [], "Input must remain usable while output queue is below its bound")
            self.assertFalse(process.stdin.closed)
            self.assertNotEqual(process.wait(timeout=8), 0)
            self.assertFalse(self.database.exists())
        finally:
            if process.poll() is None:
                process.kill()
            writer.join(timeout=2)
            self.assertFalse(writer.is_alive(), "Owned flood writer was not released with its child process")


if __name__ == "__main__":
    unittest.main(verbosity=2)
