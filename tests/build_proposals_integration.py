"""Real CLI, SQLite and stdio tests for reviewed build proposals.

The fixture import consumes the executable argument. Its test class is not
collected here; reusable setup/helpers are assigned individually below.
"""
import copy
import hashlib
import json
import os
import pathlib
import queue
import sqlite3
import subprocess
import sys
import unittest
from contextlib import closing
import build_integration as fixture


def reply(api, arguments=None, tool="propose_build", call_id="build-once"):
    value = fixture.model_reply(api, arguments)
    if arguments is not None:
        if api == "chat-completions":
            call = value["choices"][0]["message"]["tool_calls"][0]
            call["function"]["name"], call["id"] = tool, call_id
        elif api == "responses":
            value["output"][0].update(name=tool, call_id=call_id, id="function_" + call_id)
        else:
            value["content"][0].update(name=tool, id=call_id)
    return value


class UnavailableOnceHandler(fixture.Handler):
    """Keep the real request capture, but fail one model response with HTTP 503."""
    def send_response(self, code, message=None):
        if code == 200 and self.server.fail_next_model_request:
            self.server.fail_next_model_request = False
            code = 503
        super().send_response(code, message)


@unittest.skipUnless(os.name == "nt", "Approved execution requires Windows LPAC")
class ProposalTests(unittest.TestCase):
    setUp = fixture.BuildTests.setUp
    tearDown = fixture.BuildTests.tearDown
    recipe = fixture.BuildTests.recipe
    invoke = fixture.BuildTests.invoke
    show = fixture.BuildTests.show
    bundles = fixture.BuildTests.bundles
    requests = fixture.BuildTests.requests
    replies = fixture.BuildTests.replies
    client = fixture.BuildTests.client
    verify_artifacts = fixture.BuildTests.verify_artifacts
    verify_identity = fixture.BuildTests.verify_identity
    live_tree = fixture.BuildTests.live_tree
    assert_tree_cleaned = fixture.BuildTests.assert_tree_cleaned
    reclaim_crash_stage = fixture.BuildTests.reclaim_crash_stage

    def arguments(self, mode="write"):
        manifest = self.recipe(mode, timeout=20000 if "sleep" in mode else 10000)
        manifest["version"] = 2
        return {"manifest": manifest, "reason": "编译并验证已编辑的工作区"}

    def run_args(self, api="responses", *, grant=True, max_turns=2):
        args = ["run", "--workspace", self.workspace, "--prompt", "Propose a private workspace build",
                "--base-url", self.url, "--model", "proposal-fixture", "--api", api,
                "--allow-http-loopback", "--no-stream", "--max-turns", str(max_turns)]
        if grant:
            args.append("--allow-build-proposals")
        return args

    def propose(self, api="responses", arguments=None, max_turns=2, extra=()):
        arguments = self.arguments() if arguments is None else arguments
        requests, bundles = len(self.requests()), self.bundles()
        self.replies(reply(api, arguments))
        result = self.invoke(*self.run_args(api, max_turns=max_turns), *extra, expected=3)
        self.assertEqual((result["status"], result["turns"]), ("waiting_approval", 1))
        self.assertEqual(len(self.requests()), requests + 1, "A pending proposal reached the model again")
        task = result["task_id"]
        actions = self.show(task)["actions"]
        self.assertEqual(len(actions), 1)
        action = actions[0]
        self.assertEqual((action["tool"], action["status"], action["effect"]),
                         ("propose_build", "succeeded", "none"), action)
        self.assertTrue(action["result"]["ok"])
        self.assertEqual(action["continuation"]["status"], "pending")
        self.assertEqual(self.bundles(), bundles)
        self.assertFalse((self.workspace / "out").exists())
        self.assertEqual((self.workspace / "input.txt").read_text(), fixture.SOURCE)
        with closing(sqlite3.connect(self.database)) as database:
            state = json.loads(database.execute("SELECT state FROM task_checkpoints WHERE task_id=?", (task,)).fetchone()[0])
        self.assertEqual((state["phase"], state["next_call"]), ("tools", 0))
        self.assertFalse(any(item["role"] == "tool" for item in state["messages"]))
        return task, action["id"], action["result"]["output"]

    def resume_args(self, task, *extra, grant=True):
        return ["resume", task, "--workspace", self.workspace, "--allow-http-loopback",
                *(["--allow-build-proposals"] if grant else []), *extra]

    def source_action(self, task, action):
        return next(item for item in self.show(task)["actions"] if item["id"] == action)

    def consumption_events(self, task, action):
        return [event for event in self.show(task)["events"]
                if event["kind"] == "build.proposal.consumed" and event["payload"]["action_id"] == action]

    def tool_results(self, api, request, call_id="build-once"):
        body = request["body"]
        if api == "chat-completions":
            return [json.loads(item["content"]) for item in body["messages"]
                    if item.get("role") == "tool" and item.get("tool_call_id") == call_id]
        if api == "responses":
            return [json.loads(item["output"]) for item in body["input"]
                    if item.get("type") == "function_call_output" and item.get("call_id") == call_id]
        return [json.loads(item["content"]) for message in body["messages"]
                if isinstance(message.get("content"), list) for item in message["content"]
                if item.get("type") == "tool_result" and item.get("tool_use_id") == call_id]

    def combined_args(self, command, task, action, digest, *extra, grant=True):
        return [*self.decision_args(command, task, action, digest), "--continue-task", "--allow-http-loopback",
                *(["--allow-build-proposals"] if grant else []), *extra]

    def decision_args(self, command, task, action, digest=None):
        args = [command, task, "--action", action, "--workspace", self.workspace]
        if digest is not None:
            args += ["--digest", digest]
        return args

    def review(self, task, action):
        return self.invoke(*self.decision_args("build-review", task, action))

    def decide(self, command, task, action, digest, expected=0):
        return self.invoke(*self.decision_args(command, task, action, digest), expected=expected)

    def task_count(self):
        with closing(sqlite3.connect(self.database)) as database:
            return database.execute("SELECT count(*) FROM tasks").fetchone()[0]

    def database_snapshot(self):
        with closing(sqlite3.connect(self.database)) as database:
            return "\n".join(database.iterdump())

    def test_three_protocols_propose_and_review_without_execution(self):
        for api in fixture.APIS:
            with self.subTest(api=api):
                task, action, proposal = self.propose(api)
                review = self.review(task, action)
                self.assertEqual((review["task_id"], review["action_id"]), (task, action))
                self.assertEqual(review["proposal"], proposal)
                self.assertIsNone(review["decision"])
                self.assertEqual(proposal["status"], "pending_approval")
                self.assertEqual(proposal["workspace"], str(self.workspace))
                self.assertEqual(proposal["manifest"]["version"], 2)
                self.assertEqual(proposal["version"], 2)
                self.assertEqual(proposal["recipe"]["version"], 2)
                self.assertNotIn("plugin", proposal)
                unsigned = {key: value for key, value in proposal.items() if key != "digest"}
                canonical = json.dumps(unsigned, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()
                self.assertEqual(proposal["digest"], hashlib.sha256(canonical).hexdigest())
                names = {entry.get("name", entry.get("function", {}).get("name"))
                         for entry in self.requests()[-1]["body"]["tools"]}
                self.assertIn("propose_build", names)
                for name in ("build_workspace", "approve_build", "build-approve"):
                    self.assertNotIn(name, names)

    def test_approval_executes_once_without_model_and_persists_receipt(self):
        task, action, proposal = self.propose()
        requests = self.requests()
        source = self.show(task)
        key = self.env.pop("CODESTUDIO_API_KEY")
        # Approval makes no model request, but needs the source credential to
        # redact new build receipts before persistence. Missing is pre-claim.
        self.decide("build-approve", task, action, proposal["digest"], expected=2)
        self.assertEqual(self.task_count(), 1)
        self.assertEqual(self.bundles(), [])
        self.assertIsNone(self.review(task, action)["decision"])
        self.assertEqual(self.requests(), requests)
        self.env["CODESTUDIO_API_KEY"] = key
        approved = self.decide("build-approve", task, action, proposal["digest"])
        execution = approved["execution"]
        self.assertEqual((execution["provider"], execution["status"]), ("approved-build", "completed"))
        self.assertEqual(len(execution["actions"]), 1)
        self.assertEqual(execution["actions"][0]["tool"], "build_workspace")
        bundle = self.verify_artifacts(execution["actions"][0])
        self.verify_identity(json.loads((bundle / "files/out/identity-0.json").read_text()))
        exported = {str(path.relative_to(bundle)): path.read_bytes() for path in bundle.rglob("*") if path.is_file()}
        before = self.bundles()
        repeated = self.decide("build-approve", task, action, proposal["digest"])
        self.assertEqual(repeated["execution"], execution)
        self.assertEqual(self.bundles(), before)
        self.assertEqual(exported, {str(path.relative_to(bundle)): path.read_bytes() for path in bundle.rglob("*") if path.is_file()})
        self.assertEqual(self.source_action(task, action)["result"], source["actions"][0]["result"])
        self.assertEqual(self.source_action(task, action)["continuation"]["status"], "ready")
        self.assertEqual(self.show(task)["status"], "waiting_approval")
        self.assertEqual(self.requests(), requests)
        self.assertEqual(self.task_count(), 2)
        self.assertEqual(self.review(task, action)["decision"]["execution_task_id"], execution["id"])

    def test_rejection_is_durable_idempotent_and_cannot_be_approved(self):
        task, action, proposal = self.propose()
        self.env.pop("CODESTUDIO_API_KEY")
        rejected = self.decide("build-reject", task, action, proposal["digest"])
        self.assertIsNone(rejected["execution"])
        self.assertEqual(rejected["proposal"]["decision"]["decision"], "rejected")
        again = self.decide("build-reject", task, action, proposal["digest"])
        self.assertEqual(again["proposal"]["decision"], rejected["proposal"]["decision"])
        self.decide("build-approve", task, action, proposal["digest"], expected=2)
        self.assertEqual(self.task_count(), 1)
        self.assertEqual(self.bundles(), [])

    def test_approval_retains_source_mcp_manifest_protection(self):
        mcp_fixture = fixture.EXE.parent / "mcp-fixture.exe"
        self.assertTrue(mcp_fixture.is_file(), f"Build the MCP fixture first: {mcp_fixture}")
        config_directory = self.workspace / "config"
        config_directory.mkdir()
        mcp_manifest = config_directory / "mcp.json"
        marker = pathlib.Path(self.temp.name) / "mcp.started"
        mcp_manifest.write_text(json.dumps({"version": 2, "servers": [{"id": "echo",
            "command": str(mcp_fixture), "args": ["normal", str(marker)],
            "tools": ["echo"], "timeout_ms": 1000}]}), encoding="utf-8")
        original_manifest = mcp_manifest.read_bytes()
        public = config_directory / "public.txt"
        public.write_text("ordinary source file", encoding="utf-8")
        arguments = self.arguments()
        arguments["manifest"]["inputs"].append("config")
        arguments["manifest"]["outputs"].append("config")
        # The fixture accepts expanded Windows paths in a separate argv value
        # so backslashes are not accidentally interpreted as JSON escapes.
        arguments["manifest"]["steps"][0]["arguments"].append("$" + "{workspace}/config/mcp.json")
        task, action, proposal = self.propose(arguments=arguments,
            extra=("--mcp-config", mcp_manifest, "--allow-mcp", "echo"))
        self.assertTrue(marker.exists(), "The source task must actually load the explicitly granted MCP server")
        self.assertIn(str(mcp_manifest), proposal["protected_paths"])
        self.assertIn("mcp_echo_echo", {tool["name"] for tool in self.requests()[0]["body"]["tools"]})
        marker.unlink()
        requests = self.requests()
        approved = self.decide("build-approve", task, action, proposal["digest"])
        bundle = self.verify_artifacts(approved["execution"]["actions"][0])
        evidence = json.loads((bundle / "files/out/identity-0.json").read_text())
        self.assertFalse(evidence["staged_file_exists"], "The source task's MCP manifest reached the private workspace")
        self.assertEqual(evidence["staged_file_error"], 2)  # ERROR_FILE_NOT_FOUND, not access denied.
        self.assertEqual((bundle / "files/config/public.txt").read_text(), "ordinary source file")
        self.assertFalse((bundle / "files/config/mcp.json").exists())
        self.assertEqual(mcp_manifest.read_bytes(), original_manifest)
        self.assertFalse(marker.exists(), "Approval must not restart the source task's MCP server")
        self.assertEqual(self.requests(), requests)

    def test_current_approval_credential_conflict_is_rejected_before_claim(self):
        arguments = self.arguments()
        future_key = "credential-known-only-to-the-later-approval-process"
        arguments["reason"] = future_key
        task, action, proposal = self.propose(arguments=arguments)
        source = self.show(task)
        requests = self.requests()
        self.env["CODESTUDIO_API_KEY"] = future_key
        try:
            reviewed = self.invoke(*self.decision_args("build-review", task, action), expected=2)
            self.assertNotIn(future_key, json.dumps(reviewed))
            for command in ("build-approve", "build-reject"):
                rejected = self.decide(command, task, action, proposal["digest"], expected=2)
                self.assertNotIn(future_key, json.dumps(rejected))
        finally:
            self.env["CODESTUDIO_API_KEY"] = fixture.TOKEN
        self.assertEqual(self.show(task), source)
        self.assertIsNone(self.review(task, action)["decision"])
        self.assertEqual(self.task_count(), 1)
        self.assertEqual(self.bundles(), [])
        self.assertEqual(self.requests(), requests)

    def test_wrong_digest_or_modified_receipt_never_creates_execution(self):
        task, action, proposal = self.propose()
        wrong = ("0" if proposal["digest"][0] != "0" else "1") + proposal["digest"][1:]
        self.decide("build-approve", task, action, wrong, expected=2)
        with closing(sqlite3.connect(self.database)) as database:
            saved = database.execute("SELECT result FROM actions WHERE id=?", (action,)).fetchone()[0]
            modified = json.loads(saved)
            modified["output"]["recipe"]["limits"]["cpu_percent"] = 20
            database.execute("UPDATE actions SET result=? WHERE id=?", (json.dumps(modified), action))
            database.commit()
        try:
            self.invoke(*self.decision_args("build-review", task, action), expected=2)
            self.decide("build-approve", task, action, proposal["digest"], expected=2)
        finally:
            with closing(sqlite3.connect(self.database)) as database:
                database.execute("UPDATE actions SET result=? WHERE id=?", (saved, action))
                database.commit()
        self.assertIsNone(self.review(task, action)["decision"])
        self.assertEqual(self.task_count(), 1)
        self.assertEqual(self.bundles(), [])

    def test_modified_approved_proposal_blocks_resume_without_mutating_durable_state(self):
        task, action, proposal = self.propose()
        self.decide("build-approve", task, action, proposal["digest"])
        requests = self.requests()
        with closing(sqlite3.connect(self.database)) as database:
            saved = database.execute("SELECT result FROM actions WHERE id=?", (action,)).fetchone()[0]
        for field in ("reason", "recipe"):
            with self.subTest(field=field):
                receipt = json.loads(saved)
                if field == "reason":
                    receipt["output"]["reason"] += " altered after approval"
                else:
                    receipt["output"]["recipe"]["limits"]["cpu_percent"] = 20
                self.assertEqual(receipt["output"]["digest"], proposal["digest"])
                with closing(sqlite3.connect(self.database)) as database:
                    database.execute("UPDATE actions SET result=? WHERE id=?", (json.dumps(receipt), action))
                    database.commit()
                try:
                    before = self.database_snapshot()
                    self.invoke(*self.resume_args(task), expected=2)
                    self.assertEqual(self.requests(), requests)
                    self.assertEqual(self.database_snapshot(), before,
                                     "Invalid approved proposal changed tasks, events, decisions or checkpoint")
                finally:
                    with closing(sqlite3.connect(self.database)) as database:
                        database.execute("UPDATE actions SET result=? WHERE id=?", (saved, action))
                        database.commit()

    def test_new_execution_feedback_credential_collision_blocks_resume_before_mcp_start(self):
        mcp_fixture = fixture.EXE.parent / "mcp-fixture.exe"
        self.assertTrue(mcp_fixture.is_file(), f"Build the MCP fixture first: {mcp_fixture}")
        mcp_manifest = self.workspace / "mcp.json"
        marker = pathlib.Path(self.temp.name) / "feedback-mcp.started"
        mcp_manifest.write_text(json.dumps({"version": 2, "servers": [{"id": "echo",
            "command": str(mcp_fixture), "args": ["normal", str(marker)],
            "tools": ["echo"], "timeout_ms": 1000}]}), encoding="utf-8")
        grants = ("--mcp-config", mcp_manifest, "--allow-mcp", "echo")
        task, action, proposal = self.propose(extra=grants)
        self.assertTrue(marker.exists(), "The original task did not start the granted MCP fixture")
        approved = self.decide("build-approve", task, action, proposal["digest"])
        # This diagnostic key first appears in the separate approved build's
        # stdout, so a saved-checkpoint-only credential check cannot catch it.
        future_key = "job_cpu_query_error"
        output = approved["execution"]["actions"][0]["result"]["output"]["steps"][0]["output"]
        self.assertIn(future_key, output)
        with closing(sqlite3.connect(self.database)) as database:
            checkpoint = database.execute("SELECT state,binding FROM task_checkpoints WHERE task_id=?", (task,)).fetchone()
        self.assertNotIn(future_key, "\n".join(checkpoint))
        self.assertNotIn(future_key, json.dumps(proposal))
        marker.unlink()
        requests, before = self.requests(), self.database_snapshot()
        self.env["CODESTUDIO_API_KEY"] = future_key
        try:
            rejected = self.invoke(*self.resume_args(task, *grants), expected=2)
            self.assertNotIn(future_key, json.dumps(rejected))
        finally:
            self.env["CODESTUDIO_API_KEY"] = fixture.TOKEN
        self.assertFalse(marker.exists(), "Credential-conflicting execution feedback started the MCP process")
        self.assertEqual(self.requests(), requests)
        self.assertEqual(self.database_snapshot(), before,
                         "Rejected feedback changed tasks, events, decisions or checkpoint")

    def test_unsupported_receipt_versions_cannot_be_reviewed_or_approved(self):
        task, action, proposal = self.propose()
        requests = self.requests()
        with closing(sqlite3.connect(self.database)) as database:
            saved = database.execute("SELECT result FROM actions WHERE id=?", (action,)).fetchone()[0]
        for version in (1, 3, True, "2"):
            with self.subTest(version=version):
                receipt = json.loads(saved)
                unsupported = receipt["output"]
                unsupported["version"] = version
                unsupported.pop("digest")
                canonical = json.dumps(unsupported, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()
                unsupported["digest"] = hashlib.sha256(canonical).hexdigest()
                frozen = json.dumps(receipt)
                with closing(sqlite3.connect(self.database)) as database:
                    database.execute("UPDATE actions SET result=? WHERE id=?", (frozen, action))
                    database.commit()
                for command in ("build-review", "build-approve", "build-reject"):
                    digest = None if command == "build-review" else unsupported["digest"]
                    self.invoke(*self.decision_args(command, task, action, digest), expected=2)
                with closing(sqlite3.connect(self.database)) as database:
                    self.assertEqual(database.execute("SELECT result FROM actions WHERE id=?", (action,)).fetchone()[0], frozen)
                    self.assertEqual(database.execute("SELECT count(*) FROM build_proposal_decisions").fetchone()[0], 0)
        self.assertEqual(self.task_count(), 1)
        self.assertEqual(self.bundles(), [])
        self.assertEqual(self.requests(), requests)

    def test_review_is_readonly_and_cross_workspace_approval_is_rejected(self):
        self.invoke(*self.decision_args("build-review", "missing", "missing"), expected=2)
        self.assertFalse((self.workspace / ".codestudio").exists())
        task, action, proposal = self.propose()
        alternate = pathlib.Path(self.temp.name) / "other workspace"
        alternate.mkdir()
        for command in ("build-review", "build-approve", "build-reject"):
            args = [command, task, "--action", action, "--workspace", alternate, "--state", self.database]
            if command != "build-review":
                args += ["--digest", proposal["digest"]]
            self.invoke(*args, expected=2)
        self.assertEqual(self.task_count(), 1)
        self.assertIsNone(self.review(task, action)["decision"])
        self.assertEqual(self.bundles(), [])

    def test_models_cannot_enable_proposals_or_self_approve(self):
        for tool, grant in (("propose_build", False), ("approve_build", True), ("build-approve", True)):
            with self.subTest(tool=tool, grant=grant):
                arguments = self.arguments() if tool == "propose_build" else {"digest": "0" * 64}
                self.replies(reply("responses", arguments, tool=tool), reply("responses"))
                result = self.invoke(*self.run_args(grant=grant), expected=1)
                actions = self.show(result["task_id"])["actions"]
                self.assertEqual(len(actions), 1)
                self.assertEqual(actions[0]["status"], "denied")
                self.assertNotIn(tool, {item["name"] for item in self.requests()[-2]["body"]["tools"]})
        self.assertEqual(self.bundles(), [])

    def test_invalid_plan_or_sensitive_content_cannot_become_approvable(self):
        for alteration in ("unsupported-version", "system-environment", "credential", "unknown-field"):
            with self.subTest(alteration=alteration):
                arguments = self.arguments()
                if alteration == "unsupported-version":
                    arguments["manifest"]["version"] = 999
                elif alteration == "system-environment":
                    arguments["manifest"]["environment"] = {"HOME": "$" + "{workspace}"}
                elif alteration == "credential":
                    arguments["reason"] = fixture.TOKEN
                else:
                    arguments["manifest"]["unrecognized_plan_field"] = True
                self.replies(reply("responses", arguments), reply("responses"))
                result = self.invoke(*self.run_args(), expected=1)
                action = self.show(result["task_id"])["actions"][0]
                self.assertFalse(action["result"].get("ok", False))
                self.invoke(*self.decision_args("build-review", result["task_id"], action["id"]), expected=2)
        self.assertEqual(self.bundles(), [])

    def test_proposal_resume_requires_same_grant_and_reuses_receipt(self):
        task, action, proposal = self.propose()
        requests = self.requests()
        self.invoke(*self.resume_args(task), expected=2)
        self.assertEqual(self.requests(), requests)
        self.assertEqual(self.consumption_events(task, action), [])
        self.decide("build-reject", task, action, proposal["digest"])
        self.invoke(*self.resume_args(task, grant=False), expected=2)
        self.assertEqual(self.requests(), requests)
        self.replies(reply("responses"))
        resumed = self.invoke(*self.resume_args(task))
        self.assertEqual(resumed["status"], "completed")
        self.assertEqual(len(self.show(task)["actions"]), 1)
        self.assertEqual(self.review(task, action)["proposal"], proposal)
        self.assertEqual(len(self.consumption_events(task, action)), 1)
        self.assertEqual(self.bundles(), [])

    def test_three_protocols_resume_with_approved_or_rejected_feedback_once(self):
        for api in fixture.APIS:
            for command in ("build-approve", "build-reject"):
                with self.subTest(api=api, decision=command):
                    task, action, proposal = self.propose(api)
                    original = self.source_action(task, action)["result"]
                    requests = len(self.requests())
                    decided = self.decide(command, task, action, proposal["digest"])
                    self.assertEqual(len(self.requests()), requests)
                    continuation = self.source_action(task, action)["continuation"]
                    self.assertEqual(continuation["status"], "ready")
                    feedback = continuation["result"]
                    self.assertEqual((feedback["ok"], feedback["error"], feedback["effect"]), (True, "", "none"))
                    self.assertEqual(feedback["output"]["proposal"], proposal)
                    self.assertEqual(feedback["output"]["decision"], self.review(task, action)["decision"])
                    execution = decided["execution"]
                    if execution is None:
                        self.assertIsNone(feedback["output"]["execution"])
                    else:
                        executed = execution["actions"][0]
                        self.verify_artifacts(executed)
                        self.assertEqual(feedback["output"]["execution"], {
                            "task_id": execution["id"], "action_id": executed["id"],
                            "status": executed["status"], "result": executed["result"]})
                    bundles = self.bundles()
                    self.replies(reply(api))
                    resumed = self.invoke(*self.resume_args(task))
                    self.assertEqual((resumed["task_id"], resumed["status"], resumed["turns"]), (task, "completed", 2))
                    self.assertEqual(self.tool_results(api, self.requests()[-1]), [feedback])
                    self.assertEqual(self.source_action(task, action)["result"], original)
                    self.assertEqual(len(self.consumption_events(task, action)), 1)
                    self.assertEqual(self.bundles(), bundles)
                    self.invoke(*self.resume_args(task), expected=2)
                    self.assertEqual(len(self.requests()), requests + 1)

    def test_three_protocols_model_failure_reuses_consumed_feedback_without_duplicate(self):
        self.server.RequestHandlerClass = UnavailableOnceHandler
        for api in fixture.APIS:
            with self.subTest(api=api):
                self.server.fail_next_model_request = False
                task, action, proposal = self.propose(api, max_turns=3)
                original = self.source_action(task, action)["result"]
                self.decide("build-reject", task, action, proposal["digest"])
                feedback = self.source_action(task, action)["continuation"]["result"]
                self.server.fail_next_model_request = True
                self.replies(reply(api))
                failed = self.invoke(*self.resume_args(task), expected=1)
                self.assertEqual((failed["status"], failed["turns"]), ("failed", 2))
                self.assertFalse(self.server.fail_next_model_request, "The fixture never returned HTTP 503")
                first_request = self.requests()[-1]
                self.assertEqual(self.tool_results(api, first_request), [feedback])
                consumed = self.consumption_events(task, action)
                self.assertEqual(len(consumed), 1)
                self.replies(reply(api))
                resumed = self.invoke(*self.resume_args(task))
                self.assertEqual((resumed["status"], resumed["turns"]), ("completed", 3))
                self.assertEqual(self.requests()[-1], first_request)
                self.assertEqual(self.consumption_events(task, action), consumed)
                self.assertEqual(self.source_action(task, action)["result"], original)
                self.assertEqual(len(self.show(task)["actions"]), 1)
        self.assertEqual(self.bundles(), [])

    def test_known_failed_build_feedback_allows_model_to_inspect_failure_without_reexecution(self):
        task, action, proposal = self.propose(arguments=self.arguments("fail"))
        approved = self.decide("build-approve", task, action, proposal["digest"], expected=1)
        executed = approved["execution"]["actions"][0]
        self.assertEqual((executed["status"], executed["effect"]), ("failed", "none"))
        feedback = self.source_action(task, action)["continuation"]["result"]
        self.assertTrue(feedback["ok"])
        self.assertFalse(feedback["output"]["execution"]["result"]["ok"])
        self.assertEqual(feedback["output"]["execution"]["result"], executed["result"])
        self.replies(reply("responses"))
        resumed = self.invoke(*self.resume_args(task))
        self.assertEqual(resumed["status"], "completed")
        self.assertEqual(self.tool_results("responses", self.requests()[-1]), [feedback])
        self.assertEqual(self.show(approved["execution"]["id"])["actions"][0], executed)
        self.assertEqual(self.bundles(), [])
        self.assertEqual(len(self.consumption_events(task, action)), 1)

    def test_combined_approval_stops_at_new_proposal_and_repeated_old_decision_skips(self):
        task, first_action, first = self.propose(max_turns=3)
        self.replies(reply("responses", self.arguments(), call_id="second-build"))
        approved = self.invoke(*self.combined_args("build-approve", task, first_action, first["digest"]), expected=3)
        self.assertEqual(approved["continuation"]["status"], "started")
        self.assertEqual(approved["continuation"]["exit_code"], 3)
        continued = approved["continuation"]["result"]
        self.assertEqual((continued["task_id"], continued["status"], continued["turns"]), (task, "waiting_approval", 2))
        self.verify_artifacts(approved["execution"]["actions"][0])
        second_action = next(item for item in self.show(task)["actions"] if item["id"] != first_action)
        second = second_action["result"]["output"]
        self.assertEqual(second_action["continuation"]["status"], "pending")
        self.assertIsNone(self.review(task, second_action["id"])["decision"])
        self.assertEqual(len(self.consumption_events(task, first_action)), 1)
        self.assertEqual(self.consumption_events(task, second_action["id"]), [])
        requests, bundles = self.requests(), self.bundles()
        repeated = self.invoke(*self.combined_args("build-approve", task, first_action, first["digest"]))
        self.assertEqual(repeated["continuation"]["status"], "skipped")
        self.assertEqual(repeated["execution"], approved["execution"])
        self.invoke(*self.resume_args(task), expected=2)
        self.assertEqual(self.requests(), requests)
        self.assertEqual(self.bundles(), bundles)
        self.replies(reply("responses"))
        rejected = self.invoke(*self.combined_args("build-reject", task, second_action["id"], second["digest"]))
        self.assertEqual(rejected["continuation"]["status"], "started")
        self.assertEqual((rejected["continuation"]["result"]["status"], rejected["continuation"]["result"]["turns"]),
                         ("completed", 3))
        self.assertIsNone(rejected["execution"])
        for action_id, call_id in ((first_action, "build-once"), (second_action["id"], "second-build")):
            self.assertEqual(len(self.consumption_events(task, action_id)), 1)
            self.assertEqual(len(self.tool_results("responses", self.requests()[-1], call_id)), 1)
        self.assertEqual(self.bundles(), bundles)
        self.assertEqual(self.task_count(), 2)

    def test_combined_continuation_requires_original_grants_without_model_request(self):
        task, action, proposal = self.propose()
        requests = self.requests()
        rejected = self.invoke(*self.combined_args("build-reject", task, action, proposal["digest"], grant=False), expected=2)
        self.assertEqual(rejected["proposal"]["decision"]["decision"], "rejected")
        self.assertEqual(rejected["continuation"]["status"], "failed")
        self.assertTrue(rejected["continuation"]["error"])
        self.assertEqual(self.show(task)["status"], "waiting_approval")
        self.assertEqual(self.consumption_events(task, action), [])
        self.assertEqual(self.requests(), requests)
        self.replies(reply("responses"))
        resumed = self.invoke(*self.combined_args("build-reject", task, action, proposal["digest"]))
        self.assertEqual(resumed["continuation"]["result"]["status"], "completed")
        requests = self.requests()
        repeated = self.invoke(*self.combined_args("build-reject", task, action, proposal["digest"]))
        self.assertEqual(repeated["continuation"]["status"], "skipped")
        self.assertEqual(self.requests(), requests)
        self.assertEqual(len(self.consumption_events(task, action)), 1)

    def test_combined_continuation_cannot_increase_original_budget(self):
        task, action, proposal = self.propose(max_turns=1)
        requests = self.requests()
        rejected = self.invoke(*self.combined_args("build-reject", task, action, proposal["digest"],
                                                 "--max-turns", "2"), expected=2)
        self.assertEqual(rejected["continuation"]["status"], "failed")
        self.assertEqual(self.consumption_events(task, action), [])
        self.assertEqual(self.requests(), requests)
        bounded = self.invoke(*self.combined_args("build-reject", task, action, proposal["digest"]), expected=1)
        self.assertEqual((bounded["continuation"]["status"], bounded["continuation"]["exit_code"]), ("started", 1))
        self.assertEqual((bounded["continuation"]["result"]["status"], bounded["continuation"]["result"]["turns"]),
                         ("exhausted", 1))
        self.assertEqual(self.requests(), requests)
        self.assertEqual(len(self.consumption_events(task, action)), 1)
        self.replies(reply("responses"))
        resumed = self.invoke(*self.resume_args(task, "--max-turns", "2"))
        self.assertEqual((resumed["status"], resumed["turns"]), ("completed", 2))
        self.assertEqual(len(self.tool_results("responses", self.requests()[-1])), 1)
        self.assertEqual(len(self.consumption_events(task, action)), 1)

    def test_unknown_or_partial_approved_execution_blocks_original_task_feedback(self):
        task, action, proposal = self.propose()
        approved = self.decide("build-approve", task, action, proposal["digest"])
        executed = approved["execution"]["actions"][0]
        requests, bundles = self.requests(), self.bundles()
        for effect in ("unknown", "partial"):
            with self.subTest(effect=effect):
                result = copy.deepcopy(executed["result"])
                result.update(ok=False, effect=effect, error="Inspect the uncertain export")
                with closing(sqlite3.connect(self.database)) as database:
                    database.execute("UPDATE actions SET status=?,effect=?,result=? WHERE id=?",
                                     ("unknown" if effect == "unknown" else "failed", effect, json.dumps(result), executed["id"]))
                    database.execute("UPDATE tasks SET status='needs_attention' WHERE id=?", (approved["execution"]["id"],))
                    database.commit()
                self.assertEqual(self.source_action(task, action)["continuation"]["status"], "blocked")
                self.invoke(*self.resume_args(task), expected=2)
                self.assertEqual(self.consumption_events(task, action), [])
                self.assertEqual(self.requests(), requests)
                self.assertEqual(self.bundles(), bundles)


    def test_service_proposes_reviews_and_approves_with_fixed_arguments(self):
        client = self.client()
        self.replies(reply("responses", self.arguments()))
        started = client.rpc("tasks/start", {"mode": "run", "options": {
            "prompt": "Propose a workspace build", "base_url": self.url, "model": "proposal-fixture",
            "api": "responses", "allow_http_loopback": True, "stream": False, "allow_build_proposals": True}})
        finished = client.finished(started["operation_id"])
        self.assertEqual(finished["exit_code"], 3, finished)
        self.assertEqual(finished["result"]["status"], "waiting_approval")
        task = finished["result"]["task_id"]
        action = self.show(task)["actions"][0]["id"]
        review = client.rpc("builds/review", {"task_id": task, "action_id": action})
        self.assertIsNone(review["decision"])
        self.assertEqual(self.bundles(), [])
        params = {"task_id": task, "action_id": action, "digest": review["proposal"]["digest"]}
        for method in ("builds/approve", "builds/reject"):
            client.process.stdin.write((json.dumps({"jsonrpc": "2.0", "method": method, "params": params}) + "\n").encode())
            client.process.stdin.flush()
        self.assertFalse(client.rpc("operations/status")["active"])
        self.assertIsNone(client.rpc("builds/review", {"task_id": task, "action_id": action})["decision"])
        for extra in ({"workspace": str(self.workspace)}, {"state": str(self.database)}, {"options": {}}):
            client.rpc("builds/approve", {**params, **extra}, error=True)
        client.rpc("builds/approve", {**params, "digest": "not-a-digest"}, error=True)
        client.rpc("builds/review", params, error=True)
        accepted = client.rpc("builds/approve", params)
        completed = client.finished(accepted["operation_id"])
        self.assertEqual(completed["exit_code"], 0, completed)
        self.verify_artifacts(completed["result"]["execution"]["actions"][0])
        self.assertEqual(client.rpc("builds/review", {"task_id": task, "action_id": action})["decision"]["decision"], "approved")
        self.assertEqual(len(self.requests()), 1)
        feedback = self.source_action(task, action)["continuation"]["result"]
        self.replies(reply("responses"))
        resumed = client.rpc("tasks/resume", {"task_id": task, "options": {
            "allow_http_loopback": True, "allow_build_proposals": True}})
        finished = client.finished(resumed["operation_id"])
        self.assertEqual((finished["exit_code"], finished["result"]["status"]), (0, "completed"))
        self.assertEqual(self.tool_results("responses", self.requests()[-1]), [feedback])
        self.assertEqual(len(self.consumption_events(task, action)), 1)
        self.assertEqual(len(self.requests()), 2)

    def test_service_rejection_and_option_validation(self):
        task, action, proposal = self.propose()
        client = self.client()
        for value in ("true", 1, None):
            client.rpc("tasks/start", {"mode": "run", "options": {"allow_build_proposals": value}}, error=True)
        accepted = client.rpc("builds/reject", {"task_id": task, "action_id": action, "digest": proposal["digest"]})
        finished = client.finished(accepted["operation_id"])
        self.assertEqual(finished["exit_code"], 0, finished)
        self.assertEqual(finished["result"]["proposal"]["decision"]["decision"], "rejected")
        self.assertEqual(self.bundles(), [])
        self.assertEqual(self.task_count(), 1)
        self.assertEqual(len(self.requests()), 1)
        params = {"task_id": task, "action_id": action, "digest": proposal["digest"]}
        for value in (True, 1, [], "true", None):
            client.rpc("builds/reject", {**params, "continue_options": value}, error=True)
        self.assertEqual(len(self.requests()), 1)
        self.replies(reply("responses"))
        params["continue_options"] = {"allow_http_loopback": True, "allow_build_proposals": True}
        continued = client.finished(client.rpc("builds/reject", params)["operation_id"])
        self.assertEqual(continued["exit_code"], 0, continued)
        self.assertEqual(continued["result"]["continuation"]["status"], "started")
        self.assertEqual(continued["result"]["continuation"]["result"]["status"], "completed")
        repeated = client.finished(client.rpc("builds/reject", params)["operation_id"])
        self.assertEqual(repeated["result"]["continuation"]["status"], "skipped")
        self.assertEqual(len(self.requests()), 2)
        self.assertEqual(len(self.consumption_events(task, action)), 1)

    def test_service_approval_allows_review_rejects_busy_and_cancels_tree(self):
        task, action, proposal = self.propose(arguments=self.arguments("tree-sleep"))
        requests = self.requests()
        client = self.client()
        params = {"task_id": task, "action_id": action, "digest": proposal["digest"],
                  "continue_options": {"allow_http_loopback": True, "allow_build_proposals": True}}
        started = client.rpc("builds/approve", params)
        evidence = self.live_tree(client)
        reviewed = client.rpc("builds/review", {"task_id": task, "action_id": action})
        self.assertEqual(reviewed["decision"]["decision"], "approved")
        execution_id = reviewed["decision"]["execution_task_id"]
        before = self.show(execution_id)
        self.assertEqual((before["status"], before["actions"][0]["status"]), ("running", "dispatched"))
        self.assertEqual(client.rpc("builds/review", {"task_id": task, "action_id": action}), reviewed)
        self.assertEqual(self.show(execution_id), before, "Review must not append events or mutate the execution")
        for method, data in (("builds/approve", params), ("builds/reject", params),
                             ("tasks/start", {"mode": "demo", "options": {}}), ("tasks/recover", {})):
            rejected = client.rpc(method, data, error=True)
            self.assertIn("already active", rejected["message"])
        self.assertEqual(self.task_count(), 2)
        self.assertTrue(client.rpc("operations/cancel", {"operation_id": started["operation_id"]})["cancel_requested"])
        finished = client.finished(started["operation_id"])
        self.assertEqual(finished["exit_code"], 130, finished)
        self.assertEqual(finished["result"]["continuation"]["status"], "skipped")
        execution = finished["result"]["execution"]
        self.assertEqual(execution["status"], "cancelled")
        receipt = execution["actions"][0]["result"]
        self.assertEqual(receipt["effect"], "none")
        self.assertTrue(receipt["output"]["cancelled"])
        self.assert_tree_cleaned(evidence)
        self.assertEqual(self.decide("build-approve", task, action, proposal["digest"], expected=130)["execution"], execution)
        repeated = self.invoke(*self.combined_args("build-approve", task, action, proposal["digest"]), expected=130)
        self.assertEqual(repeated["continuation"]["status"], "skipped")
        self.decide("build-reject", task, action, proposal["digest"], expected=2)
        self.assertEqual(self.show(task)["status"], "waiting_approval")
        self.assertEqual(self.consumption_events(task, action), [])
        self.assertEqual(self.requests(), requests)

    def test_service_approval_stdin_eof_cancels_tree_without_replay(self):
        task, action, proposal = self.propose(arguments=self.arguments("tree-sleep"))
        requests = self.requests()
        client = self.client()
        client.rpc("builds/approve", {"task_id": task, "action_id": action, "digest": proposal["digest"],
            "continue_options": {"allow_http_loopback": True, "allow_build_proposals": True}})
        evidence = self.live_tree(client)
        execution_id = client.rpc("builds/review", {"task_id": task, "action_id": action})["decision"]["execution_task_id"]
        client.process.stdin.close()
        self.assertEqual(client.wait_exit(), 0)
        self.assert_tree_cleaned(evidence)
        execution = self.show(execution_id)
        self.assertEqual(execution["status"], "cancelled")
        self.assertEqual((execution["actions"][0]["status"], execution["actions"][0]["effect"]), ("failed", "none"))
        events = execution.pop("events")
        self.assertEqual(self.decide("build-approve", task, action, proposal["digest"], expected=130)["execution"], execution)
        self.assertEqual(self.show(execution_id)["events"], events)
        self.assertEqual(self.task_count(), 2)
        self.assertEqual(self.show(task)["status"], "waiting_approval")
        self.assertEqual(self.consumption_events(task, action), [])
        self.assertEqual(self.requests(), requests)

    def test_crashed_approval_is_claimed_once_and_unknown_effect_never_replays(self):
        task, action, proposal = self.propose(arguments=self.arguments("tree-sleep"))
        requests = self.requests()
        process = subprocess.Popen([str(fixture.EXE), *map(str, self.decision_args(
            "build-approve", task, action, proposal["digest"]))],
            env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        observer = type("ApprovalObserver", (), {})()
        observer.process, observer.queue = process, queue.Queue()
        observer.pending, observer.stderr = [], bytearray()
        try:
            evidence = self.live_tree(observer)
            physical_image = evidence[0][1].resolve(strict=True)
            stage = physical_image.parents[2]
            execution_id = self.review(task, action)["decision"]["execution_task_id"]
            self.assertEqual(self.show(execution_id)["actions"][0]["status"], "dispatched")
            process.kill()
            process.communicate(timeout=10)
            for handle, image in evidence:
                self.assertEqual(fixture.KERNEL.WaitForSingleObject(handle, 5000), 0, str(image))
            self.assertFalse(self.invoke("recover", "--workspace", self.workspace)["tools_executed"])
            execution = self.show(execution_id)
            self.assertEqual((execution["actions"][0]["status"], execution["actions"][0]["effect"]),
                             ("unknown", "unknown"))
            # `show` includes the event projection while the approval receipt
            # contains the task projection. Compare the durable task fields,
            # then assert that the second approval did not append events.
            execution_events = execution.pop("events")
            repeated = self.decide("build-approve", task, action, proposal["digest"], expected=1)
            self.assertEqual(repeated["execution"], execution)
            self.assertEqual(self.show(execution_id)["events"], execution_events)
            self.invoke("resume", execution_id, "--workspace", self.workspace, expected=2)
            self.assertEqual(self.source_action(task, action)["continuation"]["status"], "blocked")
            self.invoke(*self.resume_args(task), expected=2)
            self.assertEqual(self.consumption_events(task, action), [])
            self.assertEqual(self.task_count(), 2)
            self.assertEqual(self.bundles(), [])
            self.assertEqual(self.requests(), requests)
            self.assertEqual((self.workspace / "input.txt").read_text(), fixture.SOURCE)
            self.reclaim_crash_stage(stage, execution_id)
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)


if __name__ == "__main__":
    unittest.main(verbosity=2)
