"""Local HTTP integration tests; no account, API key or Internet is required.

Usage: python tests/provider_integration.py path/to/provider-fixture[.exe]
"""

import json
import pathlib
import subprocess
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


FIXTURE = pathlib.Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None
TOKEN = "test-credential-MUST-NOT-APPEAR-IN-DIAGNOSTICS"
CONTENT = "Hello from CodeStudio-Agent!\n"


def final(text="Fixture complete.", **fields):
    return {"choices": [{"finish_reason": "stop", "message": {
        "role": "assistant", "content": text, **fields}}]}


def call(call_id="call_write_1", name="write_file", arguments=None, text=None):
    arguments = {"path": "CODESTUDIO_DEMO.txt", "content": CONTENT} if arguments is None else arguments
    return {"choices": [{"finish_reason": "tool_calls", "message": {
        "role": "assistant", "content": text, "tool_calls": [{
            "id": call_id, "type": "function", "function": {
                "name": name,
                "arguments": json.dumps(arguments) if not isinstance(arguments, str) else arguments,
            }}]}}]}


class MockServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.responses = []
        self.requests = []
        self.lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        with self.server.lock:
            self.server.requests.append({"path": self.path, "headers": dict(self.headers), "body": json.loads(body)})
            response = self.server.responses.pop(0) if self.server.responses else {"status": 500, "body": "unexpected request"}
        status = response.get("status", 200)
        payload = response.get("body", final())
        data = (json.dumps(payload) if not isinstance(payload, str) else payload).encode()
        try:
            time.sleep(response.get("delay", 0))
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            for key, value in response.get("headers", {}).items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


class ProviderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not FIXTURE or not FIXTURE.is_file():
            raise RuntimeError("Supply the built provider-fixture executable as the first argument")
        cls.server = MockServer()
        cls.worker = threading.Thread(target=cls.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        cls.worker.start()
        cls.base_url = f"http://127.0.0.1:{cls.server.server_port}/v1"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.worker.join()

    def run_fixture(self, responses=None, **overrides):
        with self.server.lock:
            self.server.responses = responses or []
            self.server.requests = []
        config = {"base_url": self.base_url, "api_key": TOKEN, "allow_insecure_loopback": True, **overrides}
        result = subprocess.run([str(FIXTURE)], input=json.dumps(config), text=True, capture_output=True, timeout=8)
        self.assertNotIn(TOKEN, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertIn(result.returncode, (0, 1), result.stdout)
        output = json.loads(result.stdout)
        self.assertEqual(result.returncode == 0, output["ok"])
        return output

    def assert_rejected(self, payload, message):
        result = self.run_fixture([{"body": payload}])
        self.assertFalse(result["ok"])
        self.assertIn(message, result["error"])

    def test_complete_two_tool_rounds(self):
        result = self.run_fixture([
            {"body": call(text="I will write the file.")},
            {"body": call("call_read_1", "read_file", {"path": "CODESTUDIO_DEMO.txt"})},
            {"body": final("Read-back verified.")},
        ], rounds=3)
        self.assertTrue(result["ok"])
        self.assertEqual(len(result["replies"]), 3)
        self.assertEqual(result["files"]["CODESTUDIO_DEMO.txt"], CONTENT)
        requests = self.server.requests
        self.assertEqual(len(requests), 3)
        self.assertEqual([len(r["body"]["messages"]) for r in requests], [1, 3, 5])
        for request in requests:
            self.assertEqual(request["path"], "/v1/chat/completions")
            self.assertEqual(request["headers"]["Authorization"], "Bearer " + TOKEN)
            self.assertIs(request["body"]["stream"], False)
            self.assertIs(request["body"]["parallel_tool_calls"], False)
        self.assertEqual(requests[1]["body"]["messages"][1]["content"], "I will write the file.")
        self.assertEqual(json.loads(requests[2]["body"]["messages"][-1]["content"])["output"]["content"], CONTENT)

    def test_full_endpoint_and_normalized_response(self):
        payload = final("OK", hidden_gateway_field={"secret": "do-not-persist"})
        payload["usage"] = {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7, "secret": "ignored"}
        result = self.run_fixture([{"body": payload}], base_url=self.base_url + "/chat/completions/")
        self.assertTrue(result["ok"])
        reply = result["replies"][0]
        self.assertEqual(reply["message"], {"role": "assistant", "content": "OK"})
        self.assertEqual(reply["usage"], {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7})
        self.assertEqual(self.server.requests[0]["path"], "/v1/chat/completions")

    def test_http_errors_do_not_echo_credentials(self):
        for status in (400, 401, 429, 500):
            with self.subTest(status=status):
                result = self.run_fixture([{"status": status, "body": TOKEN}])
                self.assertFalse(result["ok"])
                self.assertIn(str(status), result["error"])

    def test_redirect_is_not_followed(self):
        result = self.run_fixture([{"status": 307, "body": TOKEN,
                                    "headers": {"Location": self.base_url + "/redirect?token=" + TOKEN}}])
        self.assertFalse(result["ok"])
        self.assertIn("307", result["error"])
        self.assertEqual(len(self.server.requests), 1)

    def test_url_security(self):
        invalid_urls = [
            "http://localhost:1234/v1", "http://127.0.0.2/v1", "http://127.1/v1", "http://2130706433/v1",
            "http://127.0.0.1.evil.example/v1", "http://[::ffff:127.0.0.1]/v1", "http://[::2]/v1",
            "https://user:password@example.test/v1", "https://example.test/v1?key=" + TOKEN,
            "https://example.test/v1#" + TOKEN, "https://example.test/\r\nInjected: x",
            "https://example.test\\@127.0.0.1/v1", "https://example.test:0/v1",
            "https://example.test:65536/v1", "https://example.test:/v1", "file:///tmp/model",
        ]
        for url in invalid_urls:
            with self.subTest(url=url.replace(TOKEN, "<redacted>")):
                result = self.run_fixture(base_url=url)
                self.assertFalse(result["ok"])
                self.assertEqual(self.server.requests, [])
        result = self.run_fixture(allow_insecure_loopback=False)
        self.assertFalse(result["ok"])
        self.assertEqual(self.server.requests, [])

    def test_invalid_header_credential(self):
        result = self.run_fixture(api_key="bad\r\nInjected: value")
        self.assertFalse(result["ok"])
        self.assertIn("header", result["error"])
        self.assertEqual(self.server.requests, [])

    def test_no_key_for_local_models(self):
        result = self.run_fixture([{"body": final()}], api_key="")
        self.assertTrue(result["ok"])
        self.assertNotIn("Authorization", self.server.requests[0]["headers"])

    def test_invalid_json_and_top_level_shape(self):
        for payload in ("not json " + TOKEN, "{} trailing", "[]", {}, {"choices": []}):
            with self.subTest(payload=str(payload)[:30]):
                result = self.run_fixture([{"body": payload}])
                self.assertFalse(result["ok"])
        self.assert_rejected("[" * 140 + "0" + "]" * 140, "JSON")

    def test_abnormal_finishes_and_empty_answers(self):
        for reason in ("length", "content_filter", None, "unknown", "function_call"):
            payload = final()
            payload["choices"][0]["finish_reason"] = reason
            with self.subTest(reason=reason):
                result = self.run_fixture([{"body": payload}])
                self.assertFalse(result["ok"])
        self.assert_rejected(final(""), "empty final")
        self.assert_rejected(final(" \n\t"), "empty final")
        self.assert_rejected(final(None), "empty final")
        self.assert_rejected(final("answer", refusal="refused " + TOKEN), "refused")
        self.assert_rejected(final([{"text": "unsupported"}]), "string or null")

    def test_tool_arguments_must_be_complete_objects(self):
        for arguments in ("{", "{\"path\":", "[]", "null", "\"text\"", "{} {}",
                          '{"path":"safe","path":"other"}', "{" * 140):
            with self.subTest(arguments=arguments[:30]):
                self.assert_rejected(call(arguments=arguments), "complete JSON object")

    def test_tool_call_shape_and_finish_consistency(self):
        payload = call()
        payload["choices"][0]["message"]["tool_calls"] *= 2
        self.assert_rejected(payload, "unique")
        payload = call()
        payload["choices"][0]["finish_reason"] = "stop"
        self.assert_rejected(payload, "finish_reason")
        payload = final()
        payload["choices"][0]["finish_reason"] = "tool_calls"
        self.assert_rejected(payload, "no tool calls")
        self.assert_rejected(call(name="run shell"), "malformed tool")
        self.assert_rejected(call(call_id=""), "unique")
        payload = call()
        del payload["choices"][0]["message"]["tool_calls"][0]["function"]["arguments"]
        self.assert_rejected(payload, "malformed tool")
        payload = call()
        payload["choices"][0]["message"]["tool_calls"][0]["function"]["arguments"] = {}
        self.assert_rejected(payload, "malformed tool")

    def test_reused_tool_id_is_rejected(self):
        result = self.run_fixture([{"body": call()}, {"body": call()}], rounds=2)
        self.assertFalse(result["ok"])
        self.assertIn("unique", result["error"])

    def test_response_size_limit(self):
        result = self.run_fixture([{"body": "x" * 4096}], max_response_bytes=128)
        self.assertFalse(result["ok"])
        self.assertIn("size limit", result["error"])

    def test_timeout_and_cancellation(self):
        start = time.monotonic()
        result = self.run_fixture([{"delay": 0.5}], timeout_ms=50)
        self.assertFalse(result["ok"])
        self.assertIn("timed out", result["error"])
        self.assertLess(time.monotonic() - start, 3)
        result = self.run_fixture([{"delay": 1}], cancel_after_ms=50)
        self.assertFalse(result["ok"])
        self.assertIn("cancelled", result["error"])
        result = self.run_fixture(cancelled=True)
        self.assertFalse(result["ok"])
        self.assertIn("cancelled", result["error"])
        self.assertEqual(self.server.requests, [])

    def test_offline_demo_and_readback_failure(self):
        result = self.run_fixture(demo=True, rounds=3)
        self.assertTrue(result["ok"])
        self.assertEqual(len(result["replies"]), 3)
        self.assertEqual(result["files"]["CODESTUDIO_DEMO.txt"], CONTENT)
        self.assertIn("verified", result["replies"][-1]["text"])
        self.assertEqual(self.server.requests, [])
        result = self.run_fixture(demo=True, rounds=3, corrupt_read=True)
        self.assertFalse(result["ok"])
        self.assertIn("read-back verification failed", result["error"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
