"""Exercise real OS isolation through CLI tools, MCP and resume preflight.

The model is a local HTTP fixture, not a cloud account or local inference engine.
"""
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
LINUX = sys.platform.startswith("linux")
MODE = "linux-namespace" if LINUX else "appcontainer"
SANDBOX = EXE.parent / ("sandbox-fixture" if LINUX else "sandbox-fixture.exe")
MCP = EXE.parent / ("mcp-fixture" if LINUX else "mcp-fixture.exe")
TOKEN = "sandbox-fixture-provider-credential"


def reply(tool=None):
    message = {"role": "assistant", "content": None if tool else "Verified fixture result"}
    if tool:
        message["tool_calls"] = [{"id": "sandbox-call", "type": "function", "function": {
            "name": tool[0], "arguments": json.dumps(tool[1])}}]
    return {"choices": [{"finish_reason": "tool_calls" if tool else "stop", "message": message}]}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        document = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append(document)
        status, response = self.server.responses.pop(0) if self.server.responses else (200, reply())
        body = json.dumps(response).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


@unittest.skipUnless(os.name == "nt" or LINUX, "Windows/Linux isolation integration")
class SandboxTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="codestudio-sandbox-cli-")
        self.workspace = pathlib.Path(self.temp.name) / "workspace"
        self.workspace.mkdir()
        self.database = self.workspace / ".codestudio" / "state.sqlite3"
        self.manifest = self.workspace / "mcp.json"
        self.marker = self.workspace / "mcp-started.txt"
        self.manifest.write_text(json.dumps({"version": 2, "servers": [{"id": "echo",
            "command": str(MCP), "args": ["normal", str(self.marker)], "tools": ["echo"],
            "timeout_ms": 5000}]}), encoding="utf-8")
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("CODESTUDIO_")}
        if LINUX and "CODESTUDIO_CGROUP_ROOT" in os.environ:
            self.env["CODESTUDIO_CGROUP_ROOT"] = os.environ["CODESTUDIO_CGROUP_ROOT"]
        self.env["CODESTUDIO_API_KEY"] = TOKEN
        self.env["CODESTUDIO_SANDBOX_TEST_SECRET"] = "do-not-inherit-this-sentinel"
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.server.requests = []
        self.server.responses = []
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        self.temp.cleanup()

    def invoke(self, *args, expected=0):
        process = subprocess.run([str(EXE), *map(str, args)], env=self.env, capture_output=True,
            encoding="utf-8", timeout=20)
        self.assertEqual(process.returncode, expected, process.stdout + process.stderr)
        self.assertNotIn(TOKEN, process.stdout + process.stderr)
        self.assertNotIn("do-not-inherit-this-sentinel", process.stdout + process.stderr)
        return json.loads(process.stdout)

    def run_args(self, *extra):
        return ["run", "--workspace", self.workspace, "--prompt", "Exercise the isolated fixture",
            "--base-url", f"http://127.0.0.1:{self.server.server_port}/v1", "--model", "fixture",
            "--allow-http-loopback", "--no-stream", *extra]

    def mcp_args(self):
        return ["--mcp-config", self.manifest, "--allow-mcp", "echo"]

    def resume_args(self, task_id, *extra):
        return ["resume", task_id, "--workspace", self.workspace, "--allow-http-loopback", *extra]

    def detail(self, task_id):
        return self.invoke("show", task_id, "--workspace", self.workspace)

    def test_invalid_mode_rejected_before_state_or_server_start(self):
        result = self.invoke(*self.run_args(*self.mcp_args(), "--sandbox", "invalid"), expected=2)
        self.assertIn("sandbox", result["error"].lower())
        self.assertFalse(self.database.exists())
        self.assertFalse(self.marker.exists())
        self.assertEqual(self.server.requests, [])

    def test_discovery_isolated_and_trusted_modes_have_observable_boundaries(self):
        common = ["mcp-tools", "--workspace", self.workspace, *self.mcp_args()]
        isolated = self.invoke(*common, "--sandbox", MODE)
        self.assertTrue(isolated["os_sandbox"])
        self.assertEqual([tool["function"]["name"] for tool in isolated["tools"]], ["mcp_echo_echo"])
        self.assertFalse(self.marker.exists())
        trusted = self.invoke(*common, "--sandbox", "none")
        self.assertFalse(trusted["os_sandbox"])
        self.assertTrue(self.marker.exists())
        self.assertFalse(self.database.exists())

    def test_process_policy_reaches_real_child_and_provider_can_still_connect(self):
        outside = self.workspace / "private.txt"
        outside.write_text("private sentinel", encoding="utf-8")
        new_file = self.workspace / "forbidden.txt"
        parameters = {"outside_file": str(outside), "outside_new": str(new_file), "port": self.server.server_port}
        self.server.responses = [(200, reply(("run_process", {"executable": str(SANDBOX),
            "arguments": ["probe", json.dumps(parameters)]}))), (200, reply())]
        result = self.invoke(*self.run_args("--allow-process", SANDBOX, "--sandbox", MODE))
        self.assertEqual(result["status"], "completed")
        detail = self.detail(result["task_id"])
        self.assertEqual(detail["actions"][0]["status"], "succeeded")
        report = json.loads(detail["actions"][0]["result"]["output"]["output"])
        if LINUX:
            self.assertEqual(report["seccomp"], 2)
            self.assertEqual(report["no_new_privileges"], 1)
            self.assertLessEqual(report["pid"], 3)
            for field in ("outside_read", "outside_write", "outside_create", "host_etc_read", "tool_write"):
                self.assertNotEqual(report[field], 0, report)
            self.assertEqual(report["network_socket"], 1, report)
            self.assertEqual(report["scratch_write"], 0)
            self.assertFalse(report["secret_present"])
            self.assertEqual(report["cwd"], "/work")
            self.assertFalse(new_file.exists())
            self.assertEqual(outside.read_text(), "private sentinel")
            self.assertEqual(len(self.server.requests), 2)
            return
        self.assertTrue(report["appcontainer"] and report["lpac"])
        for field in ("outside_read", "outside_write", "outside_create"):
            self.assertEqual(report[field], 5, report)
        if report["winsock_startup"] == 0:
            self.assertTrue(report["network_connect_attempted"])
            for field in ("network_loopback", "network_external"):
                self.assertEqual(report[field], 10013, report)
        else:
            self.assertEqual(report["winsock_startup"], 10107, report)
            self.assertFalse(report["network_connect_attempted"])
            self.assertNotIn("network_loopback", report)
            self.assertNotIn("network_external", report)
            self.assertEqual(report["network_afd_open"], 5, report)
        self.assertEqual(report["scratch_write"], 0)
        self.assertTrue(report["scratch_read"])
        self.assertFalse(report["secret_present"])
        self.assertFalse(pathlib.Path(report["cwd"]).exists())
        self.assertRegex(report["profile_name"], r"^codestudio-sandbox-[0-9a-f]{32}$")
        self.assertFalse((pathlib.Path(self.env["LOCALAPPDATA"]) / "Packages" / report["profile_name"]).exists())
        self.assertFalse(new_file.exists())
        self.assertEqual(outside.read_text(), "private sentinel")
        self.assertEqual(len(self.server.requests), 2)

    def test_mcp_tool_round_trip_runs_inside_isolation(self):
        self.server.responses = [(200, reply(("mcp_echo_echo", {"text": "isolated"}))), (200, reply())]
        result = self.invoke(*self.run_args(*self.mcp_args(), "--sandbox", MODE))
        action = self.detail(result["task_id"])["actions"][0]
        self.assertEqual((action["status"], action["effect"]), ("succeeded", "applied"))
        self.assertEqual(action["result"]["output"]["structuredContent"], {"text": "isolated", "calls": 1})
        self.assertFalse(self.marker.exists(), "MCP process must not create a file in the original workspace")

    def test_resume_cannot_remove_or_add_isolation_and_checks_before_mcp_launch(self):
        for mode in (MODE, "none"):
            with self.subTest(mode=mode):
                self.server.responses = [(503, {"error": {"message": "fixture unavailable"}})]
                result = self.invoke(*self.run_args(*self.mcp_args(), "--sandbox", mode), expected=1)
                self.assertEqual(result["status"], "failed")
                with closing(sqlite3.connect(self.database)) as database:
                    binding = json.loads(database.execute("SELECT binding FROM task_checkpoints WHERE task_id=?",
                        (result["task_id"],)).fetchone()[0])
                if mode == "none":
                    self.assertEqual(binding["policy"]["sandbox"], {"version": 2, "mode": "none"})
                else:
                    self.assertEqual(binding["policy"]["sandbox"], {"version": 2, "mode": MODE,
                        "network": "none", "filesystem": "private-scratch", "child_processes": "deny", "process_limit": 1})
                if self.marker.exists():
                    self.marker.unlink()
                before = len(self.server.requests)
                changed = "none" if mode == MODE else MODE
                rejected = self.invoke(*self.resume_args(result["task_id"], *self.mcp_args(),
                    "--sandbox", changed), expected=2)
                self.assertIn("binding", rejected["error"].lower())
                self.assertEqual(len(self.server.requests), before)
                self.assertFalse(self.marker.exists(), "changed binding must be checked before MCP starts")
                if mode == MODE:
                    self.invoke(*self.resume_args(result["task_id"], *self.mcp_args()), expected=2)
                    self.assertFalse(self.marker.exists(), "omitting sandbox is not authorization to remove isolation")
                self.server.responses = [(200, reply())]
                resumed = self.invoke(*self.resume_args(result["task_id"], *self.mcp_args(), "--sandbox", mode))
                self.assertEqual((resumed["task_id"], resumed["status"]), (result["task_id"], "completed"))
                if self.marker.exists():
                    self.marker.unlink()


if __name__ == "__main__":
    unittest.main()
