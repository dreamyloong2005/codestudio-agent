"""Real persistent scheduler, CLI, HTTP and crash boundaries without a live model.

Importing the resume fixture consumes the executable argument. Helpers are
assigned individually so its existing tests are not collected a second time.
"""
import json
import os
import pathlib
import sqlite3
import subprocess
import sys
import threading
import time
import unittest
from contextlib import closing
import resume_integration as fixture


class SchedulerTests(unittest.TestCase):
    invoke = fixture.ResumeTests.invoke
    run_args = fixture.ResumeTests.run_args
    resume_args = fixture.ResumeTests.resume_args
    setup_responses = fixture.ResumeTests.setup_responses
    requests = fixture.ResumeTests.requests
    start = fixture.ResumeTests.start
    kill = staticmethod(fixture.ResumeTests.kill)
    wait_for = fixture.ResumeTests.wait_for
    sql_rows = fixture.ResumeTests.sql_rows
    sql_execute = fixture.ResumeTests.sql_execute
    show = fixture.ResumeTests.show
    failed_task = fixture.ResumeTests.failed_task
    configure_mcp = fixture.ResumeTests.configure_mcp

    def setUp(self):
        fixture.ResumeTests.setUp(self)
        self.worker_started = False

    def tearDown(self):
        try:
            if self.worker_started:
                self.invoke(*self.worker_args("scheduler-stop"))
                self.wait_for(lambda: self.invoke(*self.worker_args("scheduler-status"))["state"] == "stopped")
        finally:
            fixture.ResumeTests.tearDown(self)

    def worker_args(self, command="scheduler-run", *extra):
        return [command, "--workspace", self.workspace, "--state", self.database, *extra]

    def schedule(self, task, *extra, expected=0, loopback=True):
        return self.invoke("schedule-resume", task, "--workspace", self.workspace, "--state", self.database,
            *(["--allow-http-loopback"] if loopback else []), *extra, expected=expected)

    def job(self, job_id):
        return self.invoke("schedule-show", job_id, "--workspace", self.workspace, "--state", self.database)

    def cancel(self, job_id):
        return self.invoke("schedule-cancel", job_id, "--workspace", self.workspace, "--state", self.database)

    def once(self):
        result = self.invoke(*self.worker_args("scheduler-run", "--once"))
        self.assertIn(result["processed"], (0, 1))
        return result

    def start_worker(self):
        process = self.start(*self.worker_args("scheduler-run", "--poll-ms", "25"))
        self.worker_started = True
        self.wait_for(lambda: self.invoke(*self.worker_args("scheduler-status"))["state"] == "running", process)
        return process

    def stop_worker(self, process=None):
        self.assertTrue(self.invoke(*self.worker_args("scheduler-stop"))["stop_requested"])
        self.wait_for(lambda: self.invoke(*self.worker_args("scheduler-status"))["state"] == "stopped")
        if process:
            output, error = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0, (output, error))
            self.assertNotIn(fixture.TOKEN.encode(), output + error)
        self.worker_started = False

    def wait_due(self, job):
        self.wait_for(lambda: time.time_ns() // 1_000_000 >= job["due_at_ms"])

    def database_snapshot(self):
        with closing(sqlite3.connect(self.database)) as database:
            return "\n".join(database.iterdump())

    def proposal_arguments(self):
        # Rejection exercises proposal waiting without depending on LPAC or
        # executing this fixture as a build process.
        return {"manifest": {"version": 2, "inputs": ["input.txt"],
            "mounts": [{"name": "fixture", "source": str(fixture.PROCESS.parent),
                        "inputs": [fixture.PROCESS.name]}],
            "steps": [{"executable": "${fixture}/" + fixture.PROCESS.name, "arguments": [], "cwd": "."}],
            "outputs": ["out"], "artifact_directory": "artifacts"}, "reason": "Review the bounded fixture build"}

    def waiting_task(self, max_turns=4, api="responses"):
        (self.workspace / "input.txt").write_text("source", encoding="utf-8")
        self.setup_responses({"body": fixture.reply(api, tool=("propose_build", self.proposal_arguments()))})
        result = self.invoke(*self.run_args("--allow-build-proposals", "--max-turns", str(max_turns), api=api), expected=3)
        self.assertEqual(result["status"], "waiting_approval")
        action = self.show(result["task_id"])["actions"][0]
        return result["task_id"], action

    def reject(self, task, action):
        return self.invoke("build-reject", task, "--workspace", self.workspace,
                           "--action", action["id"], "--digest", action["result"]["output"]["digest"])

    def test_three_protocols_queued_resume_survives_the_creating_process(self):
        (self.workspace / "input.txt").write_text("persisted tool input", encoding="utf-8")
        for api in fixture.APIS:
            with self.subTest(api=api):
                self.setup_responses({"body": fixture.reply(api, tool=("read_file", {"path": "input.txt"}))}, {"status": 503})
                failed = self.invoke(*self.run_args("--max-turns", "4", api=api), expected=1)
                task = failed["task_id"]
                previous = self.requests()[-1]["body"]
                queued = self.schedule(task)
                self.assertEqual((queued["task_id"], queued["status"], queued["attempts"]), (task, "queued", 0))
                self.assertGreater(queued["deadline_at_ms"], queued["due_at_ms"])
                listed = self.invoke(*self.worker_args("schedule-list"))["items"]
                self.assertIn(queued["id"], [item["id"] for item in listed])
                before = len(self.requests())
                self.setup_responses({"body": fixture.reply(api)})
                self.assertEqual(self.once()["processed"], 1)
                done = self.job(queued["id"])
                self.assertEqual((done["status"], done["attempts"]), ("completed", 1))
                self.assertEqual((self.show(task)["status"], self.show(task)["checkpoint"]["turns"]), ("completed", 3))
                self.assertEqual(len(self.requests()), before + 1)
                self.assertEqual(self.requests()[-1]["body"], previous)
                self.assertEqual(len(self.show(task)["actions"]), 1)
                self.assertEqual(self.once()["processed"], 0)
                self.assertEqual(len(self.requests()), before + 1)
        self.assertEqual(self.sql_rows("PRAGMA user_version")[0][0], 7)
        self.assertNotIn(fixture.TOKEN, self.database_snapshot())

    def test_delay_and_queued_cancellation_never_dispatch_early(self):
        task = self.failed_task()
        queued = self.schedule(task, "--delay-ms", "1500")
        requests = self.requests()
        self.setup_responses({"body": fixture.reply()})
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.requests(), requests)
        self.assertEqual(self.job(queued["id"])["attempts"], 0)
        self.wait_due(queued)
        self.assertEqual(self.once()["processed"], 1)
        self.assertEqual(self.job(queued["id"])["status"], "completed")
        second = self.failed_task()
        cancelled = self.schedule(second, "--delay-ms", "1500")
        self.cancel(cancelled["id"])
        self.cancel(cancelled["id"])
        requests = self.requests()
        self.assertEqual(self.job(cancelled["id"])["status"], "cancelled")
        self.wait_due(cancelled)
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.requests(), requests)
        self.assertEqual(self.job(cancelled["id"])["attempts"], 0)

    def test_duplicate_active_schedule_is_rejected_and_manual_resume_revokes_it(self):
        task = self.failed_task()
        queued = self.schedule(task, "--delay-ms", "1500")
        self.schedule(task, expected=2)
        self.setup_responses({"body": fixture.reply()})
        self.invoke(*self.resume_args(task))
        self.assertEqual(self.job(queued["id"])["status"], "cancelled")
        requests = self.requests()
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.requests(), requests)

    def test_invalid_schedule_parameters_have_no_persistent_or_model_effects(self):
        task = self.failed_task()
        before, requests = self.database_snapshot(), self.requests()
        for extra in (("--delay-ms", "-1"), ("--deadline-ms", "0"),
                      ("--delay-ms", "1000", "--deadline-ms", "1000"),
                      ("--max-attempts", "0"), ("--max-attempts", "101"),
                      ("--retry-delay-ms", "0"), ("--prompt", "Run an unrelated task"),
                      ("--api-key", "do-not-persist-this-option")):
            with self.subTest(extra=extra):
                self.schedule(task, *extra, expected=2)
                self.assertEqual(self.database_snapshot(), before)
                self.assertEqual(self.requests(), requests)
        self.schedule("missing-task", expected=2)
        self.assertEqual(self.database_snapshot(), before)

    def test_new_proposal_waits_for_a_new_explicit_decision(self):
        task, first_action = self.waiting_task()
        queued = self.schedule(task, "--allow-build-proposals")
        requests = self.requests()
        for _ in range(2):
            self.assertEqual(self.once()["processed"], 0)
            self.assertEqual(self.job(queued["id"])["attempts"], 0)
        self.assertEqual(self.requests(), requests)
        self.reject(task, first_action)
        self.setup_responses({"body": fixture.reply("responses", tool=("propose_build", self.proposal_arguments()), call_id="second-proposal")})
        self.assertEqual(self.once()["processed"], 1)
        waiting = self.job(queued["id"])
        self.assertEqual((waiting["status"], waiting["attempts"]), ("waiting_approval", 1))
        actions = self.show(task)["actions"]
        second = next(action for action in actions if action["id"] != first_action["id"])
        self.assertEqual(second["continuation"]["status"], "pending")
        requests = self.requests()
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.requests(), requests)
        self.reject(task, second)
        self.setup_responses({"body": fixture.reply("responses")})
        self.assertEqual(self.once()["processed"], 1)
        self.assertEqual((self.job(queued["id"])["status"], self.job(queued["id"])["attempts"]), ("completed", 2))
        consumed = [event["payload"]["action_id"] for event in self.show(task)["events"] if event["kind"] == "build.proposal.consumed"]
        self.assertCountEqual(consumed, [first_action["id"], second["id"]])
        self.assertFalse((self.workspace / "artifacts").exists())

    def test_model_failures_retry_with_backoff_and_bounded_attempts(self):
        task = self.failed_task("--max-turns", "8")
        queued = self.schedule(task, "--max-attempts", "3", "--retry-delay-ms", "1000")
        self.setup_responses({"status": 503}, {"status": 503}, {"status": 503})
        first_started = time.time_ns() // 1_000_000
        self.assertEqual(self.once()["processed"], 1)
        retry = self.job(queued["id"])
        self.assertEqual((retry["status"], retry["attempts"]), ("queued", 1))
        self.assertGreaterEqual(retry["due_at_ms"], first_started + 1000)
        requests = self.requests()
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.requests(), requests)
        self.wait_due(retry)
        second_started = time.time_ns() // 1_000_000
        self.assertEqual(self.once()["processed"], 1)
        retry = self.job(queued["id"])
        self.assertEqual((retry["status"], retry["attempts"]), ("queued", 2))
        self.assertGreaterEqual(retry["due_at_ms"], second_started + 2000)
        self.wait_due(retry)
        self.assertEqual(self.once()["processed"], 1)
        exhausted = self.job(queued["id"])
        self.assertEqual((exhausted["status"], exhausted["attempts"]), ("exhausted", 3))
        self.assertEqual(self.show(task)["checkpoint"]["turns"], 4)
        requests = self.requests()
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.requests(), requests)

    def test_expired_job_and_model_budget_never_gain_new_turns(self):
        task = self.failed_task("--max-turns", "1")
        self.schedule(task, "--max-turns", "2", expected=2)
        requests = self.requests()
        self.schedule(task, expected=2)
        self.assertEqual(self.requests(), requests)
        pending_budget, action = self.waiting_task(max_turns=1)
        queued = self.schedule(pending_budget, "--allow-build-proposals")
        self.reject(pending_budget, action)
        requests = self.requests()
        self.once()
        self.assertEqual(self.job(queued["id"])["status"], "exhausted")
        self.assertEqual(self.show(pending_budget)["checkpoint"]["turns"], 1)
        self.assertEqual(self.requests(), requests)
        pending, _ = self.waiting_task()
        expiring = self.schedule(pending, "--allow-build-proposals", "--deadline-ms", "100")
        self.wait_for(lambda: time.time_ns() // 1_000_000 >= expiring["deadline_at_ms"])
        requests = self.requests()
        self.once()
        self.assertEqual((self.job(expiring["id"])["status"], self.job(expiring["id"])["attempts"]), ("expired", 0))
        self.assertEqual(self.requests(), requests)

    def test_grants_and_changed_mcp_configuration_block_before_launch(self):
        manifest, marker, grants = self.configure_mcp()
        task = self.failed_task("--allow-write", *grants)
        marker.unlink()
        requests = self.requests()
        self.schedule(task, expected=2)
        self.schedule(task, "--allow-write", expected=2)
        self.schedule(task, "--allow-write", *grants, "--allow-process", fixture.PROCESS, expected=2)
        self.assertEqual(self.requests(), requests)
        self.assertFalse(marker.exists())
        queued = self.schedule(task, "--allow-write", *grants)
        manifest.write_text(manifest.read_text(encoding="utf-8") + "\n", encoding="utf-8")
        self.once()
        self.assertEqual(self.job(queued["id"])["status"], "blocked")
        self.assertEqual(self.requests(), requests)
        self.assertFalse(marker.exists())

    def test_running_job_cancel_is_persistent_and_does_not_retry(self):
        task = self.failed_task()
        queued = self.schedule(task)
        release = threading.Event()
        self.releases.append(release)
        self.setup_responses({"body": fixture.reply(), "release": release})
        process = self.start_worker()
        self.wait_for(lambda: len(self.requests()) == 2, process)
        self.cancel(queued["id"])
        self.wait_for(lambda: self.job(queued["id"])["status"] == "cancelled", process)
        self.assertEqual(self.show(task)["status"], "cancelled")
        release.set()
        requests = self.requests()
        self.stop_worker(process)
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.requests(), requests)

    def test_worker_stop_cancels_active_job_and_preserves_other_queue(self):
        first, second = self.failed_task(), self.failed_task()
        active = self.schedule(first)
        other = self.schedule(second, "--delay-ms", "1500")
        release = threading.Event()
        self.releases.append(release)
        self.setup_responses({"body": fixture.reply(), "release": release})
        process = self.start_worker()
        self.wait_for(lambda: len(self.requests()) == 3, process)
        self.stop_worker(process)
        release.set()
        self.assertEqual(self.job(active["id"])["status"], "cancelled")
        self.assertEqual((self.job(other["id"])["status"], self.job(other["id"])["attempts"]), ("queued", 0))
        self.setup_responses({"body": fixture.reply()})
        self.wait_due(other)
        self.assertEqual(self.once()["processed"], 1)
        self.assertEqual(self.job(other["id"])["status"], "completed")

    def test_two_workers_cannot_claim_the_same_job(self):
        task = self.failed_task()
        queued = self.schedule(task)
        release = threading.Event()
        self.releases.append(release)
        self.setup_responses({"body": fixture.reply(), "release": release})
        process = self.start_worker()
        self.wait_for(lambda: len(self.requests()) == 2, process)
        self.invoke(*self.worker_args("scheduler-run", "--once"), expected=2)
        self.assertEqual(len(self.requests()), 2)
        self.assertEqual(self.job(queued["id"])["attempts"], 1)
        release.set()
        self.wait_for(lambda: self.job(queued["id"])["status"] == "completed", process)
        self.stop_worker(process)

    def test_idle_worker_stop_preserves_delayed_and_approval_waiting_jobs(self):
        delayed_task = self.failed_task()
        delayed = self.schedule(delayed_task, "--delay-ms", "60000")
        waiting_task, _ = self.waiting_task()
        waiting = self.schedule(waiting_task, "--allow-build-proposals")
        requests = self.requests()
        process = self.start_worker()
        self.stop_worker(process)
        for original in (delayed, waiting):
            with self.subTest(job=original["id"]):
                preserved = self.job(original["id"])
                self.assertIn(preserved["status"], ("queued", "waiting_approval"))
                self.assertEqual(preserved["attempts"], 0)
                for field in ("task_id", "due_at_ms", "deadline_at_ms", "max_attempts"):
                    self.assertEqual(preserved[field], original[field])
        self.assertEqual(self.requests(), requests)
        self.assertEqual(self.show(delayed_task)["status"], "failed")
        self.assertEqual(self.show(waiting_task)["status"], "waiting_approval")

    def test_foreground_worker_keeps_processing_after_stdin_eof(self):
        task, action = self.waiting_task()
        queued = self.schedule(task, "--allow-build-proposals")
        # Complete unrelated approval before the worker's polling can acquire
        # its short execution lock. Gate model completion across stdin EOF so
        # this test checks worker lifetime without racing an approval mutation.
        self.reject(task, action)
        release = threading.Event()
        self.releases.append(release)
        self.setup_responses({"body": fixture.reply("responses"), "release": release})
        process = subprocess.Popen([str(fixture.EXE), *map(str, self.worker_args("scheduler-run", "--poll-ms", "25"))],
                                   env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(process)
        self.worker_started = True
        self.wait_for(lambda: self.invoke(*self.worker_args("scheduler-status"))["state"] == "running", process)
        self.wait_for(lambda: len(self.requests()) == 2, process)
        process.stdin.close()
        process.stdin = None  # communicate() must not flush the intentionally closed pipe.
        release.set()
        self.wait_for(lambda: self.job(queued["id"])["status"] == "completed", process)
        self.assertIsNone(process.poll(), "Worker lifetime was tied to its parent's stdin")
        self.assertEqual(self.show(task)["status"], "completed")
        self.assertEqual(len(self.requests()), 2)
        self.stop_worker(process)

    def test_custom_state_scheduler_lock_is_protected_from_write_file(self):
        self.database = self.workspace / "custom-state.sqlite3"
        self.setup_responses({"status": 503})
        first = self.invoke(*self.run_args("--state", self.database, "--allow-write"), expected=1)
        queued = self.schedule(first["task_id"], "--allow-write")
        lock_name = self.database.name + ".scheduler.run.lock"
        attempted = "must not overwrite scheduler ownership"
        self.setup_responses({"body": fixture.reply(tool=("write_file", {"path": lock_name, "content": attempted}))},
                             {"body": fixture.reply()})
        self.assertEqual(self.once()["processed"], 1)
        detail = self.invoke("show", first["task_id"], "--workspace", self.workspace, "--state", self.database)
        self.assertEqual((detail["status"], detail["actions"][0]["status"]), ("needs_attention", "denied"))
        self.assertEqual(detail["actions"][0]["effect"], "none")
        self.assertEqual(self.job(queued["id"])["status"], "blocked")
        scheduler_lock = self.workspace / lock_name
        if scheduler_lock.exists():
            self.assertNotEqual(scheduler_lock.read_bytes(), attempted.encode())
        self.assertFalse((self.workspace / ".codestudio" / "state.sqlite3").exists())

    def test_model_pending_worker_crash_recovers_only_owned_task_without_replaying_tools(self):
        (self.workspace / "input.txt").write_text("original", encoding="utf-8")
        self.setup_responses({"body": fixture.reply(tool=("read_file", {"path": "input.txt"}))}, {"status": 503})
        first = self.invoke(*self.run_args("--max-turns", "5"), expected=1)
        queued = self.schedule(first["task_id"])
        release = threading.Event()
        self.releases.append(release)
        self.setup_responses({"body": fixture.reply(), "release": release})
        process = self.start_worker()
        self.wait_for(lambda: len(self.requests()) == 3, process)
        before = self.requests()[-1]["body"]
        self.kill(process)
        self.worker_started = False
        release.set()
        self.setup_responses({"body": fixture.reply()})
        self.once()
        job = self.job(queued["id"])
        if job["status"] == "queued":
            self.wait_due(job)
            self.once()
        self.assertEqual((self.job(queued["id"])["status"], self.job(queued["id"])["attempts"]), ("completed", 2))
        self.assertEqual(self.requests()[-1]["body"], before)
        detail = self.show(first["task_id"])
        self.assertEqual((detail["status"], detail["checkpoint"]["turns"]), ("completed", 4))
        self.assertEqual(len(detail["actions"]), 1)
        self.assertEqual(sum(event["kind"] == "action.dispatched" for event in detail["events"]), 1)

    def test_dispatched_process_crash_blocks_scheduler_without_restarting_mcp(self):
        manifest, marker, mcp_grants = self.configure_mcp()
        grants = ["--allow-process", fixture.PROCESS, "--process-timeout-ms", "15000", *mcp_grants]
        task = self.failed_task(*grants)
        queued = self.schedule(task, *grants)
        self.setup_responses({"body": fixture.reply(tool=("run_process", {
            "executable": str(fixture.PROCESS), "arguments": ["sleep", "10000"]}))})
        process = self.start_worker()
        self.wait_for(lambda: self.sql_rows("SELECT id FROM actions WHERE task_id=? AND status='dispatched'", (task,)), process)
        self.kill(process)
        self.worker_started = False
        marker.unlink()
        requests = self.requests()
        self.once()
        self.assertEqual(self.job(queued["id"])["status"], "blocked")
        self.assertEqual(self.show(task)["actions"][0]["effect"], "unknown")
        self.assertEqual(self.once()["processed"], 0)
        self.assertFalse(marker.exists())
        self.assertEqual(self.requests(), requests)

    def test_scheduler_recovery_does_not_recover_unrelated_running_task(self):
        scheduled_task = self.failed_task()
        self.schedule(scheduled_task)
        release = threading.Event()
        self.releases.append(release)
        self.setup_responses({"body": fixture.reply(), "release": release})
        unrelated = self.start(*self.run_args())
        self.wait_for(lambda: len(self.requests()) == 2, unrelated)
        unrelated_task = self.sql_rows("SELECT id FROM tasks WHERE status='running'")[0][0]
        self.assertNotEqual(unrelated_task, scheduled_task)
        self.kill(unrelated)
        release.set()
        def snapshot():
            return (self.sql_rows("SELECT * FROM tasks WHERE id=?", (unrelated_task,)),
                    self.sql_rows("SELECT * FROM task_checkpoints WHERE task_id=?", (unrelated_task,)),
                    self.sql_rows("SELECT * FROM events WHERE task_id=? ORDER BY seq", (unrelated_task,)))
        before, requests = snapshot(), self.requests()
        self.once()
        self.assertEqual(snapshot(), before, "The scheduler recovered a task outside its own claimed jobs")
        self.assertEqual(self.requests(), requests)

    @unittest.skipUnless(os.name == "nt" or sys.platform.startswith("linux"), "native detached worker startup")
    def test_hidden_worker_survives_control_process_eof_and_stops_by_owned_control(self):
        task, action = self.waiting_task()
        queued = self.schedule(task, "--allow-build-proposals")
        before, requests = self.database_snapshot(), self.requests()
        inherited = self.workspace / "must-not-be-inherited.txt"
        inherited.write_text("private parent handle")
        with inherited.open("rb") as parent_handle:
            launcher = subprocess.Popen([str(fixture.EXE), *map(str, self.worker_args("scheduler-start", "--poll-ms", "25"))],
                env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                **({"pass_fds": (parent_handle.fileno(),)} if os.name != "nt" else {}))
        self.children.append(launcher)
        self.worker_started = True
        output, error = launcher.communicate(input=b"", timeout=10)
        if launcher.returncode == 2:
            rejected = json.loads(output)
            unavailable = "Cannot detach scheduler from the parent Job; run scheduler-run from an independent terminal"
            if rejected.get("error") == unavailable:
                self.worker_started = False
                self.assertEqual(self.invoke(*self.worker_args("scheduler-status"))["state"], "stopped")
                self.assertIn(self.job(queued["id"])["status"], ("queued", "waiting_approval"))
                self.assertEqual(self.job(queued["id"])["attempts"], 0)
                self.assertEqual(self.requests(), requests)
                self.assertEqual(self.database_snapshot(), before)
                self.skipTest("The enclosing Windows Job prohibits detached worker startup; refusal left the queue untouched")
        self.assertEqual(launcher.returncode, 0, (output, error))
        started = json.loads(output)
        self.assertTrue(started["started"])
        self.assertNotEqual(started["pid"], launcher.pid)
        control = self.invoke(*self.worker_args("scheduler-status"))
        self.assertEqual((control["state"], control["pid"], control["owner"]), ("running", started["pid"], started["owner"]))
        if os.name != "nt":
            fd_root = pathlib.Path("/proc") / str(started["pid"]) / "fd"
            for number in (0, 1, 2):
                self.assertEqual(os.readlink(fd_root / str(number)), "/dev/null")
            open_paths = []
            for path in fd_root.iterdir():
                try:
                    open_paths.append(os.readlink(path))
                except FileNotFoundError:
                    pass  # The worker may close a descriptor during the snapshot.
            self.assertNotIn(str(inherited), open_paths)
        self.assertEqual(len(self.requests()), 1)
        self.setup_responses({"body": fixture.reply("responses")})
        self.reject(task, action)
        self.wait_for(lambda: self.job(queued["id"])["status"] == "completed")
        self.assertEqual(self.show(task)["status"], "completed")
        self.stop_worker()
        stopped = self.invoke(*self.worker_args("scheduler-status"))
        self.assertEqual((stopped["state"], stopped["owner"]), ("stopped", started["owner"]))

    def test_worker_uses_current_secret_without_persisting_it_in_schedule(self):
        task = self.failed_task()
        queued = self.schedule(task)
        self.assertNotIn(fixture.TOKEN, self.database_snapshot())
        fresh = "scheduler-current-credential-never-persist"
        self.setup_responses({"body": fixture.reply()})
        result = self.invoke(*self.worker_args("scheduler-run", "--once"), env=dict(self.env, CODESTUDIO_API_KEY=fresh))
        self.assertEqual(result["processed"], 1)
        self.assertEqual(self.requests()[-1]["headers"]["Authorization"], "Bearer " + fresh)
        self.assertEqual(self.job(queued["id"])["status"], "completed")
        self.assertNotIn(fresh, self.database_snapshot())


if __name__ == "__main__":
    unittest.main(verbosity=2)
