"""Owned sandbox journals, real isolated children and explicit crash reclamation.

Each test uses a private registry and remembers only IDs emitted by its own
fixture hosts. Existing unregistered directories are never adopted or removed.
"""
import concurrent.futures
import ctypes
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid
from contract_schema import Contract

EXE = Path(sys.argv.pop(1)).resolve()
SUFFIX = ".exe" if os.name == "nt" else ""
HOST = EXE.parent / ("sandbox-resource-fixture" + SUFFIX)
IMAGE = EXE.parent / ("sandbox-fixture" + SUFFIX)
_arguments = sys.argv[:]
sys.argv[:] = [_arguments[0], str(EXE)]
try:
    import service_integration as service_fixture
finally:
    sys.argv[:] = _arguments


class ResourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="codestudio-resource-test-", dir=Path(tempfile.gettempdir()).resolve())
        self.root = Path(self.temp.name)
        self.workspace = self.root / "workspace"
        self.workspace.mkdir()
        self.registry = self.root / "private registry"
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("CODESTUDIO_")}
        if "CODESTUDIO_CGROUP_ROOT" in os.environ:
            self.env["CODESTUDIO_CGROUP_ROOT"] = os.environ["CODESTUDIO_CGROUP_ROOT"]
        self.env["CODESTUDIO_SANDBOX_REGISTRY"] = str(self.registry)
        self.children = []
        self.clients = []
        self.owned = {}
        self.restore = []
        self.assertTrue(HOST.is_file(), "Build sandbox-resource-fixture")

    def tearDown(self):
        failures = []
        for client in self.clients:
            try:
                client.close()
            except Exception as error:
                failures.append(error)
        for process in self.children:
            if process.poll() is None:
                process.kill()
            try:
                process.communicate(timeout=10)
            except Exception as error:
                failures.append(error)
        for restore in reversed(self.restore):
            try:
                restore()
            except Exception as error:
                failures.append(error)
        for resource_id in self.owned:
            try:
                result = self.wait_reclaimable(resource_id)
                if result["state"] != "absent":
                    result = self.reclaim(resource_id)
                self.assertIn(result["state"], ("reclaimed", "absent"), result)
            except Exception as error:
                failures.append(error)
        if not failures:
            self.temp.cleanup()
        else:
            # Do not discard the ownership evidence if product cleanup refuses.
            self.temp._finalizer.detach()
            failures[0].add_note(f"Owned registry preserved at {self.registry}")
            for error in failures[1:]:
                failures[0].add_note(str(error))
            raise failures[0]

    def capture(self, *arguments):
        return subprocess.run([str(EXE), *map(str, arguments)], cwd=self.workspace,
                              env=self.env, capture_output=True, timeout=30)

    def invoke(self, *arguments, expected=0):
        result = self.capture(*arguments)
        if expected is not None:
            self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        else:
            self.assertIn(result.returncode, (0, 1), result.stdout + result.stderr)
        return json.loads(result.stdout)

    def listing(self, *arguments):
        return self.invoke("sandbox-resources", *arguments)

    def reclaim(self, resource_id, dry=False):
        return self.invoke("sandbox-reclaim", "--id", resource_id,
                           *(["--dry-run"] if dry else []), expected=None)

    def wait_reclaimable(self, resource_id):
        deadline = time.monotonic() + 8
        while True:
            result = self.reclaim(resource_id, dry=True)
            if result["state"] in ("reclaimable", "absent"):
                return result
            if time.monotonic() >= deadline:
                self.fail(f"Owned resource did not become reclaimable: {result}")
            time.sleep(0.025)

    def start_host(self):
        process = subprocess.Popen([str(HOST), "hold", str(IMAGE), str(self.workspace)],
                                   cwd=self.workspace, env=self.env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(process)
        ready = queue.Queue()
        reader = threading.Thread(target=lambda: ready.put(process.stdout.readline()), daemon=True)
        reader.start()
        try:
            line = ready.get(timeout=12)
        except queue.Empty:
            process.kill()
            out, err = process.communicate(timeout=10)
            self.fail(f"Fixture readiness timed out: {out!r} {err!r}")
        data = json.loads(line)
        self.assertNotIn("fixture_error", data, data)
        row = data["resource"]
        self.assertRegex(row["id"], r"^[0-9a-f]{32}$")
        self.assertEqual(row["owner"]["pid"], process.pid)
        self.owned[row["id"]] = row
        return process, row

    def orphan(self):
        process, row = self.start_host()
        process.kill()
        process.communicate(timeout=10)
        self.wait_reclaimable(row["id"])
        return row

    def stage(self, row):
        return Path(next(item["path"] for item in row["resources"] if item["kind"] == "staging"))

    def record_path(self, row):
        return self.registry / row["id"] / "record.json"

    @staticmethod
    def snapshot(path):
        if not path.exists():
            return None
        result = {}
        for item in [path, *path.rglob("*")]:
            state = item.lstat()
            result[item.relative_to(path).as_posix()] = {
                "mode": state.st_mode, "modified": state.st_mtime_ns,
                "size": state.st_size,
                # Windows holds an exclusive byte-range lease. Its empty payload
                # cannot be read while active; still compare its metadata.
                "sha256": hashlib.sha256(item.read_bytes()).hexdigest()
                if item.is_file() and not (os.name == "nt" and item.name == "lease.lock") else None}
        return result

    def test_read_only_empty_registry_and_absent_reclaim_create_nothing(self):
        before = self.snapshot(self.workspace)
        self.assertEqual(self.listing()["items"], [])
        self.assertEqual(self.reclaim("0" * 32, dry=True)["state"], "absent")
        self.assertEqual(self.reclaim("0" * 32)["state"], "absent")
        self.assertFalse(self.registry.exists())
        self.assertEqual(self.snapshot(self.workspace), before)

    def test_invalid_options_refuse_before_registry_or_database_creation(self):
        cases = (("sandbox-resources", "--limit", "0"), ("sandbox-resources", "--limit", "101"),
                 ("sandbox-resources", "--after", "../escape"), ("sandbox-resources", "--workspace", self.workspace),
                 ("sandbox-reclaim",), ("sandbox-reclaim", "--id", "A" * 32),
                 ("sandbox-reclaim", "--id", "../escape"), ("sandbox-reclaim", "--id", "0" * 32, "--force"))
        for arguments in cases:
            with self.subTest(arguments=arguments):
                self.assertNotEqual(self.capture(*arguments).returncode, 0)
        self.assertFalse(self.registry.exists())
        self.assertFalse((self.workspace / ".codestudio").exists())

    def test_matching_temporary_prefix_without_journal_is_never_adopted(self):
        resource_id = uuid.uuid4().hex
        name = ("codestudio-sandbox-" if os.name == "nt" else "codestudio-isolation-") + resource_id
        unregistered = Path(tempfile.gettempdir()).resolve() / name
        unregistered.mkdir()
        sentinel = unregistered / "owned-test-sentinel.txt"
        sentinel.write_text("not registered", encoding="utf-8")
        try:
            self.assertEqual(self.listing()["items"], [])
            self.assertEqual(self.reclaim(resource_id, dry=True)["state"], "absent")
            self.assertEqual(self.reclaim(resource_id)["state"], "absent")
            self.assertEqual(sentinel.read_text(), "not registered")
        finally:
            # Exact files created above, no enumeration or prefix-based removal.
            sentinel.unlink()
            unregistered.rmdir()

    def test_active_real_sandbox_cannot_be_reclaimed(self):
        process, row = self.start_host()
        self.assertEqual(row["state"], "active")
        self.assertEqual(row["task_id"], "owned-resource-fixture-task")
        self.assertEqual(Path(row["workspace"]), self.workspace)
        self.assertEqual(row["phase"], "ready")
        before = self.snapshot(self.registry)
        self.assertEqual(self.reclaim(row["id"], dry=True)["state"], "active")
        self.assertEqual(self.reclaim(row["id"])["state"], "active")
        self.assertEqual(self.snapshot(self.registry), before)
        self.assertIsNone(process.poll())
        self.assertTrue(self.stage(row).exists())

    def test_normal_destruction_removes_resources_and_journal(self):
        process, row = self.start_host()
        process.stdin.write(b"stop\n")
        process.stdin.flush()
        out, err = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 0, out + err)
        self.assertFalse(self.stage(row).exists())
        self.assertEqual(self.listing()["items"], [])
        self.assertFalse(self.record_path(row).exists())

    def test_host_crash_preview_preserves_bytes_then_explicit_reclaim_is_idempotent(self):
        row = self.orphan()
        stage = self.stage(row)
        before = (self.snapshot(self.registry), self.snapshot(stage), self.snapshot(self.workspace))
        preview = self.reclaim(row["id"], dry=True)
        self.assertEqual(preview["state"], "reclaimable")
        self.assertTrue(preview["dry_run"])
        self.assertEqual((self.snapshot(self.registry), self.snapshot(stage), self.snapshot(self.workspace)), before)
        result = self.reclaim(row["id"])
        self.assertEqual(result["state"], "reclaimed", result)
        self.assertFalse(stage.exists())
        self.assertFalse(self.record_path(row).exists())
        self.assertEqual(self.reclaim(row["id"])["state"], "absent")
        self.assertEqual(self.snapshot(self.workspace), before[2])

    def test_bounded_pages_are_stable_and_do_not_reclaim(self):
        first = self.orphan()
        second = self.orphan()
        expected = sorted((first["id"], second["id"]))
        before = self.snapshot(self.registry)
        page = self.listing("--limit", "1")
        self.assertEqual([item["id"] for item in page["items"]], expected[:1])
        self.assertEqual(page["next_after"], expected[0])
        page = self.listing("--after", page["next_after"], "--limit", "1")
        self.assertEqual([item["id"] for item in page["items"]], expected[1:])
        self.assertIsNone(page["next_after"])
        self.assertEqual(self.snapshot(self.registry), before)

    def test_concurrent_reclaimers_remove_one_owned_resource_once(self):
        row = self.orphan()
        barrier = threading.Barrier(2)
        def reclaim():
            barrier.wait(timeout=5)
            return self.reclaim(row["id"])
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = [future.result(timeout=20) for future in (pool.submit(reclaim), pool.submit(reclaim))]
        self.assertEqual(sum(result["state"] == "reclaimed" for result in results), 1, results)
        self.assertTrue(all(result["state"] in ("reclaimed", "absent", "active", "blocked") for result in results), results)
        self.assertEqual(self.reclaim(row["id"])["state"], "absent")

    def test_replaced_staging_directory_is_not_adopted(self):
        row = self.orphan()
        stage = self.stage(row)
        saved = stage.with_name(stage.name + "-owned-saved")
        stage.rename(saved)
        stage.mkdir()
        sentinel = stage / "unrelated.txt"
        sentinel.write_text("replacement must survive", encoding="utf-8")
        def restore():
            self.assertEqual(sentinel.read_text(), "replacement must survive")
            sentinel.unlink()
            stage.rmdir()
            saved.rename(stage)
        self.restore.append(restore)
        before = self.snapshot(stage)
        self.assertEqual(self.reclaim(row["id"])["state"], "blocked")
        self.assertEqual(self.snapshot(stage), before)
        self.assertTrue(self.record_path(row).is_file())

    @unittest.skipUnless(os.name == "nt", "Windows normal teardown compares the captured staging identity")
    def test_normal_destruction_protects_staging_identity_during_replacement(self):
        process, row = self.start_host()
        stage = self.stage(row)
        saved = stage.with_name(stage.name + "-owned-saved")
        original = self.snapshot(stage)
        try:
            stage.rename(saved)
        except PermissionError as error:
            if error.winerror != 5:
                raise
            # A live Windows image/working-directory handle can already pin
            # this tree. That refusal must preserve ownership and still allow
            # ordinary teardown, without weakening the fixture's share modes.
            self.assertEqual(self.snapshot(stage), original)
            self.assertFalse(saved.exists())
            self.assertTrue(self.record_path(row).is_file())
            process.stdin.write(b"stop\n")
            process.stdin.flush()
            out, err = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, out + err)
            self.assertFalse(stage.exists())
            self.assertFalse(self.record_path(row).exists())
            self.assertEqual(self.listing()["items"], [])
            return
        sentinel = stage / "unrelated.txt"
        def restore():
            if not saved.exists():
                return
            if stage.exists():
                self.assertEqual(sentinel.read_text(), "replacement must survive normal teardown")
                sentinel.unlink()
                stage.rmdir()
            saved.rename(stage)
        self.restore.append(restore)
        stage.mkdir()
        sentinel.write_text("replacement must survive normal teardown", encoding="utf-8")
        before = self.snapshot(stage)
        process.stdin.write(b"stop\n")
        process.stdin.flush()
        out, err = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 0, out + err)
        self.assertEqual(self.snapshot(stage), before)
        self.assertTrue(self.record_path(row).is_file())
        self.assertEqual(self.reclaim(row["id"])["state"], "blocked")
        self.assertEqual(self.snapshot(stage), before)
        # Restore the exact captured native object, then let the product reclaim
        # it. The test never substitutes a fresh directory for owned evidence.
        restore()
        self.assertEqual(self.reclaim(row["id"])["state"], "reclaimed")
        self.assertFalse(stage.exists())
        self.assertFalse(self.record_path(row).exists())

    def test_hardlinked_record_is_refused_without_altering_other_link(self):
        row = self.orphan()
        record = self.record_path(row)
        other = self.root / "record-other-link.json"
        original = record.read_bytes()
        os.link(record, other)
        self.restore.append(other.unlink)
        result = self.reclaim(row["id"])
        self.assertEqual(result["state"], "blocked", result)
        self.assertEqual(other.read_bytes(), original)
        self.assertTrue(self.stage(row).exists())

    def test_registry_path_replacement_cannot_redirect_reclamation(self):
        row = self.orphan()
        saved = self.registry.with_name("saved owned registry")
        self.registry.rename(saved)
        outside = self.root / "unrelated directory"
        outside.mkdir()
        sentinel = outside / "sentinel.txt"
        sentinel.write_text("unrelated registry target", encoding="utf-8")
        if os.name == "nt":
            result = subprocess.run(["cmd", "/c", "mklink", "/J", str(self.registry), str(outside)],
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.registry.symlink_to(outside, target_is_directory=True)
        def restore():
            if os.name == "nt":
                os.rmdir(self.registry)
            else:
                self.registry.unlink()
            saved.rename(self.registry)
        self.restore.append(restore)
        before = self.snapshot(outside)
        self.assertNotEqual(self.capture("sandbox-resources").returncode, 0)
        result = self.capture("sandbox-reclaim", "--id", row["id"])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.snapshot(outside), before)
        self.assertTrue(self.stage(row).exists())

    def test_malformed_owner_identity_blocks_and_preserves_registry(self):
        row = self.orphan()
        record = self.record_path(row)
        original = record.read_bytes()
        changed = json.loads(original)
        changed["owner"]["identity"] = "not-a-process-identity"
        record.write_text(json.dumps(changed), encoding="utf-8")
        self.restore.append(lambda: record.write_bytes(original))
        before = self.snapshot(self.registry)
        result = self.reclaim(row["id"])
        self.assertEqual(result["state"], "blocked", result)
        self.assertEqual(self.snapshot(self.registry), before)
        self.assertTrue(self.stage(row).exists())

    def test_reused_pid_observation_never_signals_the_different_live_process(self):
        row = self.orphan()
        record = self.record_path(row)
        original = record.read_bytes()
        changed = json.loads(original)
        changed["owner"]["pid"] = os.getpid()
        record.write_text(json.dumps(changed), encoding="utf-8")
        self.restore.append(lambda: record.write_bytes(original) if record.exists() else None)
        self.assertEqual(self.reclaim(row["id"], dry=True)["state"], "reclaimable")
        self.assertEqual(self.reclaim(row["id"])["state"], "reclaimed")
        # Reaching this assertion proves the PID named in the stale journal was
        # not killed. The birth identity continues to identify the dead host.
        self.assertGreater(os.getpid(), 0)

    def test_cancelled_reclaim_preserves_resources_for_retry(self):
        row = self.orphan()
        before = (self.snapshot(self.registry), self.snapshot(self.stage(row)))
        result = subprocess.run([str(HOST), "cancel", row["id"]], env=self.env,
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["state"], "cancelled")
        self.assertEqual((self.snapshot(self.registry), self.snapshot(self.stage(row))), before)
        self.assertEqual(self.reclaim(row["id"])["state"], "reclaimed")

    def test_nested_link_cannot_escape_owned_staging(self):
        row = self.orphan()
        stage = self.stage(row)
        outside = self.root / "external"
        outside.mkdir()
        sentinel = outside / "sentinel.txt"
        sentinel.write_text("not part of the sandbox", encoding="utf-8")
        link = stage / "work" / "outside-link"
        if os.name == "nt":
            result = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.restore.append(lambda: os.rmdir(link) if link.exists() else None)
        else:
            link.symlink_to(outside, target_is_directory=True)
            self.restore.append(lambda: link.unlink() if link.is_symlink() else None)
        before = self.snapshot(outside)
        result = self.reclaim(row["id"])
        self.assertIn(result["state"], ("reclaimed", "blocked", "incomplete"), result)
        self.assertEqual(self.snapshot(outside), before)

    @unittest.skipUnless(os.name == "nt", "Windows file sharing provides deterministic partial cleanup refusal")
    def test_incomplete_cleanup_keeps_journal_and_retries_after_file_unlock(self):
        row = self.orphan()
        held = self.stage(row) / "work" / "held.txt"
        held.write_text("owned file temporarily open", encoding="utf-8")
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p,
                                      ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.CreateFileW(str(held), 0x80000000, 1, None, 3, 0x80, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            result = self.reclaim(row["id"])
            self.assertIn(result["state"], ("incomplete", "blocked"), result)
            self.assertTrue(self.record_path(row).exists())
            self.assertEqual(held.read_text(), "owned file temporarily open")
        finally:
            kernel.CloseHandle(handle)
        self.assertEqual(self.reclaim(row["id"])["state"], "reclaimed")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux cgroup topology refusal")
    def test_nonempty_cgroup_keeps_journal_until_owned_blocker_is_removed(self):
        row = self.orphan()
        group = Path(next(resource["path"] for resource in row["resources"] if resource["kind"] == "cgroup"))
        blocker = group / "owned-test-blocker"
        blocker.mkdir()
        try:
            result = self.reclaim(row["id"])
            self.assertIn(result["state"], ("blocked", "incomplete"), result)
            self.assertTrue(self.record_path(row).exists())
            self.assertTrue(blocker.is_dir())
        finally:
            blocker.rmdir()
        self.assertEqual(self.reclaim(row["id"])["state"], "reclaimed")

    def test_service_listing_preview_and_explicit_reclaim_share_cli_contract(self):
        row = self.orphan()
        client = service_fixture.Client(self.workspace, self.env)
        self.clients.append(client)
        contract = Contract()
        def rpc(method, params=None):
            result = client.rpc(method, params)
            contract.validate_response(method, {"jsonrpc": "2.0", "id": 1, "result": result})
            return result
        initialized = client.initialize()
        contract.validate_response("initialize", {"jsonrpc": "2.0", "id": 1, "result": initialized})
        self.assertTrue(initialized["capabilities"]["sandbox_resources"])
        listed = rpc("sandboxes/list", {"limit": 1})
        self.assertEqual([item["id"] for item in listed["items"]], [row["id"]])
        for method, params in (("sandboxes/list", {"limit": True}), ("sandboxes/list", {"limit": 0}),
                               ("sandboxes/list", {"after": "../escape"}),
                               ("sandboxes/reclaim", {"id": "../escape"}),
                               ("sandboxes/reclaim", {"id": row["id"], "dry_run": "yes"})):
            error = client.rpc(method, params, error=True)
            contract.validate_response(method, {"jsonrpc": "2.0", "id": 1, "error": error})
            self.assertEqual(error["code"], -32602)
        before = self.snapshot(self.registry)
        client.send_raw(json.dumps({"jsonrpc": "2.0", "method": "sandboxes/reclaim", "params": {"id": row["id"]}}) + "\n")
        rpc("sandboxes/list")  # Ordered barrier proves notification was processed.
        self.assertEqual(self.snapshot(self.registry), before)
        accepted = rpc("sandboxes/reclaim", {"id": row["id"], "dry_run": True})
        done = client.finished(accepted["operation_id"])
        contract.validate_completion("sandboxes/reclaim", done)
        self.assertEqual(done["result"]["state"], "reclaimable")
        self.assertEqual(self.snapshot(self.registry), before)
        accepted = rpc("sandboxes/reclaim", {"id": row["id"]})
        done = client.finished(accepted["operation_id"])
        contract.validate_completion("sandboxes/reclaim", done)
        self.assertEqual(done["result"]["state"], "reclaimed")
        self.assertFalse((self.workspace / ".codestudio" / "state.sqlite3").exists())


if __name__ == "__main__":
    unittest.main()
