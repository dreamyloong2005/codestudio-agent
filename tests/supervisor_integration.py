"""Exercise opt-in worker supervision through real processes and durable state.

Model replies and tool processes are local fixtures. Deliberate crashes target
an OS process handle (Windows) or pidfd (Linux), after checking the worker's
executable and parent, never a PID copied from an old ledger entry.
"""
import ctypes
import json
import os
import pathlib
import select
import signal
import subprocess
import threading
import time
import unittest

import scheduler_integration as scheduler_fixture

fixture = scheduler_fixture.fixture


class OwnedProcess:
    """Retain a process identity while a test observes or deliberately kills it."""

    def __init__(self, pid, parent_pid=None):
        self.pid = pid
        self.handle = None
        self.fd = None
        try:
            if os.name == "nt":
                from ctypes import wintypes

                class ProcessEntry(ctypes.Structure):
                    _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD),
                        ("pid", wintypes.DWORD), ("heap", ctypes.c_size_t),
                        ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                        ("parent", wintypes.DWORD), ("priority", wintypes.LONG),
                        ("flags", wintypes.DWORD), ("name", wintypes.WCHAR * 260)]

                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
                kernel.OpenProcess.restype = wintypes.HANDLE
                kernel.CloseHandle.argtypes = [wintypes.HANDLE]
                kernel.CloseHandle.restype = wintypes.BOOL
                kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                    wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
                kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
                kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
                kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
                kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
                kernel.Process32FirstW.restype = wintypes.BOOL
                kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
                kernel.Process32NextW.restype = wintypes.BOOL
                kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
                kernel.WaitForSingleObject.restype = wintypes.DWORD
                kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
                kernel.TerminateProcess.restype = wintypes.BOOL
                self.kernel = kernel
                self.handle = kernel.OpenProcess(0x100001 | 0x1000, False, pid)
                if not self.handle:
                    raise ctypes.WinError(ctypes.get_last_error())
                path = ctypes.create_unicode_buffer(32768)
                length = wintypes.DWORD(len(path))
                if not kernel.QueryFullProcessImageNameW(self.handle, 0, path, ctypes.byref(length)):
                    raise ctypes.WinError(ctypes.get_last_error())
                executable = pathlib.Path(path.value)
                snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
                if snapshot == ctypes.c_void_p(-1).value:
                    raise ctypes.WinError(ctypes.get_last_error())
                try:
                    entry = ProcessEntry()
                    entry.size = ctypes.sizeof(entry)
                    available = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
                    observed_parent = None
                    while available:
                        if entry.pid == pid:
                            observed_parent = entry.parent
                            break
                        available = kernel.Process32NextW(snapshot, ctypes.byref(entry))
                finally:
                    kernel.CloseHandle(snapshot)
            else:
                self.fd = os.pidfd_open(pid)
                proc = pathlib.Path("/proc") / str(pid)
                executable = pathlib.Path(os.readlink(proc / "exe"))
                # comm may itself contain spaces or ')'; fields after its last
                # ')' start with state, ppid, and the remaining stat fields.
                observed_parent = int((proc / "stat").read_text().rsplit(")", 1)[1].split()[1])
            if executable.resolve() != fixture.EXE:
                raise AssertionError(f"PID {pid} is not the tested executable: {executable}")
            if parent_pid is not None and observed_parent != parent_pid:
                raise AssertionError(f"PID {pid} belongs to parent {observed_parent}, not {parent_pid}")
            if not self.alive():
                raise AssertionError(f"PID {pid} exited before identity was established")
        except BaseException:
            self.close()
            raise

    def alive(self):
        if os.name == "nt":
            return self.kernel.WaitForSingleObject(self.handle, 0) == 258
        return not select.select([self.fd], [], [], 0)[0]

    def kill(self):
        if self.alive():
            if os.name == "nt":
                if not self.kernel.TerminateProcess(self.handle, 137):
                    raise ctypes.WinError(ctypes.get_last_error())
            else:
                signal.pidfd_send_signal(self.fd, signal.SIGKILL)

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


class SupervisorTests(unittest.TestCase):
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
    worker_args = scheduler_fixture.SchedulerTests.worker_args
    schedule = scheduler_fixture.SchedulerTests.schedule
    job = scheduler_fixture.SchedulerTests.job
    database_snapshot = scheduler_fixture.SchedulerTests.database_snapshot
    proposal_arguments = scheduler_fixture.SchedulerTests.proposal_arguments
    waiting_task = scheduler_fixture.SchedulerTests.waiting_task
    reject = scheduler_fixture.SchedulerTests.reject

    def setUp(self):
        fixture.ResumeTests.setUp(self)
        self.identities = []
        self.supervisors = []
        self.detached = []

    def tearDown(self):
        try:
            # Also stop a detached launch whose readiness response failed an
            # assertion before we could retain its OS process identity.
            if self.database.exists():
                self.invoke(*self.worker_args("scheduler-stop"))
            for process in self.supervisors:
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    self.kill(process)
            for identity in self.detached:
                deadline = time.monotonic() + 5
                while identity.alive() and time.monotonic() < deadline:
                    time.sleep(0.02)
                identity.kill()
        finally:
            for identity in self.identities:
                try:
                    identity.kill()
                finally:
                    identity.close()
            fixture.ResumeTests.tearDown(self)

    def status(self):
        return self.invoke(*self.worker_args("scheduler-status"))

    def supervisor_status(self):
        return self.invoke(*self.worker_args("scheduler-supervisor-status"))

    def supervisor_events(self, after=0, limit=100):
        return self.invoke(*self.worker_args("scheduler-supervisor-events", "--after", str(after),
                                             "--limit", str(limit)))

    def diagnostic_phase(self, phase, process=None):
        return self.wait_for(lambda: (state if (state := self.supervisor_status())["record"] is not None
            and state["record"]["phase"] == phase else None), process)

    def assert_terminal_diagnostics(self, process, reason, restarts):
        state = self.supervisor_status()
        record = state["record"]
        self.assertEqual((record["pid"], record["phase"], record["exit_reason"], record["restarts"]),
                         (process.pid, "stopped", reason, restarts))
        self.assertEqual(state["observation"]["state"], "not_running")
        self.assertEqual(record["next_restart_at_ms"], 0)
        self.assertGreaterEqual(record["finished_at_ms"], record["started_at_ms"])
        self.assertGreater(record["finished_at_ms"], 0)
        return record

    def identity(self, pid, parent_pid=None):
        identity = OwnedProcess(pid, parent_pid)
        self.identities.append(identity)
        return identity

    def start_supervisor(self, *extra):
        previous = self.status()
        process = subprocess.Popen([str(fixture.EXE), *map(str, self.worker_args(
            "scheduler-supervise", "--poll-ms", "20", *extra))], env=self.env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(process)
        self.supervisors.append(process)
        state = self.wait_for(lambda: (state if (state := self.status())["state"] == "running"
            and state["generation"] > previous["generation"] else None), process)
        self.assertNotEqual(state["pid"], process.pid)
        worker = self.identity(state["pid"], process.pid)
        return process, state, worker

    def restarted(self, process, previous):
        state = self.wait_for(lambda: (state if (state := self.status())["state"] == "running"
            and state["generation"] > previous["generation"] else None), process)
        self.assertEqual(state["owner"], previous["owner"])
        self.assertEqual(state["stop_epoch"], previous["stop_epoch"])
        self.assertEqual(state["generation"], previous["generation"] + 1)
        return state, self.identity(state["pid"], process.pid)

    def crash(self, worker):
        worker.kill()
        self.wait_for(lambda: not worker.alive())

    def result(self, process, reason, restarts, exit_code=0):
        output, error = process.communicate(timeout=8)
        self.assertEqual(process.returncode, exit_code, (output, error))
        self.assertNotIn(fixture.TOKEN.encode(), output + error)
        result = json.loads(output)
        self.assertEqual((result["supervised"], result["state"], result["reason"], result["restarts"],
                          result["supervisor_pid"]), (True, "stopped", reason, restarts, process.pid))
        return result

    def stop(self, process, restarts=0):
        stopped = self.invoke(*self.worker_args("scheduler-stop"))
        self.assertTrue(stopped["stop_requested"])
        return self.result(process, "stop_requested", restarts)

    def test_clean_stop_preserves_delayed_job_and_supervisor_survives_stdin_eof(self):
        task = self.failed_task()
        queued = self.schedule(task, "--delay-ms", "60000")
        requests = self.requests()
        process, state, worker = self.start_supervisor()
        process.stdin.close()
        process.stdin = None
        # Query through an independent process after EOF before stopping.
        self.assertEqual(self.status()["owner"], state["owner"])
        self.assertIsNone(process.poll())
        self.assertEqual(self.stop(process)["last_exit_code"], 0)
        self.assertFalse(worker.alive())
        self.assertEqual((self.job(queued["id"])["status"], self.job(queued["id"])["attempts"]), ("queued", 0))
        self.assertEqual(self.requests(), requests)

    def test_abnormal_exit_restarts_with_same_owner_and_a_new_generation(self):
        process, original, worker = self.start_supervisor("--restart-delay-ms", "2000")
        running = self.diagnostic_phase("running", process)
        record = running["record"]
        self.assertEqual((record["owner"], record["pid"], record["worker_pid"], record["generation"],
                          record["stop_epoch"], record["poll_ms"], record["max_restarts"],
                          record["restart_delay_ms"]),
                         (original["owner"], process.pid, worker.pid, original["generation"],
                          original["stop_epoch"], 20, 3, 2000))
        self.assertEqual(pathlib.Path(record["workspace"]), self.workspace.resolve())
        self.assertTrue(record["id"])
        self.assertTrue(record["process_identity"])
        self.assertEqual(running["observation"]["state"], "matching_process")
        self.assertTrue(running["worker_matches"])
        self.assertIsNone(record["last_exit_code"])
        self.assertEqual(record["finished_at_ms"], 0)
        self.crash(worker)
        backoff = self.diagnostic_phase("backoff", process)
        self.assertEqual(backoff["record"]["id"], record["id"])
        self.assertEqual(backoff["record"]["restarts"], 0)
        self.assertNotEqual(backoff["record"]["last_exit_code"], 0)
        self.assertGreater(backoff["record"]["next_restart_at_ms"], backoff["record"]["updated_at_ms"])
        self.assertEqual(backoff["observation"]["state"], "matching_process")
        replacement, next_worker = self.restarted(process, original)
        self.assertTrue(next_worker.alive())
        resumed = self.diagnostic_phase("running", process)
        self.assertEqual((resumed["record"]["id"], resumed["record"]["restarts"],
                          resumed["record"]["worker_pid"], resumed["record"]["generation"],
                          resumed["record"]["next_restart_at_ms"]),
                         (record["id"], 1, next_worker.pid, replacement["generation"], 0))
        self.assertEqual(self.stop(process, restarts=1)["last_exit_code"], 0)
        self.assertFalse(next_worker.alive())
        self.assertEqual(self.status()["generation"], replacement["generation"])
        terminal = self.assert_terminal_diagnostics(process, "stop_requested", 1)
        events = self.supervisor_events()["items"]
        self.assertTrue(all(event["run_id"] == record["id"] for event in events))
        backoffs = [event["payload"] for event in events if event["kind"] == "supervisor.backoff"]
        self.assertEqual(len(backoffs), 1)
        self.assertEqual(backoffs[0]["restarts"], 0)
        self.assertNotEqual(backoffs[0]["last_exit_code"], 0)
        self.assertGreater(backoffs[0]["next_restart_at_ms"], backoffs[0]["updated_at_ms"])
        restarted = [event["payload"] for event in events if event["kind"] == "supervisor.running"]
        self.assertEqual([item["generation"] for item in restarted],
                         [original["generation"], replacement["generation"]])
        self.assertEqual([item["restarts"] for item in restarted], [0, 1])
        self.assertEqual(events[-1]["kind"], "supervisor.stopped")
        self.assertEqual(events[-1]["payload"], terminal)

    def test_stop_during_backoff_prevents_any_replacement(self):
        process, original, worker = self.start_supervisor("--restart-delay-ms", "2000")
        self.crash(worker)
        self.diagnostic_phase("backoff", process)
        stopped = self.stop(process)
        self.assertNotEqual(stopped["last_exit_code"], 0)
        final = self.status()
        self.assertEqual((final["state"], final["owner"], final["generation"], final["stop_epoch"]),
                         ("stopped", original["owner"], original["generation"], original["stop_epoch"] + 1))
        self.assert_terminal_diagnostics(process, "stop_requested", 0)

    def test_restart_limits_zero_and_one_count_total_replacements(self):
        for limit in (0, 1):
            with self.subTest(limit=limit):
                process, original, worker = self.start_supervisor("--restart-delay-ms", "100", "--max-restarts", str(limit))
                self.crash(worker)
                if limit:
                    _, replacement = self.restarted(process, original)
                    self.crash(replacement)
                result = self.result(process, "restart_limit", limit, exit_code=2)
                self.assertIsInstance(result["last_exit_code"], int)
                self.assertNotEqual(result["last_exit_code"], 0)
                self.assertEqual(self.status()["generation"], original["generation"] + limit)
                record = self.assert_terminal_diagnostics(process, "restart_limit", limit)
                self.assertEqual(record["last_exit_code"], result["last_exit_code"])

    def test_supervisor_admission_locks_remain_owned_through_backoff(self):
        process, original, worker = self.start_supervisor("--restart-delay-ms", "3000")
        self.crash(worker)
        self.invoke(*self.worker_args("scheduler-supervise", "--poll-ms", "20"), expected=2)
        self.invoke(*self.worker_args("scheduler-run", "--once"), expected=2)
        state = self.status()
        self.assertEqual((state["owner"], state["generation"]), (original["owner"], original["generation"]))
        self.stop(process)

    def test_restarted_worker_recovers_model_pending_without_replaying_applied_write(self):
        self.setup_responses({"body": fixture.reply(tool=("write_file", {"path": "once.txt", "content": "original"}))},
                             {"status": 503})
        failed = self.invoke(*self.run_args("--allow-write", "--max-turns", "5"), expected=1)
        task = failed["task_id"]
        queued = self.schedule(task, "--allow-write")
        release = threading.Event()
        self.releases.append(release)
        self.setup_responses({"body": fixture.reply(), "release": release})
        process, original, worker = self.start_supervisor("--restart-delay-ms", "700")
        self.wait_for(lambda: len(self.requests()) == 3, process)
        interrupted_request = self.requests()[-1]["body"]
        self.crash(worker)
        (self.workspace / "once.txt").write_text("changed after crash", encoding="utf-8")
        release.set()
        self.setup_responses({"body": fixture.reply()})
        self.restarted(process, original)
        self.wait_for(lambda: self.job(queued["id"])["status"] == "completed", process)
        self.assertEqual(self.job(queued["id"])["attempts"], 2)
        self.assertEqual(len(self.requests()), 4)
        self.assertEqual(self.requests()[-1]["body"], interrupted_request)
        self.assertEqual((self.workspace / "once.txt").read_text(), "changed after crash")
        detail = self.show(task)
        self.assertEqual((detail["status"], len(detail["actions"])), ("completed", 1))
        self.assertEqual(sum(event["kind"] == "action.dispatched" for event in detail["events"]), 1)
        self.stop(process, restarts=1)

    def test_unknown_process_effect_blocks_recovery_before_http_or_mcp_restart(self):
        _, marker, mcp_grants = self.configure_mcp()
        grants = ["--allow-process", fixture.PROCESS, "--process-timeout-ms", "15000", *mcp_grants]
        task = self.failed_task(*grants)
        queued = self.schedule(task, *grants)
        self.setup_responses({"body": fixture.reply(tool=("run_process", {
            "executable": str(fixture.PROCESS), "arguments": ["sleep", "10000"]}))})
        process, original, worker = self.start_supervisor("--restart-delay-ms", "700")
        self.wait_for(lambda: self.sql_rows("SELECT id FROM actions WHERE task_id=? AND status='dispatched'", (task,)), process)
        self.crash(worker)
        self.assertTrue(marker.exists())
        marker.unlink()
        requests = self.requests()
        self.restarted(process, original)
        self.wait_for(lambda: self.job(queued["id"])["status"] == "blocked", process)
        self.assertEqual(self.show(task)["actions"][0]["effect"], "unknown")
        self.assertFalse(marker.exists(), "An unsafe continuation launched MCP before being rejected")
        self.assertEqual(self.requests(), requests)
        self.stop(process, restarts=1)

    def test_supervisor_death_terminates_its_worker_and_releases_admission(self):
        process, original, worker = self.start_supervisor()
        running = self.diagnostic_phase("running", process)["record"]
        self.kill(process)
        self.wait_for(lambda: not worker.alive())
        stale = self.supervisor_status()
        self.assertEqual((stale["record"]["id"], stale["record"]["phase"], stale["record"]["finished_at_ms"]),
                         (running["id"], "running", 0))
        self.assertEqual(stale["observation"]["state"], "not_running")
        # A read must not turn an observed process death into a fabricated
        # terminal transition or rewrite the last heartbeat.
        before = self.database_snapshot()
        self.assertEqual(self.supervisor_status()["record"], stale["record"])
        self.supervisor_events()
        self.assertEqual(self.database_snapshot(), before)
        result = self.invoke(*self.worker_args("scheduler-run", "--once"))
        self.assertEqual((result["processed"], result["state"]), (0, "stopped"))
        self.assertEqual(self.status()["generation"], original["generation"] + 1)
        unmatched = self.supervisor_status()
        self.assertFalse(unmatched["worker_matches"])
        self.assertEqual(unmatched["record"], stale["record"])
        replacement, _, _ = self.start_supervisor()
        current = self.diagnostic_phase("running", replacement)["record"]
        self.assertNotEqual(current["id"], running["id"])
        self.stop(replacement)
        rows = self.supervisor_events()["items"]
        retired = [row for row in rows if row["run_id"] == running["id"]
                   and row["payload"]["phase"] == "stopped"]
        self.assertEqual(len(retired), 1)
        self.assertEqual(retired[0]["payload"]["exit_reason"], "superseded")
        self.assertGreater(retired[0]["payload"]["finished_at_ms"], 0)
        self.assertEqual(rows[-1]["run_id"], current["id"])

    def test_heartbeat_has_no_lifecycle_events_and_event_pages_preserve_both_runs(self):
        process, _, _ = self.start_supervisor()
        active = self.diagnostic_phase("running", process)
        initial_events = self.supervisor_events()["items"]
        heartbeat = active["record"]["heartbeat_at_ms"]
        self.wait_for(lambda: self.supervisor_status()["record"]["heartbeat_at_ms"] > heartbeat, process)
        self.assertEqual(self.supervisor_events()["items"], initial_events)
        self.stop(process)
        first = self.assert_terminal_diagnostics(process, "stop_requested", 0)
        process, _, _ = self.start_supervisor()
        self.diagnostic_phase("running", process)
        self.stop(process)
        second = self.assert_terminal_diagnostics(process, "stop_requested", 0)
        self.assertNotEqual(first["id"], second["id"])
        self.assertEqual(self.sql_rows("PRAGMA user_version"), [(7,)])

        before = self.database_snapshot()
        cursor, rows = 0, []
        for _ in range(50):
            page = self.supervisor_events(cursor, limit=2)
            self.assertLessEqual(len(page["items"]), 2)
            self.assertTrue(all(row["seq"] > cursor for row in page["items"]))
            if page["items"]:
                self.assertEqual(page["next_after"], page["items"][-1]["seq"])
            rows.extend(page["items"])
            cursor = page["next_after"]
            if not page["has_more"]:
                break
        else:
            self.fail("Supervisor event pagination did not terminate")
        self.assertEqual(rows, self.supervisor_events()["items"])
        self.assertEqual([row["seq"] for row in rows], sorted({row["seq"] for row in rows}))
        self.assertEqual({row["run_id"] for row in rows}, {first["id"], second["id"]})
        for row in rows:
            self.assertEqual(row["run_id"], row["payload"]["id"])
            self.assertGreaterEqual(row["created_at_ms"], row["payload"]["started_at_ms"])
            self.assertTrue(row["kind"].startswith("supervisor."))
        self.assertEqual(rows[-1]["payload"], second)
        empty = self.supervisor_events(cursor)
        self.assertEqual(empty, {"items": [], "next_after": cursor, "has_more": False})
        combined = self.status()["supervisor"]
        self.assertEqual(combined["record"], second)
        self.assertEqual(combined["observation"]["state"], "not_running")
        self.assertEqual(self.database_snapshot(), before)

    def test_empty_diagnostics_and_invalid_query_options_never_create_state(self):
        empty = self.supervisor_status()
        self.assertIsNone(empty["record"])
        self.assertEqual(empty["observation"]["state"], "not_checked")
        self.assertGreater(empty["observation"]["observed_at_ms"], 0)
        self.assertFalse(empty["worker_matches"])
        self.assertIsNone(self.status()["supervisor"]["record"])
        self.assertEqual(self.supervisor_events(), {"items": [], "next_after": 0, "has_more": False})
        self.assertEqual(self.supervisor_events(9223372036854775807),
                         {"items": [], "next_after": 9223372036854775807, "has_more": False})
        cases = [("scheduler-supervisor-status", "--after", "0"),
                 ("scheduler-supervisor-status", "--poll-ms", "100"),
                 ("scheduler-supervisor-status", "--supervise"),
                 ("scheduler-supervisor-events", "--after", "-1"),
                 ("scheduler-supervisor-events", "--after", "1.0"),
                 ("scheduler-supervisor-events", "--after", "9223372036854775808"),
                 ("scheduler-supervisor-events", "--limit", "0"),
                 ("scheduler-supervisor-events", "--limit", "101"),
                 ("scheduler-supervisor-events", "--limit", "true"),
                 ("scheduler-supervisor-events", "--limit", "1", "--limit", "2"),
                 ("scheduler-supervisor-events", "--owner", "injected")]
        for command, *extra in cases:
            with self.subTest(command=command, extra=extra):
                self.invoke(*self.worker_args(command, *extra), expected=2)
                self.assertFalse(self.database.exists())
                self.assertFalse((self.workspace / ".codestudio").exists())
        self.assertEqual(self.requests(), [])

    def test_worker_correlation_requires_both_owner_and_generation_without_claiming_liveness(self):
        process, _, worker = self.start_supervisor()
        self.diagnostic_phase("running", process)
        self.stop(process)
        stopped = self.supervisor_status()
        self.assertFalse(worker.alive())
        self.assertEqual(stopped["observation"]["state"], "not_running")
        self.assertTrue(stopped["worker_matches"], "Worker correlation must not claim process liveness")
        record = stopped["record"]
        cases = [("different-owner", record["generation"], False),
                 (record["owner"], record["generation"] + 1, False),
                 (record["owner"], record["generation"], True)]
        for owner, generation, expected in cases:
            with self.subTest(owner=owner, generation=generation):
                self.sql_execute("UPDATE scheduler_control SET owner=?,generation=? WHERE singleton=1",
                                 (owner, generation))
                before = self.database_snapshot()
                observed = self.supervisor_status()
                self.assertEqual(observed["record"], record)
                self.assertEqual(observed["worker_matches"], expected)
                self.assertEqual(self.status()["supervisor"]["worker_matches"], expected)
                self.assertEqual(self.database_snapshot(), before)

    def test_complete_stale_child_fences_reject_without_changing_the_ledger(self):
        process, original, _ = self.start_supervisor()
        self.stop(process)
        stopped = self.status()
        before, requests = self.database_snapshot(), self.requests()
        for epoch, generation in ((original["stop_epoch"], stopped["generation"]),
                                  (stopped["stop_epoch"], stopped["generation"] - 1)):
            with self.subTest(epoch=epoch, generation=generation):
                self.invoke(*self.worker_args("scheduler-child", "--owner", "stale-launch-fixture",
                    "--poll-ms", "20", "--expected-stop-epoch", str(epoch),
                    "--expected-generation", str(generation)), expected=2)
                self.assertEqual(self.database_snapshot(), before)
                self.assertEqual(self.requests(), requests)

    if os.name != "nt":
        # Windows children do not have a shared console in this harness, so
        # Ctrl+Break cannot be delivered portably. Linux tests exercise the
        # real signal handlers without manufacturing a cancellation in SQLite.
        def test_cooperative_supervisor_signal_stops_child_without_restart(self):
            process, original, worker = self.start_supervisor()
            process.send_signal(signal.SIGTERM)
            result = self.result(process, "cancelled", 0)
            self.assertEqual(result["last_exit_code"], 0)
            self.assertFalse(worker.alive())
            state = self.status()
            self.assertEqual((state["state"], state["generation"], state["stop_epoch"]),
                             ("stopped", original["generation"], original["stop_epoch"] + 1))

        def test_normal_worker_signal_exit_is_not_restarted(self):
            process, original, worker = self.start_supervisor()
            signal.pidfd_send_signal(worker.fd, signal.SIGTERM)
            result = self.result(process, "normal_exit", 0)
            self.assertEqual(result["last_exit_code"], 0)
            self.assertFalse(worker.alive())
            state = self.status()
            self.assertEqual((state["state"], state["generation"], state["stop_epoch"]),
                             ("stopped", original["generation"], original["stop_epoch"]))

    def test_invalid_supervisor_options_never_create_state(self):
        cases = [("scheduler-supervise", *extra) for extra in (
            ("--poll-ms", "19"), ("--poll-ms", "5001"), ("--max-restarts", "-1"),
            ("--max-restarts", "101"), ("--max-restarts", "1.5"), ("--restart-delay-ms", "99"),
            ("--restart-delay-ms", "60001"), ("--once",), ("--owner", "injected-owner"),
            ("--max-restarts", "1", "--max-restarts", "2"), ("--supervise",))]
        cases += [("scheduler-start", "--max-restarts", "1"),
                  ("scheduler-start", "--restart-delay-ms", "100"),
                  ("scheduler-start", "--supervise", "--max-restarts", "101"),
                  ("scheduler-child", "--owner", "incomplete-fence"),
                  ("scheduler-child", "--owner", "incomplete-fence", "--expected-stop-epoch", "0"),
                  ("scheduler-run", "--expected-stop-epoch", "0", "--expected-generation", "0")]
        for command, *extra in cases:
            with self.subTest(command=command, extra=extra):
                self.invoke(*self.worker_args(command, *extra), expected=2)
                self.assertFalse(self.database.exists())
                self.assertFalse((self.workspace / ".codestudio").exists())
        self.assertEqual(self.requests(), [])

    def test_detached_supervisor_preserves_control_pipes_and_supports_owned_stop(self):
        task, action = self.waiting_task()
        queued = self.schedule(task, "--allow-build-proposals")
        before, requests = self.database_snapshot(), self.requests()
        launcher = self.start(*self.worker_args("scheduler-start", "--supervise", "--poll-ms", "20",
                                                "--max-restarts", "1", "--restart-delay-ms", "100"))
        output, error = launcher.communicate(timeout=8)
        result = json.loads(output)
        if os.name == "nt" and launcher.returncode == 2 and "Cannot detach scheduler from the parent Job" in result.get("error", ""):
            self.assertEqual(self.status()["state"], "stopped")
            self.assertEqual(self.database_snapshot(), before)
            self.assertEqual(self.requests(), requests)
            return
        self.assertEqual(launcher.returncode, 0, (output, error))
        self.assertEqual((result["started"], result["supervised"]), (True, True))
        self.assertNotEqual(result["supervisor_pid"], launcher.pid)
        supervisor = self.identity(result["supervisor_pid"])
        self.detached.append(supervisor)
        state = self.status()
        self.assertEqual((state["owner"], state["pid"]), (result["owner"], result["pid"]))
        worker = self.identity(state["pid"], supervisor.pid)
        self.setup_responses({"body": fixture.reply("responses")})
        self.reject(task, action)
        self.wait_for(lambda: self.job(queued["id"])["status"] == "completed")
        self.assertEqual(self.show(task)["status"], "completed")
        self.invoke(*self.worker_args("scheduler-stop"))
        self.wait_for(lambda: not worker.alive() and not supervisor.alive())
        self.assertEqual(self.status()["state"], "stopped")

    def test_custom_database_supervisor_lock_cannot_be_overwritten_by_write_file(self):
        self.database = self.workspace / "custom-state.sqlite3"
        self.setup_responses({"status": 503})
        first = self.invoke(*self.run_args("--state", self.database, "--allow-write"), expected=1)
        queued = self.schedule(first["task_id"], "--allow-write")
        lock_name = self.database.name + ".supervisor.run.lock"
        attempted = "must not overwrite supervisor ownership"
        self.setup_responses({"body": fixture.reply(tool=("write_file", {"path": lock_name, "content": attempted}))},
                             {"body": fixture.reply()})
        process, _, _ = self.start_supervisor()
        self.wait_for(lambda: self.job(queued["id"])["status"] == "blocked", process)
        detail = self.invoke("show", first["task_id"], "--workspace", self.workspace, "--state", self.database)
        self.assertEqual((detail["status"], detail["actions"][0]["status"], detail["actions"][0]["effect"]),
                         ("needs_attention", "denied", "none"))
        lock_path = self.workspace / lock_name
        self.assertTrue(lock_path.exists())
        self.assertFalse((self.workspace / ".codestudio" / "state.sqlite3").exists())
        self.stop(process)
        # The Windows ownership lock intentionally denies reads until its
        # holder exits; inspect bytes only after verifying the denied action.
        self.assertNotEqual(lock_path.read_bytes(), attempted.encode())

    def test_changed_ownership_during_backoff_prevents_stale_replacement(self):
        process, original, worker = self.start_supervisor("--restart-delay-ms", "2000")
        self.crash(worker)
        self.sql_execute("UPDATE scheduler_control SET owner=?,generation=generation+1,state='stopped' WHERE singleton=1",
                         ("replacement-ownership-fixture",))
        self.result(process, "ownership_changed", 0)
        state = self.status()
        self.assertEqual((state["owner"], state["generation"], state["stop_epoch"]),
                         ("replacement-ownership-fixture", original["generation"] + 1, original["stop_epoch"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
