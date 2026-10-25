"""Validate real service frames and recovery rules for an independent client.

Usage: python tests/client_contract_integration.py <agent-executable>
Add --service-suite to run the original service scenarios with frame validation
instead of the focused contract suite (no duplicate run by default).
Requires the pinned development dependency in tests/requirements.txt. Provider
traffic stays in the local fixture; no cloud account or inference is used.
"""
import json
import os
import sqlite3
import subprocess
import sys
import unittest
from contextlib import closing

from contract_schema import Contract
from jsonschema import ValidationError

# The shared fixture consumes argv[1] on import. Give it only its executable,
# then restore unittest arguments without importing its TestCase into discovery.
if len(sys.argv) < 2:
    raise SystemExit("Usage: client_contract_integration.py <agent-executable>")
_arguments = sys.argv[:]
_service_suite = "--service-suite" in _arguments[2:]
if _service_suite:
    _arguments.remove("--service-suite")
sys.argv[:] = _arguments[:2]
try:
    import service_integration as fixture
finally:
    sys.argv[:] = [_arguments[0], *_arguments[2:]]


class ContractClient(fixture.Client):
    def __init__(self, *args, **kwargs):
        self.request_methods = {}
        self.session_id = None
        super().__init__(*args, **kwargs)

    def send(self, method, params=None):
        self.request_methods[self.next_id] = method
        return super().send(method, params)

    def initialize(self):
        result = super().initialize()
        self.session_id = result["session_id"]
        return result

    def lookup(self, operation_id, session_id=None, error=False):
        return self.rpc("operations/get", {
            "session_id": self.session_id if session_id is None else session_id,
            "operation_id": operation_id,
        }, error=error)


def validate_client_frames(case, contract):
    for client in case.clients:
        operation_methods = {}
        for frame in client.frames:
            with case.subTest(frame_method=frame.get("method"), request_id=frame.get("id")):
                try:
                    method = client.request_methods.get(frame.get("id"))
                    result = frame.get("result")
                    if isinstance(result, dict) and result.get("status") == "accepted":
                        operation_methods[result["operation_id"]] = method
                    if frame.get("method") == "operation/finished":
                        operation = frame["params"]["operation_id"]
                        case.assertIn(operation, operation_methods, "Completion preceded its acceptance")
                        method = operation_methods[operation]
                    contract.validate_frame(frame, method)
                except ValidationError as error:
                    # Large model results must not explode failure logs. The
                    # JSON path and bounded schema reason identify the fault.
                    path = "/".join(map(str, error.absolute_path)) or "/"
                    case.fail(f"Contract mismatch at {path}: {error.message[:400]}")


class ClientContractTests(unittest.TestCase):
    # Reuse fixture lifecycle and HTTP helpers without rerunning its test suite.
    setUp = fixture.ServiceTests.setUp
    responses = fixture.ServiceTests.responses
    requests = fixture.ServiceTests.requests
    options = fixture.ServiceTests.options
    gate = fixture.ServiceTests.gate
    wait_for = fixture.ServiceTests.wait_for
    sql = fixture.ServiceTests.sql

    @classmethod
    def setUpClass(cls):
        cls.contract = Contract()

    def tearDown(self):
        fixture.ServiceTests.tearDown(self)
        validate_client_frames(self, self.contract)

    def client(self, initialize=True):
        client = ContractClient(self.workspace, self.env)
        self.clients.append(client)
        if initialize:
            client.initialize()
        return client

    def test_supervised_start_contract_and_independent_client_lifecycle(self):
        client = self.client(initialize=False)
        self.assertTrue(client.initialize()["capabilities"]["worker_supervision"])
        operation = self.accepted(client)
        self.assertEqual(client.finished(operation)["exit_code"], 0)
        request = client.send("scheduler/start", {"supervise": True, "max_restarts": 1,
                                                  "restart_delay_ms": 100, "poll_ms": 20})
        reply = client.receive(lambda frame: frame.get("id") == request)
        if "error" in reply:
            self.assertEqual(os.name, "nt")
            self.assertIn("Cannot detach scheduler from the parent Job", reply["error"]["message"])
            self.assertEqual(client.rpc("scheduler/status")["state"], "stopped")
            return
        result = reply["result"]
        try:
            self.assertEqual((result["started"], result["supervised"], result["max_restarts"],
                              result["restart_delay_ms"]), (True, True, 1, 100))
            self.assertNotEqual(result["pid"], result["supervisor_pid"])
            client.close()
            replacement = self.client()
            state = replacement.rpc("scheduler/status")
            self.assertEqual((state["owner"], state["pid"], state["state"]),
                             (result["owner"], result["pid"], "running"))
        finally:
            control = self.client()
            control.rpc("scheduler/stop")
            # An independent worker can acquire both locks only after supervisor
            # and child are gone; a persisted stopped flag alone is insufficient.
            def released():
                probe = subprocess.run([str(fixture.EXE), "scheduler-run", "--once", "--workspace",
                                        str(self.workspace)], env=self.env, capture_output=True, timeout=5)
                return probe.returncode == 0
            self.wait_for(released)

    def test_supervisor_diagnostics_survive_client_reconnection_and_page_durable_runs(self):
        client = self.client(initialize=False)
        self.assertTrue(client.initialize()["capabilities"]["supervisor_diagnostics"])
        self.assertIsNone(client.rpc("scheduler/supervisor/status")["record"])
        self.assertEqual(client.rpc("scheduler/supervisor/events"),
                         {"items": [], "next_after": 0, "has_more": False})
        self.assertFalse(self.database.exists())
        runs = []
        for _ in range(2):
            process = subprocess.Popen([str(fixture.EXE), "scheduler-supervise", "--workspace",
                str(self.workspace), "--poll-ms", "20", "--max-restarts", "2", "--restart-delay-ms", "200"],
                env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.raw_children.append(process)
            try:
                active = self.wait_for(lambda: (state if (state := client.rpc("scheduler/supervisor/status"))["record"]
                    and state["record"]["pid"] == process.pid and state["record"]["phase"] == "running" else None))
                record = active["record"]
                self.assertEqual(active["observation"]["state"], "matching_process")
                self.assertTrue(active["worker_matches"])
                self.assertEqual((record["max_restarts"], record["restart_delay_ms"], record["restarts"]), (2, 200, 0))
                self.assertTrue(record["process_identity"])
                self.assertIsNone(record["last_exit_code"])
                client.close()
                client = self.client()
                combined = client.rpc("scheduler/status")
                self.assertEqual(combined["supervisor"]["record"]["id"], record["id"])
                self.assertEqual(combined["supervisor"]["record"]["worker_pid"], combined["pid"])
                self.assertTrue(combined["supervisor"]["worker_matches"])
                self.assertEqual(combined["supervisor"]["observation"]["state"], "matching_process")
            finally:
                client.rpc("scheduler/stop")
                output, error = process.communicate(timeout=8)
                self.assertEqual(process.returncode, 0, (output, error))
                self.assertNotIn(fixture.TOKEN.encode(), output + error)
            stopped = client.rpc("scheduler/supervisor/status")
            self.assertEqual((stopped["record"]["phase"], stopped["record"]["exit_reason"],
                              stopped["observation"]["state"]), ("stopped", "stop_requested", "not_running"))
            self.assertGreater(stopped["record"]["finished_at_ms"], 0)
            runs.append(stopped["record"])
        self.assertNotEqual(runs[0]["id"], runs[1]["id"])
        with closing(sqlite3.connect(self.database)) as database:
            before = "\n".join(database.iterdump())
        client.close()
        client = self.client()
        self.assertEqual(client.rpc("scheduler/supervisor/status")["record"], runs[-1])
        after, rows = 0, []
        for _ in range(50):
            page = client.rpc("scheduler/supervisor/events", {"after": after, "limit": 2})
            self.assertLessEqual(len(page["items"]), 2)
            self.assertTrue(all(row["seq"] > after for row in page["items"]))
            if page["items"]:
                self.assertEqual(page["next_after"], page["items"][-1]["seq"])
            rows.extend(page["items"])
            after = page["next_after"]
            if not page["has_more"]:
                break
        else:
            self.fail("Supervisor event pages did not terminate")
        self.assertEqual(rows, client.rpc("scheduler/supervisor/events")["items"])
        self.assertEqual([row["seq"] for row in rows], sorted({row["seq"] for row in rows}))
        self.assertEqual({row["run_id"] for row in rows}, {run["id"] for run in runs})
        self.assertEqual(rows[-1]["payload"], runs[-1])
        self.assertEqual(client.rpc("scheduler/supervisor/events", {"after": after}),
                         {"items": [], "next_after": after, "has_more": False})
        with closing(sqlite3.connect(self.database)) as database:
            self.assertEqual("\n".join(database.iterdump()), before)
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 7)
        self.assertEqual(self.requests(), [])

    @unittest.skipUnless(os.name == "nt" or sys.platform.startswith("linux"),
                         "Windows or Linux login autostart")
    def test_supervised_autostart_registration_survives_client_and_rejects_conflicting_options(self):
        client = self.client(initialize=False)
        initialized = client.initialize()
        self.assertTrue(initialized["capabilities"]["supervised_autostart"])
        before = client.rpc("scheduler/autostart/status")
        if not before["available"]:
            self.skipTest("Autostart unavailable: " + before["reason"])
        self.assertFalse(before["registered"])
        self.assertFalse(before["supervised"])
        self.assertNotIn("max_restarts", before)
        self.assertNotIn("restart_delay_ms", before)
        params = {"supervise": True, "poll_ms": 25, "max_restarts": 7, "restart_delay_ms": 1500}
        try:
            created = client.rpc("scheduler/autostart/enable", params)
            self.assertEqual((created["registered"], created["enabled"], created["created"],
                              created["started"], created["supervised"]), (True, True, True, False, True))
            self.assertEqual(created["task_name"], before["task_name"])
            # Registration belongs to the workspace, independently of the RPC
            # connection. Reconnect before querying or controlling it.
            client.close()
            client = self.client()
            observed = client.rpc("scheduler/autostart/status")
            self.assertEqual((observed["registered"], observed["enabled"], observed["conflict"],
                              observed["supervised"], observed["poll_ms"], observed["max_restarts"],
                              observed["restart_delay_ms"], observed["started"]),
                             (True, True, False, True, 25, 7, 1500, False))
            self.assertFalse(client.rpc("scheduler/autostart/enable", params)["created"])
            for changed in ({"poll_ms": 25}, {**params, "poll_ms": 100},
                            {**params, "max_restarts": 8}, {**params, "restart_delay_ms": 2000}):
                with self.subTest(conflicting_options=changed):
                    error = client.rpc("scheduler/autostart/enable", changed, error=True)
                    self.assert_error(error, "command_failed", "query_state", "unknown", client.session_id)
                    self.assertIn("conflicts", error["message"])
                    self.assertEqual(client.rpc("scheduler/autostart/status"), observed)
            for action in ("status", "disable"):
                with self.subTest(action=action):
                    error = client.rpc("scheduler/autostart/" + action, {"supervise": True}, error=True)
                    self.assert_error(error, "invalid_params", "fix_request", "not_started",
                                      client.session_id, -32602)
                    self.assertEqual(client.rpc("scheduler/autostart/status"), observed)
            removed = client.rpc("scheduler/autostart/disable")
            self.assertEqual((removed["removed"], removed["registered"], removed["supervised"],
                              removed["max_restarts"], removed["restart_delay_ms"]),
                             (True, False, True, 7, 1500))
            self.assertEqual(client.rpc("scheduler/autostart/status"), before)
            self.assertFalse(client.rpc("scheduler/autostart/disable")["removed"])
            ordinary = client.rpc("scheduler/autostart/enable")
            self.assertEqual(ordinary["task_name"], before["task_name"])
            self.assertFalse(ordinary["supervised"])
            self.assertNotIn("max_restarts", ordinary)
            self.assertNotIn("restart_delay_ms", ordinary)
            plain = client.rpc("scheduler/autostart/status")
            error = client.rpc("scheduler/autostart/enable", params, error=True)
            self.assert_error(error, "command_failed", "query_state", "unknown", client.session_id)
            self.assertEqual(client.rpc("scheduler/autostart/status"), plain)
            self.assertFalse(client.rpc("operations/status")["active"])
            self.assertEqual(client.rpc("scheduler/status")["state"], "stopped")
            self.assertEqual(self.requests(), [])
            self.assertFalse(self.database.exists(), "Autostart RPC launched work immediately")
            self.assertFalse((self.workspace / ".codestudio").exists())
        finally:
            # Only the platform's complete ownership check may remove this
            # test's registration. Never delete an arbitrary native task or PID.
            cleanup = self.client()
            removed = cleanup.rpc("scheduler/autostart/disable")
            self.assertFalse(removed["registered"])
            self.assertEqual(cleanup.rpc("scheduler/autostart/status"), before)

    def assert_error(self, error, kind, recovery, outcome, session_id=None, code=None):
        if code is not None:
            self.assertEqual(error["code"], code)
        self.assertEqual(error["data"]["kind"], kind)
        self.assertEqual(error["data"]["recovery"], recovery)
        self.assertEqual(error["data"]["outcome"], outcome)
        self.assertIsInstance(error["data"]["session_id"], str)
        self.assertTrue(error["data"]["session_id"])
        if session_id is not None:
            self.assertEqual(error["data"]["session_id"], session_id)
        self.assertNotIn(fixture.TOKEN, json.dumps(error))

    def accepted(self, client, method="tasks/start", params=None):
        if params is None:
            params = {"mode": "demo", "options": {}}
        result = client.rpc(method, params)
        self.assertEqual(result["session_id"], client.session_id)
        self.assertEqual(result["status"], "accepted")
        self.assertTrue(result["operation_id"])
        return result["operation_id"]

    def assert_retained(self, client, operation, done, method="tasks/start"):
        snapshot = client.lookup(operation)
        self.assertEqual(snapshot["session_id"], client.session_id)
        self.assertEqual(snapshot["operation_id"], operation)
        self.assertEqual(snapshot["method"], method)
        self.assertEqual(snapshot["state"], "finished")
        self.assertEqual(snapshot["completion"], done)
        return snapshot

    def test_session_identity_capabilities_and_read_only_initialization(self):
        client = self.client(initialize=False)
        before = client.rpc("operations/status", error=True)
        self.assert_error(before, "not_initialized", "initialize", "not_started", code=-32002)
        initialized = client.initialize()
        self.assertEqual(initialized["session_id"], before["data"]["session_id"])
        self.assertTrue(initialized["capabilities"]["operation_lookup"])
        self.assertTrue(initialized["capabilities"]["structured_errors"])
        self.assertEqual(initialized["capabilities"]["supervised_autostart"],
                         os.name == "nt" or sys.platform.startswith("linux"))
        self.assertEqual(initialized["capabilities"]["supervisor_diagnostics"],
                         os.name == "nt" or sys.platform.startswith("linux"))
        self.assertEqual(initialized["limits"]["operation_history"], 64)
        self.assertEqual(initialized["limits"]["operation_result_bytes"], 262144)
        status = client.rpc("operations/status")
        self.assertEqual(status["session_id"], client.session_id)
        self.assertFalse(status["active"])
        again = client.rpc("initialize", {"protocol_version": 2}, error=True)
        self.assert_error(again, "already_initialized", "none", "not_started", client.session_id)
        other = self.client()
        self.assertNotEqual(other.session_id, client.session_id)
        self.assertEqual(client.rpc("tasks/list")["items"], [])
        self.assertFalse(self.database.exists())
        self.assertEqual(self.requests(), [])

    def test_structured_input_errors_do_not_start_work(self):
        client = self.client()
        for wire, kind, code in (
            ("{broken\n", "parse_error", -32700),
            ('{"jsonrpc":"2.0","id":true,"method":"tasks/start"}\n', "invalid_request", -32600),
            ('{"jsonrpc":"2.0","id":77,"method":"tasks/start","params":[]}\n', "invalid_params", -32602),
        ):
            with self.subTest(kind=kind):
                client.send_raw(wire)
                error = client.receive(lambda frame: "error" in frame)["error"]
                self.assert_error(error, kind, "fix_request", "not_started", client.session_id, code)
        unknown = client.rpc("not/a-method", error=True)
        self.assert_error(unknown, "method_not_found", "fix_request", "not_started", client.session_id, -32601)
        rejected = client.rpc("tasks/start", {"mode": "demo", "options": {fixture.TOKEN: True}}, error=True)
        self.assert_error(rejected, "invalid_params", "fix_request", "not_started", client.session_id, -32602)
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / "CODESTUDIO_DEMO.txt").exists())
        self.assertEqual(self.requests(), [])

    def test_lookup_requires_both_identities_and_refuses_unknown_without_side_effects(self):
        client = self.client()
        valid = {"session_id": client.session_id, "operation_id": "not-retained"}
        for params in ({}, {"operation_id": "missing"}, {"session_id": client.session_id},
                       {**valid, "session_id": ""}, {**valid, "operation_id": None},
                       {**valid, "session_id": 1}, {**valid, "operation_id": []},
                       {**valid, "session_id": "界" * 43},
                       {**valid, "operation_id": "x" * 129}, {**valid, "retry": True}):
            with self.subTest(params=params):
                rejection = client.rpc("operations/get", params, error=True)
                self.assert_error(rejection, "invalid_params", "fix_request", "not_started", client.session_id, -32602)
        self.assert_error(client.lookup("not-retained", error=True),
                          "operation_not_found", "query_state", "unknown", client.session_id)
        self.assert_error(client.lookup("not-retained", session_id="another-session", error=True),
                          "session_mismatch", "query_state", "unknown", client.session_id)
        self.assertFalse(self.database.exists())
        self.assertEqual(self.requests(), [])

    def test_error_message_redacts_before_utf8_safe_byte_truncation(self):
        client = self.client()
        # The credential crosses byte 8192 in the original diagnostic. Removing
        # it after truncation would leak its prefix. The trailing UTF-8 text
        # also requires a character boundary when the replacement is bounded.
        key = "a" + "界" * 2720 + fixture.TOKEN + "界" * 300
        error = client.rpc("operations/get", {
            "session_id": client.session_id, "operation_id": "absent", key: True,
        }, error=True)
        self.assert_error(error, "invalid_params", "fix_request", "not_started", client.session_id, -32602)
        self.assertTrue(error["data"]["message_truncated"])
        encoded = error["message"].encode("utf-8", errors="strict")
        self.assertLessEqual(len(encoded), 8192)
        self.assertGreater(len(encoded), 8100)
        self.assertEqual(encoded.decode("utf-8", errors="strict"), error["message"])
        self.assertIn("[REDACTED]", error["message"])
        self.assertNotIn("service-test", error["message"])
        self.assertFalse(self.database.exists())
        self.assertEqual(self.requests(), [])

    def test_running_lookup_and_busy_rejection_are_observations_only(self):
        release = self.gate()
        self.responses({"body": fixture.final(), "before_headers": release})
        client = self.client()
        operation = self.accepted(client, params={"mode": "run", "options": self.options()})
        self.wait_for(lambda: len(self.requests()) == 1)
        before = self.sql("SELECT count(*) FROM events")
        for _ in range(3):
            snapshot = client.lookup(operation)
            self.assertEqual(snapshot, {
                "session_id": client.session_id, "operation_id": operation,
                "method": "tasks/start", "state": "running",
                "cancel_requested": False, "completion": None,
            })
        self.assertEqual(self.sql("SELECT count(*) FROM events"), before)
        for method, params in (("tasks/start", {"mode": "demo", "options": {}}), ("tasks/recover", {})):
            self.assert_error(client.rpc(method, params, error=True),
                              "busy", "wait_for_idle", "not_started", client.session_id)
        self.assertEqual(len(self.requests()), 1)
        self.assertFalse((self.workspace / "CODESTUDIO_DEMO.txt").exists())
        release.set()
        done = client.finished(operation)
        self.assertEqual(done["exit_code"], 0)
        self.assert_retained(client, operation, done)

    def test_completed_lookup_matches_redacted_notification_and_never_replays_tools(self):
        self.responses(
            {"body": fixture.call("write_file", {"path": "once.txt", "content": "original"})},
            {"body": fixture.final("Verified " + fixture.TOKEN)},
        )
        client = self.client()
        operation = self.accepted(client, params={"mode": "run", "options": self.options(allow_write=True)})
        done = client.finished(operation)
        self.assertEqual(done["result"]["summary"], "Verified [REDACTED]")
        task_id = done["result"]["task_id"]
        target = self.workspace / "once.txt"
        target.write_text("user changed after the receipt", encoding="utf-8")
        before = self.sql("SELECT count(*) FROM events")
        for _ in range(3):
            self.assert_retained(client, operation, done)
            detail = client.rpc("tasks/get", {"task_id": task_id})
            self.assertEqual(detail["action_count"], 1)
        self.assertEqual(self.sql("SELECT count(*) FROM events"), before)
        self.assertEqual(target.read_text(encoding="utf-8"), "user changed after the receipt")
        self.assertEqual(len(self.requests()), 2)

    def test_async_failure_and_synchronous_failure_have_stable_redacted_errors(self):
        client = self.client()
        missing = client.rpc("tasks/get", {"task_id": fixture.TOKEN}, error=True)
        self.assert_error(missing, "command_failed", "query_state", "unknown", client.session_id)
        operation = self.accepted(client, "tasks/resume", {"task_id": fixture.TOKEN, "options": {}})
        done = client.finished(operation)
        self.assertEqual(done["exit_code"], 2)
        self.assert_error(done["error"], "operation_failed", "query_state", "unknown", client.session_id, -32000)
        self.assert_retained(client, operation, done, "tasks/resume")
        self.assertEqual(self.requests(), [])

    def test_completion_history_keeps_newest_64_and_eviction_is_not_no_effect(self):
        client = self.client()
        operations = []
        # Invalid cron syntax fails before any state is written, but still
        # completes a genuine asynchronous command and consumes a receipt slot.
        for index in range(65):
            operation = self.accepted(client, "crons/create", {
                "expression": "invalid", "timezone": "UTC", "options": self.options(),
            })
            done = client.finished(operation)
            self.assertEqual(done["exit_code"], 2, index)
            operations.append((operation, done))
        self.assertEqual(len({operation for operation, _ in operations}), 65)
        self.assert_error(client.lookup(operations[0][0], error=True),
                          "operation_not_found", "query_state", "unknown", client.session_id)
        for operation, done in operations[1:]:
            self.assert_retained(client, operation, done, "crons/create")
        self.assertFalse(self.database.exists())
        self.assertEqual(self.requests(), [])

    def test_large_result_is_explicitly_omitted_with_a_durable_reference(self):
        # UTF-8 bytes, rather than character count, determine the cache bound.
        summary = "界" * 90000 + fixture.TOKEN
        self.responses({"body": fixture.final(summary)})
        client = self.client()
        operation = self.accepted(client, params={"mode": "run", "options": self.options()})
        done = client.finished(operation)
        self.assertEqual(done["exit_code"], 0)
        snapshot = client.lookup(operation)
        retained = snapshot["completion"]
        self.assertEqual(snapshot["state"], "finished")
        self.assertEqual(retained["operation_id"], done["operation_id"])
        self.assertEqual(retained["session_id"], done["session_id"])
        self.assertEqual(retained["exit_code"], done["exit_code"])
        self.assertIsNone(retained["result"])
        self.assertTrue(retained["result_omitted"])
        result_bytes = len(json.dumps(done["result"], ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
        self.assertEqual(retained["result_bytes"], result_bytes)
        self.assertGreater(result_bytes, 262144)
        task_id = done["result"]["task_id"]
        self.assertEqual(retained["result_refs"], {"task_id": task_id})
        self.assertLess(len(json.dumps(snapshot).encode()), 4096)
        detail = client.rpc("tasks/get", {"task_id": task_id})
        self.assertEqual(detail["status"], "completed")
        self.assertTrue(detail["summary_omitted"])
        self.assertEqual(len(self.requests()), 1)

    def test_restarting_loses_receipt_not_durable_effects(self):
        client = self.client()
        operation = self.accepted(client)
        done = client.finished(operation)
        task_id = done["result"]["task_id"]
        original_session = client.session_id
        client.close()
        target = self.workspace / "CODESTUDIO_DEMO.txt"
        target.write_text("preserve after restart", encoding="utf-8")
        replacement = self.client()
        self.assertNotEqual(original_session, replacement.session_id)
        self.assert_error(replacement.lookup(operation, session_id=original_session, error=True),
                          "session_mismatch", "query_state", "unknown", replacement.session_id)
        self.assert_error(replacement.lookup(operation, error=True),
                          "operation_not_found", "query_state", "unknown", replacement.session_id)
        detail = replacement.rpc("tasks/get", {"task_id": task_id})
        self.assertEqual((detail["status"], detail["action_count"]), ("completed", 2))
        self.assertEqual(target.read_text(encoding="utf-8"), "preserve after restart")
        self.assertEqual(replacement.rpc("tasks/list")["items"][0]["id"], task_id)
        self.assertEqual(self.requests(), [])

    def test_cancellation_race_requires_final_receipt_and_preserves_durable_resume(self):
        release = self.gate()
        self.responses({"body": fixture.final(), "before_headers": release})
        client = self.client()
        operation = self.accepted(client, params={"mode": "run", "options": self.options()})
        self.wait_for(lambda: len(self.requests()) == 1)
        self.assert_error(client.rpc("operations/cancel", {"operation_id": "wrong-operation"}, error=True),
                          "operation_not_found", "query_operation", "not_started", client.session_id)
        self.assertEqual(client.lookup(operation)["state"], "running")
        cancelled = client.rpc("operations/cancel", {"operation_id": operation})
        self.assertTrue(cancelled["cancel_requested"])
        observed = client.lookup(operation)
        self.assertTrue(observed["cancel_requested"])
        self.assertIn(observed["state"], ("running", "finished"))
        done = client.finished(operation)
        self.assertEqual(done["exit_code"], 130)
        self.assert_retained(client, operation, done)
        self.assert_error(client.rpc("operations/cancel", {"operation_id": operation}, error=True),
                          "operation_not_found", "query_operation", "not_started", client.session_id)
        task_id = done["result"]["task_id"]
        self.assertEqual(client.rpc("tasks/get", {"task_id": task_id})["status"], "cancelled")
        release.set()
        self.responses({"body": fixture.final("Resumed after explicit authorization")})
        resumed = self.accepted(client, "tasks/resume", {
            "task_id": task_id, "options": {"allow_http_loopback": True},
        })
        continued = client.finished(resumed)
        self.assertEqual(continued["exit_code"], 0)
        self.assertEqual(continued["result"]["task_id"], task_id)
        self.assert_retained(client, resumed, continued, "tasks/resume")
        self.assert_retained(client, operation, done)
        self.assertEqual(client.rpc("tasks/get", {"task_id": task_id})["status"], "completed")
        self.assertEqual(len(self.requests()), 2)

    def test_review_rejection_and_continuation_preserve_one_decision_and_one_feedback(self):
        # This validates the complete approval protocol on either platform
        # without approving or launching a build. The mounted executable only
        # identifies a genuine frozen plan for the user to review and reject.
        self.workspace = self.workspace.resolve()
        self.database = self.workspace / ".codestudio" / "state.sqlite3"
        build_fixture = fixture.EXE.parent / ("build-fixture.exe" if fixture.os.name == "nt" else "build-fixture")
        self.assertTrue(build_fixture.is_file(), f"Build fixture missing: {build_fixture}")
        source = self.workspace / "input.txt"
        source.write_text("original workspace source\n", encoding="utf-8")
        manifest = {
            "version": 2, "inputs": ["input.txt"],
            "mounts": [{"name": "fixture", "source": str(build_fixture.parent), "inputs": [build_fixture.name]}],
            "steps": [{"executable": "${fixture}/" + build_fixture.name, "arguments": ["write", json.dumps({
                "outside_file": str(self.workspace.parent / "outside-secret.txt"),
                "outside_new": str(self.workspace.parent / "must-not-create.txt"),
                "loopback_port": self.server.server_port,
            })], "cwd": "."}],
            "outputs": ["out"], "artifact_directory": "artifacts",
            "limits": {"timeout_ms": 10000, "processes": 8, "memory_mb": 256, "cpu_percent": 80},
        }
        self.responses({"body": fixture.call("propose_build", {
            "manifest": manifest, "reason": "Review this build plan before execution",
        }, call_id="contract-proposal")})
        client = self.client()
        proposed_operation = self.accepted(client, params={
            "mode": "run", "options": self.options(allow_build_proposals=True),
        })
        proposed = client.finished(proposed_operation)
        self.assertEqual((proposed["exit_code"], proposed["result"]["status"]), (3, "waiting_approval"))
        self.assert_retained(client, proposed_operation, proposed)
        task_id = proposed["result"]["task_id"]
        approval = proposed["result"]["approval"]
        identity = {"task_id": task_id, "action_id": approval["action_id"]}
        before = self.sql("SELECT count(*) FROM events")
        review = client.rpc("builds/review", identity)
        self.assertEqual(review["task_id"], task_id)
        self.assertEqual(review["action_id"], approval["action_id"])
        self.assertEqual(review["proposal"]["digest"], approval["digest"])
        self.assertEqual(review["proposal"]["status"], "pending_approval")
        self.assertIsNone(review["decision"])
        self.assertEqual(self.sql("SELECT count(*) FROM events"), before)
        self.assertEqual(len(self.requests()), 1)
        self.assertFalse((self.workspace / "artifacts").exists())

        self.responses({"body": fixture.final("Build rejected; continued without execution")})
        params = {**identity, "digest": approval["digest"], "continue_options": {
            "allow_http_loopback": True, "allow_build_proposals": True,
        }}
        rejection = self.accepted(client, "builds/reject", params)
        rejected = client.finished(rejection)
        self.assertEqual(rejected["exit_code"], 0)
        decision = rejected["result"]
        self.assertEqual(decision["proposal"]["decision"]["decision"], "rejected")
        self.assertIsNone(decision["execution"])
        self.assertEqual(decision["continuation"]["status"], "started")
        self.assertEqual(decision["continuation"]["exit_code"], 0)
        self.assertEqual(decision["continuation"]["result"]["task_id"], task_id)
        self.assertEqual(decision["continuation"]["result"]["status"], "completed")
        self.assert_retained(client, rejection, rejected, "builds/reject")
        self.assertEqual(client.rpc("builds/review", identity), decision["proposal"])
        feedback = [json.loads(item["content"]) for item in self.requests()[-1]["body"]["messages"]
                    if item.get("role") == "tool" and item.get("tool_call_id") == "contract-proposal"]
        self.assertEqual(len(feedback), 1)
        self.assertEqual(feedback[0]["output"]["decision"]["decision"], "rejected")
        self.assertIsNone(feedback[0]["output"]["execution"])

        # An explicit repeated decision is acknowledged without advancing the
        # completed task or injecting the same feedback again. Client lookup
        # itself never repeats this command.
        repeated_operation = self.accepted(client, "builds/reject", params)
        repeated = client.finished(repeated_operation)
        self.assertEqual(repeated["exit_code"], 0)
        self.assertEqual(repeated["result"]["continuation"]["status"], "skipped")
        self.assert_retained(client, repeated_operation, repeated, "builds/reject")
        self.assert_retained(client, rejection, rejected, "builds/reject")
        detail = client.rpc("tasks/get", {"task_id": task_id})
        self.assertEqual((detail["status"], detail["action_count"]), ("completed", 1))
        events = client.rpc("events/list", {"after": 0, "limit": 100})["items"]
        self.assertEqual(sum(event["kind"] == "build.proposal.consumed" for event in events), 1)
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(1,)])
        self.assertEqual(self.sql("SELECT count(*) FROM build_proposal_decisions"), [(1,)])
        self.assertEqual(len(self.requests()), 2)
        self.assertFalse((self.workspace / "out").exists())
        self.assertFalse((self.workspace / "artifacts").exists())
        self.assertEqual(source.read_text(encoding="utf-8"), "original workspace source\n")

    def test_event_subscription_failure_has_same_structured_error_contract(self):
        client = self.client()
        operation = self.accepted(client)
        client.finished(operation)
        # Corrupt only this test's disposable DB to exercise the live reader
        # failure boundary, without relying on transient timing or permissions.
        with closing(sqlite3.connect(self.database)) as database:
            database.execute("ALTER TABLE events RENAME TO unavailable_events")
            database.commit()
        client.rpc("events/subscribe", {"after": 0})
        params = client.receive(lambda frame: frame.get("method") == "events/error")["params"]
        self.assertEqual(params["after"], 0)
        self.assertEqual(params["message"], params["error"]["message"])
        self.assert_error(params["error"], "command_failed", "query_state", "unknown", client.session_id)
        self.assertFalse(client.rpc("operations/status")["active"])


if __name__ == "__main__":
    if _service_suite:
        # Opt-in reuse validates real scheduler, cron, approval and task shapes
        # already covered by the larger fixture suite, without copying tests.
        fixture.Client = ContractClient
        _original_teardown = fixture.ServiceTests.tearDown
        _contract = Contract()

        def _validated_teardown(case):
            _original_teardown(case)
            validate_client_frames(case, _contract)

        fixture.ServiceTests.tearDown = _validated_teardown
        unittest.main(module=fixture, verbosity=2)
    else:
        unittest.main(verbosity=2)
