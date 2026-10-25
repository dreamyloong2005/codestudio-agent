"""Exercise durable resume through the real CLI, HTTP transport and SQLite ledger.

All model replies are local fixtures. Crashes are deliberate child termination;
no service, model download, account or cloud inference is needed.
"""
import copy
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
MCP = EXE.parent / ("mcp-fixture.exe" if os.name == "nt" else "mcp-fixture")
TOKEN = "resume-credential-never-persist"
APIS = ("chat-completions", "responses", "messages")


def reply(api="chat-completions", tool=None, call_id="once", text="Resume complete", opaque="opaque-continuation"):
    if api == "chat-completions":
        message = {"role": "assistant", "content": None if tool else text}
        if tool:
            message["tool_calls"] = [{"id": call_id, "type": "function", "function": {
                "name": tool[0], "arguments": json.dumps(tool[1])}}]
        return {"choices": [{"finish_reason": "tool_calls" if tool else "stop", "message": message}]}
    if api == "responses":
        output = ([{"id": "reasoning_1", "type": "reasoning", "summary": [], "encrypted_content": opaque},
                   {"type": "function_call", "id": "function_1", "call_id": call_id,
                    "name": tool[0], "arguments": json.dumps(tool[1]), "status": "completed"}]
                  if tool else [{"id": "message_1", "type": "message", "role": "assistant", "status": "completed",
                                 "content": [{"type": "output_text", "text": text, "annotations": []}]}])
        return {"id": "response_1", "status": "completed", "output": output,
                "usage": {"input_tokens": 7, "output_tokens": 3, "total_tokens": 10}}
    content = ([{"type": "thinking", "thinking": "Private fixture thinking", "signature": opaque},
                {"type": "redacted_thinking", "data": "opaque-redacted-block"},
                {"type": "tool_use", "id": call_id, "name": tool[0], "input": tool[1]}]
               if tool else [{"type": "text", "text": text}])
    return {"type": "message", "id": "message_1", "role": "assistant", "model": "fixture",
            "content": content, "stop_reason": "tool_use" if tool else "end_turn", "stop_sequence": None,
            "usage": {"input_tokens": 7, "output_tokens": 3}}


def stream_reply(tool=None, call_id="once", text="Streaming resume complete"):
    def event(delta, finish=None):
        return "data: " + json.dumps({"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}) + "\n\n"
    delta = ({"tool_calls": [{"index": 0, "id": call_id, "type": "function",
                             "function": {"name": tool[0], "arguments": json.dumps(tool[1])}}]}
             if tool else {"content": text})
    body = event({"role": "assistant"}) + event(delta) + event({}, "tool_calls" if tool else "stop") + "data: [DONE]\n\n"
    return {"raw": body.encode(), "content_type": "text/event-stream"}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with self.server.guard:
            self.server.requests.append({"path": self.path, "headers": dict(self.headers), "body": body})
            spec = self.server.responses.pop(0) if self.server.responses else {"status": 500}
        if "release" in spec:
            spec["release"].wait(15)
        data = spec.get("raw", json.dumps(spec.get("body", {"error": "fixture unavailable"})).encode())
        try:
            self.send_response(spec.get("status", 200))
            self.send_header("Content-Type", spec.get("content_type", "application/json"))
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
            self.wfile.flush()
        except (ConnectionError, OSError):
            pass


class ResumeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="codestudio-resume-")
        self.workspace = pathlib.Path(self.temp.name) / "workspace with spaces"
        self.workspace.mkdir()
        self.database = self.workspace / ".codestudio" / "state.sqlite3"
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("CODESTUDIO_")}
        if sys.platform.startswith("linux") and "CODESTUDIO_CGROUP_ROOT" in os.environ:
            self.env["CODESTUDIO_CGROUP_ROOT"] = os.environ["CODESTUDIO_CGROUP_ROOT"]
        self.env["CODESTUDIO_API_KEY"] = TOKEN
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.server.guard = threading.Lock()
        self.server.responses = []
        self.server.requests = []
        self.worker = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        self.worker.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}/v1"
        self.releases = []
        self.children = []

    def tearDown(self):
        for process in self.children:
            self.kill(process)
        for release in self.releases:
            release.set()
        self.server.shutdown()
        self.server.server_close()
        self.worker.join(timeout=5)
        self.temp.cleanup()

    def setup_responses(self, *responses):
        with self.server.guard:
            self.server.responses = list(responses)

    def requests(self):
        with self.server.guard:
            return copy.deepcopy(self.server.requests)

    def invoke(self, *args, expected=0, env=None):
        process = subprocess.run([str(EXE), *map(str, args)], env=env or self.env, capture_output=True,
                                 text=True, encoding="utf-8", timeout=15)
        self.assertEqual(process.returncode, expected, process.stdout + process.stderr)
        self.assertNotIn(TOKEN, process.stdout + process.stderr)
        return json.loads(process.stdout)

    def run_args(self, *extra, api="chat-completions", streaming=False):
        return ["run", "--workspace", self.workspace, "--prompt", "Safely continue this engineering task",
                "--base-url", self.url, "--model", "resume-fixture", "--api", api,
                "--allow-http-loopback", *([] if streaming else ["--no-stream"]), *extra]

    def resume_args(self, task_id, *extra, loopback=True):
        return ["resume", task_id, "--workspace", self.workspace,
                *(["--allow-http-loopback"] if loopback else []), *extra]

    def start(self, *args):
        process = subprocess.Popen([str(EXE), *map(str, args)], env=self.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(process)
        return process

    @staticmethod
    def kill(process):
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=5)

    def wait_for(self, condition, process=None):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            try:
                value = condition()
                if value:
                    return value
            except sqlite3.OperationalError:
                pass
            if process and process.poll() is not None:
                out, err = process.communicate()
                self.fail(f"runner exited before checkpoint boundary: {out!r} {err!r}")
            time.sleep(0.02)
        self.fail("durable boundary was not reached before deadline")

    def sql_rows(self, sql, parameters=()):
        if not self.database.exists():
            return []
        with closing(sqlite3.connect(self.database)) as database:
            return database.execute(sql, parameters).fetchall()

    def sql_execute(self, sql, parameters=()):
        with closing(sqlite3.connect(self.database)) as database:
            database.execute(sql, parameters)
            database.commit()

    def show(self, task_id):
        return self.invoke("show", task_id, "--workspace", self.workspace)

    def failed_task(self, *extra, api="chat-completions"):
        self.setup_responses({"status": 503})
        result = self.invoke(*self.run_args(*extra, api=api), expected=1)
        self.assertEqual(result["status"], "failed")
        return result["task_id"]

    def configure_mcp(self):
        manifest = self.workspace / "mcp.json"
        marker = self.workspace / "mcp.started"
        manifest.write_text(json.dumps({"version": 2, "servers": [{"id": "echo", "command": str(MCP),
            "args": ["normal", str(marker)], "tools": ["echo"], "timeout_ms": 1000}]}), encoding="utf-8")
        return manifest, marker, ["--mcp-config", manifest, "--allow-mcp", "echo"]

    def test_crash_after_receipt_preserves_action_and_reissues_only_model_request(self):
        release = threading.Event()
        self.releases.append(release)
        self.setup_responses({"body": reply(tool=("write_file", {"path": "once.txt", "content": "original"}))},
                             {"body": reply(), "release": release})
        process = self.start(*self.run_args("--allow-write"))
        self.wait_for(lambda: len(self.requests()) == 2, process)
        task_id = self.sql_rows("SELECT id FROM tasks")[0][0]
        self.assertEqual(self.sql_rows("SELECT status,effect FROM actions"), [("succeeded", "applied")])
        self.kill(process)
        release.set()
        before = self.requests()[1]["body"]
        blocked = self.invoke(*self.resume_args(task_id, "--allow-write"), expected=2)
        self.assertIn("recover", blocked["error"].lower())
        self.assertEqual(len(self.requests()), 2)
        recovered = self.invoke("recover", "--workspace", self.workspace)
        self.assertFalse(recovered["tools_executed"])
        (self.workspace / "once.txt").write_text("changed after crash", encoding="utf-8")
        self.setup_responses({"body": reply()})
        result = self.invoke(*self.resume_args(task_id, "--allow-write"))
        self.assertEqual((result["task_id"], result["status"], result["turns"]), (task_id, "completed", 3))
        self.assertEqual(self.requests()[2]["body"], before)
        self.assertEqual((self.workspace / "once.txt").read_text(), "changed after crash")
        detail = self.show(task_id)
        self.assertEqual(len(detail["actions"]), 1)
        self.assertEqual(sum(item["kind"] == "action.dispatched" for item in detail["events"]), 1)
        self.assertEqual(sum(item["kind"] == "task.resumed" for item in detail["events"]), 1)

    def test_unknown_dispatched_process_blocks_resume_before_http_or_plugin_launch(self):
        _, marker, mcp_args = self.configure_mcp()
        grants = ["--allow-process", PROCESS, "--process-timeout-ms", "15000", *mcp_args]
        self.setup_responses({"body": reply(tool=("run_process", {"executable": str(PROCESS),
                                                                 "arguments": ["sleep", "10000"]}))})
        process = self.start(*self.run_args(*grants))
        self.wait_for(lambda: self.sql_rows("SELECT id FROM actions WHERE status='dispatched'"), process)
        self.kill(process)
        recovered = self.invoke("recover", "--workspace", self.workspace)
        task_id = recovered["recovered"][0]["task_id"]
        self.assertEqual(recovered["recovered"][0]["unknown_actions"], 1)
        self.assertTrue(marker.exists())
        marker.unlink()
        self.invoke(*self.resume_args(task_id, *grants), expected=2)
        self.assertFalse(marker.exists(), "unsafe task launched an MCP process before rejecting resume")
        self.assertEqual(len(self.requests()), 1)
        self.assertEqual(self.show(task_id)["actions"][0]["effect"], "unknown")

    def test_exhaustion_requires_larger_total_budget_and_keeps_receipt(self):
        (self.workspace / "input.txt").write_text("content", encoding="utf-8")
        self.setup_responses({"body": reply(tool=("read_file", {"path": "input.txt"}))})
        first = self.invoke(*self.run_args("--max-turns", "1"), expected=1)
        self.assertEqual((first["status"], first["turns"]), ("exhausted", 1))
        self.invoke(*self.resume_args(first["task_id"]), expected=2)
        self.assertEqual(len(self.requests()), 1)
        self.setup_responses({"body": reply()})
        final = self.invoke(*self.resume_args(first["task_id"], "--max-turns", "2"))
        self.assertEqual((final["task_id"], final["turns"]), (first["task_id"], 2))
        detail = self.show(first["task_id"])
        self.assertEqual(len(detail["actions"]), 1)
        self.assertIsInstance(detail["checkpoint"], dict)
        self.assertNotIn("messages", detail["checkpoint"], "show should summarize, not duplicate full conversation")

    def test_failed_model_retry_restores_saved_config_and_uses_current_secret(self):
        task_id = self.failed_task()
        changed_env = dict(self.env, CODESTUDIO_BASE_URL="https://invalid.invalid/v1", CODESTUDIO_MODEL="wrong",
                           CODESTUDIO_API="messages", CODESTUDIO_API_KEY="new-resume-test-secret")
        self.setup_responses({"body": reply()})
        result = self.invoke(*self.resume_args(task_id), env=changed_env)
        self.assertEqual((result["task_id"], result["status"], result["turns"]), (task_id, "completed", 2))
        self.assertEqual(self.requests()[0]["body"], self.requests()[1]["body"])
        self.assertEqual(self.requests()[1]["headers"]["Authorization"], "Bearer new-resume-test-secret")
        with closing(sqlite3.connect(self.database)) as database:
            dump = "\n".join(database.iterdump())
        self.assertNotIn(TOKEN, dump)
        self.assertNotIn("new-resume-test-secret", dump)

    def test_cancelled_safe_boundary_is_eligible_for_explicit_resume(self):
        # Materialize a cancelled durable outcome at the same safe model-pending
        # boundary. Native signal handling is covered below the CLI on Windows,
        # where CTRL+C is unavailable to children without an attached console.
        task_id = self.failed_task()
        self.sql_execute("UPDATE tasks SET status='cancelled' WHERE id=?", (task_id,))
        self.setup_responses({"body": reply()})
        result = self.invoke(*self.resume_args(task_id))
        self.assertEqual((result["task_id"], result["status"], result["turns"]), (task_id, "completed", 2))

    def test_model_endpoint_api_and_workspace_mismatch_reject_before_request(self):
        task_id = self.failed_task()
        for override in (["--model", "different"], ["--base-url", self.url + "/different"], ["--api", "responses"]):
            with self.subTest(override=override):
                self.invoke(*self.resume_args(task_id, *override), expected=2)
                self.assertEqual(len(self.requests()), 1)
        other = pathlib.Path(self.temp.name) / "other workspace"
        other.mkdir()
        self.invoke("resume", task_id, "--workspace", other, "--state", self.database,
                    "--allow-http-loopback", expected=2)
        self.assertEqual(len(self.requests()), 1)

    def test_write_grant_must_be_repeated_and_cannot_be_added(self):
        task_id = self.failed_task("--allow-write")
        self.invoke(*self.resume_args(task_id), expected=2)
        self.assertEqual(len(self.requests()), 1)
        self.setup_responses({"body": reply()})
        self.invoke(*self.resume_args(task_id, "--allow-write"))
        second = self.failed_task()
        before = len(self.requests())
        self.invoke(*self.resume_args(second, "--allow-write"), expected=2)
        self.assertEqual(len(self.requests()), before)

    def test_process_and_insecure_loopback_grants_must_be_repeated(self):
        task_id = self.failed_task("--allow-process", PROCESS)
        self.invoke(*self.resume_args(task_id), expected=2)
        self.invoke(*self.resume_args(task_id, "--allow-process", PROCESS, loopback=False), expected=2)
        self.assertEqual(len(self.requests()), 1)
        self.setup_responses({"body": reply()})
        self.invoke(*self.resume_args(task_id, "--allow-process", PROCESS))

    def test_mcp_grant_and_configuration_guard_run_before_server_launch(self):
        manifest, marker, grants = self.configure_mcp()
        task_id = self.failed_task(*grants)
        self.assertTrue(marker.exists())
        marker.unlink()
        self.invoke(*self.resume_args(task_id), expected=2)
        self.invoke(*self.resume_args(task_id, "--mcp-config", manifest), expected=2)
        self.assertFalse(marker.exists())
        original = manifest.read_text(encoding="utf-8")
        changed = json.loads(original)
        changed["servers"][0]["args"][0] = "stderr"
        manifest.write_text(json.dumps(changed), encoding="utf-8")
        self.invoke(*self.resume_args(task_id, *grants), expected=2)
        self.assertFalse(marker.exists(), "changed manifest launched a process before binding comparison")
        self.assertEqual(len(self.requests()), 1)
        manifest.write_text(original, encoding="utf-8")
        self.setup_responses({"body": reply()})
        self.invoke(*self.resume_args(task_id, *grants))
        self.assertTrue(marker.exists())

    def test_unsupported_checkpoint_protocol_versions_are_rejected_without_rewrite(self):
        task_id = self.failed_task()
        original = self.sql_rows("SELECT version,state,binding FROM task_checkpoints WHERE task_id=?", (task_id,))[0]
        self.assertEqual(original[0], 2)
        self.assertEqual(json.loads(original[1])["version"], 2)
        self.assertEqual(json.loads(original[2])["version"], 2)
        self.assertEqual(self.sql_rows("PRAGMA user_version")[0][0], 7)
        self.assertEqual({row[0] for row in self.sql_rows("SELECT DISTINCT schema_version FROM events")}, {2})
        for field in ("version", "state", "binding"):
            for version in ((0, 1, 3) if field == "version" else (0, 1, 3, True, "2")):
                with self.subTest(field=field, version=version):
                    if field == "version":
                        value = version
                    else:
                        value = json.loads(original[1 if field == "state" else 2])
                        value["version"] = version
                        value = json.dumps(value)
                    self.sql_execute(f"UPDATE task_checkpoints SET {field}=? WHERE task_id=?", (value, task_id))
                    frozen = self.sql_rows("SELECT version,state,binding FROM task_checkpoints WHERE task_id=?", (task_id,))[0]
                    self.invoke(*self.resume_args(task_id), expected=2)
                    self.assertEqual(len(self.requests()), 1)
                    self.assertEqual(self.sql_rows("SELECT version,state,binding FROM task_checkpoints WHERE task_id=?", (task_id,))[0], frozen)
                    self.sql_execute("UPDATE task_checkpoints SET version=?,state=?,binding=? WHERE task_id=?", (*original, task_id))
        self.assertEqual(self.show(task_id)["status"], "failed")

    def test_current_secret_collision_rejected_before_mcp_process_start(self):
        _, marker, grants = self.configure_mcp()
        task_id = self.failed_task(*grants)
        marker.unlink()
        # A rotated key that already occurs in saved history would require
        # altering the checkpoint; reject before starting the granted process.
        changed_env = dict(self.env, CODESTUDIO_API_KEY="engineering task")
        self.invoke(*self.resume_args(task_id, *grants), env=changed_env, expected=2)
        self.assertFalse(marker.exists())
        self.assertEqual(len(self.requests()), 1)
        self.assertEqual(self.sql_rows("SELECT status FROM tasks WHERE id=?", (task_id,))[0][0], "failed")

    def test_redacted_native_continuation_is_not_resumed(self):
        (self.workspace / "input.txt").write_text("safe", encoding="utf-8")
        self.setup_responses({"body": reply("responses", tool=("read_file", {"path": "input.txt"}), opaque=TOKEN)},
                             {"status": 503})
        result = self.invoke(*self.run_args(api="responses"), expected=1)
        self.assertEqual(result["status"], "failed")
        task_id = result["task_id"]
        state = json.loads(self.sql_rows("SELECT state FROM task_checkpoints WHERE task_id=?", (task_id,))[0][0])
        self.assertFalse(state["resumable"])
        with closing(sqlite3.connect(self.database)) as database:
            self.assertNotIn(TOKEN, "\n".join(database.iterdump()))
        self.invoke(*self.resume_args(task_id), expected=2)
        self.assertEqual(len(self.requests()), 2)

    def test_terminal_task_cannot_be_resumed(self):
        self.setup_responses({"body": reply()})
        completed = self.invoke(*self.run_args())
        self.invoke(*self.resume_args(completed["task_id"]), expected=2)
        self.setup_responses({"body": reply(tool=("write_file", {"path": "denied.txt", "content": "denied"}))},
                             {"body": reply()})
        attention = self.invoke(*self.run_args(), expected=1)
        self.assertEqual(attention["status"], "needs_attention")
        before = len(self.requests())
        self.invoke(*self.resume_args(attention["task_id"]), expected=2)
        self.assertEqual(len(self.requests()), before)

    def test_invalid_checkpoint_fields_are_rejected_without_network(self):
        task_id = self.failed_task()
        original = json.loads(self.sql_rows("SELECT state FROM task_checkpoints WHERE task_id=?", (task_id,))[0][0])
        for field, invalid in (("messages", {}), ("turns", -1), ("pending_calls", "bad"), ("next_call", 1000)):
            with self.subTest(field=field):
                state = {**original, field: invalid}
                self.sql_execute("UPDATE task_checkpoints SET state=? WHERE task_id=?", (json.dumps(state), task_id))
                self.invoke(*self.resume_args(task_id), expected=2)
                self.assertEqual(len(self.requests()), 1)
                self.assertEqual(self.sql_rows("SELECT status FROM tasks WHERE id=?", (task_id,))[0][0], "failed")
        self.sql_execute("UPDATE task_checkpoints SET state=? WHERE task_id=?", (json.dumps(original), task_id))

    def test_invalid_checkpoint_rejected_before_mcp_process_start(self):
        _, marker, grants = self.configure_mcp()
        task_id = self.failed_task(*grants)
        original = json.loads(self.sql_rows("SELECT state FROM task_checkpoints WHERE task_id=?", (task_id,))[0][0])
        self.assertTrue(marker.exists())
        marker.unlink()
        malformed_messages = copy.deepcopy(original["messages"])
        malformed_messages[1]["content"] = "does not match the recorded task prompt"
        for field, invalid in (("messages", {}), ("messages", malformed_messages), ("next_call", 1)):
            with self.subTest(field=field, invalid=invalid):
                state = {**original, field: invalid}
                self.sql_execute("UPDATE task_checkpoints SET state=? WHERE task_id=?", (json.dumps(state), task_id))
                self.invoke(*self.resume_args(task_id, *grants), expected=2)
                self.assertFalse(marker.exists(), "invalid checkpoint launched an MCP child before validation")
                self.assertEqual(len(self.requests()), 1)
                self.assertEqual(self.sql_rows("SELECT status FROM tasks WHERE id=?", (task_id,))[0][0], "failed")
        self.sql_execute("UPDATE task_checkpoints SET state=? WHERE task_id=?", (json.dumps(original), task_id))

    def test_duplicate_tool_id_is_rejected_across_resumed_turns(self):
        (self.workspace / "input.txt").write_text("safe", encoding="utf-8")
        tool = ("read_file", {"path": "input.txt"})
        self.setup_responses({"body": reply(tool=tool)})
        first = self.invoke(*self.run_args("--max-turns", "1"), expected=1)
        self.setup_responses({"body": reply(tool=tool)})
        result = self.invoke(*self.resume_args(first["task_id"], "--max-turns", "3"), expected=1)
        self.assertEqual(result["status"], "failed")
        self.assertEqual(len(self.show(first["task_id"])["actions"]), 1)
        self.assertEqual(len(self.requests()), 2)

    def test_all_three_apis_preserve_native_continuation_across_restart(self):
        (self.workspace / "input.txt").write_text("你好 resume", encoding="utf-8")
        for api in APIS:
            with self.subTest(api=api):
                before = len(self.requests())
                self.setup_responses({"body": reply(api, tool=("read_file", {"path": "input.txt"}))}, {"status": 503})
                first = self.invoke(*self.run_args(api=api), expected=1)
                self.assertEqual(first["status"], "failed")
                failed_request = self.requests()[before + 1]
                self.setup_responses({"body": reply(api)})
                result = self.invoke(*self.resume_args(first["task_id"]))
                replay = self.requests()[before + 2]
                self.assertEqual((result["task_id"], result["turns"]), (first["task_id"], 3))
                self.assertEqual(replay["path"], failed_request["path"])
                self.assertEqual(replay["body"], failed_request["body"])
                self.assertEqual(len(self.show(first["task_id"])["actions"]), 1)
                if api == "responses":
                    self.assertIn("opaque-continuation", json.dumps(replay["body"]["input"]))
                elif api == "messages":
                    self.assertIn("opaque-continuation", json.dumps(replay["body"]["messages"]))
                    self.assertIn("opaque-redacted-block", json.dumps(replay["body"]["messages"]))

    def test_streaming_mode_restored_on_resume(self):
        (self.workspace / "input.txt").write_text("safe", encoding="utf-8")
        self.setup_responses(stream_reply(tool=("read_file", {"path": "input.txt"})), {"status": 503})
        first = self.invoke(*self.run_args(streaming=True), expected=1)
        self.assertEqual(first["status"], "failed")
        self.setup_responses(stream_reply())
        result = self.invoke(*self.resume_args(first["task_id"]))
        self.assertEqual(result["status"], "completed")
        self.assertTrue(self.requests()[-1]["body"]["stream"])
        self.assertEqual(self.requests()[1]["body"], self.requests()[2]["body"])


if __name__ == "__main__":
    unittest.main()
