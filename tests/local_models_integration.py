"""Local model discovery/capability tests; only ephemeral loopback fixtures are used."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CLI = pathlib.Path(sys.argv.pop(1)).resolve()
KEY = "cloud-key-must-not-leak-to-local-services"
PROBE_TOKEN = "codestudio-probe"
ACK = "codestudio-probe-ack"
APIS = ("chat-completions", "responses", "messages")


def reply(api, tool=False, text=ACK, arguments=None, name="connection_probe"):
    args = {"token": PROBE_TOKEN} if arguments is None else arguments
    if api == "chat-completions":
        message = {"role": "assistant", "content": None if tool else text}
        if tool:
            message["tool_calls"] = [{"id": "call_1", "type": "function", "function": {
                "name": name, "arguments": json.dumps(args)}}]
        return {"choices": [{"finish_reason": "tool_calls" if tool else "stop", "message": message}]}
    if api == "responses":
        item = {"type": "function_call", "id": "fc_1", "call_id": "call_1", "name": name,
                "arguments": json.dumps(args), "status": "completed"} if tool else {
                    "type": "message", "id": "msg_1", "role": "assistant", "status": "completed",
                    "content": [{"type": "output_text", "text": text, "annotations": []}]}
        return {"id": "resp_1", "status": "completed", "output": [item]}
    block = {"type": "tool_use", "id": "call_1", "name": name, "input": args} if tool else {
        "type": "text", "text": text}
    return {"id": "msg_1", "type": "message", "role": "assistant", "content": [block],
            "stop_reason": "tool_use" if tool else "end_turn", "usage": {"input_tokens": 4, "output_tokens": 6}}


def streaming(api, tool=False, text=ACK):
    def event(kind, **fields):
        payload = json.dumps({"type": kind, **fields})
        return f"event: {kind}\ndata: {payload}\n\n"
    final = reply(api, tool, text)
    if api == "chat-completions":
        message = final["choices"][0]["message"]
        delta = {"role": "assistant"}
        if tool:
            delta["tool_calls"] = [{"index": 0, **message["tool_calls"][0]}]
        else:
            delta["content"] = text
        chunks = [{"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
                  {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls" if tool else "stop"}]}]
        return "".join("data: " + json.dumps(chunk) + "\n\n" for chunk in chunks) + "data: [DONE]\n\n"
    if api == "responses":
        item = final["output"][0]
        initial = {**item, "status": "in_progress"}
        if tool:
            initial["arguments"] = ""
        else:
            initial["content"] = []
        data = event("response.created", response={"id": "resp_1", "status": "in_progress", "output": []})
        data += event("response.output_item.added", output_index=0, item=initial)
        if tool:
            data += event("response.function_call_arguments.done", output_index=0,
                          item_id=item["id"], arguments=item["arguments"])
        else:
            data += event("response.output_text.done", output_index=0, item_id=item["id"],
                          content_index=0, text=text)
        data += event("response.output_item.done", output_index=0, item=item)
        return data + event("response.completed", response=final)
    block = final["content"][0]
    initial = {**block, "input": {}} if tool else {"type": "text", "text": ""}
    delta = {"type": "input_json_delta", "partial_json": json.dumps(block["input"])} if tool else {
        "type": "text_delta", "text": text}
    return (event("message_start", message={**final, "content": [], "stop_reason": None}) +
            event("content_block_start", index=0, content_block=initial) +
            event("content_block_delta", index=0, delta=delta) + event("content_block_stop", index=0) +
            event("message_delta", delta={"stop_reason": final["stop_reason"]}, usage={"output_tokens": 6}) +
            event("message_stop"))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def handle_request(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        with self.server.lock:
            self.server.requests.append({"method": self.command, "path": self.path,
                                         "headers": dict(self.headers), "body": json.loads(raw) if raw else None})
            spec = self.server.responses.pop(0) if self.server.responses else {"status": 500, "body": "unexpected"}
        body = spec.get("body", {"data": []})
        data = (body if isinstance(body, str) else json.dumps(body)).encode("utf-8")
        try:
            time.sleep(spec.get("delay", 0))
            self.send_response(spec.get("status", 200))
            self.send_header("Content-Type", spec.get("type", "application/json"))
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            for key, value in spec.get("headers", {}).items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(data)
        except (ConnectionError, OSError):
            pass

    do_GET = handle_request
    do_POST = handle_request


class LocalModelsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.server.daemon_threads = True
        cls.server.lock = threading.Lock()
        cls.server.requests = []
        cls.server.responses = []
        cls.worker = threading.Thread(target=cls.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        cls.worker.start()
        cls.url = f"http://127.0.0.1:{cls.server.server_port}/v1"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.worker.join()

    def invoke(self, command, responses=None, options=(), url=None):
        with self.server.lock:
            self.server.responses = responses or []
            self.server.requests = []
        env = {**os.environ, "CODESTUDIO_API_KEY": KEY, "OPENAI_API_KEY": KEY,
               "CODESTUDIO_BASE_URL": "https://cloud.invalid/v1", "CODESTUDIO_MODEL": "cloud-model",
               "CODESTUDIO_API": "messages"}
        with tempfile.TemporaryDirectory(prefix="codestudio-local-models-") as directory:
            result = subprocess.run([str(CLI), command, "--base-url", self.url if url is None else url, *options],
                                    cwd=directory, env=env, capture_output=True, text=True, encoding="utf-8", timeout=8)
            self.assertEqual(list(pathlib.Path(directory).iterdir()), [], "discovery/probe must not create task state")
        self.assertNotIn(KEY, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        return result.returncode, json.loads(result.stdout)

    def test_catalog_sorted_minimal_get_and_no_cloud_credentials(self):
        status, data = self.invoke("models", [{"body": {"data": [
            {"id": "z-model", "owned_by": "fixture", "secret_metadata": "ignored"}, {"id": "a-model"}]}}])
        self.assertEqual(status, 0, data)
        self.assertEqual(data, {"endpoint": self.url + "/models", "count": 2,
                                "models": [{"id": "a-model"}, {"id": "z-model"}]})
        request = self.server.requests[0]
        self.assertEqual((request["method"], request["path"], request["body"]), ("GET", "/v1/models", None))
        self.assertEqual(request["headers"]["Accept"], "application/json")
        self.assertNotIn("Authorization", request["headers"])
        self.assertNotIn("x-api-key", request["headers"])

    def test_catalog_explicit_key_and_redacted_output(self):
        status, data = self.invoke("models", [{"body": {"data": [{"id": KEY}]}}], options=["--use-api-key"])
        self.assertEqual(status, 0, data)
        self.assertEqual(self.server.requests[0]["headers"]["Authorization"], "Bearer " + KEY)

    def test_catalog_empty_is_success(self):
        status, data = self.invoke("models", [{"body": {"data": []}}])
        self.assertEqual((status, data["count"], data["models"]), (0, 0, []))

    def test_ollama_null_catalog_is_empty(self):
        # Ollama 0.30.10 /v1/models when /api/tags reports models: [].
        status, data = self.invoke("models", [{"body": {"object": "list", "data": None}}])
        self.assertEqual(status, 0, data)
        self.assertEqual(data, {"endpoint": self.url + "/models", "count": 0, "models": []})

    def test_invalid_catalogs_rejected(self):
        for body in ('{"data":[],"data":[]}', {"models": []}, {"data": ["wrong"]},
                     {"data": None}, {"object": "error", "data": None}, {"object": 42, "data": None},
                     {"object": "list"}, {"object": "list", "data": {}},
                     {"object": "list", "data": "null"}, {"object": "list", "data": 0},
                     {"data": [{"id": "duplicate"}, {"id": "duplicate"}]}, {"data": [{"id": "bad\nname"}]},
                     {"data": [{"id": str(i)} for i in range(4097)]}):
            with self.subTest(body=str(body)[:80]):
                status, data = self.invoke("models", [{"body": body}])
                self.assertNotEqual(status, 0, data)
                self.assertIn("error", data)

    def test_http_error_body_not_reflected(self):
        status, data = self.invoke("models", [{"status": 401, "body": KEY}])
        self.assertNotEqual(status, 0, data)
        self.assertIn("HTTP status 401", data["error"])

    def test_redirect_never_followed(self):
        status, data = self.invoke("models", [{"status": 302, "headers": {"Location": self.url + "/stolen"}}])
        self.assertNotEqual(status, 0, data)
        self.assertEqual(len(self.server.requests), 1)

    def test_remote_and_named_loopback_rejected_before_network(self):
        for command in ("models", "model-check"):
            for url in ("https://example.com/v1", "http://localhost/v1", "https://127.0.0.1.example.com/v1"):
                with self.subTest(command=command, url=url):
                    status, data = self.invoke(command, options=["--model", "fixture"] if command == "model-check" else [], url=url)
                    self.assertNotEqual(status, 0, data)
                    self.assertEqual(self.server.requests, [])

    def test_probe_all_protocols_and_streaming_modes(self):
        for api in APIS:
            for stream in (False, True):
                with self.subTest(api=api, stream=stream):
                    responses = [{"body": streaming(api, tool) if stream else reply(api, tool),
                                  "type": "text/event-stream" if stream else "application/json"} for tool in (True, False)]
                    options = ["--model", "fixture-model", "--api", api] + ([] if stream else ["--no-stream"])
                    status, data = self.invoke("model-check", responses, options)
                    self.assertEqual(status, 0, data)
                    self.assertEqual(data["status"], "compatible")
                    self.assertEqual(data["capabilities"], {"inference": True, "tool_call": True,
                                                           "tool_result": True, "streaming": True if stream else None})
                    self.assertEqual(len(self.server.requests), 2)
                    for request in self.server.requests:
                        self.assertEqual(request["method"], "POST")
                        self.assertEqual(request["body"]["model"], "fixture-model")
                        self.assertNotIn("Authorization", request["headers"])
                        self.assertNotIn("x-api-key", request["headers"])
                    continuation = json.dumps(self.server.requests[1]["body"])
                    self.assertIn(ACK, continuation)
                    self.assertNotIn("cloud-model", continuation)

    def test_probe_requires_explicit_model(self):
        status, data = self.invoke("model-check")
        self.assertNotEqual(status, 0, data)
        self.assertEqual(self.server.requests, [])

    def test_no_tool_or_wrong_argument_is_unsupported(self):
        for body in (reply("chat-completions", text="I cannot use tools"),
                     reply("chat-completions", True, arguments={"token": "wrong"}),
                     reply("chat-completions", True, name="write_file")):
            with self.subTest(body=body):
                status, data = self.invoke("model-check", [{"body": body}], ["--model", "fixture", "--no-stream"])
                self.assertNotEqual(status, 0, data)
                self.assertEqual(data["status"], "unsupported")
                self.assertFalse(data["capabilities"]["tool_call"])
                self.assertEqual(len(self.server.requests), 1)

    def test_wrong_followup_is_unsupported(self):
        status, data = self.invoke("model-check", [{"body": reply("chat-completions", True)},
                                                  {"body": reply("chat-completions", text="wrong")}],
                                   ["--model", "fixture", "--no-stream"])
        self.assertNotEqual(status, 0, data)
        self.assertEqual(data["status"], "unsupported")
        self.assertTrue(data["capabilities"]["tool_call"])
        self.assertFalse(data["capabilities"]["tool_result"])

    def test_malformed_and_truncated_probe_is_error(self):
        for spec in ({"body": "{broken"}, {"body": "data: {}\n\n", "type": "text/event-stream"}):
            with self.subTest(spec=spec):
                options = ["--model", "fixture"] + ([] if "type" in spec else ["--no-stream"])
                status, data = self.invoke("model-check", [spec], options)
                self.assertNotEqual(status, 0, data)
                self.assertEqual(data["status"], "error")

    def test_probe_explicit_messages_key(self):
        status, data = self.invoke("model-check", [{"body": reply("messages", True)}, {"body": reply("messages")}],
                                   ["--model", "fixture", "--api", "messages", "--no-stream", "--use-api-key"])
        self.assertEqual(status, 0, data)
        self.assertEqual(self.server.requests[0]["headers"]["x-api-key"], KEY)
        self.assertNotIn("Authorization", self.server.requests[0]["headers"])

    def test_catalog_timeout(self):
        status, data = self.invoke("models", [{"delay": 0.4}], ["--timeout-ms", "100"])
        self.assertNotEqual(status, 0, data)
        self.assertIn("timed out", data["error"])


if __name__ == "__main__":
    unittest.main()
