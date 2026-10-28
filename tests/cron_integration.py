"""Recurring fresh tasks through the real CLI, scheduler, HTTP and SQLite.

Models are local fixtures. Each test uses a daily UTC slot one hour in the past,
so the next real boundary is approximately 23 hours away. Due timestamps are
moved in the test database without waiting for a wall-clock schedule boundary.
"""
import json
import os
import signal
import subprocess
import sys
import sqlite3
import threading
import time
import unittest
from contextlib import closing
from datetime import datetime, timezone
from zoneinfo import ZoneInfo
import resume_integration as fixture

DAY_MS = 24 * 60 * 60 * 1000


class CronTests(unittest.TestCase):
    invoke = fixture.ResumeTests.invoke
    setup_responses = fixture.ResumeTests.setup_responses
    requests = fixture.ResumeTests.requests
    start = fixture.ResumeTests.start
    kill = staticmethod(fixture.ResumeTests.kill)
    wait_for = fixture.ResumeTests.wait_for
    sql_rows = fixture.ResumeTests.sql_rows
    sql_execute = fixture.ResumeTests.sql_execute
    show = fixture.ResumeTests.show
    configure_mcp = fixture.ResumeTests.configure_mcp

    def setUp(self):
        fixture.ResumeTests.setUp(self)
        self.worker_started = False
        self.slot_ms = time.time_ns() // 60_000_000_000 * 60000 - 60 * 60 * 1000
        slot = datetime.fromtimestamp(self.slot_ms / 1000, timezone.utc)
        self.expression = f"{slot.minute} {slot.hour} * * *"

    def tearDown(self):
        try:
            if self.worker_started:
                self.invoke(*self.worker_args("scheduler-stop"))
                self.wait_for(lambda: self.invoke(*self.worker_args("scheduler-status"))["state"] == "stopped")
        finally:
            fixture.ResumeTests.tearDown(self)

    def worker_args(self, command="scheduler-run", *extra):
        return [command, "--workspace", self.workspace, "--state", self.database, *extra]

    def create_args(self, *extra, api="responses", expression=None, zone="UTC"):
        return ["cron-create", "--workspace", self.workspace, "--state", self.database,
                "--cron", self.expression if expression is None else expression, "--timezone", zone,
                "--prompt", "Perform this independent scheduled engineering check",
                "--base-url", self.url, "--model", "cron-fixture", "--api", api,
                "--allow-http-loopback", "--no-stream", *extra]

    def create(self, *extra, **kwargs):
        return self.invoke(*self.create_args(*extra, **kwargs))

    def cron(self, cron_id):
        return self.invoke("cron-show", cron_id, "--workspace", self.workspace, "--state", self.database)

    def control(self, cron_id, action, expected=0):
        return self.invoke("cron-" + action, cron_id, "--workspace", self.workspace,
                           "--state", self.database, expected=expected)

    def job(self, job_id):
        return self.invoke("schedule-show", job_id, "--workspace", self.workspace, "--state", self.database)

    def occurrences(self, cron_id):
        return self.sql_rows("SELECT scheduled_at_ms,task_id,job_id FROM cron_occurrences WHERE cron_id=? ORDER BY scheduled_at_ms",
                             (cron_id,))

    def due(self, cron_id, age_completed=False):
        """Move the schedule into the past, preserving a unique logical slot.

        Aging the single occurrence by one day frees the known latest slot
        without sleeping. Native Store tests verify exact clock boundaries.
        """
        with closing(sqlite3.connect(self.database)) as database:
            if age_completed:
                self.assertEqual(database.execute("SELECT count(*) FROM cron_occurrences WHERE cron_id=?", (cron_id,)).fetchone()[0], 1)
                database.execute("UPDATE cron_occurrences SET scheduled_at_ms=scheduled_at_ms-? WHERE cron_id=?", (DAY_MS, cron_id))
                database.execute("UPDATE cron_schedules SET last_due_at_ms=last_due_at_ms-? WHERE id=?", (DAY_MS, cron_id))
            database.execute("UPDATE cron_schedules SET next_due_at_ms=? WHERE id=?",
                             (self.slot_ms - DAY_MS, cron_id))
            database.commit()

    def once(self, env=None):
        result = self.invoke(*self.worker_args("scheduler-run", "--once"), env=env)
        self.assertIn(result["processed"], (0, 1))
        return result

    def start_worker(self):
        process = self.start(*self.worker_args("scheduler-run", "--poll-ms", "25"))
        self.worker_started = True
        self.wait_for(lambda: self.invoke(*self.worker_args("scheduler-status"))["state"] == "running", process)
        return process

    def stop_worker(self, process):
        self.invoke(*self.worker_args("scheduler-stop"))
        self.wait_for(lambda: self.invoke(*self.worker_args("scheduler-status"))["state"] == "stopped")
        output, error = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, (output, error))
        self.assertNotIn(fixture.TOKEN.encode(), output + error)
        self.worker_started = False

    def database_snapshot(self):
        with closing(sqlite3.connect(self.database)) as database:
            return "\n".join(database.iterdump())

    def proposal_arguments(self):
        return {"manifest": {"version": 2, "inputs": ["input.txt"],
            "mounts": [{"name": "fixture", "source": str(fixture.PROCESS.parent),
                        "inputs": [fixture.PROCESS.name]}],
            "steps": [{"executable": "${fixture}/" + fixture.PROCESS.name, "arguments": [], "cwd": "."}],
            "outputs": ["out"], "artifact_directory": "artifacts"}, "reason": "Review this occurrence"}

    def reject(self, task, action):
        return self.invoke("build-reject", task, "--workspace", self.workspace,
                           "--action", action["id"], "--digest", action["result"]["output"]["digest"])

    def detached_worker(self):
        result = self.invoke(*self.worker_args("scheduler-start", "--supervise", "--poll-ms", "20",
                                              "--max-restarts", "1", "--restart-delay-ms", "100"))
        self.worker_started = True
        self.assertTrue(result["started"])
        self.assertTrue(result["supervised"])
        self.assertNotEqual(result["pid"], result["supervisor_pid"])
        return result

    def stop_detached_worker(self):
        self.invoke(*self.worker_args("scheduler-stop"))
        # Successful foreground admission proves that the detached supervisor
        # and its child released both ownership locks, not just a database flag.
        def released():
            result = subprocess.run([str(fixture.EXE), *map(str, self.worker_args("scheduler-run", "--once"))],
                                    env=self.env, capture_output=True, timeout=5)
            return result.returncode == 0
        self.wait_for(released)
        self.worker_started = False

    def reject_with_running_worker(self, task, action):
        def accepted():
            result = subprocess.run([str(fixture.EXE), "build-reject", task, "--workspace", str(self.workspace),
                "--action", action["id"], "--digest", action["result"]["output"]["digest"]],
                env=self.env, capture_output=True, text=True, timeout=5)
            if result.returncode == 2 and "in use by another runner" in result.stdout:
                return False  # Admission failed before a decision; wait for the worker's short poll lock.
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return True
        self.wait_for(accepted)

    @unittest.skipUnless(sys.platform == "darwin", "macOS native detached cron integration")
    def test_detached_cron_timezone_coalescing_approval_and_independent_budgets(self):
        local = datetime.fromtimestamp(self.slot_ms / 1000, ZoneInfo("America/New_York"))
        (self.workspace / "input.txt").write_text("source", encoding="utf-8")
        created = self.create("--allow-build-proposals", "--max-turns", "2", "--max-runs", "2",
                              expression=f"{local.minute} {local.hour} * * *", zone="America/New_York")
        self.setup_responses({"body": fixture.reply("responses", tool=("propose_build", self.proposal_arguments()))})
        self.sql_execute("UPDATE cron_schedules SET next_due_at_ms=? WHERE id=?",
                         (self.slot_ms - 7 * DAY_MS, created["id"]))
        self.detached_worker()
        try:
            self.wait_for(lambda: self.occurrences(created["id"]))
            first = self.occurrences(created["id"])[0]
            self.wait_for(lambda: self.job(first[2])["status"] == "waiting_approval")
            self.assertEqual(first[0], self.slot_ms)
            self.assertEqual(self.cron(created["id"])["timezone"], "America/New_York")
            self.due(created["id"], age_completed=True)
            self.wait_for(lambda: self.cron(created["id"])["next_due_at_ms"] > self.slot_ms)
            self.assertEqual(len(self.occurrences(created["id"])), 1)
            self.assertEqual(len(self.requests()), 1)
            self.setup_responses({"body": fixture.reply("responses", text="first private result")})
            self.reject_with_running_worker(first[1], self.show(first[1])["actions"][0])
            self.wait_for(lambda: self.job(first[2])["status"] == "completed")
            self.setup_responses({"body": fixture.reply("responses", tool=("propose_build", self.proposal_arguments()),
                                                       call_id="fresh-background-proposal")})
            self.due(created["id"])
            self.wait_for(lambda: len(self.occurrences(created["id"])) == 2)
            second = self.occurrences(created["id"])[1]
            self.wait_for(lambda: self.job(second[2])["status"] == "waiting_approval")
            self.assertNotEqual(first[1], second[1])
            self.assertNotIn("first private result", json.dumps(self.requests()[-1]["body"]))
            action = self.show(second[1])["actions"][0]
            self.assertEqual(action["continuation"]["status"], "pending")
            self.setup_responses({"body": fixture.reply("responses")})
            self.reject_with_running_worker(second[1], action)
            self.wait_for(lambda: self.job(second[2])["status"] == "completed")
            self.assertEqual(self.cron(created["id"])["status"], "exhausted")
            for occurrence in (first, second):
                self.assertEqual(self.show(occurrence[1])["checkpoint"]["turns"], 2)
            self.assertEqual(len(self.requests()), 4)
            self.assertFalse((self.workspace / "artifacts").exists())
        finally:
            self.stop_detached_worker()

    @unittest.skipUnless(sys.platform == "darwin", "macOS native detached cron integration")
    def test_detached_supervisor_crash_blocks_unknown_effect_without_replay(self):
        manifest, marker, grants = self.configure_mcp()
        created = self.create("--allow-process", fixture.PROCESS, "--process-timeout-ms", "15000", *grants)
        self.setup_responses({"body": fixture.reply("responses", tool=("run_process", {
            "executable": str(fixture.PROCESS), "arguments": ["sleep", "10000"]}))})
        self.due(created["id"])
        started = self.detached_worker()
        try:
            self.wait_for(lambda: self.sql_rows("SELECT id FROM actions WHERE status='dispatched'"))
            occurrence = self.occurrences(created["id"])[0]
            state = self.invoke(*self.worker_args("scheduler-status"))
            self.assertEqual((state["pid"], state["owner"]), (started["pid"], started["owner"]))
            marker.unlink()
            requests = self.requests()
            os.kill(started["pid"], signal.SIGKILL)
            self.wait_for(lambda: self.job(occurrence[2])["status"] == "blocked")
            self.assertEqual(self.show(occurrence[1])["actions"][0]["effect"], "unknown")
            self.assertEqual(self.cron(created["id"])["status"], "paused")
            self.due(created["id"])
            self.stop_detached_worker()
            self.once()
            self.assertEqual(self.occurrences(created["id"]), [occurrence])
            self.assertEqual(self.requests(), requests)
            self.assertFalse(marker.exists())
        finally:
            if self.worker_started:
                self.stop_detached_worker()

    def test_enrollment_creates_only_a_template_and_readonly_listing(self):
        created = self.create("--name", "Weekday check", "--max-runs", "2", expression="0 9 * * 1-5")
        self.assertEqual((created["name"], created["status"], created["run_count"], created["max_runs"]),
                         ("Weekday check", "active", 0, 2))
        self.assertEqual(created["timezone"], "UTC")
        self.assertGreater(created["next_due_at_ms"], created["created_at_ms"])
        self.assertEqual(self.sql_rows("SELECT count(*) FROM tasks"), [(0,)])
        self.assertEqual(self.sql_rows("SELECT count(*) FROM scheduler_jobs"), [(0,)])
        self.assertEqual(self.requests(), [])
        before = self.database_snapshot()
        self.assertEqual(self.cron(created["id"]), created)
        listed = self.invoke(*self.worker_args("cron-list"))
        self.assertEqual([item["id"] for item in listed["items"]], [created["id"]])
        self.assertFalse(listed["has_more"])
        self.assertEqual(self.database_snapshot(), before)
        self.assertEqual(self.sql_rows("PRAGMA user_version"), [(7,)])
        self.assertNotIn(fixture.TOKEN, before)

    def test_three_protocols_each_occurrence_gets_fresh_task_history_and_budget(self):
        for api in fixture.APIS:
            with self.subTest(api=api):
                created = self.create("--max-turns", "1", "--max-runs", "2", api=api)
                requests_before = len(self.requests())
                self.setup_responses({"body": fixture.reply(api, text="first occurrence private result")})
                self.due(created["id"])
                self.assertEqual(self.once()["processed"], 1)
                first = self.occurrences(created["id"])[0]
                self.assertEqual(self.job(first[2])["status"], "completed")
                self.assertEqual(self.show(first[1])["checkpoint"]["turns"], 1)
                first_request = self.requests()[-1]["body"]
                self.setup_responses({"body": fixture.reply(api, text="second occurrence result")})
                self.due(created["id"], age_completed=True)
                self.assertEqual(self.once()["processed"], 1)
                occurrences = self.occurrences(created["id"])
                self.assertEqual(len(occurrences), 2)
                self.assertNotEqual(occurrences[0][1], occurrences[1][1])
                self.assertEqual(self.job(occurrences[1][2])["status"], "completed")
                self.assertEqual(self.show(occurrences[1][1])["checkpoint"]["turns"], 1)
                self.assertEqual(self.requests()[-1]["body"], first_request)
                self.assertNotIn("first occurrence private result", json.dumps(self.requests()[-1]["body"]))
                self.assertEqual(len(self.requests()), requests_before + 2)
                exhausted = self.cron(created["id"])
                self.assertEqual((exhausted["status"], exhausted["run_count"]), ("exhausted", 2))
                self.control(created["id"], "resume", expected=2)
                self.assertEqual(self.once()["processed"], 0)

    def test_missed_triggers_coalesce_and_worker_restart_never_duplicates_slot(self):
        created = self.create()
        self.sql_execute("UPDATE cron_schedules SET next_due_at_ms=? WHERE id=?",
                         (self.slot_ms - 7 * DAY_MS, created["id"]))
        self.setup_responses({"body": fixture.reply("responses")})
        self.assertEqual(self.once()["processed"], 1)
        occurrence = self.occurrences(created["id"])
        self.assertEqual(len(occurrence), 1)
        self.assertEqual(occurrence[0][0], self.slot_ms)
        # Even an accidentally repeated due marker cannot emit the same slot.
        self.due(created["id"])
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.occurrences(created["id"]), occurrence)
        self.assertEqual(self.cron(created["id"])["run_count"], 1)
        self.assertEqual(len(self.requests()), 1)

    def test_pause_resume_and_cancel_control_future_occurrences(self):
        created = self.create()
        self.assertEqual(self.control(created["id"], "pause")["status"], "paused")
        self.due(created["id"])
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.occurrences(created["id"]), [])
        resumed = self.control(created["id"], "resume")
        self.assertEqual(resumed["status"], "active")
        # Resume skips the paused past slot and returns the exact future daily
        # slot. The worker cannot legitimately reach it during this test.
        self.assertEqual(resumed["next_due_at_ms"], self.slot_ms + DAY_MS)
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.control(created["id"], "cancel")["status"], "cancelled")
        self.control(created["id"], "cancel")
        self.control(created["id"], "resume", expected=2)
        self.due(created["id"])
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.requests(), [])

    def test_approval_wait_does_not_overlap_and_next_occurrence_needs_own_decision(self):
        (self.workspace / "input.txt").write_text("source", encoding="utf-8")
        created = self.create("--allow-build-proposals", "--max-turns", "3", "--max-runs", "2")
        self.setup_responses({"body": fixture.reply("responses", tool=("propose_build", self.proposal_arguments()))})
        self.due(created["id"])
        self.assertEqual(self.once()["processed"], 1)
        first = self.occurrences(created["id"])[0]
        self.assertEqual(self.job(first[2])["status"], "waiting_approval")
        action = self.show(first[1])["actions"][0]
        # A later due slot while approval is pending must not create a task.
        self.due(created["id"], age_completed=True)
        requests = self.requests()
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(len(self.occurrences(created["id"])), 1)
        self.assertEqual(self.requests(), requests)
        self.reject(first[1], action)
        self.setup_responses({"body": fixture.reply("responses")})
        self.assertEqual(self.once()["processed"], 1)
        self.assertEqual(self.job(first[2])["status"], "completed")
        self.due(created["id"])
        self.setup_responses({"body": fixture.reply("responses", tool=("propose_build", self.proposal_arguments()),
                                                   call_id="new-occurrence-proposal")})
        self.assertEqual(self.once()["processed"], 1)
        second = self.occurrences(created["id"])[1]
        self.assertNotEqual(first[1], second[1])
        self.assertEqual(self.job(second[2])["status"], "waiting_approval")
        second_action = self.show(second[1])["actions"][0]
        self.assertNotEqual(second_action["id"], action["id"])
        self.assertEqual(second_action["continuation"]["status"], "pending")
        self.assertEqual(self.once()["processed"], 0)
        self.reject(second[1], second_action)
        self.setup_responses({"body": fixture.reply("responses")})
        self.assertEqual(self.once()["processed"], 1)
        self.assertEqual(self.job(second[2])["status"], "completed")
        self.assertEqual(self.cron(created["id"])["status"], "exhausted")
        self.assertFalse((self.workspace / "artifacts").exists())

    def test_pause_preserves_active_work_and_cancel_stops_it(self):
        for action in ("pause", "cancel"):
            with self.subTest(action=action):
                created = self.create()
                release = threading.Event()
                self.releases.append(release)
                before = len(self.requests())
                self.setup_responses({"body": fixture.reply("responses"), "release": release})
                self.due(created["id"])
                process = self.start_worker()
                self.wait_for(lambda: len(self.requests()) == before + 1, process)
                occurrence = self.occurrences(created["id"])[0]
                self.control(created["id"], action)
                if action == "pause":
                    self.assertEqual(self.job(occurrence[2])["status"], "running")
                    self.assertFalse(self.job(occurrence[2])["cancel_requested"])
                    release.set()
                    expected = "completed"
                else:
                    expected = "cancelled"
                self.wait_for(lambda: self.job(occurrence[2])["status"] == expected, process)
                release.set()
                self.assertEqual(self.show(occurrence[1])["status"], expected)
                self.stop_worker(process)
                self.due(created["id"])
                self.assertEqual(self.once()["processed"], 0)
                self.assertEqual(len(self.occurrences(created["id"])), 1)

    def test_worker_crash_preserves_receipt_without_duplicate_occurrence(self):
        created = self.create("--allow-write", "--max-turns", "4", "--retry-delay-ms", "100")
        release = threading.Event()
        self.releases.append(release)
        self.setup_responses({"body": fixture.reply("responses", tool=("write_file", {"path": "once.txt", "content": "original"}))},
                             {"body": fixture.reply("responses"), "release": release})
        self.due(created["id"])
        process = self.start_worker()
        self.wait_for(lambda: len(self.requests()) == 2, process)
        occurrence = self.occurrences(created["id"])[0]
        self.assertEqual(self.sql_rows("SELECT status,effect FROM actions"), [("succeeded", "applied")])
        pending_request = self.requests()[-1]["body"]
        self.kill(process)
        self.worker_started = False
        release.set()
        # A replay would overwrite this later user change.
        (self.workspace / "once.txt").write_text("user edit after crash", encoding="utf-8")
        self.setup_responses({"body": fixture.reply("responses")})
        self.once()
        job = self.job(occurrence[2])
        if job["status"] == "queued":
            self.wait_for(lambda: time.time_ns() // 1_000_000 >= job["due_at_ms"])
            self.once()
        self.assertEqual(self.job(occurrence[2])["status"], "completed")
        self.assertEqual(self.occurrences(created["id"]), [occurrence])
        self.assertEqual(self.cron(created["id"])["run_count"], 1)
        self.assertEqual(self.requests()[-1]["body"], pending_request)
        self.assertEqual((self.workspace / "once.txt").read_text(encoding="utf-8"), "user edit after crash")
        detail = self.show(occurrence[1])
        self.assertEqual(len(detail["actions"]), 1)
        self.assertEqual(sum(event["kind"] == "action.dispatched" for event in detail["events"]), 1)

    def test_changed_configuration_or_grants_blocks_and_pauses_series(self):
        for mutation in ("manifest", "grants"):
            with self.subTest(mutation=mutation):
                manifest, marker, grants = self.configure_mcp()
                created = self.create(*grants)
                marker.unlink()
                if mutation == "manifest":
                    manifest.write_text(manifest.read_text(encoding="utf-8") + "\n", encoding="utf-8")
                else:
                    argv = json.loads(self.sql_rows("SELECT resume_argv FROM cron_schedules WHERE id=?", (created["id"],))[0][0])
                    argv.append("--allow-write")
                    self.sql_execute("UPDATE cron_schedules SET resume_argv=? WHERE id=?", (json.dumps(argv), created["id"]))
                self.due(created["id"])
                self.once()
                occurrence = self.occurrences(created["id"])[0]
                self.assertEqual(self.job(occurrence[2])["status"], "blocked")
                self.assertEqual(self.once()["processed"], 0)
                self.assertEqual(self.cron(created["id"])["status"], "paused")
                self.assertEqual(self.cron(created["id"])["run_count"], 1)
                self.control(created["id"], "resume", expected=2)
                self.assertEqual(self.requests(), [])
                self.assertFalse(marker.exists())

    def test_unknown_dispatched_effect_pauses_without_new_task_or_mcp_start(self):
        manifest, marker, grants = self.configure_mcp()
        created = self.create("--allow-process", fixture.PROCESS, "--process-timeout-ms", "15000", *grants)
        self.setup_responses({"body": fixture.reply("responses", tool=("run_process", {
            "executable": str(fixture.PROCESS), "arguments": ["sleep", "10000"]}))})
        self.due(created["id"])
        process = self.start_worker()
        self.wait_for(lambda: self.sql_rows("SELECT id FROM actions WHERE status='dispatched'"), process)
        occurrence = self.occurrences(created["id"])[0]
        self.kill(process)
        self.worker_started = False
        marker.unlink()
        requests = self.requests()
        self.once()
        self.assertEqual(self.job(occurrence[2])["status"], "blocked")
        self.assertEqual(self.show(occurrence[1])["actions"][0]["effect"], "unknown")
        self.due(created["id"])
        self.assertEqual(self.once()["processed"], 0)
        self.assertEqual(self.cron(created["id"])["status"], "paused")
        self.assertEqual(self.occurrences(created["id"]), [occurrence])
        self.assertEqual(self.requests(), requests)
        self.assertFalse(marker.exists())

    def test_worker_uses_current_secret_and_never_persists_it(self):
        created = self.create()
        self.assertNotIn(fixture.TOKEN, self.database_snapshot())
        fresh = "cron-worker-current-credential-never-persist"
        self.setup_responses({"body": fixture.reply("responses")})
        self.due(created["id"])
        self.assertEqual(self.once(env=dict(self.env, CODESTUDIO_API_KEY=fresh))["processed"], 1)
        self.assertEqual(self.requests()[-1]["headers"]["Authorization"], "Bearer " + fresh)
        self.assertNotIn(fresh, self.database_snapshot())
        self.assertNotIn(fixture.TOKEN, self.database_snapshot())

    def test_invalid_enrollment_rejects_before_mcp_or_model_side_effects(self):
        self.create()
        manifest, marker, grants = self.configure_mcp()
        before = self.database_snapshot()
        invalid = [({}, ("--max-runs", "-1")), ({}, ("--max-runs", "1000001")),
                   ({}, ("--deadline-ms", "0")), ({}, ("--max-attempts", "0")),
                   ({}, ("--retry-delay-ms", "0")), ({}, ("--name", "")),
                   ({}, ("--api-key", "inline-credential-must-not-persist")),
                   ({"expression": "* * * *"}, ()), ({"expression": "60 * * * *"}, ()),
                   ({"expression": "*/0 * * * *"}, ()), ({"expression": "0 0 31 2 *"}, ()),
                   ({"zone": "Not/ARealZone"}, ())]
        for options, extra in invalid:
            with self.subTest(options=options, extra=extra):
                self.invoke(*self.create_args(*grants, *extra, **options), expected=2)
                self.assertEqual(self.database_snapshot(), before)
                self.assertEqual(self.requests(), [])
                self.assertFalse(marker.exists())

    def test_preview_exposes_iana_dst_fold_and_skips_gap_without_database(self):
        def epoch(value):
            return int(datetime.fromisoformat(value).replace(tzinfo=timezone.utc).timestamp() * 1000)

        folded = self.invoke(*self.worker_args("cron-preview", "--cron", "30 1 * * *",
            "--timezone", "America/New_York", "--after", str(epoch("2026-11-01T00:00:00")), "--count", "2"))
        self.assertEqual([item["at_ms"] for item in folded["occurrences"]],
                         [epoch("2026-11-01T05:30:00"), epoch("2026-11-01T06:30:00")])
        self.assertTrue(all(item["local"].startswith("2026-11-01 01:30") for item in folded["occurrences"]))
        self.assertNotEqual(folded["occurrences"][0]["local"], folded["occurrences"][1]["local"])
        gap = self.invoke(*self.worker_args("cron-preview", "--cron", "30 2 * * *",
            "--timezone", "America/New_York", "--after", str(epoch("2026-03-08T00:00:00")), "--count", "1"))
        self.assertEqual(gap["occurrences"][0]["at_ms"], epoch("2026-03-09T06:30:00"))
        self.assertTrue(gap["occurrences"][0]["local"].startswith("2026-03-09 02:30"))
        self.assertEqual((gap["misfire"], gap["overlap"]), ("coalesce", "skip"))
        for extra in (("--count", "0"), ("--count", "101"), ("--after", "-1")):
            self.invoke(*self.worker_args("cron-preview", "--cron", "* * * * *", "--timezone", "UTC", *extra), expected=2)
        self.assertFalse(self.database.exists())
        self.assertEqual(self.requests(), [])


if __name__ == "__main__":
    unittest.main()
