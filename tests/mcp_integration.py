"""Exercise MCP grants, real child calls and durable receipts through the CLI."""
import json
import os
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

EXE = pathlib.Path(sys.argv.pop(1)).resolve()
FIXTURE = EXE.parent / ("mcp-fixture.exe" if os.name == "nt" else "mcp-fixture")
TOKEN = "mcp-test-api-key"


def final():
    return {"choices": [{"finish_reason": "stop", "message": {"role": "assistant", "content": "Finished"}}]}


def call(arguments, name="mcp_echo_echo"):
    return {"choices": [{"finish_reason": "tool_calls", "message": {
        "role": "assistant", "content": None, "tool_calls": [{"id": "echo-call", "type": "function",
        "function": {"name": name, "arguments": json.dumps(arguments)}}]}}]}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append(data)
        response = self.server.responses.pop(0) if self.server.responses else final()
        body = json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class McpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.worker = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.worker.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.worker.join()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="codestudio-mcp-cli-")
        self.workspace = pathlib.Path(self.temp.name) / "workspace with spaces"
        self.workspace.mkdir()
        self.manifest = self.workspace / "mcp.json"
        self.marker = self.workspace / "server.started"
        self.database = self.workspace / ".codestudio" / "state.sqlite3"
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("CODESTUDIO_")}
        self.env["CODESTUDIO_API_KEY"] = TOKEN
        self.server.responses = []
        self.server.requests = []
        self.configure()

    def tearDown(self):
        self.temp.cleanup()

    def configure(self, mode="normal", **overrides):
        server = {"id": "echo", "command": str(FIXTURE), "args": [mode, str(self.marker)],
                  "tools": ["echo"], "timeout_ms": 1000}
        server.update(overrides)
        self.manifest.write_text(json.dumps({"version": 2, "servers": [server]}), encoding="utf-8")

    def invoke(self, *args, expected=0):
        process = subprocess.run([str(EXE), *map(str, args)], capture_output=True,
                                 encoding="utf-8", env=self.env, timeout=15)
        self.assertEqual(process.returncode, expected, process.stdout + process.stderr)
        for secret in (TOKEN, "sensitive-payload", "sensitive backend", "ignored diagnostics"):
            self.assertNotIn(secret, process.stdout + process.stderr)
        return json.loads(process.stdout)

    def args(self, *extra, grant=True):
        return ["run", "--workspace", self.workspace, "--prompt", "Exercise the granted echo tool",
                "--base-url", f"http://127.0.0.1:{self.server.server_port}/v1", "--model", "fixture",
                "--allow-http-loopback", "--no-stream", "--mcp-config", self.manifest,
                *(["--allow-mcp", "echo"] if grant else []), *extra]

    def detail(self, result):
        return self.invoke("show", result["task_id"], "--workspace", self.workspace)

    def test_ungranted_server_never_starts_or_exposes_tools(self):
        self.server.responses = [final()]
        self.invoke(*self.args(grant=False))
        self.assertFalse(self.marker.exists())
        names = [x["function"]["name"] for x in self.server.requests[0]["tools"]]
        self.assertEqual(names, ["read_file"])

    def test_discovery_requires_grant_and_has_no_task_database(self):
        args = ["mcp-tools", "--workspace", self.workspace, "--mcp-config", self.manifest]
        self.invoke(*args, expected=2)
        self.assertFalse(self.marker.exists())
        result = self.invoke(*args, "--allow-mcp", "echo")
        self.assertEqual([x["function"]["name"] for x in result["tools"]], ["mcp_echo_echo"])
        self.assertFalse(result["os_sandbox"])
        self.assertTrue(self.marker.exists())
        self.assertFalse(self.database.exists())

    def test_unknown_grant_and_invalid_later_entry_do_not_start_any_server(self):
        self.invoke(*self.args("--allow-mcp", "missing"), expected=2)
        self.assertFalse(self.marker.exists())
        document = json.loads(self.manifest.read_text())
        document["servers"].append({"id": "invalid", "command": "relative.exe", "args": [], "tools": ["echo"]})
        self.manifest.write_text(json.dumps(document))
        self.invoke(*self.args(), expected=2)
        self.assertFalse(self.marker.exists())
        self.assertEqual(self.server.requests, [])

    def test_unsupported_manifest_versions_rejected_before_server_or_model_dispatch(self):
        document = json.loads(self.manifest.read_text())
        for version in (0, 1, 3, True, "2"):
            with self.subTest(version=version):
                document["version"] = version
                self.manifest.write_text(json.dumps(document), encoding="utf-8")
                for args in (self.args(), ["mcp-tools", "--workspace", self.workspace,
                             "--mcp-config", self.manifest, "--allow-mcp", "echo"]):
                    rejected = self.invoke(*args, expected=2)
                    self.assertIn("Unsupported MCP manifest version", json.dumps(rejected))
                self.assertFalse(self.marker.exists())
                self.assertEqual(self.server.requests, [])

    def test_round_trip_records_frozen_schema_intent_and_result(self):
        self.server.responses = [call({"text": "hello"}), final()]
        result = self.invoke(*self.args())
        detail = self.detail(result)
        action = detail["actions"][0]
        self.assertEqual((action["tool"], action["status"], action["effect"]),
                         ("mcp_echo_echo", "succeeded", "applied"))
        receipt = action["result"]["output"]["structuredContent"]
        self.assertEqual(receipt, {"text": "hello", "calls": 1})
        model_result = json.loads(self.server.requests[1]["messages"][-1]["content"])
        self.assertEqual(model_result["output"]["structuredContent"], receipt)
        kinds = [x["kind"] for x in detail["events"]]
        self.assertLess(kinds.index("action.prepared"), kinds.index("action.dispatched"))
        self.assertLess(kinds.index("action.dispatched"), kinds.index("action.finished"))
        schemas = next(x["payload"] for x in detail["events"] if x["kind"] == "tools.available")
        self.assertEqual([x["function"]["name"] for x in schemas], ["read_file", "mcp_echo_echo"])

    def test_bad_arguments_and_ungranted_tool_are_denied(self):
        for arguments, name in [({"text": 42}, "mcp_echo_echo"), ({"text": "a", "extra": True}, "mcp_echo_echo"),
                                ({"text": "a"}, "mcp_echo_hidden")]:
            with self.subTest(arguments=arguments, name=name):
                self.server.responses = [call(arguments, name), final()]
                result = self.invoke(*self.args(), expected=1)
                self.assertEqual(result["status"], "needs_attention")
                action = self.detail(result)["actions"][0]
                self.assertEqual((action["status"], action["effect"]), ("denied", "none"))

    def test_uncertain_plugin_failures_stop_without_another_model_request(self):
        for mode in ("timeout", "eof", "malformed-call", "rpc-error", "list-change", "bad-result", "tool-error"):
            with self.subTest(mode=mode):
                self.configure(mode)
                self.server.requests = []
                self.server.responses = [call({"text": "hello"}), final()]
                result = self.invoke(*self.args(), expected=1)
                self.assertEqual(result["status"], "needs_attention")
                action = self.detail(result)["actions"][0]
                self.assertEqual((action["status"], action["effect"]), ("unknown", "unknown"))
                self.assertEqual(len(self.server.requests), 1)

    def test_stderr_and_server_side_requests_do_not_break_tool_loop(self):
        for mode in ("stderr", "side-requests"):
            with self.subTest(mode=mode):
                self.configure(mode)
                self.server.responses = [call({"text": "hello"}), final()]
                result = self.invoke(*self.args())
                self.assertEqual(self.detail(result)["actions"][0]["status"], "succeeded")

    def test_plugin_result_key_redacted_from_model_and_database(self):
        self.server.responses = [call({"text": TOKEN}), final()]
        result = self.invoke(*self.args())
        self.detail(result)
        self.assertNotIn(TOKEN, self.server.requests[1]["messages"][-1]["content"])
        with closing(sqlite3.connect(self.database)) as db:
            self.assertNotIn(TOKEN, "\n".join(db.iterdump()))

    def test_manifest_is_protected_from_file_tools(self):
        self.server.responses = [call({"path": "mcp.json"}, "read_file"), final()]
        result = self.invoke(*self.args(), expected=1)
        self.assertEqual(self.detail(result)["actions"][0]["status"], "denied")

    def test_invalid_prompt_does_not_launch_plugin(self):
        args = self.args()
        args[args.index("--prompt") + 1] = ""
        self.invoke(*args, expected=2)
        self.assertFalse(self.marker.exists())
        self.assertEqual(self.server.requests, [])

    def test_aggregate_tool_limit_rejected_before_any_server_starts(self):
        document = json.loads(self.manifest.read_text())
        first = document["servers"][0]
        first["tools"] = ["echo", *[f"tool_{i}" for i in range(64)]]
        second = dict(first, id="second")
        document["servers"].append(second)
        self.manifest.write_text(json.dumps(document))
        self.invoke("mcp-tools", "--workspace", self.workspace, "--mcp-config", self.manifest,
                    "--allow-mcp", "echo", "--allow-mcp", "second", expected=2)
        self.assertFalse(self.marker.exists(), "predictably oversized configuration launched a child")

    def test_documented_python_example_runs_through_real_tool_loop(self):
        script = pathlib.Path(__file__).resolve().parents[1] / "examples" / "mcp_echo.py"
        self.configure(command=sys.executable, args=[str(script)])
        self.server.responses = [call({"text": "你好 MCP"}), final()]
        result = self.invoke(*self.args())
        output = self.detail(result)["actions"][0]["result"]["output"]
        self.assertEqual(output["content"], [{"type": "text", "text": "你好 MCP"}])


if __name__ == "__main__":
    unittest.main()
