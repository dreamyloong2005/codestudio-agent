"""Real stdio/SQLite crash and retry tests for durable task requests.

Usage: python tests/durable_requests_integration.py <agent-executable>
       [--process-fixture <path>] [--mcp-fixture <path>] [unittest options]
Explicit fixture paths allow testing an unmodified extracted/installed package.
Only disposable workspaces, synthetic loopback HTTP and owned fixture processes
are used. There is no real model, OAuth account or permanent registration.
"""
import argparse
import json
import os
import pathlib
import sqlite3
import subprocess
import sys
import unicodedata
import unittest
from contextlib import closing

from contract_schema import Contract
from jsonschema import ValidationError

_parser = argparse.ArgumentParser(description=__doc__, add_help=False)
_parser.add_argument("agent", type=pathlib.Path)
_parser.add_argument("--process-fixture", type=pathlib.Path)
_parser.add_argument("--mcp-fixture", type=pathlib.Path)
_arguments, _unittest_arguments = _parser.parse_known_args()
_agent = _arguments.agent.resolve()
_suffix = ".exe" if os.name == "nt" else ""
PROCESS_FIXTURE = (_arguments.process_fixture or _agent.parent / ("process-fixture" + _suffix)).resolve()
MCP_FIXTURE = (_arguments.mcp_fixture or _agent.parent / ("mcp-fixture" + _suffix)).resolve()
for _label, _path in (("Agent", _agent), ("process fixture", PROCESS_FIXTURE), ("MCP fixture", MCP_FIXTURE)):
    if not _path.is_file() or not os.access(_path, os.X_OK):
        _parser.error(f"{_label} must name an existing executable: {_path}")
_program = sys.argv[0]
sys.argv[:] = [_program, str(_agent)]
try:
    import service_integration as fixture
finally:
    sys.argv[:] = [_program, *_unittest_arguments]
fixture.PROCESS = PROCESS_FIXTURE


class DurableClient(fixture.Client):
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


class DurableRequestTests(unittest.TestCase):
    setUp = fixture.ServiceTests.setUp
    responses = fixture.ServiceTests.responses
    requests = fixture.ServiceTests.requests
    options = fixture.ServiceTests.options
    gate = fixture.ServiceTests.gate
    wait_for = fixture.ServiceTests.wait_for
    sql = fixture.ServiceTests.sql
    raw_client = fixture.ServiceTests.raw_client
    raw_send = staticmethod(fixture.ServiceTests.raw_send)
    raw_receive = fixture.ServiceTests.raw_receive

    @classmethod
    def setUpClass(cls):
        cls.contract = Contract()

    def tearDown(self):
        fixture.ServiceTests.tearDown(self)
        for client in self.clients:
            operations = {}
            for frame in client.frames:
                method = client.request_methods.get(frame.get("id"))
                result = frame.get("result")
                if isinstance(result, dict) and result.get("status") == "accepted":
                    operations[result["operation_id"]] = method
                if frame.get("method") == "operation/finished":
                    operation = frame["params"]["operation_id"]
                    self.assertIn(operation, operations, "Completion preceded durable acceptance")
                    method = operations[operation]
                try:
                    self.contract.validate_frame(frame, method)
                except ValidationError as error:
                    path = "/".join(map(str, error.absolute_path)) or "/"
                    self.fail(f"Durable contract mismatch at {path}: {error.message[:400]}")

    def client(self, initialize=True, state=None, workspace=None):
        client = DurableClient(self.workspace if workspace is None else workspace, self.env, state)
        self.clients.append(client)
        if initialize:
            client.initialize()
        return client

    def bootstrap(self, client):
        result = client.rpc("state/initialize")
        self.assertRegex(result["database_id"], r"^[0-9a-f]{32}$")
        self.assertEqual(pathlib.Path(result["workspace"]), self.workspace.resolve())
        return result["database_id"]

    @staticmethod
    def keyed(database_id, key, **params):
        return {"database_id": database_id, "request_key": key, **params}

    def receipt(self, client, database_id, key, error=False):
        return client.rpc("requests/get", {"database_id": database_id, "request_key": key}, error=error)

    def accepted(self, client, params, method="tasks/start", replayed=False):
        result = client.rpc(method, params)
        self.assertEqual(result["status"], "accepted")
        self.assertEqual(result["replayed"], replayed)
        receipt = result["durable_request"]
        self.assertEqual((receipt["database_id"], receipt["request_key"], receipt["method"]),
                         (params["database_id"], params["request_key"], method))
        for key in ("operation_id", "session_id"):
            self.assertEqual(result[key], receipt[key])
        return result

    def completed(self, client, params, method="tasks/start"):
        accepted = self.accepted(client, params, method)
        done = client.finished(accepted["operation_id"])
        receipt = self.receipt(client, params["database_id"], params["request_key"])
        self.assertEqual(receipt["state"], "finished")
        self.assertEqual(receipt["completion"], done)
        return accepted, done, receipt

    def assert_kind(self, error, kind, outcome=None):
        self.assertEqual(error["data"]["kind"], kind, error)
        if outcome is not None:
            self.assertEqual(error["data"]["outcome"], outcome, error)

    def test_identity_bootstrap_is_explicit_and_idempotent_without_creating_tasks(self):
        client = self.client(initialize=False)
        initialized = client.initialize()
        self.assertTrue(initialized["capabilities"]["durable_requests"])
        self.assertEqual(initialized["limits"]["durable_requests"], 10000)
        self.assertIsNone(initialized["database_id"])
        self.assertFalse(self.database.exists())
        database_id = self.bootstrap(client)
        self.assertEqual(self.bootstrap(client), database_id)
        self.assertEqual(self.sql("PRAGMA user_version"), [(8,)])
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(0,)])
        self.assertEqual(self.sql("SELECT count(*) FROM events"), [(0,)])
        client.close()
        replacement = self.client(initialize=False)
        self.assertEqual(replacement.initialize()["database_id"], database_id)
        self.assertEqual(self.requests(), [])

    def test_key_and_database_identity_must_be_valid_and_supplied_together(self):
        client = self.client()
        for fields in ({"request_key": "request-1"}, {"database_id": "a" * 32},
                       {"request_key": "", "database_id": "a" * 32},
                       {"request_key": "x" * 129, "database_id": "a" * 32},
                       {"request_key": "has spaces", "database_id": "a" * 32},
                       {"request_key": "中文", "database_id": "a" * 32},
                       {"request_key": True, "database_id": "a" * 32},
                       {"request_key": "ok", "database_id": "invalid"}):
            with self.subTest(fields=fields):
                for method, params in (("tasks/start", {"mode": "demo", "options": {}}),
                                       ("tasks/resume", {"task_id": "missing", "options": {}})):
                    error = client.rpc(method, {**params, **fields}, error=True)
                    self.assertEqual(error["code"], -32602)
        self.assertFalse(self.database.exists())
        self.assertEqual(self.requests(), [])

    def test_missing_or_mismatched_receipt_is_unknown_and_lookup_is_read_only(self):
        client = self.client()
        self.assert_kind(self.receipt(client, "a" * 32, "never-recorded", error=True),
                         "database_mismatch", "unknown")
        self.assertFalse(self.database.exists())
        database_id = self.bootstrap(client)
        self.assert_kind(self.receipt(client, database_id, "never-recorded", error=True),
                         "request_not_found", "unknown")
        self.assert_kind(self.receipt(client, "a" * 32, "never-recorded", error=True),
                         "database_mismatch", "unknown")
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(0,)])
        self.assertEqual(self.requests(), [])

    def test_known_credential_cannot_be_persisted_as_a_request_identifier(self):
        client = self.client()
        database_id = self.bootstrap(client)
        before = self.sql("SELECT count(*) FROM durable_requests")
        params = self.keyed(database_id, fixture.TOKEN, mode="demo", options={})
        error = client.rpc("tasks/start", params, error=True)
        self.assertEqual(error["code"], -32602)
        self.assertNotIn(fixture.TOKEN, json.dumps(error))
        self.assertEqual(self.sql("SELECT count(*) FROM durable_requests"), before)
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(0,)])
        self.assertFalse((self.workspace / "CODESTUDIO_DEMO.txt").exists())

    def test_completed_start_repeats_after_restart_without_replaying_file_effects(self):
        client = self.client()
        database_id = self.bootstrap(client)
        params = self.keyed(database_id, "demo:once", mode="demo", options={})
        accepted, done, receipt = self.completed(client, params)
        self.assertEqual(done["exit_code"], 0)
        self.assertEqual(receipt["task_id"], done["result"]["task_id"])
        target = self.workspace / "CODESTUDIO_DEMO.txt"
        target.write_text("user-owned edit after the receipt", encoding="utf-8")
        counts = self.sql("SELECT count(*) FROM events")
        client.close()
        replacement = self.client()
        self.assertNotEqual(replacement.session_id, accepted["session_id"])
        self.assertEqual(self.receipt(replacement, database_id, "demo:once"), receipt)
        replay = self.accepted(replacement, params, replayed=True)
        self.assertEqual(replay["durable_request"], receipt)
        self.assertEqual(target.read_text(encoding="utf-8"), "user-owned edit after the receipt")
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(1,)])
        self.assertEqual(self.sql("SELECT count(*) FROM actions"), [(2,)])
        self.assertEqual(self.sql("SELECT count(*) FROM events"), counts)

    def test_duplicate_active_request_precedes_busy_and_changed_authority_conflicts(self):
        release = self.gate()
        self.responses({"body": fixture.final(), "before_headers": release})
        client = self.client()
        database_id = self.bootstrap(client)
        params = self.keyed(database_id, "active", mode="run", options=self.options())
        accepted = self.accepted(client, params)
        self.wait_for(lambda: len(self.requests()) == 1)
        # Reordered object keys are the same canonical submitted request.
        reordered = dict(reversed(list(params.items())))
        reordered["options"] = dict(reversed(list(params["options"].items())))
        duplicate = self.accepted(client, reordered, replayed=True)
        self.assertEqual(duplicate["operation_id"], accepted["operation_id"])
        self.assertEqual(duplicate["durable_request"]["state"], "incomplete")
        for changed in ({**params, "options": self.options(allow_write=True)},
                        {**params, "options": self.options(prompt="different task")},
                        {**params, "mode": "demo", "options": {}}):
            self.assert_kind(client.rpc("tasks/start", changed, error=True), "request_conflict")
        self.assert_kind(client.rpc("tasks/start", {**params, "request_key": "different"}, error=True), "busy")
        self.assert_kind(self.receipt(client, database_id, "different", error=True), "request_not_found")
        self.assert_kind(client.rpc("state/initialize", error=True), "busy")
        release.set()
        self.assertEqual(client.finished(accepted["operation_id"])["exit_code"], 0)
        self.assertEqual(len(self.requests()), 1)
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(1,)])

    def test_two_services_contending_for_one_key_only_dispatch_once(self):
        release = self.gate()
        self.responses({"body": fixture.final(), "before_headers": release})
        first, second = self.client(), self.client()
        database_id = self.bootstrap(first)
        params = self.keyed(database_id, "concurrent", mode="run", options=self.options())
        pending = [(client, client.send("tasks/start", params)) for client in (first, second)]
        replies = [(client, client.receive(lambda frame, key=request: frame.get("id") == key))
                   for client, request in pending]
        owners = [(client, reply["result"]) for client, reply in replies
                  if "result" in reply and not reply["result"]["replayed"]]
        self.assertEqual(len(owners), 1, replies)
        owner, accepted = owners[0]
        self.wait_for(lambda: len(self.requests()) == 1)
        for client, reply in replies:
            if "error" in reply:
                self.assert_kind(reply["error"], "busy")
            repeated = self.accepted(client, params, replayed=True)
            self.assertEqual(repeated["operation_id"], accepted["operation_id"])
        release.set()
        self.assertEqual(owner.finished(accepted["operation_id"])["exit_code"], 0)
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(1,)])
        self.assertEqual(len(self.requests()), 1)

    def test_lost_acceptance_can_be_looked_up_by_key_after_service_death(self):
        process = self.raw_client()
        self.raw_send(process, 2, "state/initialize")
        database_id = self.raw_receive(process)["result"]["database_id"]
        params = self.keyed(database_id, "unread-acceptance", mode="demo", options={})
        self.raw_send(process, 3, "tasks/start", params)
        # Never read either accepted or finished from this client's stdout.
        observer = self.client()
        receipt = self.wait_for(lambda: self._finished_receipt(observer, database_id, "unread-acceptance"))
        process.kill()
        process.wait(timeout=8)
        repeated = self.accepted(observer, params, replayed=True)
        self.assertEqual(repeated["durable_request"], receipt)
        self.assertEqual(receipt["completion"]["exit_code"], 0)
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(1,)])
        self.assertEqual(self.sql("SELECT count(*) FROM actions"), [(2,)])

    def _finished_receipt(self, client, database_id, key):
        request = client.send("requests/get", {"database_id": database_id, "request_key": key})
        reply = client.receive(lambda frame: frame.get("id") == request)
        if "error" in reply:
            self.assert_kind(reply["error"], "request_not_found")
            return None
        return reply["result"] if reply["result"]["state"] == "finished" else None

    def test_crash_during_mcp_discovery_keeps_admission_without_retrying_preflight(self):
        marker = self.workspace / "mcp-started.txt"
        manifest = self.workspace / "mcp.json"
        manifest.write_text(json.dumps({"version": 2, "servers": [{"id": "echo",
            "command": str(MCP_FIXTURE), "args": ["hang-initialize", str(marker)],
            "tools": ["echo"], "timeout_ms": 30000}]}), encoding="utf-8")
        client = self.client()
        database_id = self.bootstrap(client)
        params = self.keyed(database_id, "preflight-crash", mode="run", options=self.options(
            mcp_config=str(manifest), allow_mcp=["echo"]))
        accepted = self.accepted(client, params)
        self.wait_for(marker.exists)
        before = self.receipt(client, database_id, "preflight-crash")
        self.assertEqual(before["state"], "incomplete")
        self.assertIsNone(before["task_id"])
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(0,)])
        client.process.kill()
        client.wait_exit()
        marker.write_text("do not relaunch discovery", encoding="utf-8")
        # A repeat returns the original request, without consulting new external
        # file contents or changing the authorization captured by that request.
        manifest.unlink()
        replacement = self.client()
        repeated = self.accepted(replacement, params, replayed=True)
        self.assertEqual(repeated["operation_id"], accepted["operation_id"])
        self.assertEqual(repeated["durable_request"], before)
        self.assertEqual(marker.read_text(encoding="utf-8"), "do not relaunch discovery")
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(0,)])
        self.assertEqual(self.requests(), [])

    def test_crash_after_task_admission_exposes_atomic_task_and_checkpoint(self):
        release = self.gate()
        self.responses({"body": fixture.final(), "before_headers": release})
        client = self.client()
        database_id = self.bootstrap(client)
        params = self.keyed(database_id, "attached-crash", mode="run", options=self.options())
        accepted = self.accepted(client, params)
        self.wait_for(lambda: len(self.requests()) == 1)
        before = self.receipt(client, database_id, "attached-crash")
        self.assertEqual(before["state"], "incomplete")
        task_id = before["task_id"]
        self.assertTrue(task_id)
        self.assertEqual(self.sql("SELECT count(*) FROM task_checkpoints WHERE task_id=?", (task_id,)), [(1,)])
        client.process.kill()
        client.wait_exit()
        release.set()
        replacement = self.client()
        self.assertEqual(self.receipt(replacement, database_id, "attached-crash"), before)
        repeated = self.accepted(replacement, params, replayed=True)
        self.assertEqual(repeated["operation_id"], accepted["operation_id"])
        self.assertEqual(replacement.rpc("tasks/get", {"task_id": task_id})["status"], "running")
        self.assertFalse(replacement.rpc("tasks/recover")["tools_executed"])
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(1,)])
        self.assertEqual(len(self.requests()), 1)

    def test_unknown_dispatched_effect_remains_blocked_after_keyed_crash(self):
        self.responses({"body": fixture.call("run_process", {
            "executable": str(fixture.PROCESS), "arguments": ["sleep", "10000"]})})
        client = self.client()
        database_id = self.bootstrap(client)
        options = self.options(allow_process=[str(fixture.PROCESS)], process_timeout_ms=15000)
        params = self.keyed(database_id, "unknown-effect", mode="run", options=options)
        accepted = self.accepted(client, params)
        self.wait_for(lambda: self.sql("SELECT id FROM actions WHERE status='dispatched'"))
        client.process.kill()
        client.wait_exit()
        replacement = self.client()
        receipt = self.receipt(replacement, database_id, "unknown-effect")
        self.assertEqual((receipt["state"], receipt["completion"]), ("incomplete", None))
        self.assertEqual(self.accepted(replacement, params, replayed=True)["operation_id"], accepted["operation_id"])
        recovered = replacement.rpc("tasks/recover")
        self.assertEqual(recovered["recovered"][0]["unknown_actions"], 1)
        self.assertEqual(self.sql("SELECT status,effect FROM actions"), [("unknown", "unknown")])
        continuation = self.keyed(database_id, "explicit-resume", task_id=receipt["task_id"], options={
            "allow_process": [str(fixture.PROCESS)], "process_timeout_ms": 15000, "allow_http_loopback": True})
        _, failed, _ = self.completed(replacement, continuation, "tasks/resume")
        self.assertEqual(failed["exit_code"], 2)
        self.assertEqual(len(self.requests()), 1)
        self.assertEqual(self.sql("SELECT count(*) FROM actions"), [(1,)])

    def test_resume_repeat_reuses_original_operation_and_never_resumes_twice(self):
        self.responses({"body": fixture.call("write_file", {"path": "once.txt", "content": "first write"})})
        client = self.client()
        database_id = self.bootstrap(client)
        initial = self.keyed(database_id, "initial", mode="run", options=self.options(allow_write=True, max_turns=1))
        _, exhausted, _ = self.completed(client, initial)
        self.assertEqual(exhausted["result"]["status"], "exhausted")
        task_id = exhausted["result"]["task_id"]
        target = self.workspace / "once.txt"
        target.write_text("preserve the completed tool effect", encoding="utf-8")
        self.responses({"body": fixture.final()})
        params = self.keyed(database_id, "continue", task_id=task_id, options={
            "max_turns": 2, "allow_http_loopback": True, "allow_write": True})
        accepted, done, receipt = self.completed(client, params, "tasks/resume")
        self.assertEqual((done["exit_code"], done["result"]["task_id"]), (0, task_id))
        client.close()
        replacement = self.client()
        repeated = self.accepted(replacement, params, "tasks/resume", replayed=True)
        self.assertEqual(repeated["operation_id"], accepted["operation_id"])
        self.assertEqual(repeated["durable_request"], receipt)
        for changed in ({**params, "options": {**params["options"], "allow_write": False}},
                        {**params, "options": {**params["options"], "max_turns": 3}}):
            self.assert_kind(replacement.rpc("tasks/resume", changed, error=True), "request_conflict")
        self.assertEqual(self.sql("SELECT count(*) FROM events WHERE kind='task.resumed'"), [(1,)])
        self.assertEqual(target.read_text(encoding="utf-8"), "preserve the completed tool effect")
        self.assertEqual(self.sql("SELECT count(*) FROM actions"), [(1,)])
        self.assertEqual(len(self.requests()), 2)

    def test_method_change_with_same_key_is_a_conflict(self):
        client = self.client()
        database_id = self.bootstrap(client)
        _, done, _ = self.completed(client, self.keyed(database_id, "method-key", mode="demo", options={}))
        error = client.rpc("tasks/resume", self.keyed(database_id, "method-key",
            task_id=done["result"]["task_id"], options={}), error=True)
        self.assert_kind(error, "request_conflict")
        self.assertEqual(self.sql("SELECT count(*) FROM events WHERE kind='task.resumed'"), [(0,)])

    def test_same_database_and_key_cannot_retarget_another_workspace(self):
        client = self.client()
        database_id = self.bootstrap(client)
        params = self.keyed(database_id, "workspace-bound", mode="demo", options={})
        self.completed(client, params)
        other_workspace = self.workspace.parent / "another workspace"
        other_workspace.mkdir()
        other_client = self.client(workspace=other_workspace, state=self.database)
        self.assert_kind(other_client.rpc("tasks/start", params, error=True), "request_conflict")
        self.assert_kind(self.receipt(other_client, database_id, "workspace-bound", error=True), "request_conflict")
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(1,)])
        self.assertFalse((other_workspace / "CODESTUDIO_DEMO.txt").exists())

    @unittest.skipUnless(sys.platform == "darwin", "Darwin temporary-directory alias")
    def test_darwin_var_alias_canonicalizes_to_the_same_durable_request(self):
        canonical = self.workspace.resolve()
        if not str(canonical).startswith("/private/var/"):
            self.skipTest("The test temporary directory is not under /private/var")
        alias = pathlib.Path(str(canonical).removeprefix("/private"))
        self.assertTrue(alias.samefile(canonical))
        first = self.client(workspace=canonical)
        database_id = self.bootstrap(first)
        params = self.keyed(database_id, "var-alias", mode="demo", options={})
        _, _, receipt = self.completed(first, params)
        target = canonical / "CODESTUDIO_DEMO.txt"
        target.write_text("keep original request effects", encoding="utf-8")
        first.close()
        second = self.client(initialize=False, workspace=alias)
        initialized = second.initialize()
        self.assertEqual(initialized["workspace"], str(canonical))
        self.assertEqual(initialized["state_database"], str(self.database.resolve()))
        self.assertEqual(initialized["database_id"], database_id)
        self.assertEqual(self.receipt(second, database_id, "var-alias"), receipt)
        self.assertEqual(self.accepted(second, params, replayed=True)["durable_request"], receipt)
        self.assertEqual(target.read_text(encoding="utf-8"), "keep original request effects")
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(1,)])

    @unittest.skipUnless(sys.platform == "darwin", "Darwin filesystem aliases and flock")
    def test_darwin_case_and_unicode_aliases_preserve_binding_and_share_active_lock(self):
        # The runtime's std::filesystem::canonical resolves APFS spelling aliases
        # to the same workspace. A repeated key must reuse the original receipt,
        # while a different key still contends for the same kernel-held locks.
        self.workspace = self.workspace.resolve() / "Workspacé"
        self.workspace.mkdir()
        self.database = self.workspace / ".codestudio" / "state.sqlite3"
        variants = (self.workspace.with_name("wORKSPACÉ"),
                    self.workspace.with_name(unicodedata.normalize("NFD", self.workspace.name)))
        aliases = [path for path in variants if path.exists() and path.samefile(self.workspace)]
        if len(aliases) != len(variants):
            self.skipTest("The test volume does not expose both case and Unicode aliases")
        release = self.gate()
        self.responses({"body": fixture.final(), "before_headers": release})
        owner = self.client()
        database_id = self.bootstrap(owner)
        params = self.keyed(database_id, "original-spelling", mode="run", options=self.options())
        accepted = self.accepted(owner, params)
        self.wait_for(lambda: len(self.requests()) == 1)
        original = self.receipt(owner, database_id, params["request_key"])
        peers = []
        for index, alias in enumerate(aliases):
            with self.subTest(alias=alias.name):
                peer = self.client(initialize=False, workspace=alias)
                peers.append(peer)
                initialized = peer.initialize()
                self.assertEqual(initialized["database_id"], database_id)
                self.assertEqual(initialized["workspace"], str(self.workspace))
                self.assertEqual(initialized["state_database"], str(self.database))
                self.assertEqual(self.receipt(peer, database_id, params["request_key"]), original)
                repeated = self.accepted(peer, params, replayed=True)
                self.assertEqual(repeated["operation_id"], accepted["operation_id"])
                self.assertEqual(repeated["durable_request"], original)
                other = {**params, "request_key": f"alias-new-{index}"}
                self.assert_kind(peer.rpc("tasks/start", other, error=True), "busy")
                self.assert_kind(self.receipt(peer, database_id, other["request_key"], error=True), "request_not_found")
        release.set()
        self.assertEqual(owner.finished(accepted["operation_id"])["exit_code"], 0)
        finished = self.receipt(owner, database_id, params["request_key"])
        for peer in peers:
            self.assertEqual(self.accepted(peer, params, replayed=True)["durable_request"], finished)
        self.assertEqual(self.sql("SELECT count(*) FROM durable_requests"), [(1,)])
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(1,)])
        self.assertEqual(len(self.requests()), 1)

    @unittest.skipIf(os.name == "nt", "POSIX flock lifetime")
    def test_posix_admission_locks_release_after_service_crash_without_replaying_request(self):
        import fcntl
        release = self.gate()
        self.responses({"body": fixture.final(), "before_headers": release})
        owner = self.client()
        database_id = self.bootstrap(owner)
        params = self.keyed(database_id, "crashed-owner", mode="run", options=self.options())
        accepted = self.accepted(owner, params)
        self.wait_for(lambda: len(self.requests()) == 1)
        locks = (self.workspace / ".codestudio/workspace.run.lock",
                 pathlib.Path(str(self.database) + ".run.lock"))
        for path in locks:
            with path.open("rb") as stream, self.assertRaises(BlockingIOError):
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        owner.process.kill()
        owner.wait_exit()
        release.set()
        for path in locks:
            with path.open("rb") as stream:
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                fcntl.flock(stream, fcntl.LOCK_UN)
        replacement = self.client()
        repeated = self.accepted(replacement, params, replayed=True)
        self.assertEqual(repeated["operation_id"], accepted["operation_id"])
        self.assertEqual(repeated["durable_request"]["state"], "incomplete")
        self.assertEqual(len(self.requests()), 1)
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(1,)])

    def test_schema7_refused_by_service_without_changing_the_old_database(self):
        self.database.parent.mkdir()
        with closing(sqlite3.connect(self.database)) as database:
            database.execute("CREATE TABLE preserved(value TEXT)")
            database.execute("INSERT INTO preserved VALUES('keep the old state')")
            database.execute("PRAGMA user_version=7")
            database.commit()
        original = self.database.read_bytes()
        # StateReader rejects the old format during service construction, before
        # reading any RPC. Verify fail-fast rather than expecting RPC errors from
        # a process which has already exited; each CLI check gets its own process.
        for command in ("serve", "history", "demo"):
            with self.subTest(command=command):
                result = subprocess.run([str(fixture.EXE), command, "--workspace", str(self.workspace)],
                    env=self.env, stdin=subprocess.DEVNULL, capture_output=True, timeout=8)
                self.assertEqual(result.returncode, 2)
                if command == "serve":
                    self.assertEqual(result.stdout, b"")
                self.assertIn(b"Unsupported state schema", result.stdout + result.stderr)
                self.assertEqual(self.database.read_bytes(), original)
        self.assertEqual(self.database.read_bytes(), original)
        with closing(sqlite3.connect(self.database)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone(), (7,))
            self.assertEqual(database.execute("SELECT * FROM preserved").fetchall(), [("keep the old state",)])
            self.assertEqual(database.execute("PRAGMA journal_mode").fetchone(), ("delete",))
        self.assertFalse(pathlib.Path(str(self.database) + "-wal").exists())
        self.assertFalse(pathlib.Path(str(self.database) + "-shm").exists())
        self.assertFalse((self.workspace / "CODESTUDIO_DEMO.txt").exists())
        self.assertEqual(self.requests(), [])

    def test_preflight_failure_is_durable_and_cannot_be_repurposed_with_new_grants(self):
        client = self.client()
        database_id = self.bootstrap(client)
        params = self.keyed(database_id, "failed-resume", task_id="not-a-task", options={})
        accepted, done, receipt = self.completed(client, params, "tasks/resume")
        self.assertEqual(done["exit_code"], 2)
        self.assertIsNone(receipt["task_id"])
        self.assertEqual(self.accepted(client, params, "tasks/resume", replayed=True)["operation_id"], accepted["operation_id"])
        changed = {**params, "options": {"allow_write": True}}
        self.assert_kind(client.rpc("tasks/resume", changed, error=True), "request_conflict")
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(0,)])
        self.assertEqual(self.requests(), [])

    def test_durable_receipt_survives_session_cache_eviction_and_restart(self):
        client = self.client()
        database_id = self.bootstrap(client)
        params = self.keyed(database_id, "retained", mode="demo", options={})
        accepted, _, receipt = self.completed(client, params)
        for _ in range(65):
            result = client.rpc("crons/create", {"expression": "invalid", "timezone": "UTC", "options": self.options()})
            self.assertEqual(client.finished(result["operation_id"])["exit_code"], 2)
        self.assert_kind(client.rpc("operations/get", {"session_id": client.session_id,
            "operation_id": accepted["operation_id"]}, error=True), "operation_not_found")
        self.assertEqual(self.receipt(client, database_id, "retained"), receipt)
        client.close()
        replacement = self.client()
        self.assertEqual(self.accepted(replacement, params, replayed=True)["durable_request"], receipt)
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(1,)])

    def test_durable_completion_is_redacted_before_database_persistence(self):
        self.responses({"body": fixture.final("result " + fixture.TOKEN)})
        client = self.client()
        database_id = self.bootstrap(client)
        params = self.keyed(database_id, "redacted", mode="run", options=self.options())
        _, done, receipt = self.completed(client, params)
        self.assertEqual(done["result"]["summary"], "result [REDACTED]")
        self.assertNotIn(fixture.TOKEN, json.dumps(receipt))
        client.close()
        for path in (self.database, pathlib.Path(str(self.database) + "-wal")):
            if path.exists():
                self.assertNotIn(fixture.TOKEN.encode(), path.read_bytes())
        replacement = self.client()
        self.assertEqual(self.receipt(replacement, database_id, "redacted"), receipt)
        self.assertEqual(len(self.requests()), 1)

    def test_custom_credential_is_redacted_before_its_operation_scope_ends(self):
        secret = "durable-custom-credential-not-the-default"
        self.env["DURABLE_FIXTURE_CREDENTIAL"] = secret
        self.responses({"body": fixture.final("custom result " + secret)})
        client = self.client()
        database_id = self.bootstrap(client)
        params = self.keyed(database_id, "custom-credential", mode="run", options=self.options(
            credential="env:DURABLE_FIXTURE_CREDENTIAL"))
        _, done, receipt = self.completed(client, params)
        self.assertEqual(done["result"]["summary"], "custom result [REDACTED]")
        self.assertNotIn(secret, json.dumps(receipt))
        self.assertEqual(self.requests()[0]["headers"]["Authorization"], "Bearer " + secret)
        client.close()
        self.assertNotIn(secret.encode(), bytes(client.raw) + bytes(client.stderr))
        for path in (self.database, pathlib.Path(str(self.database) + "-wal")):
            if path.exists():
                self.assertNotIn(secret.encode(), path.read_bytes())
        replacement = self.client()
        self.assertEqual(self.receipt(replacement, database_id, "custom-credential"), receipt)
        self.assertEqual(len(self.requests()), 1)

    def test_large_durable_completion_keeps_task_reference_and_explicit_omission(self):
        summary = "界" * 90000 + fixture.TOKEN
        self.responses({"body": fixture.final(summary)})
        client = self.client()
        database_id = self.bootstrap(client)
        params = self.keyed(database_id, "large", mode="run", options=self.options())
        accepted = self.accepted(client, params)
        done = client.finished(accepted["operation_id"])
        self.assertEqual(done["exit_code"], 0)
        receipt = self.receipt(client, database_id, "large")
        retained = receipt["completion"]
        self.assertIsNone(retained["result"])
        self.assertTrue(retained["result_omitted"])
        self.assertGreater(retained["result_bytes"], 262144)
        self.assertEqual(retained["result_refs"], {"task_id": done["result"]["task_id"]})
        self.assertEqual(receipt["task_id"], done["result"]["task_id"])
        self.assertLess(len(json.dumps(receipt)), 4096)
        client.close()
        self.assertEqual(self.receipt(self.client(), database_id, "large"), receipt)

    def test_replaced_database_rejects_old_identity_without_repeating_request(self):
        client = self.client()
        original_id = self.bootstrap(client)
        params = self.keyed(original_id, "original", mode="demo", options={})
        self.completed(client, params)
        replacement_path = self.workspace / "replacement.sqlite3"
        replacement = self.client(state=replacement_path)
        replacement_id = replacement.rpc("state/initialize")["database_id"]
        self.assertNotEqual(replacement_id, original_id)
        client.close()
        replacement.close()
        replacement_path.replace(self.database)
        observer = self.client(initialize=False)
        self.assertEqual(observer.initialize()["database_id"], replacement_id)
        self.assert_kind(self.receipt(observer, original_id, "original", error=True), "database_mismatch")
        self.assert_kind(observer.rpc("tasks/start", params, error=True), "database_mismatch")
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(0,)])

    def test_removed_database_is_not_recreated_by_retry_or_query(self):
        client = self.client()
        database_id = self.bootstrap(client)
        client.close()
        self.database.unlink()
        replacement = self.client()
        params = self.keyed(database_id, "old-identity", mode="demo", options={})
        self.assert_kind(replacement.rpc("tasks/start", params, error=True), "database_mismatch")
        self.assert_kind(self.receipt(replacement, database_id, "old-identity", error=True), "database_mismatch")
        self.assertFalse(self.database.exists())

    def test_failed_completion_commit_never_fabricates_success_or_replays_task(self):
        client = self.client()
        database_id = self.bootstrap(client)
        # Real SQLite commit failure at the last persistence boundary. The task
        # may finish, but a failed receipt write cannot become durable success.
        with closing(sqlite3.connect(self.database)) as database:
            database.execute("""CREATE TRIGGER reject_request_completion
                BEFORE UPDATE OF completion ON durable_requests
                WHEN NEW.completion IS NOT NULL
                BEGIN SELECT RAISE(ABORT, 'fixture completion write failure'); END""")
            database.commit()
        params = self.keyed(database_id, "completion-fault", mode="demo", options={})
        accepted = self.accepted(client, params)
        done = client.finished(accepted["operation_id"])
        self.assertEqual(done["exit_code"], 2)
        self.assert_kind(done["error"], "operation_failed", "unknown")
        receipt = self.receipt(client, database_id, "completion-fault")
        self.assertEqual((receipt["state"], receipt["completion"]), ("incomplete", None))
        self.assertTrue(receipt["task_id"])
        self.assertEqual(client.rpc("tasks/get", {"task_id": receipt["task_id"]})["status"], "completed")
        target = self.workspace / "CODESTUDIO_DEMO.txt"
        target.write_text("preserve after failed receipt commit", encoding="utf-8")
        with closing(sqlite3.connect(self.database)) as database:
            database.execute("DROP TRIGGER reject_request_completion")
            database.commit()
        client.close()
        replacement = self.client()
        repeated = self.accepted(replacement, params, replayed=True)
        self.assertEqual(repeated["durable_request"], receipt)
        self.assertEqual(target.read_text(encoding="utf-8"), "preserve after failed receipt commit")
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(1,)])
        self.assertEqual(self.sql("SELECT count(*) FROM actions"), [(2,)])

    def test_failed_admission_commit_is_not_accepted_and_never_dispatches(self):
        client = self.client()
        database_id = self.bootstrap(client)
        with closing(sqlite3.connect(self.database)) as database:
            database.execute("""CREATE TRIGGER reject_request_admission
                BEFORE INSERT ON durable_requests
                BEGIN SELECT RAISE(ABORT, 'fixture admission write failure'); END""")
            database.commit()
        params = self.keyed(database_id, "admission-fault", mode="demo", options={})
        client.rpc("tasks/start", params, error=True)
        self.assert_kind(self.receipt(client, database_id, "admission-fault", error=True), "request_not_found")
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), [(0,)])
        self.assertEqual(self.sql("SELECT count(*) FROM durable_requests"), [(0,)])
        self.assertFalse((self.workspace / "CODESTUDIO_DEMO.txt").exists())
        self.assertFalse(client.rpc("operations/status")["active"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
