"""Current-user login registration, without starting a worker or logging out.

Every registration uses a private temporary workspace and is removed in finally.
Only an explicit platform-service-unavailable status permits a skip. Linux
uses a private XDG_CONFIG_HOME, without altering the running user manager.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest


EXE = pathlib.Path(sys.argv.pop(1)).resolve()
TOKEN = "autostart-fixture-credential-not-for-registration"


@unittest.skipUnless(os.name == "nt" or sys.platform.startswith("linux"), "Windows or Linux autostart integration")
class AutostartTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="codestudio-autostart-")
        self.addCleanup(self.temp.cleanup)
        self.workspace = pathlib.Path(self.temp.name) / "workspace with spaces 工作区"
        self.workspace.mkdir()
        self.database = self.workspace / "custom state.sqlite3"
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("CODESTUDIO_")}
        self.env["CODESTUDIO_API_KEY"] = TOKEN
        if sys.platform.startswith("linux"):
            self.config = pathlib.Path(self.temp.name) / "private configuration"
            self.env["XDG_CONFIG_HOME"] = str(self.config)
        status = self.invoke("status")
        if not status["available"]:
            self.skipTest("Autostart unavailable: " + status["reason"])
        self.assertFalse(status["registered"], "A fresh temporary workspace unexpectedly owns an existing task")
        self.task_name = status["task_name"]
        self.addCleanup(self.remove_registration)

    def invoke(self, action, *extra, expected=0, workspace=None, database=None):
        result = subprocess.run([str(EXE), "scheduler-autostart-" + action,
            "--workspace", str(workspace or self.workspace), "--state", str(database or self.database),
            *map(str, extra)], env=self.env, capture_output=True, text=True, encoding="utf-8", timeout=20)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertNotIn(TOKEN, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def remove_registration(self):
        # Disable verifies the exact owned definition before deleting it. Never
        # delete a task by an arbitrary name or kill a recorded PID in cleanup.
        removed = self.invoke("disable")
        self.assertFalse(removed["registered"], self.task_name)

    def test_status_and_absent_disable_do_not_create_state_or_launch_work(self):
        first = self.invoke("status")
        self.assertEqual(first["trigger"], "current_user_logon" if os.name == "nt" else "user_manager_start")
        self.assertFalse(first["enabled"])
        self.assertFalse(first["conflict"])
        self.assertFalse(first["started"])
        self.assertFalse(first["supervised"])
        self.assertNotIn("max_restarts", first)
        self.assertNotIn("restart_delay_ms", first)
        disabled = self.invoke("disable")
        self.assertFalse(disabled["removed"])
        self.assertEqual(self.invoke("status"), first)
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_enable_readback_idempotency_configuration_conflict_and_remove(self):
        created = self.invoke("enable", "--poll-ms", "25")
        self.assertTrue(created["registered"])
        self.assertTrue(created["enabled"])
        self.assertTrue(created["created"])
        self.assertFalse(created["started"])
        self.assertEqual(created["task_name"], self.task_name)
        observed = self.invoke("status")
        self.assertEqual((observed["registered"], observed["enabled"], observed["conflict"], observed["poll_ms"]),
                         (True, True, False, 25))
        self.assertFalse(observed["supervised"])
        self.assertNotIn("max_restarts", observed)
        self.assertNotIn("restart_delay_ms", observed)
        self.assertFalse(self.invoke("enable", "--poll-ms", "25")["created"])
        self.assertIn("conflicts", self.invoke("enable", "--poll-ms", "100", expected=2)["error"])
        self.assertEqual(self.invoke("status"), observed, "Conflicting configuration changed registration")
        removed = self.invoke("disable")
        self.assertTrue(removed["removed"])
        self.assertFalse(self.invoke("status")["registered"])
        self.assertFalse(self.database.exists(), "Registering logon startup launched the worker immediately")
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_supervised_enable_readback_limits_conflicts_and_remove(self):
        options = ("--supervise", "--poll-ms", "25", "--max-restarts", "7", "--restart-delay-ms", "1500")
        created = self.invoke("enable", *options)
        self.assertEqual((created["registered"], created["enabled"], created["created"], created["started"]),
                         (True, True, True, False))
        self.assertEqual(created["task_name"], self.task_name)
        observed = self.invoke("status")
        self.assertEqual((observed["supervised"], observed["poll_ms"], observed["max_restarts"],
                          observed["restart_delay_ms"], observed["conflict"]), (True, 25, 7, 1500, False))
        self.assertFalse(self.invoke("enable", *options)["created"])
        for changed in (("--poll-ms", "25"),
                        ("--supervise", "--poll-ms", "100", "--max-restarts", "7", "--restart-delay-ms", "1500"),
                        ("--supervise", "--poll-ms", "25", "--max-restarts", "8", "--restart-delay-ms", "1500"),
                        ("--supervise", "--poll-ms", "25", "--max-restarts", "7", "--restart-delay-ms", "2000")):
            with self.subTest(options=changed):
                self.assertIn("conflicts", self.invoke("enable", *changed, expected=2)["error"])
                self.assertEqual(self.invoke("status"), observed)
        removed = self.invoke("disable")
        self.assertTrue(removed["removed"])
        self.assertFalse(removed["registered"])
        self.assertEqual((removed["supervised"], removed["poll_ms"], removed["max_restarts"],
                          removed["restart_delay_ms"]), (True, 25, 7, 1500))
        self.assertFalse(self.invoke("status")["supervised"])
        self.assertFalse(self.database.exists(), "Registering the supervisor launched it immediately")
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_mode_switch_requires_explicit_disable_and_preserves_identity(self):
        ordinary = self.invoke("enable")
        self.invoke("enable", "--supervise", expected=2)
        self.assertFalse(self.invoke("status")["supervised"])
        self.assertTrue(self.invoke("disable")["removed"])
        supervised = self.invoke("enable", "--supervise")
        self.assertEqual(supervised["task_name"], ordinary["task_name"])
        self.assertEqual((supervised["supervised"], supervised["poll_ms"], supervised["max_restarts"],
                          supervised["restart_delay_ms"]), (True, 100, 3, 1000))
        self.invoke("enable", expected=2)
        self.assertTrue(self.invoke("status")["supervised"])
        self.assertTrue(self.invoke("disable")["removed"])
        restored = self.invoke("enable")
        self.assertEqual(restored["task_name"], ordinary["task_name"])
        self.assertFalse(restored["supervised"])
        self.assertNotIn("max_restarts", restored)
        self.assertNotIn("restart_delay_ms", restored)
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_supervised_limit_boundaries_are_preserved(self):
        for poll, restarts, delay in ((20, 0, 100), (5000, 100, 60000)):
            with self.subTest(poll=poll, restarts=restarts, delay=delay):
                created = self.invoke("enable", "--supervise", "--poll-ms", poll,
                                      "--max-restarts", restarts, "--restart-delay-ms", delay)
                self.assertTrue(created["created"])
                observed = self.invoke("status")
                self.assertEqual((observed["poll_ms"], observed["max_restarts"], observed["restart_delay_ms"]),
                                 (poll, restarts, delay))
                self.assertTrue(self.invoke("disable")["removed"])
        self.assertFalse(self.database.exists())

    def test_invalid_supervision_flags_do_not_register_or_create_state(self):
        before = self.invoke("status")
        invalid = (("--max-restarts", "3"), ("--restart-delay-ms", "1000"),
                   ("--supervise", "--max-restarts", "-1"), ("--supervise", "--max-restarts", "101"),
                   ("--supervise", "--max-restarts", "3oops"), ("--supervise", "--max-restarts"),
                   ("--supervise", "--restart-delay-ms", "99"), ("--supervise", "--restart-delay-ms", "60001"),
                   ("--supervise", "--restart-delay-ms", "1.5"), ("--supervise", "--restart-delay-ms"),
                   ("--supervise", "--poll-ms", "19"), ("--supervise", "--poll-ms", "5001"),
                   ("--supervise", "--once"))
        for flags in invalid:
            with self.subTest(flags=flags):
                self.invoke("enable", *flags, expected=2)
                self.assertEqual(self.invoke("status"), before)
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_status_and_disable_reject_configuration_flags_without_changing_registration(self):
        self.invoke("enable", "--supervise", "--max-restarts", "7", "--restart-delay-ms", "1500")
        observed = self.invoke("status")
        for action in ("status", "disable"):
            for flags in (("--supervise",), ("--poll-ms", "100"), ("--max-restarts", "3"),
                          ("--restart-delay-ms", "1000")):
                with self.subTest(action=action, flags=flags):
                    self.invoke(action, *flags, expected=2)
                    self.assertEqual(self.invoke("status"), observed)
        self.assertFalse(self.database.exists())

    def test_state_and_workspace_scope_have_distinct_owned_names(self):
        other_workspace = pathlib.Path(self.temp.name) / "second workspace"
        other_workspace.mkdir()
        same_workspace = self.invoke("status", database=self.workspace / "other state.sqlite3")
        other = self.invoke("status", workspace=other_workspace, database=self.database)
        self.assertNotEqual(same_workspace["task_name"], self.task_name)
        self.assertNotEqual(other["task_name"], self.task_name)
        self.assertNotEqual(other["task_name"], same_workspace["task_name"])
        self.assertFalse(same_workspace["registered"])
        self.assertFalse(other["registered"])
        self.assertFalse(self.database.exists())

    def test_invalid_poll_cannot_register_or_create_workspace_state(self):
        before = self.invoke("status")
        for value in ("0", "19", "5001", "not-a-number"):
            with self.subTest(value=value):
                self.invoke("enable", "--poll-ms", value, expected=2)
                self.assertEqual(self.invoke("status"), before)
        self.assertFalse(self.database.exists())
        self.assertFalse((self.workspace / ".codestudio").exists())

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux systemd unit ownership")
    def test_linux_unit_contract_and_systemd_syntax(self):
        for supervised in (False, True):
            with self.subTest(supervised=supervised):
                flags = ("--supervise", "--max-restarts", "7", "--restart-delay-ms", "1500") if supervised else ()
                created = self.invoke("enable", *flags)
                path = pathlib.Path(created["unit_path"])
                self.assertEqual(path.parent, self.config / "systemd" / "user")
                link = path.parent / "default.target.wants" / path.name
                self.assertTrue(link.is_symlink())
                self.assertEqual(os.readlink(link), "../" + path.name)
                contents = path.read_text()
                self.assertIn("ExecStart=:", contents)
                self.assertIn(" scheduler-supervise " if supervised else " scheduler-run ", contents)
                self.assertIn("\nWorkingDirectory=" + str(self.workspace).replace("%", "%%") + "\n", contents)
                self.assertIn("Restart=no", contents)
                self.assertIn("WantedBy=default.target", contents)
                self.assertNotIn(TOKEN, contents)
                self.assertNotIn("Environment=", contents)
                if supervised:
                    self.assertIn("# Supervised=true\n# MaxRestarts=7\n# RestartDelayMS=1500\n", contents)
                    self.assertIn("--max-restarts 7 --restart-delay-ms 1500", contents)
                verified = subprocess.run(["systemd-analyze", "--user", "verify", str(path)],
                    env=self.env, capture_output=True, text=True, timeout=10)
                self.assertEqual(verified.returncode, 0, verified.stdout + verified.stderr)
                self.assertFalse(self.database.exists())
                self.assertTrue(self.invoke("disable")["removed"])

    @unittest.skipUnless(os.name == "nt", "Windows native Task Scheduler readback")
    def test_windows_supervised_native_definition_has_no_second_restart_policy(self):
        created = self.invoke("enable", "--supervise", "--poll-ms", "25",
                              "--max-restarts", "7", "--restart-delay-ms", "1500")
        env = dict(self.env, CODESTUDIO_TEST_AUTOSTART_NAME=created["task_name"])
        # Read only the uniquely owned temporary registration; never launch it.
        script = """$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$service = New-Object -ComObject 'Schedule.Service'
$service.Connect()
$definition = $service.GetFolder('\\').GetTask($env:CODESTUDIO_TEST_AUTOSTART_NAME).Definition
[ordered]@{Arguments=$definition.Actions.Item(1).Arguments; RestartCount=$definition.Settings.RestartCount;
    AllowDemandStart=$definition.Settings.AllowDemandStart; RunLevel=$definition.Principal.RunLevel;
    LogonType=$definition.Principal.LogonType; TriggerType=$definition.Triggers.Item(1).Type;
    TriggerCount=$definition.Triggers.Count; ActionCount=$definition.Actions.Count} | ConvertTo-Json -Compress
"""
        result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                                env=env, capture_output=True, text=True, encoding="utf-8", timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        actual = json.loads(result.stdout)
        self.assertTrue(actual["Arguments"].startswith("scheduler-supervise --workspace "))
        self.assertIn("--poll-ms 25", actual["Arguments"])
        self.assertIn("--max-restarts 7 --restart-delay-ms 1500", actual["Arguments"])
        self.assertEqual((actual["RestartCount"], actual["AllowDemandStart"], actual["RunLevel"], actual["LogonType"],
                          actual["TriggerType"], actual["TriggerCount"], actual["ActionCount"]), (0, False, 0, 3, 9, 1, 1))
        self.assertNotIn(TOKEN, result.stdout + result.stderr)
        self.assertFalse(self.database.exists())

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux supervised unit ownership")
    def test_linux_supervised_metadata_tampering_refuses_control(self):
        created = self.invoke("enable", "--supervise", "--max-restarts", "7", "--restart-delay-ms", "1500")
        path = pathlib.Path(created["unit_path"])
        original = path.read_text()
        replacements = (("# Supervised=true\n", "# Supervised=false\n"),
                        ("# MaxRestarts=7\n", "# MaxRestarts=8\n"),
                        ("# MaxRestarts=7\n", "# MaxRestarts=07\n"),
                        ("# MaxRestarts=7\n", ""),
                        ("# RestartDelayMS=1500\n", "# RestartDelayMS=99\n"),
                        ("--restart-delay-ms 1500", "--restart-delay-ms 2000"),
                        ("Restart=no", "Restart=always"),
                        (" scheduler-supervise ", " scheduler-run "))
        for before, after in replacements:
            with self.subTest(before=before, after=after):
                self.assertIn(before, original)
                tampered = original.replace(before, after, 1)
                path.write_text(tampered)
                try:
                    self.assertTrue(self.invoke("status")["conflict"])
                    self.invoke("enable", "--supervise", "--max-restarts", "7", "--restart-delay-ms", "1500", expected=2)
                    self.invoke("disable", expected=2)
                    self.assertEqual(path.read_text(), tampered)
                finally:
                    path.write_text(original)
        self.assertTrue(self.invoke("status")["supervised"])
        self.assertFalse(self.database.exists())

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux systemd unit ownership")
    def test_linux_modified_unit_and_foreign_enable_link_are_not_removed(self):
        created = self.invoke("enable")
        path = pathlib.Path(created["unit_path"])
        original = path.read_text()
        path.write_text(original + "# external edit\n")
        try:
            self.assertTrue(self.invoke("status")["conflict"])
            self.invoke("enable", expected=2)
            self.invoke("disable", expected=2)
            self.assertEqual(path.read_text(), original + "# external edit\n")
        finally:
            path.write_text(original)
        link = path.parent / "default.target.wants" / path.name
        link.unlink()
        link.symlink_to("../unrelated.service")
        try:
            self.assertTrue(self.invoke("status")["conflict"])
            self.invoke("disable", expected=2)
            self.assertEqual(os.readlink(link), "../unrelated.service")
        finally:
            link.unlink()
            link.symlink_to("../" + path.name)

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux systemd unit ownership")
    def test_linux_orphan_enable_link_and_symlink_unit_fail_closed(self):
        status = self.invoke("status")
        path = pathlib.Path(status["unit_path"])
        path.parent.mkdir(parents=True, mode=0o700)
        link = path.parent / "default.target.wants" / path.name
        link.parent.mkdir(mode=0o700)
        link.symlink_to("../" + path.name)
        try:
            self.assertTrue(self.invoke("status")["conflict"])
            self.invoke("disable", expected=2)
            self.assertTrue(link.is_symlink())
        finally:
            link.unlink()
        outside = pathlib.Path(self.temp.name) / "unrelated.service"
        outside.write_text("foreign unit\n")
        path.symlink_to(outside)
        try:
            self.invoke("enable", expected=2)
            self.invoke("disable", expected=2)
            self.assertEqual(outside.read_text(), "foreign unit\n")
        finally:
            path.unlink()


if __name__ == "__main__":
    unittest.main(verbosity=2)
