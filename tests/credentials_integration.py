"""Credential selection, rotation and secret boundaries through real CLI/HTTP.

No real model or user credential is used. Native store tests own only randomly
named synthetic entries and remove them in tearDown. Linux tests requiring a
live Secret Service skip after checking its honest unavailable/locked result;
run inside a private test bus/keyring to exercise their successful path.
"""
import json
import os
import pathlib
import re
import sqlite3
import subprocess
import sys
import threading
import unittest
import uuid
from contextlib import closing

import resume_integration as fixture

# This fixture also consumes an executable argument; isolate that import from
# unittest's remaining arguments. Do not import its TestCase into discovery.
_arguments = sys.argv[:]
sys.argv[:] = [_arguments[0], str(fixture.EXE)]
try:
    import service_integration as service_fixture
finally:
    sys.argv[:] = _arguments


class CredentialTests(unittest.TestCase):
    run_args = fixture.ResumeTests.run_args
    resume_args = fixture.ResumeTests.resume_args
    setup_responses = fixture.ResumeTests.setup_responses
    requests = fixture.ResumeTests.requests
    start = fixture.ResumeTests.start
    kill = staticmethod(fixture.ResumeTests.kill)
    wait_for = fixture.ResumeTests.wait_for
    sql_rows = fixture.ResumeTests.sql_rows
    sql_execute = fixture.ResumeTests.sql_execute
    configure_mcp = fixture.ResumeTests.configure_mcp

    def setUp(self):
        fixture.ResumeTests.setUp(self)
        # Build grants reject aliases; Windows TEMP may contain an 8.3 name.
        self.workspace = self.workspace.resolve()
        self.database = self.workspace / ".codestudio" / "state.sqlite3"
        self.first = "fixture-credential-old-" + uuid.uuid4().hex
        self.second = "fixture-credential-new-" + uuid.uuid4().hex
        self.secrets = (fixture.TOKEN, self.first, self.second)
        self.variable = "CODESTUDIO_TEST_SELECTED_KEY"
        self.env[self.variable] = self.first
        self.reference = "env:" + self.variable
        self.name = "test-credential-" + uuid.uuid4().hex
        self.system_reference = "system:" + self.name
        self.system_written = False
        self.clients = []

    def tearDown(self):
        try:
            for release in self.releases:
                release.set()
            for process in self.children:
                self.kill(process)
            for client in self.clients:
                client.close()
                for secret in self.secrets:
                    self.assertNotIn(secret.encode(), client.raw + client.stderr)
            if self.system_written:
                data = self.invoke("credential-delete", "--name", self.name)
                self.assertIn(data["state"], ("deleted", "missing"))
                self.assertEqual(self.invoke("credential-status", "--credential", self.system_reference)["state"], "missing")
        finally:
            fixture.ResumeTests.tearDown(self)

    def capture(self, *args, env=None, input=None):
        result = subprocess.run([str(fixture.EXE), *map(str, args)], cwd=self.workspace,
            env=self.env if env is None else env, input=input, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=20)
        for secret in self.secrets:
            self.assertNotIn(secret.encode(), result.stdout + result.stderr)
        return result

    def invoke(self, *args, expected=0, env=None, input=None):
        result = self.capture(*args, env=env, input=input)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def assert_secrets_absent(self):
        for path in self.workspace.rglob("*"):
            if path.is_file():
                data = path.read_bytes()
                for secret in self.secrets:
                    self.assertNotIn(secret.encode(), data, str(path))
        if self.database.exists():
            with closing(sqlite3.connect(self.database)) as database:
                dump = "\n".join(database.iterdump())
            for secret in self.secrets:
                self.assertNotIn(secret, dump)

    def header_key(self, request):
        headers = {key.lower(): value for key, value in request["headers"].items()}
        return headers.get("x-api-key") or headers.get("authorization", "").removeprefix("Bearer ")

    def require_system(self, secret=None):
        result = self.capture("credential-set", "--name", self.name, "--stdin",
                              input=(secret or self.first).encode() + b"\n")
        data = json.loads(result.stdout)
        if data.get("state") in ("locked", "unavailable") and os.name != "nt":
            self.assertEqual(result.returncode, 2)
            self.assertFalse(self.database.exists())
            self.skipTest("private Linux Secret Service is " + data["state"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(data["state"], "available")
        self.assertEqual(data["reference"], self.system_reference)
        self.system_written = True
        return data

    def set_system(self, secret):
        return self.invoke("credential-set", "--name", self.name, "--stdin", input=secret.encode() + b"\r\n")

    def test_status_and_invalid_references_have_no_workspace_side_effects(self):
        result = self.invoke("credential-status", "--credential", self.reference)
        self.assertEqual((result["reference"], result["state"]), (self.reference, "available"))
        missing_env = dict(self.env)
        del missing_env[self.variable]
        self.assertEqual(self.invoke("credential-status", "--credential", self.reference,
                                    env=missing_env)["state"], "missing")
        for reference in ("system:", "system:../other", "env:KEY=secret", "env:2KEY", "plain:secret"):
            with self.subTest(reference=reference):
                self.invoke("credential-status", "--credential", reference, expected=2)
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_stdin_validation_and_inline_secret_options_are_rejected(self):
        for payload in (b"", b"\n", b"contains space", b"line1\nline2", b"embedded\0NUL", b"a" * 2049):
            with self.subTest(length=len(payload)):
                self.invoke("credential-set", "--name", self.name, "--stdin", input=payload, expected=2)
        self.invoke("credential-set", "--name", self.name, "--value", self.first, expected=2)
        self.invoke("credential-set", "--name", self.name, input=self.first.encode(), expected=2)
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_explicit_environment_reference_authenticates_each_provider_protocol(self):
        for api in fixture.APIS:
            with self.subTest(api=api):
                self.setup_responses({"body": fixture.reply(api)})
                result = self.invoke(*self.run_args("--credential", self.reference, api=api))
                self.assertEqual(result["status"], "completed")
                self.assertEqual(self.header_key(self.requests()[-1]), self.first)
                binding = json.loads(self.sql_rows("SELECT binding FROM task_checkpoints WHERE task_id=?",
                                                   (result["task_id"],))[0][0])
                self.assertEqual(binding["configuration"]["provider"]["credential"], self.reference)
        self.assert_secrets_absent()

    def test_missing_reference_fails_before_http_or_mcp_launch(self):
        _, marker, grants = self.configure_mcp()
        missing_env = dict(self.env)
        del missing_env[self.variable]
        result = self.invoke(*self.run_args("--credential", self.reference, *grants), env=missing_env, expected=2)
        self.assertIn("missing", result["error"].lower())
        self.assertFalse(marker.exists())
        self.assertEqual(self.requests(), [])
        self.assertFalse(self.database.exists())
        self.assert_secrets_absent()

    def test_resume_cannot_switch_reference_even_when_values_match(self):
        _, marker, grants = self.configure_mcp()
        self.setup_responses({"status": 503})
        task = self.invoke(*self.run_args("--credential", self.reference, *grants), expected=1)["task_id"]
        marker.unlink()
        self.env["CODESTUDIO_TEST_OTHER_KEY"] = self.first
        self.invoke(*self.resume_args(task, "--credential", "env:CODESTUDIO_TEST_OTHER_KEY", *grants), expected=2)
        self.assertFalse(marker.exists())
        self.assertEqual(len(self.requests()), 1)
        self.setup_responses({"body": fixture.reply()})
        self.assertEqual(self.invoke(*self.resume_args(task, *grants))["status"], "completed")
        self.assertEqual(self.header_key(self.requests()[-1]), self.first)
        self.assert_secrets_absent()

    def test_rotated_reference_resumes_without_replaying_successful_tool(self):
        self.setup_responses({"body": fixture.reply(tool=("write_file", {"path": "once.txt", "content": "original"}))},
                             {"status": 503})
        task = self.invoke(*self.run_args("--credential", self.reference, "--allow-write", "--max-turns", "4"),
                           expected=1)["task_id"]
        (self.workspace / "once.txt").write_text("already handled externally", encoding="utf-8")
        self.env[self.variable] = self.second
        self.setup_responses({"body": fixture.reply()})
        result = self.invoke(*self.resume_args(task, "--allow-write"))
        self.assertEqual((result["status"], result["turns"]), ("completed", 3))
        self.assertEqual(self.header_key(self.requests()[-1]), self.second)
        self.assertEqual((self.workspace / "once.txt").read_text(), "already handled externally")
        self.assertEqual(self.sql_rows("SELECT count(*) FROM actions")[0][0], 1)
        self.assert_secrets_absent()

    def test_missing_reference_in_checkpoint_is_not_silently_converted(self):
        self.setup_responses({"status": 503})
        task = self.invoke(*self.run_args("--credential", self.reference), expected=1)["task_id"]
        binding = json.loads(self.sql_rows("SELECT binding FROM task_checkpoints WHERE task_id=?", (task,))[0][0])
        del binding["configuration"]["provider"]["credential"]
        changed = json.dumps(binding)
        self.sql_execute("UPDATE task_checkpoints SET binding=? WHERE task_id=?", (changed, task))
        self.invoke(*self.resume_args(task), expected=2)
        self.assertEqual(len(self.requests()), 1)
        self.assertEqual(self.sql_rows("SELECT binding FROM task_checkpoints WHERE task_id=?", (task,))[0][0], changed)

    def test_split_stream_secret_is_redacted_before_cli_and_persistence(self):
        def chunk(delta, finish=None):
            return "data: " + json.dumps({"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}) + "\n\n"
        events = chunk({"role": "assistant"})
        for piece in ["Start ", *self.first, " end"]:
            events += chunk({"content": piece})
        events += chunk({}, "stop") + "data: [DONE]\n\n"
        self.setup_responses({"raw": events.encode(), "content_type": "text/event-stream"})
        result = self.capture(*self.run_args("--credential", self.reference, streaming=True))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b"[REDACTED]", result.stdout + result.stderr)
        self.assert_secrets_absent()

    def test_streaming_overlapping_default_and_selected_keys_do_not_expose_suffix(self):
        shorter, selected = "prefix", "prefix-credential"
        self.env["CODESTUDIO_API_KEY"] = shorter
        self.env[self.variable] = selected
        self.secrets += (shorter, selected)

        def chunk(delta, finish=None):
            return "data: " + json.dumps({"choices": [{"index": 0, "delta": delta,
                "finish_reason": finish}]}) + "\n\n"

        events = chunk({"role": "assistant"})
        for fragment in ("Before ", "prefix", "-credential", " after"):
            events += chunk({"content": fragment})
        events += chunk({}, "stop") + "data: [DONE]\n\n"
        self.setup_responses({"raw": events.encode(), "content_type": "text/event-stream"})
        result = self.capture(*self.run_args("--credential", self.reference, streaming=True))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b"[REDACTED]", result.stdout + result.stderr)
        self.assertNotIn(b"-credential", result.stdout + result.stderr,
                         "a shorter credential match exposed the selected credential's suffix")
        self.assertEqual(self.header_key(self.requests()[0]), selected)
        with closing(sqlite3.connect(self.database)) as database:
            self.assertNotIn("-credential", "\n".join(database.iterdump()))
        self.assert_secrets_absent()

    def test_approved_build_output_uses_source_credential_redaction_before_persistence(self):
        build_fixture = fixture.EXE.parent / ("build-fixture.exe" if os.name == "nt" else "build-fixture")
        self.assertTrue(build_fixture.is_file())
        selected = "job_cpu_query_error" if os.name == "nt" else "no_new_privileges"
        self.env[self.variable] = selected
        self.secrets += (selected,)
        (self.workspace / "input.txt").write_text("nonsecret build input", encoding="utf-8")
        proposal = {"manifest": {"version": 2, "inputs": ["input.txt"],
            "mounts": [{"name": "fixture", "source": str(build_fixture.parent), "inputs": [build_fixture.name]}],
            "steps": [{"executable": "${fixture}/" + build_fixture.name,
                       "arguments": ["write", "{}"], "cwd": "."}],
            "outputs": ["out/payload.txt"], "artifact_directory": "artifacts",
            "limits": {"timeout_ms": 10000, "processes": 8, "memory_mb": 256, "cpu_percent": 80}},
            "reason": "Build the explicitly reviewed source"}
        for continuing in (False, True):
            with self.subTest(continue_task=continuing):
                self.setup_responses({"body": fixture.reply(tool=("propose_build", proposal))})
                task = self.invoke(*self.run_args("--credential", self.reference, "--allow-build-proposals"),
                                   expected=3)["task_id"]
                action = self.invoke("show", task, "--workspace", self.workspace)["actions"][0]
                digest = action["result"]["output"]["digest"]
                arguments = ["build-approve", task, "--workspace", self.workspace,
                             "--action", action["id"], "--digest", digest]
                before = len(self.requests())
                if continuing:
                    self.setup_responses({"body": fixture.reply()})
                    arguments += ["--continue-task", "--allow-http-loopback", "--allow-build-proposals"]
                approved = self.invoke(*arguments)
                self.assertEqual(approved["execution"]["status"], "completed")
                receipt = approved["execution"]["actions"][0]
                captured = receipt["result"]["output"]["steps"][0]["output"]
                self.assertIn("[REDACTED]", captured)
                self.assertNotIn(selected, captured)
                stored = self.invoke("show", approved["execution"]["id"], "--workspace", self.workspace)
                self.assertEqual(stored["actions"][0]["result"], receipt["result"])
                self.assertEqual(len(self.requests()), before + int(continuing))
                replay = self.invoke(*arguments)
                self.assertEqual(replay["execution"], approved["execution"])
                self.assertEqual(len(self.requests()), before + int(continuing))
                self.assert_secrets_absent()

    def test_selected_secret_is_redacted_from_failure_receipts(self):
        self.setup_responses({"status": 401, "body": {"error": self.first}})
        result = self.invoke(*self.run_args("--credential", self.reference), expected=1)
        self.assertEqual(result["status"], "failed")
        self.assertEqual(len(self.requests()), 1, "authentication failure replayed a request")
        self.assert_secrets_absent()

    def test_stdio_run_resume_preserves_reference_and_redacts_operation_errors(self):
        client = service_fixture.Client(self.workspace, self.env)
        self.clients.append(client)
        client.initialize()
        options = {"prompt": "Exercise explicitly selected credentials", "base_url": self.url,
                   "model": "credential-fixture", "api": "chat-completions", "stream": False,
                   "allow_http_loopback": True, "credential": self.reference}
        self.setup_responses({"status": 401, "body": {"error": self.first}})
        failed = client.finished(client.start(options))
        self.assertEqual(failed["exit_code"], 1)
        task = failed["result"]["task_id"]
        self.assertEqual(self.header_key(self.requests()[-1]), self.first)
        self.setup_responses({"body": fixture.reply()})
        accepted = client.rpc("tasks/resume", {"task_id": task, "options": {
            "allow_http_loopback": True, "credential": self.reference}})
        completed = client.finished(accepted["operation_id"])
        self.assertEqual(completed["exit_code"], 0)
        self.assertEqual(completed["result"]["status"], "completed")
        self.assertEqual([self.header_key(request) for request in self.requests()], [self.first, self.first])
        self.assert_secrets_absent()

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux Secret Service session transport")
    def test_unavailable_secret_service_does_not_fall_back_to_environment(self):
        unavailable = dict(self.env, DBUS_SESSION_BUS_ADDRESS="unix:path=/nonexistent-codestudio-test-bus-" + uuid.uuid4().hex)
        result = self.invoke("credential-status", "--credential", self.system_reference,
                             env=unavailable, expected=2)
        self.assertEqual(result["state"], "unavailable")
        self.assertFalse((self.workspace / ".codestudio").exists())
        self.invoke(*self.run_args("--credential", self.system_reference), env=unavailable, expected=2)
        self.assertEqual(self.requests(), [])
        self.assert_secrets_absent()

    @unittest.skipUnless(sys.platform.startswith("linux") and
                         os.environ.get("CODESTUDIO_TEST_PRIVATE_SECRET_SERVICE") == "1" and
                         os.environ.get("CODESTUDIO_TEST_KEYRING_DAEMON"),
                         "requires explicitly owned private Linux Secret Service")
    def test_locked_private_collection_refuses_read_update_delete_and_retains_secret(self):
        daemon = pathlib.Path(os.environ["CODESTUDIO_TEST_KEYRING_DAEMON"])
        self.assertTrue(daemon.is_absolute() and daemon.is_file(), "private keyring daemon must be an existing absolute path")
        self.assertTrue(self.env.get("DBUS_SESSION_BUS_ADDRESS", "").startswith("unix:"))
        for variable in ("XDG_DATA_HOME", "XDG_RUNTIME_DIR"):
            self.assertTrue(pathlib.Path(self.env.get(variable, "")).is_absolute(),
                            "private runner must set absolute XDG directories")
        self.require_system()

        def dbus(method, *arguments):
            result = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.secrets",
                "--object-path", "/org/freedesktop/secrets", "--method", "org.freedesktop.Secret.Service." + method,
                *arguments], env=self.env, capture_output=True, timeout=10, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout.strip()

        alias = dbus("ReadAlias", "default")
        match = re.fullmatch(r"\(objectpath '(/org/freedesktop/secrets/collection/[A-Za-z0-9_]+)',\)", alias)
        self.assertIsNotNone(match, "default collection did not have a concrete owned-service path")
        collection = match.group(1)
        try:
            locked = dbus("Lock", "['" + collection + "']")
            self.assertIn(collection, locked)
            self.assertNotIn("/prompt/", locked, "lock unexpectedly requested an interactive prompt")
            self.assertEqual(self.invoke("credential-status", "--credential", self.system_reference,
                                         expected=2)["state"], "locked")
            self.assertEqual(self.invoke("credential-set", "--name", self.name, "--stdin",
                                         input=self.second.encode(), expected=2)["state"], "locked")
            self.assertEqual(self.invoke("credential-delete", "--name", self.name,
                                         expected=2)["state"], "locked")
            self.invoke(*self.run_args("--credential", self.system_reference), expected=2)
            self.assertEqual(self.requests(), [])
        finally:
            unlock = pathlib.Path(os.environ["CODESTUDIO_TEST_KEYRING_UNLOCK"])
            self.assertTrue(unlock.is_absolute() and unlock.is_file())
            private_env = dict(self.env, CODESTUDIO_TEST_PRIVATE_SECRET_SERVICE="1")
            unlocked = subprocess.run([str(unlock), collection], env=private_env,
                capture_output=True, timeout=15)
            self.assertEqual(unlocked.returncode, 0, unlocked.stdout + unlocked.stderr)
        self.assertEqual(self.invoke("credential-status", "--credential", self.system_reference)["state"], "available")
        self.setup_responses({"body": fixture.reply()})
        self.invoke(*self.run_args("--credential", self.system_reference))
        self.assertEqual(self.header_key(self.requests()[-1]), self.first)
        self.assert_secrets_absent()

    def test_system_store_roundtrip_replace_and_delete(self):
        self.require_system()
        self.assertEqual(self.invoke("credential-status", "--credential", self.system_reference)["state"], "available")
        self.setup_responses({"body": fixture.reply()})
        self.invoke(*self.run_args("--credential", self.system_reference))
        self.assertEqual(self.header_key(self.requests()[-1]), self.first)
        self.set_system(self.second)
        self.setup_responses({"body": fixture.reply()})
        self.invoke(*self.run_args("--credential", self.system_reference))
        self.assertEqual(self.header_key(self.requests()[-1]), self.second)
        maximum = "max-" + "a" * 2044
        self.secrets += (maximum,)
        self.invoke("credential-set", "--name", self.name, "--stdin", input=maximum.encode())
        self.setup_responses({"body": fixture.reply()})
        self.invoke(*self.run_args("--credential", self.system_reference))
        self.assertEqual(self.header_key(self.requests()[-1]), maximum)
        self.assertEqual(self.invoke("credential-delete", "--name", self.name)["state"], "deleted")
        self.assertEqual(self.invoke("credential-delete", "--name", self.name)["state"], "missing")
        self.assertEqual(self.invoke("credential-status", "--credential", self.system_reference)["state"], "missing")
        self.assert_secrets_absent()

    def test_system_rotation_does_not_change_in_flight_task_key(self):
        self.require_system()
        (self.workspace / "input.txt").write_text("task input", encoding="utf-8")
        release = threading.Event()
        self.releases.append(release)
        self.setup_responses({"body": fixture.reply(tool=("read_file", {"path": "input.txt"})), "release": release},
                             {"body": fixture.reply()})
        process = self.start(*self.run_args("--credential", self.system_reference))
        self.wait_for(lambda: len(self.requests()) == 1, process)
        self.set_system(self.second)
        release.set()
        out, err = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 0, out + err)
        for secret in self.secrets:
            self.assertNotIn(secret.encode(), out + err)
        self.assertEqual([self.header_key(request) for request in self.requests()], [self.first, self.first])
        self.setup_responses({"body": fixture.reply()})
        self.invoke(*self.run_args("--credential", self.system_reference))
        self.assertEqual(self.header_key(self.requests()[-1]), self.second)
        self.assert_secrets_absent()

    def test_running_worker_resolves_rotated_system_reference_when_job_starts(self):
        self.require_system()
        self.setup_responses({"status": 503})
        task = self.invoke(*self.run_args("--credential", self.system_reference), expected=1)["task_id"]
        job = self.invoke("schedule-resume", task, "--workspace", self.workspace,
                          "--allow-http-loopback", "--delay-ms", "3000")
        process = self.start("scheduler-run", "--workspace", self.workspace, "--poll-ms", "25")
        self.wait_for(lambda: self.invoke("scheduler-status", "--workspace", self.workspace)["state"] == "running", process)
        self.set_system(self.second)
        self.setup_responses({"body": fixture.reply()})
        self.wait_for(lambda: self.invoke("schedule-show", job["id"], "--workspace", self.workspace)["status"] == "completed", process)
        self.invoke("scheduler-stop", "--workspace", self.workspace)
        out, err = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 0, out + err)
        for secret in self.secrets:
            self.assertNotIn(secret.encode(), out + err)
        self.assertEqual(self.header_key(self.requests()[-1]), self.second)
        self.assertEqual(len(self.requests()), 2)
        self.assert_secrets_absent()


if __name__ == "__main__":
    unittest.main(verbosity=2)
