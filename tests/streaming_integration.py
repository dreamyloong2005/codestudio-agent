"""Exercise real HTTP streaming and CLI with local fixtures for all three APIs."""
import json
import os
import pathlib
import queue
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FIXTURE = pathlib.Path(sys.argv.pop(1)).resolve()
CLI = pathlib.Path(sys.argv.pop(1)).resolve()
APIS = ("chat-completions", "responses", "messages")
TOKEN = "stream-test-credential-keep-private"


def sse(data, name=None):
    raw = data if isinstance(data, str) else json.dumps(data, ensure_ascii=False)
    return ((f"event: {name}\r\n" if name else "") + f"data: {raw}\r\n\r\n").encode()


def events(api, text="Hello 世界", tool=None, call_id="call_1", fragments=None):
    fragments = fragments or [text[:3], text[3:]]
    if api == "chat-completions":
        def chunk(delta, finish=None):
            return sse({"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]})
        result = [chunk({"role": "assistant"})]
        if tool:
            name, args = tool
            raw = json.dumps(args)
            result += [chunk({"tool_calls": [{"index": 0, "id": call_id, "type": "function",
                                              "function": {"name": name, "arguments": ""}}]})]
            result += [chunk({"tool_calls": [{"index": 0, "function": {"arguments": part}}]})
                       for part in [raw[:5], raw[5:]]]
        else:
            result += [chunk({"content": part}) for part in fragments]
        return result + [chunk({}, "tool_calls" if tool else "stop"),
                         sse({"choices": [], "usage": {"prompt_tokens": 7, "completion_tokens": 3, "total_tokens": 10}}),
                         sse("[DONE]")]
    if api == "responses":
        def event(kind, **fields):
            return sse({"type": kind, **fields}, kind)
        result = [event("response.created", response={"id": "resp_fixture", "status": "in_progress", "output": []})]
        if tool:
            name, args = tool
            raw = json.dumps(args)
            item = {"type": "function_call", "id": "fc_fixture", "call_id": call_id,
                    "name": name, "arguments": "", "status": "in_progress"}
            result += [event("response.output_item.added", output_index=0, item=item)]
            result += [event("response.function_call_arguments.delta", output_index=0,
                             item_id="fc_fixture", delta=part) for part in [raw[:5], raw[5:]]]
            result += [event("response.function_call_arguments.done", output_index=0, item_id="fc_fixture", arguments=raw)]
            item = {**item, "arguments": raw, "status": "completed"}
        else:
            item = {"type": "message", "id": "msg_fixture", "role": "assistant", "status": "in_progress", "content": []}
            part = {"type": "output_text", "text": "", "annotations": []}
            result += [event("response.output_item.added", output_index=0, item=item),
                       event("response.content_part.added", output_index=0, item_id="msg_fixture", content_index=0, part=part)]
            result += [event("response.output_text.delta", output_index=0, item_id="msg_fixture", content_index=0, delta=x)
                       for x in fragments]
            part = {**part, "text": "".join(fragments)}
            result += [event("response.output_text.done", output_index=0, item_id="msg_fixture", content_index=0, text=part["text"]),
                       event("response.content_part.done", output_index=0, item_id="msg_fixture", content_index=0, part=part)]
            item = {**item, "status": "completed", "content": [part]}
        result += [event("response.output_item.done", output_index=0, item=item),
                   event("response.completed", response={"id": "resp_fixture", "status": "completed", "output": [item],
                                                        "usage": {"input_tokens": 7, "output_tokens": 3, "total_tokens": 10}})]
        return result
    def event(kind, **fields):
        return sse({"type": kind, **fields}, kind)
    result = [event("message_start", message={"id": "msg_fixture", "type": "message", "role": "assistant", "content": [],
                                              "stop_reason": None, "usage": {"input_tokens": 7, "output_tokens": 1}})]
    if tool:
        name, args = tool
        raw = json.dumps(args)
        result += [event("content_block_start", index=0, content_block={"type": "tool_use", "id": call_id, "name": name, "input": {}})]
        result += [event("content_block_delta", index=0, delta={"type": "input_json_delta", "partial_json": x})
                   for x in [raw[:5], raw[5:]]]
    else:
        result += [event("content_block_start", index=0, content_block={"type": "text", "text": ""})]
        result += [event("content_block_delta", index=0, delta={"type": "text_delta", "text": x}) for x in fragments]
    return result + [event("content_block_stop", index=0),
                     event("message_delta", delta={"stop_reason": "tool_use" if tool else "end_turn", "stop_sequence": None},
                           usage={"output_tokens": 3}), event("message_stop")]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *_):
        pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with self.server.guard:
            self.server.requests.append((self.path, dict(self.headers), body))
            spec = self.server.responses.pop(0) if self.server.responses else {"status": 500, "parts": [b"unexpected"]}
        parts = spec["parts"]
        try:
            self.send_response(spec.get("status", 200))
            self.send_header("Content-Type", spec.get("content_type", "text/event-stream; charset=utf-8"))
            if spec.get("chunked"):
                self.send_header("Transfer-Encoding", "chunked")
            else:
                self.send_header("Content-Length", str(sum(map(len, parts))))
            self.send_header("Connection", "close")
            self.end_headers()
            for index, part in enumerate(parts):
                if index == spec.get("pause_at"):
                    spec["release"].wait(3)
                # Deliberately split CRLF, UTF-8 and JSON at arbitrary byte boundaries.
                for start in range(0, len(part), 7):
                    chunk = part[start:start+7]
                    self.wfile.write((f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n") if spec.get("chunked") else chunk)
                    self.wfile.flush()
            if spec.get("chunked") and not spec.get("truncate_http"):
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
        except (ConnectionError, OSError):
            pass


class StreamingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.server.daemon_threads = True
        cls.server.guard = threading.Lock()
        cls.server.responses = []
        cls.server.requests = []
        cls.worker = threading.Thread(target=cls.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        cls.worker.start()
        cls.url = f"http://127.0.0.1:{cls.server.server_port}/v1"
    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.worker.join()
    def setup_responses(self, specs):
        with self.server.guard:
            self.server.responses = specs
            self.server.requests = []
    def config(self, api, **extra):
        return {"base_url": self.url, "api": api, "api_key": TOKEN, "stream": True,
                "emit_deltas": True, "allow_insecure_loopback": True, "timeout_ms": 2000, **extra}
    def invoke(self, api, specs, **extra):
        self.setup_responses(specs)
        result = subprocess.run([str(FIXTURE)], input=json.dumps(self.config(api, **extra)),
                                text=True, encoding="utf-8", capture_output=True, timeout=8)
        self.assertNotIn(TOKEN, result.stdout + result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(result.returncode, 0 if data["ok"] else 1)
        deltas = [json.loads(line)["delta"] for line in result.stderr.splitlines()]
        return data, "".join(deltas)
    def test_all_apis_stream_text_and_usage(self):
        for api in APIS:
            with self.subTest(api=api):
                data, text = self.invoke(api, [{"parts": [b"\xef\xbb\xbf: keepalive\r\n\r\n", *events(api)]}])
                self.assertTrue(data["ok"], data)
                self.assertEqual(text, "Hello 世界")
                self.assertEqual(data["replies"][0]["text"], text)
                self.assertEqual(data["replies"][0]["usage"]["completion_tokens"], 3)
                path, headers, body = self.server.requests[0]
                self.assertEqual(path, "/v1/" + ("chat/completions" if api == "chat-completions" else api))
                self.assertTrue(body["stream"])
                if api == "chat-completions":
                    self.assertEqual(body["stream_options"], {"include_usage": True})
                if api == "messages":
                    self.assertEqual(headers["x-api-key"], TOKEN)
                    self.assertEqual(headers["anthropic-version"], "2023-06-01")
                    self.assertNotIn("Authorization", headers)
                else:
                    self.assertEqual(headers["Authorization"], "Bearer " + TOKEN)
    def test_three_turn_tool_roundtrip_for_each_api(self):
        for api in APIS:
            with self.subTest(api=api):
                specs = [{"parts": events(api, tool=("write_file", {"path": "a.txt", "content": "body"}))},
                         {"parts": events(api, tool=("read_file", {"path": "a.txt"}), call_id="call_2")},
                         {"parts": events(api, text="Done")}]
                data, text = self.invoke(api, specs, rounds=3)
                self.assertTrue(data["ok"], data)
                self.assertEqual(data["files"], {"a.txt": "body"})
                self.assertEqual(text, "Done")
                second = self.server.requests[1][2]
                if api == "responses":
                    self.assertFalse(second["store"])
                    self.assertTrue(any(x.get("type") == "function_call_output" and x["call_id"] == "call_1"
                                        for x in second["input"]))
                elif api == "messages":
                    self.assertEqual(second["messages"][-1]["content"][0]["type"], "tool_result")
                    self.assertEqual(second["messages"][-1]["content"][0]["tool_use_id"], "call_1")
                else:
                    self.assertEqual(second["messages"][-1]["tool_call_id"], "call_1")
    def test_text_arrives_before_server_releases_terminal(self):
        for api in APIS:
            with self.subTest(api=api):
                release = threading.Event()
                parts = events(api)
                self.setup_responses([{"parts": parts, "pause_at": len(parts)-1, "release": release}])
                process = subprocess.Popen([str(FIXTURE)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, text=True, encoding="utf-8")
                received = queue.Queue()
                threading.Thread(target=lambda: received.put(process.stderr.readline()), daemon=True).start()
                try:
                    process.stdin.write(json.dumps(self.config(api)))
                    process.stdin.close()
                    process.stdin = None
                    first = received.get(timeout=2)
                    self.assertTrue(json.loads(first)["delta"])
                    self.assertIsNone(process.poll(), "provider completed before terminal event")
                finally:
                    release.set()
                    out, _ = process.communicate(timeout=5)
                self.assertTrue(json.loads(out)["ok"], out)
    def test_missing_terminal_rejects_complete_looking_tool(self):
        for api in APIS:
            with self.subTest(api=api):
                data, _ = self.invoke(api, [{"parts": events(api, tool=("write_file", {"path": "a", "content": "x"}))[:-1]}])
                self.assertFalse(data["ok"])
    def test_multiline_sse_data_is_one_json_event(self):
        chunk = {"choices": [{"index": 0, "delta": {"role": "assistant", "content": "Multiline"}, "finish_reason": "stop"}]}
        wire = ("\n".join("data: " + line for line in json.dumps(chunk, indent=2).splitlines()) + "\n\n").encode()
        data, text = self.invoke("chat-completions", [{"parts": [wire, sse("[DONE]")]}])
        self.assertTrue(data["ok"], data)
        self.assertEqual(text, "Multiline")
    def test_chat_tool_deltas_accept_optional_null_fields(self):
        def chunk(delta, finish=None):
            return sse({"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]})
        parts = [chunk({"tool_calls": [{"index": 0, "id": "call_1", "type": "function",
                                        "function": {"name": "write_file", "arguments": ""}}]}),
                 chunk({"role": None, "tool_calls": [{"index": 0, "id": None, "type": None,
                                        "function": {"name": None, "arguments": '{"path":"a","content":"x"}'}}]}),
                 chunk({}, "tool_calls"), sse("[DONE]")]
        data, _ = self.invoke("chat-completions", [{"parts": parts}])
        self.assertTrue(data["ok"], data)
        self.assertEqual(data["files"], {"a": "x"})
    def test_http_chunked_transfer_and_transport_truncation(self):
        for api in APIS:
            with self.subTest(api=api):
                data, text = self.invoke(api, [{"parts": events(api), "chunked": True}])
                self.assertTrue(data["ok"], data)
                self.assertEqual(text, "Hello 世界")
                data, _ = self.invoke(api, [{"parts": events(api), "chunked": True, "truncate_http": True}])
                self.assertFalse(data["ok"], "incomplete HTTP body accepted after protocol terminal")
    def test_post_terminal_and_duplicate_key_rejected(self):
        for api in APIS:
            with self.subTest(api=api):
                data, _ = self.invoke(api, [{"parts": [*events(api), *events(api)]}])
                self.assertFalse(data["ok"])
                data, _ = self.invoke(api, [{"parts": [sse('{"type":"error","type":"message_stop"}')]}])
                self.assertFalse(data["ok"])
    def test_selected_full_endpoint_and_conflicting_endpoint(self):
        for api in APIS:
            with self.subTest(api=api):
                suffix = "chat/completions" if api == "chat-completions" else api
                data, _ = self.invoke(api, [{"parts": events(api)}], base_url=self.url+"/"+suffix)
                self.assertTrue(data["ok"], data)
                self.assertEqual(self.server.requests[0][0], "/v1/"+suffix)
                wrong = "messages" if api != "messages" else "responses"
                data, _ = self.invoke(api, [], base_url=self.url+"/"+wrong)
                self.assertFalse(data["ok"])
                self.assertEqual(self.server.requests, [])
    def test_in_stream_error_never_echoes_response_body(self):
        for api in APIS:
            with self.subTest(api=api):
                error = sse({"type": "error", "error": {"message": TOKEN}}, "error")
                data, _ = self.invoke(api, [{"parts": [*events(api)[:-1], error]}])
                self.assertFalse(data["ok"])
    def test_http_failure_body_is_never_streamed(self):
        for api in APIS:
            with self.subTest(api=api):
                data, text = self.invoke(api, [{"status": 401, "parts": events(api, text=TOKEN)}])
                self.assertFalse(data["ok"])
                self.assertEqual(text, "")
    def test_content_type_malformed_json_and_size_limit(self):
        for api in APIS:
            with self.subTest(api=api):
                for spec, extra in [({"parts": events(api), "content_type": "application/json"}, {}),
                                    ({"parts": [sse("{broken")]}, {}),
                                    ({"parts": events(api, text="x"*1024)}, {"max_response_bytes": 128})]:
                    data, _ = self.invoke(api, [spec], **extra)
                    self.assertFalse(data["ok"])
    def test_cancel_and_timeout_during_stream(self):
        for api in APIS:
            with self.subTest(api=api):
                for options, expected in [({"timeout_ms": 100}, "timed out"),
                                          ({"cancel_after_ms": 100}, "cancelled")]:
                    release = threading.Event()
                    try:
                        data, _ = self.invoke(api, [{"parts": events(api), "pause_at": 1, "release": release}], **options)
                        self.assertFalse(data["ok"])
                        self.assertIn(expected, data["error"])
                    finally:
                        release.set()
    def test_non_stream_responses_and_messages(self):
        for api in ("responses", "messages"):
            if api == "responses":
                response = {"status": "completed", "output": [{"type": "message", "role": "assistant",
                    "id": "msg_1", "status": "completed", "content": [{"type": "output_text", "text": "OK", "annotations": []}]}]}
            else:
                response = {"type": "message", "role": "assistant", "content": [{"type": "text", "text": "OK"}],
                            "stop_reason": "end_turn", "usage": {"input_tokens": 1, "output_tokens": 1}}
            data, text = self.invoke(api, [{"parts": [json.dumps(response).encode()], "content_type": "application/json"}], stream=False)
            self.assertTrue(data["ok"], data)
            self.assertEqual(text, "")
            self.assertEqual(data["replies"][0]["text"], "OK")
    def cli_args(self, workspace, api):
        return [str(CLI), "run", "--workspace", str(workspace), "--prompt", "fixture", "--api", api,
                "--base-url", self.url, "--model", "fixture", "--allow-http-loopback", "--allow-write"]
    def cli_env(self):
        env = {k: v for k, v in os.environ.items() if not k.startswith("CODESTUDIO_")}
        env["CODESTUDIO_API_KEY"] = TOKEN
        return env
    def test_cli_stream_redacts_split_key_and_preserves_json_stdout(self):
        for api in APIS:
            with self.subTest(api=api), tempfile.TemporaryDirectory(prefix="codestudio-stream-") as folder:
                fragments = ["Result: ", TOKEN[:5], TOKEN[5:17], TOKEN[17:], " verified 世界\x1b[31m"]
                self.setup_responses([{"parts": events(api, fragments=fragments)}])
                result = subprocess.run(self.cli_args(folder, api), env=self.cli_env(), capture_output=True,
                                        text=True, encoding="utf-8", timeout=8)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertNotIn(TOKEN, result.stdout+result.stderr)
                self.assertIn("[REDACTED]", result.stderr)
                self.assertIn("世界", result.stderr)
                self.assertNotIn("\x1b", result.stderr)
                self.assertEqual(json.loads(result.stdout)["status"], "completed")
                with closing(sqlite3.connect(pathlib.Path(folder)/".codestudio/state.sqlite3")) as db:
                    self.assertNotIn(TOKEN, "\n".join(db.iterdump()))
    def test_cli_short_text_is_displayed_before_terminal(self):
        for api in APIS:
            with self.subTest(api=api), tempfile.TemporaryDirectory(prefix="codestudio-stream-") as folder:
                release = threading.Event()
                parts = events(api, fragments=["Hi\n"])
                self.setup_responses([{"parts": parts, "pause_at": len(parts)-1, "release": release}])
                process = subprocess.Popen(self.cli_args(folder, api), env=self.cli_env(), stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, text=True, encoding="utf-8")
                received = queue.Queue()
                threading.Thread(target=lambda: received.put(process.stderr.readline()), daemon=True).start()
                try:
                    self.assertEqual(received.get(timeout=1), "Hi\n")
                    self.assertIsNone(process.poll())
                finally:
                    release.set()
                    out, _ = process.communicate(timeout=5)
                self.assertEqual(json.loads(out)["status"], "completed")
    def test_cli_incomplete_stream_never_executes_tool(self):
        for api in APIS:
            with self.subTest(api=api), tempfile.TemporaryDirectory(prefix="codestudio-stream-") as folder:
                parts = events(api, tool=("write_file", {"path": "must-not-exist", "content": "bad"}))
                self.setup_responses([{"parts": parts[:-1]}])
                result = subprocess.run(self.cli_args(folder, api), env=self.cli_env(), capture_output=True,
                                        text=True, encoding="utf-8", timeout=8)
                self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
                self.assertEqual(json.loads(result.stdout)["status"], "failed")
                self.assertFalse((pathlib.Path(folder)/"must-not-exist").exists())
                with closing(sqlite3.connect(pathlib.Path(folder)/".codestudio/state.sqlite3")) as db:
                    self.assertEqual(db.execute("SELECT count(*) FROM actions").fetchone()[0], 0)
    def test_cli_streaming_tools_are_persisted_and_replayed(self):
        for api in APIS:
            with self.subTest(api=api), tempfile.TemporaryDirectory(prefix="codestudio-stream-") as folder:
                self.setup_responses([
                    {"parts": events(api, tool=("write_file", {"path": "actual.txt", "content": "verified"}))},
                    {"parts": events(api, tool=("read_file", {"path": "actual.txt"}), call_id="read_2")},
                    {"parts": events(api, text="Verified file")},
                ])
                result = subprocess.run(self.cli_args(folder, api), env=self.cli_env(), capture_output=True,
                                        text=True, encoding="utf-8", timeout=8)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertEqual(json.loads(result.stdout)["status"], "completed")
                self.assertEqual((pathlib.Path(folder)/"actual.txt").read_text(), "verified")
                with closing(sqlite3.connect(pathlib.Path(folder)/".codestudio/state.sqlite3")) as db:
                    self.assertEqual(db.execute("SELECT count(*) FROM actions WHERE status='succeeded'").fetchone()[0], 2)
                self.assertEqual(len(self.server.requests), 3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
