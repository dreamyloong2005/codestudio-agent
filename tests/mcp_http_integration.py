"""Exercise remote MCP through real CLI/service, loopback HTTP and the ledger.

All credentials and servers are synthetic. The fixture uses no external
network, model account, installed plugin or real operating-system credential.
"""
import json
import os
import sqlite3
import subprocess
import sys
import time
import unittest
import uuid
from contextlib import closing

import resume_integration as fixture
import scheduler_integration as scheduler_fixture
from mcp_http_fixture import CURSOR, PROTOCOL, UNICODE_CURSOR, RemoteMcp

# service_integration consumes the executable argument at import, just as the
# model fixture does. Do not expose either imported TestCase to discovery.
_arguments = sys.argv[:]
sys.argv[:] = [_arguments[0], str(fixture.EXE)]
try:
    import service_integration as service_fixture
finally:
    sys.argv[:] = _arguments


class RemoteMcpTests(unittest.TestCase):
    run_args = fixture.ResumeTests.run_args
    resume_args = fixture.ResumeTests.resume_args
    setup_responses = fixture.ResumeTests.setup_responses
    requests = fixture.ResumeTests.requests
    start = fixture.ResumeTests.start
    kill = staticmethod(fixture.ResumeTests.kill)
    wait_for = fixture.ResumeTests.wait_for
    sql_rows = fixture.ResumeTests.sql_rows
    worker_args = scheduler_fixture.SchedulerTests.worker_args
    schedule = scheduler_fixture.SchedulerTests.schedule
    job = scheduler_fixture.SchedulerTests.job
    once = scheduler_fixture.SchedulerTests.once

    def setUp(self):
        fixture.ResumeTests.setUp(self)
        self.workspace = self.workspace.resolve()
        self.database = self.workspace / ".codestudio" / "state.sqlite3"
        self.manifest = self.workspace / "remote-mcp.json"
        self.remote = RemoteMcp()
        self.clients = []
        self.token = "remote-only-" + uuid.uuid4().hex
        self.variable = "CODESTUDIO_TEST_REMOTE_TOKEN"
        self.reference = "env:" + self.variable
        self.env[self.variable] = self.token
        self.secrets = [fixture.TOKEN, self.token]
        self.configure()

    def tearDown(self):
        try:
            self.remote.server.release.set()
            for client in self.clients:
                client.close()
                for secret in self.secrets:
                    self.assertNotIn(secret.encode(), bytes(client.raw) + bytes(client.stderr))
            self.remote.close()
        finally:
            fixture.ResumeTests.tearDown(self)

    def configure(self, mode="json", anonymous=False, **overrides):
        self.remote.server.mode = mode
        server = {"id": "remote", "transport": "streamable-http", "url": self.remote.url,
                  "allow_http_loopback": True, "tools": ["echo"], "timeout_ms": 1500}
        if not anonymous:
            server["credential"] = self.reference
        server.update(overrides)
        self.write_manifest([server])
        return server

    def write_manifest(self, servers):
        self.manifest.write_text(json.dumps({"version": 2, "servers": servers}), encoding="utf-8")

    def grants(self, grant=True):
        return ["--mcp-config", self.manifest, *(["--allow-mcp", "remote"] if grant else [])]

    def capture(self, *args, env=None):
        result = subprocess.run([str(fixture.EXE), *map(str, args)], cwd=self.workspace,
                                env=self.env if env is None else env, capture_output=True, timeout=20)
        for secret in self.secrets:
            self.assertNotIn(secret.encode(), result.stdout + result.stderr)
        return result

    def invoke(self, *args, expected=0, env=None):
        process = self.capture(*args, env=env)
        self.assertEqual(process.returncode, expected, process.stdout + process.stderr)
        return json.loads(process.stdout)

    def discover(self, expected=0, *extra):
        return self.invoke("mcp-tools", "--workspace", self.workspace, *self.grants(), *extra,
                           expected=expected)

    def run_call(self, text="你好 remote MCP", expected=0, name="mcp_remote_echo", arguments=None):
        self.setup_responses({"body": fixture.reply(tool=(name, {"text": text} if arguments is None else arguments))},
                             {"body": fixture.reply()})
        return self.invoke(*self.run_args(*self.grants()), expected=expected)

    def detail(self, result):
        return self.invoke("show", result["task_id"], "--workspace", self.workspace)

    def assert_unknown(self, result):
        self.assertEqual(result["status"], "needs_attention")
        action = self.detail(result)["actions"][0]
        self.assertEqual((action["status"], action["effect"]), ("unknown", "unknown"))

    def assert_no_task_effects(self):
        # run/serve acquire the workspace lock and materialize an empty Store
        # before execution preflight. Rejecting a configuration must still
        # leave no task or action, even when the schema database exists.
        if self.database.exists():
            with closing(sqlite3.connect(self.database.as_uri() + "?mode=ro", uri=True)) as database:
                self.assertEqual(database.execute("SELECT count(*) FROM tasks").fetchone(), (0,))
                self.assertEqual(database.execute("SELECT count(*) FROM actions").fetchone(), (0,))

    def assert_secrets_absent(self):
        for path in self.workspace.rglob("*"):
            if path.is_file():
                for secret in self.secrets:
                    self.assertNotIn(secret.encode(), path.read_bytes(), str(path))
        if self.database.exists():
            with closing(sqlite3.connect(self.database)) as database:
                dump = "\n".join(database.iterdump())
            for secret in self.secrets:
                self.assertNotIn(secret, dump)

    def service(self):
        client = service_fixture.Client(self.workspace, self.env)
        self.clients.append(client)
        client.initialize()
        return client

    def service_options(self):
        return {"prompt": "Exercise the remote MCP server", "base_url": self.url,
                "model": "fixture", "api": "chat-completions", "stream": False,
                "allow_http_loopback": True, "mcp_config": str(self.manifest), "allow_mcp": ["remote"]}

    def test_json_discovery_and_call_record_frozen_schema_and_receipt(self):
        discovered = self.discover()
        self.assertEqual([item["function"]["name"] for item in discovered["tools"]], ["mcp_remote_echo"])
        self.assertFalse(discovered["os_sandbox"])
        self.assertFalse(self.database.exists())
        result = self.run_call()
        detail = self.detail(result)
        action = detail["actions"][0]
        self.assertEqual((action["tool"], action["status"], action["effect"]),
                         ("mcp_remote_echo", "succeeded", "applied"))
        receipt = action["result"]["output"]["structuredContent"]
        self.assertEqual(receipt, {"text": "你好 remote MCP", "calls": 1})
        model_receipt = json.loads(self.requests()[1]["body"]["messages"][-1]["content"])
        self.assertEqual(model_receipt["output"]["structuredContent"], receipt)
        available = next(item["payload"] for item in detail["events"] if item["kind"] == "tools.available")
        self.assertEqual([item["function"]["name"] for item in available], ["read_file", "mcp_remote_echo"])
        self.assertEqual(len(self.remote.records("tools/call")), 1)

    def test_sse_fragmented_utf8_bom_multiline_data_and_newline_variants(self):
        self.configure("sse")
        result = self.run_call(text="跨字节 UTF-8 🧪")
        self.assertEqual(self.detail(result)["actions"][0]["result"]["output"]["content"],
                         [{"type": "text", "text": "跨字节 UTF-8 🧪"}])
        self.assertEqual(self.remote.records(verb="GET"), [])

    def test_sse_server_ping_is_replied_to_in_a_separate_post(self):
        self.configure("ping")
        self.run_call()
        replies = [record for record in self.remote.records(verb="POST")
                   if record["body"].get("id") == "server-ping" and "method" not in record["body"]]
        self.assertEqual(len(replies), 1)
        self.assertEqual(replies[0]["body"], {"jsonrpc": "2.0", "id": "server-ping", "result": {}})

    def test_unsupported_server_request_gets_method_not_found_without_executing_it(self):
        self.configure("unsupported-request")
        self.run_call()
        replies = [record for record in self.remote.records(verb="POST")
                   if record["body"].get("id") == "server-unsupported" and "method" not in record["body"]]
        self.assertEqual(len(replies), 1)
        self.assertEqual(replies[0]["body"]["error"]["code"], -32601)
        self.assertEqual(len(self.requests()), 2)
        self.assertEqual(len(self.remote.records("tools/call")), 1)

    def test_session_and_negotiated_protocol_headers_and_session_delete(self):
        self.discover()
        initialize = self.remote.records("initialize")[0]
        self.assertEqual(initialize["body"]["params"]["protocolVersion"], PROTOCOL)
        self.assertNotIn("mcp-session-id", initialize["headers"])
        for record in self.remote.records():
            self.assertIn("application/json", record["headers"].get("accept", ""))
            self.assertIn("text/event-stream", record["headers"].get("accept", ""))
            if (record["body"] or {}).get("method") != "initialize":
                self.assertEqual(record["headers"].get("mcp-session-id"), "remote-session-1")
                self.assertEqual(record["headers"].get("mcp-protocol-version"), PROTOCOL)
        self.assertEqual(len(self.remote.records(verb="DELETE")), 1)

    def test_stateless_server_and_delete_refusal_are_supported(self):
        self.configure("stateless")
        self.run_call()
        self.assertTrue(all("mcp-session-id" not in record["headers"] for record in self.remote.records()))
        self.assertEqual(self.remote.records(verb="DELETE"), [])
        self.configure()
        self.remote.server.delete_status = 405
        self.discover()
        self.assertEqual(len(self.remote.records(verb="DELETE")), 1)

    def test_selected_remote_bearer_credential_is_separate_from_model_key(self):
        self.run_call()
        for record in self.remote.records():
            self.assertEqual(record["headers"].get("authorization"), "Bearer " + self.token)
            self.assertNotIn(fixture.TOKEN, json.dumps(record))
        for record in self.requests():
            self.assertEqual(record["headers"].get("Authorization"), "Bearer " + fixture.TOKEN)
            self.assertNotIn(self.token, json.dumps(record))
        self.assert_secrets_absent()

    def test_anonymous_remote_never_inherits_model_credential(self):
        self.configure(anonymous=True)
        self.run_call()
        self.assertTrue(all("authorization" not in record["headers"] for record in self.remote.records()))
        self.assertTrue(all(fixture.TOKEN not in json.dumps(record) for record in self.remote.records()))

    def test_unauthorized_initialize_has_no_retry_model_or_tool_call(self):
        self.configure("unauthorized")
        self.invoke(*self.run_args(*self.grants()), expected=2)
        self.assertEqual(len(self.remote.records("initialize")), 1)
        self.assertEqual(self.remote.records("tools/call"), [])
        self.assertEqual(self.requests(), [])
        self.assert_no_task_effects()

    def test_redirect_is_not_followed_and_credential_is_not_forwarded(self):
        self.configure("redirect")
        self.discover(expected=2)
        records = self.remote.records()
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["path"], "/mcp")
        self.assertEqual(self.requests(), [])

    def test_ungranted_remote_and_unknown_grant_never_open_network(self):
        self.setup_responses({"body": fixture.reply()})
        self.invoke(*self.run_args(*self.grants(grant=False)))
        self.assertEqual(self.remote.records(), [])
        self.assertEqual([item["function"]["name"] for item in self.requests()[0]["body"]["tools"]], ["read_file"])
        self.invoke(*self.run_args(*self.grants(), "--allow-mcp", "missing"), expected=2)
        self.assertEqual(self.remote.records(), [])
        self.assertEqual(len(self.requests()), 1)

    def test_invalid_later_entry_prevents_earlier_network_and_stdio_launch(self):
        remote = self.configure()
        marker = self.workspace / "stdio.started"
        local = {"id": "local", "command": str(fixture.MCP), "args": ["normal", str(marker)],
                 "tools": ["echo"], "timeout_ms": 1000}
        invalid = dict(remote, id="invalid", url="http://example.com/mcp")
        for servers in ([local, remote, invalid], [remote, local, invalid]):
            with self.subTest(first=servers[0]["id"]):
                self.write_manifest(servers)
                self.invoke(*self.run_args(*self.grants(), "--allow-mcp", "local"), expected=2)
                self.assertFalse(marker.exists())
                self.assertEqual(self.remote.records(), [])
                self.assertEqual(self.requests(), [])
                self.assert_no_task_effects()

    def test_missing_selected_remote_credential_prevents_all_side_effects(self):
        remote = self.configure()
        marker = self.workspace / "stdio.started"
        self.write_manifest([{"id": "local", "command": str(fixture.MCP), "args": ["normal", str(marker)],
                              "tools": ["echo"], "timeout_ms": 1000}, remote])
        environment = dict(self.env)
        del environment[self.variable]
        self.invoke(*self.run_args(*self.grants(), "--allow-mcp", "local"), env=environment, expected=2)
        self.assertFalse(marker.exists())
        self.assertEqual(self.remote.records(), [])
        self.assertEqual(self.requests(), [])
        self.assert_no_task_effects()

    def test_remote_execution_is_rejected_with_local_os_sandbox(self):
        mode = "appcontainer" if os.name == "nt" else "linux-namespace"
        self.discover(2, "--sandbox", mode)
        self.assertEqual(self.remote.records(), [])
        self.assertFalse(self.database.exists())

    def test_insecure_or_ambiguous_urls_fail_before_network(self):
        for overrides in ({"allow_http_loopback": False}, {"url": self.remote.url.replace("127.0.0.1", "localhost")},
                          {"url": self.remote.url + "?token=forbidden"}, {"url": self.remote.url + "#fragment"},
                          {"url": self.remote.url.replace("127.0.0.1", "user@127.0.0.1")},
                          {"url": "file:///tmp/mcp"}):
            with self.subTest(overrides=overrides):
                self.configure(**overrides)
                self.discover(expected=2)
                self.assertEqual(self.remote.records(), [])
                self.assertFalse(self.database.exists())

    def test_invalid_and_mixed_transport_fields_fail_before_network(self):
        for overrides in ({"command": str(fixture.MCP), "args": []}, {"args": []},
                          {"transport": "http"}, {"transport": "stdio"},
                          {"allow_http_loopback": "true"}, {"credential": "literal-token"},
                          {"api_key": "must-not-be-accepted"}):
            with self.subTest(overrides=overrides):
                self.configure(**overrides)
                self.discover(expected=2)
                self.assertEqual(self.remote.records(), [])
                self.assertEqual(self.requests(), [])
                self.assertFalse(self.database.exists())

    def test_interrupted_sse_resumes_with_cursor_dedup_without_post_replay(self):
        self.configure("resume")
        self.run_call()
        gets = self.remote.records(verb="GET")
        self.assertEqual(len(gets), 1)
        # HTTP headers arrive as ISO-8859-1 text in Python's HTTP parser.
        self.assertEqual(gets[0]["headers"].get("last-event-id", "").encode("latin-1").decode("utf-8"), CURSOR)
        self.assertEqual(gets[0]["headers"].get("mcp-session-id"), "remote-session-1")
        self.assertEqual(len(self.remote.records("tools/call")), 1)
        replies = [record for record in self.remote.records(verb="POST")
                   if record["body"].get("id") == "server-ping" and "method" not in record["body"]]
        self.assertEqual(len(replies), 1, "replayed SSE event dispatched a duplicate server response")

    def test_unicode_event_cursor_has_explicit_platform_behavior_without_replay(self):
        self.configure("resume")
        self.remote.server.cursor = UNICODE_CURSOR
        if os.name == "nt":
            # WinHTTP currently rejects a cursor it cannot faithfully send in
            # Last-Event-ID. It must never substitute bytes or resend a POST.
            self.assert_unknown(self.run_call(expected=1))
            self.assertEqual(self.remote.records(verb="GET"), [])
        else:
            self.run_call()
            gets = self.remote.records(verb="GET")
            self.assertEqual(len(gets), 1)
            self.assertEqual(gets[0]["headers"]["last-event-id"].encode("latin-1").decode("utf-8"), UNICODE_CURSOR)
        self.assertEqual(len(self.remote.records("tools/call")), 1)

    def test_server_retry_delay_is_observed_before_resume(self):
        self.configure("resume")
        self.remote.server.retry_ms = 250
        self.run_call()
        first = self.remote.records("tools/call")[0]["at"]
        resumed = self.remote.records(verb="GET")[0]["at"]
        self.assertGreaterEqual(resumed - first, 0.20)
        self.assertLess(resumed - first, 1.5)

    def test_reconnect_count_is_bounded_and_uncertain_effect_stops_task(self):
        self.configure("resume-exhausted")
        result = self.run_call(expected=1)
        self.assert_unknown(result)
        self.assertEqual(len(self.remote.records(verb="GET")), 2)
        self.assertEqual(len(self.remote.records("tools/call")), 1)
        self.assertEqual(len(self.requests()), 1)

    def test_retry_delay_does_not_extend_original_tool_deadline(self):
        self.configure("retry-deadline", timeout_ms=350)
        started = time.monotonic()
        result = self.run_call(expected=1)
        self.assert_unknown(result)
        self.assertLess(time.monotonic() - started, 2.5)
        self.assertEqual(self.remote.records(verb="GET"), [])
        self.assertEqual(len(self.remote.records("tools/call")), 1)

    def test_session_404_does_not_replay_and_fresh_load_initializes_new_session(self):
        self.configure("session-expired")
        result = self.run_call(expected=1)
        self.assert_unknown(result)
        self.assertEqual(len(self.remote.records("initialize")), 1)
        self.assertEqual(len(self.remote.records("tools/call")), 1)
        self.configure()
        self.run_call()
        self.assertEqual(len(self.remote.records("initialize")), 2)
        calls = self.remote.records("tools/call")
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[1]["headers"].get("mcp-session-id"), "remote-session-2")

    def test_session_change_during_call_is_unknown_without_reinitialization(self):
        self.configure("session-changed")
        result = self.run_call(expected=1)
        self.assert_unknown(result)
        self.assertEqual(len(self.remote.records("initialize")), 1)
        self.assertEqual(len(self.remote.records("tools/call")), 1)
        self.assertEqual(len(self.requests()), 1)

    def test_tools_list_changed_invalidates_frozen_schema_without_followup(self):
        self.configure("list-changed")
        result = self.run_call(expected=1)
        self.assert_unknown(result)
        self.assertEqual(len(self.remote.records("tools/list")), 1)
        self.assertEqual(len(self.remote.records("tools/call")), 1)
        self.assertEqual(len(self.requests()), 1)

    def test_tool_call_401_is_unknown_and_never_retried(self):
        self.configure("call-unauthorized")
        result = self.run_call(expected=1)
        self.assert_unknown(result)
        self.assertEqual(len(self.remote.records("initialize")), 1)
        self.assertEqual(len(self.remote.records("tools/call")), 1)
        self.assertEqual(self.remote.records(verb="GET"), [])
        self.assertEqual(len(self.requests()), 1)

    def test_disconnect_without_cursor_blocks_resume_before_model_or_mcp_io(self):
        self.configure("disconnect")
        result = self.run_call(expected=1)
        self.assert_unknown(result)
        self.assertEqual(self.remote.records(verb="GET"), [])
        before = self.remote.records()
        self.invoke(*self.resume_args(result["task_id"], *self.grants()), expected=2)
        self.assertEqual(self.remote.records(), before)
        self.assertEqual(len(self.requests()), 1)

    def test_remote_token_in_tool_result_is_redacted_from_model_and_database(self):
        self.remote.server.output_text = "before " + self.token + " after"
        result = self.run_call()
        output = self.detail(result)["actions"][0]["result"]["output"]
        self.assertIn("[REDACTED]", json.dumps(output))
        self.assertNotIn(self.token, json.dumps(self.requests()))
        self.assert_secrets_absent()

    def test_remote_token_reflected_in_tool_schema_is_rejected_before_model_request(self):
        self.configure("schema-secret")
        self.remote.server.output_text = self.token
        self.invoke(*self.run_args(*self.grants()), expected=2)
        self.assertEqual(self.requests(), [])
        self.assertEqual(self.remote.records("tools/call"), [])
        self.assert_no_task_effects()
        self.assert_secrets_absent()

    def test_remote_token_split_across_model_stream_is_redacted_before_emission(self):
        def chunk(delta, finish=None):
            return "data: " + json.dumps({"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}) + "\n\n"
        stream = chunk({"role": "assistant"})
        for fragment in ["before ", *self.token, " after"]:
            stream += chunk({"content": fragment})
        stream += chunk({}, "stop") + "data: [DONE]\n\n"
        self.setup_responses({"raw": stream.encode(), "content_type": "text/event-stream"})
        result = self.capture(*self.run_args(*self.grants(), streaming=True))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b"[REDACTED]", result.stdout + result.stderr)
        self.assert_secrets_absent()

    def test_rotated_remote_credential_is_resolved_on_resume_without_tool_replay(self):
        self.setup_responses({"body": fixture.reply(tool=("mcp_remote_echo", {"text": "once"}))}, {"status": 503})
        result = self.invoke(*self.run_args(*self.grants()), expected=1)
        self.assertEqual(self.detail(result)["actions"][0]["status"], "succeeded")
        boundary = len(self.remote.records())
        replacement = "rotated-remote-" + uuid.uuid4().hex
        self.env[self.variable] = replacement
        self.secrets.append(replacement)
        self.setup_responses({"body": fixture.reply()})
        resumed = self.invoke(*self.resume_args(result["task_id"], *self.grants()))
        self.assertEqual(resumed["status"], "completed")
        self.assertEqual(len(self.remote.records("tools/call")), 1)
        self.assertTrue(self.remote.records()[boundary:])
        self.assertTrue(all(record["headers"].get("authorization") == "Bearer " + replacement
                            for record in self.remote.records()[boundary:]))
        self.assert_secrets_absent()

    def test_resume_manifest_endpoint_or_credential_change_rejected_before_io(self):
        original = self.configure()
        self.setup_responses({"status": 503})
        result = self.invoke(*self.run_args(*self.grants()), expected=1)
        boundary = self.remote.records()
        other_variable = "CODESTUDIO_TEST_OTHER_REMOTE_TOKEN"
        self.env[other_variable] = self.token
        for overrides in ({"url": self.remote.url + "/changed"}, {"credential": "env:" + other_variable}):
            with self.subTest(overrides=overrides):
                self.write_manifest([dict(original, **overrides)])
                self.invoke(*self.resume_args(result["task_id"], *self.grants()), expected=2)
                self.assertEqual(self.remote.records(), boundary)
                self.assertEqual(len(self.requests()), 1)
        self.write_manifest([original])
        self.setup_responses({"body": fixture.reply()})
        self.assertEqual(self.invoke(*self.resume_args(result["task_id"], *self.grants()))["status"], "completed")

    def test_approved_build_redacts_saved_remote_credential_when_current_manifest_is_changed_or_deleted(self):
        build_fixture = fixture.EXE.parent / ("build-fixture.exe" if os.name == "nt" else "build-fixture")
        self.assertTrue(build_fixture.is_file())
        # The existing build fixture emits these diagnostics without receiving
        # a secret argument, environment variable, input file or model message.
        selected = "job_cpu_query_error" if os.name == "nt" else "no_new_privileges"
        self.secrets.append(selected)
        self.assertNotEqual(selected, fixture.TOKEN)
        (self.workspace / "input.txt").write_text("nonsecret build input", encoding="utf-8")
        proposal = {"manifest": {"version": 2, "inputs": ["input.txt"],
            "mounts": [{"name": "fixture", "source": str(build_fixture.parent), "inputs": [build_fixture.name]}],
            "steps": [{"executable": "${fixture}/" + build_fixture.name,
                       "arguments": ["write", "{}"], "cwd": "."}],
            "outputs": ["out/payload.txt"], "artifact_directory": "artifacts",
            "limits": {"timeout_ms": 10000, "processes": 8, "memory_mb": 256, "cpu_percent": 80}},
            "reason": "Build the explicitly reviewed source"}
        for mutation in ("changed", "deleted"):
            with self.subTest(manifest=mutation):
                self.env[self.variable] = self.token
                original = self.configure()
                self.setup_responses({"body": fixture.reply(tool=("propose_build", proposal))})
                source = self.invoke(*self.run_args(*self.grants(), "--allow-build-proposals"), expected=3)
                task = source["task_id"]
                action = self.detail(source)["actions"][0]
                binding = json.loads(self.sql_rows("SELECT binding FROM task_checkpoints WHERE task_id=?", (task,))[0][0])
                snapshot = json.loads(binding["configuration"]["mcp"]["manifest"])
                self.assertEqual(snapshot["servers"][0]["credential"], self.reference)
                self.assertNotEqual(binding["configuration"]["provider"]["credential"], self.reference)

                # Approval must resolve the source snapshot's reference at the
                # new value, independently of the mutable current manifest.
                self.env[self.variable] = selected
                if mutation == "changed":
                    self.env["CODESTUDIO_TEST_REPLACEMENT_REMOTE"] = "unrelated-remote-credential"
                    self.write_manifest([dict(original, credential="env:CODESTUDIO_TEST_REPLACEMENT_REMOTE",
                                              url=self.remote.url + "/changed")])
                else:
                    self.manifest.unlink()
                before_remote, before_model = self.remote.records(), self.requests()
                arguments = ["build-approve", task, "--workspace", self.workspace,
                             "--action", action["id"], "--digest", action["result"]["output"]["digest"]]
                approved = self.invoke(*arguments)
                self.assertEqual(approved["execution"]["status"], "completed")
                receipt = approved["execution"]["actions"][0]
                captured = receipt["result"]["output"]["steps"][0]["output"]
                self.assertIn("[REDACTED]", captured)
                self.assertNotIn(selected, captured)
                stored = self.invoke("show", approved["execution"]["id"], "--workspace", self.workspace)
                self.assertEqual(stored["actions"][0]["result"], receipt["result"])
                self.assertEqual(self.remote.records(), before_remote)
                self.assertEqual(self.requests(), before_model)
                replay = self.invoke(*arguments)
                self.assertEqual(replay["execution"], approved["execution"])
                self.assertEqual(self.remote.records(), before_remote)
                self.assertEqual(self.requests(), before_model)
                self.assert_secrets_absent()

    def test_scheduler_continuation_uses_rotated_remote_credential_without_replaying_completed_tool(self):
        self.setup_responses({"body": fixture.reply(tool=("mcp_remote_echo", {"text": "once"}))}, {"status": 503})
        failed = self.invoke(*self.run_args(*self.grants(), "--max-turns", "4"), expected=1)
        task = failed["task_id"]
        self.assertEqual(self.detail(failed)["actions"][0]["status"], "succeeded")
        pending_model = self.requests()[-1]["body"]
        queued = self.schedule(task, *self.grants())
        self.assertEqual((queued["status"], queued["attempts"]), ("queued", 0))
        before_remote, before_model = len(self.remote.records()), len(self.requests())
        replacement = "worker-remote-" + uuid.uuid4().hex
        self.env[self.variable] = replacement
        self.secrets.append(replacement)
        self.setup_responses({"body": fixture.reply()})
        self.assertEqual(self.once()["processed"], 1)
        done = self.job(queued["id"])
        self.assertEqual((done["status"], done["attempts"]), ("completed", 1))
        detail = self.detail(failed)
        self.assertEqual((detail["status"], detail["checkpoint"]["turns"]), ("completed", 3))
        self.assertEqual(len(detail["actions"]), 1)
        self.assertEqual(len(self.remote.records("tools/call")), 1)
        self.assertEqual(len(self.requests()), before_model + 1)
        self.assertEqual(self.requests()[-1]["body"], pending_model)
        worker_remote = self.remote.records()[before_remote:]
        self.assertTrue(worker_remote)
        self.assertTrue(all(record["headers"].get("authorization") == "Bearer " + replacement
                            for record in worker_remote))
        final_remote, final_model = self.remote.records(), self.requests()
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.remote.records(), final_remote)
        self.assertEqual(self.requests(), final_model)
        self.assert_secrets_absent()

    def test_invalid_protocol_session_content_schema_and_oversized_discovery_are_rejected(self):
        for mode in ("invalid-protocol", "invalid-session", "invalid-content-type", "invalid-schema", "batch", "oversized"):
            with self.subTest(mode=mode):
                self.configure(mode)
                self.discover(expected=2)
                self.assertFalse(self.database.exists())
                self.assertEqual(self.remote.records("tools/call"), [])
                self.assertEqual(self.requests(), [])

    def test_malformed_or_error_tool_responses_are_unknown_without_model_followup(self):
        for mode in ("malformed-json", "wrong-id", "tool-error", "invalid-result", "rpc-error"):
            with self.subTest(mode=mode):
                self.configure(mode)
                before = len(self.requests())
                result = self.run_call(expected=1)
                self.assert_unknown(result)
                self.assertEqual(len(self.requests()), before + 1)

    def test_schema_and_ungranted_tool_denials_never_dispatch_remote_call(self):
        for arguments, name in (({"text": 42}, "mcp_remote_echo"),
                                ({"text": "ok", "extra": True}, "mcp_remote_echo"),
                                ({"text": "ok"}, "mcp_remote_hidden")):
            with self.subTest(arguments=arguments, name=name):
                result = self.run_call(arguments=arguments, name=name, expected=1)
                action = self.detail(result)["actions"][0]
                self.assertEqual((action["status"], action["effect"]), ("denied", "none"))
                self.assertEqual(self.remote.records("tools/call"), [])

    def test_timeout_sends_cancellation_notification_and_retains_unknown_effect(self):
        self.configure("timeout", timeout_ms=350)
        started = time.monotonic()
        result = self.run_call(expected=1)
        self.assert_unknown(result)
        self.assertLess(time.monotonic() - started, 2.5)
        call = self.remote.records("tools/call")[0]["body"]
        notifications = self.remote.records("notifications/cancelled")
        self.assertEqual(len(notifications), 1)
        self.assertEqual(notifications[0]["body"]["params"]["requestId"], call["id"])
        self.assertEqual(len(self.requests()), 1)

    def test_service_cancel_sends_remote_notification_and_never_replays_tool(self):
        self.configure("timeout", timeout_ms=10000)
        self.setup_responses({"body": fixture.reply(tool=("mcp_remote_echo", {"text": "uncertain"}))})
        client = self.service()
        operation = client.start(self.service_options())
        self.wait_for(lambda: self.remote.records("tools/call"))
        client.rpc("operations/cancel", {"operation_id": operation})
        done = client.finished(operation)
        # A dispatched remote operation cannot prove rollback on cancellation.
        # The kernel's durable unknown-effect stop takes precedence over the
        # ordinary cancelled-task status and must continue blocking resume.
        self.assertEqual(done["exit_code"], 1, done)
        self.assertEqual(done["result"]["status"], "needs_attention", done)
        notifications = self.remote.records("notifications/cancelled")
        self.assertEqual(len(notifications), 1)
        self.assertEqual(notifications[0]["body"]["params"]["requestId"],
                         self.remote.records("tools/call")[0]["body"]["id"])
        self.assertEqual(self.sql_rows("SELECT effect FROM actions"), [("unknown",)])
        self.assertEqual(len(self.requests()), 1)
        before = self.remote.records()
        self.invoke(*self.resume_args(done["result"]["task_id"], *self.grants()), expected=2)
        self.assertEqual(self.remote.records(), before)

    def test_cancel_during_initialize_does_not_send_forbidden_cancellation(self):
        self.configure("hang-initialize", timeout_ms=10000)
        client = self.service()
        operation = client.start(self.service_options())
        self.wait_for(lambda: self.remote.records("initialize"))
        client.rpc("operations/cancel", {"operation_id": operation})
        done = client.finished(operation)
        # Discovery fails before a task exists, so service reports its explicit
        # initialization failure contract rather than a task cancellation.
        self.assertEqual(done["exit_code"], 2, done)
        self.assertEqual(done["error"]["data"]["kind"], "operation_failed")
        self.assertEqual(done["error"]["data"]["outcome"], "unknown")
        self.assertNotIn("result", done)
        self.assertEqual(len(self.remote.records("initialize")), 1)
        self.assertEqual(self.remote.records("notifications/cancelled"), [])
        self.assertEqual(self.remote.records("tools/call"), [])
        self.assertEqual(self.requests(), [])
        self.assert_no_task_effects()


if __name__ == "__main__":
    unittest.main()
