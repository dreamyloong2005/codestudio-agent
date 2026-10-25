"""Exercise extracted native deliveries and user installation in owned temp roots.

No default installation, PATH change, startup registration, inference or real
credential lookup is requested. The parent runner builds/packages first.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import queue
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
from contextlib import closing, redirect_stdout
from unittest.mock import patch


parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("agent", type=Path)
parser.add_argument("--package", type=Path, required=True)
arguments, remaining = parser.parse_known_args()
AGENT = arguments.agent.resolve()
PACKAGE = arguments.package.resolve()
sys.argv[:] = [sys.argv[0], *remaining]
EXECUTABLE = "codestudio-agent.exe" if os.name == "nt" else "codestudio-agent"
RECEIPT = ".codestudio-install.json"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def profile_snapshot():
    if os.name == "nt":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment") as key:
                return winreg.QueryValueEx(key, "Path")
        except FileNotFoundError:
            return None
    return {name: digest(Path.home() / name) if (Path.home() / name).is_file() else None
            for name in (".profile", ".bash_profile", ".bashrc", ".zprofile", ".zshrc")}


class DeliveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not AGENT.is_file() or not PACKAGE.is_dir():
            raise RuntimeError("Build the native Agent and extract/create its package before verification")
        cls.original_manifest = json.loads((PACKAGE / "package.json").read_text(encoding="utf-8"))
        if digest(PACKAGE / EXECUTABLE) != digest(AGENT):
            raise RuntimeError("The package executable does not match the native binary under verification")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="codestudio-delivery-")
        self.root = Path(self.temporary.name).resolve()
        self.package = self.root / "extracted package"
        shutil.copytree(PACKAGE, self.package)
        self.prefix = self.root / "user applications" / "CodeStudio-Agent"
        self.prefix.parent.mkdir()
        self.workspace = self.root / "user workspace"
        self.workspace.mkdir()
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("CODESTUDIO_")}
        self.children = []
        self.worker_started = False

    def tearDown(self):
        try:
            if self.worker_started and (self.prefix / EXECUTABLE).is_file():
                self.agent("scheduler-stop", "--workspace", self.workspace)
                self.wait_for(lambda: self.agent("scheduler-status", "--workspace", self.workspace)["state"] == "stopped")
            for child in self.children:
                if child.poll() is None:
                    child.kill()
                child.communicate(timeout=8)
        finally:
            self.temporary.cleanup()

    def invoke(self, operation, expected=0, package=None, prefix=None, script=None):
        source = self.package if package is None else package
        command = [sys.executable, str(script or self.package / "install.py"), operation,
                   "--prefix", str(self.prefix if prefix is None else prefix)]
        if operation in ("install", "upgrade"):
            command += ["--package", str(source)]
        result = subprocess.run(command, cwd=self.root, env=self.env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        response = json.loads(result.stdout)
        self.assertEqual(response["product"], "CodeStudio-Agent")
        self.assertEqual(response["operation"], operation)
        if expected:
            self.assertEqual(response["status"], "error")
        return response

    def install(self):
        result = self.invoke("install")
        self.assertEqual(result["status"], "installed")
        self.assertTrue((self.prefix / RECEIPT).is_file())
        return result

    def agent(self, *args, expected=0, input=None):
        result = subprocess.run([str(self.prefix / EXECUTABLE), *map(str, args)], cwd=self.workspace,
                                env=self.env, input=input, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def tree_snapshot(self, directory):
        if not directory.exists():
            return None
        return {str(path.relative_to(directory)): digest(path) for path in directory.rglob("*") if path.is_file()}

    def manifest(self):
        return json.loads((self.package / "package.json").read_text(encoding="utf-8"))

    def save_manifest(self, manifest, refresh_files=False):
        if refresh_files:
            for record in manifest["files"]:
                path = self.package / record["path"]
                record["sha256"] = digest(path)
                record["size"] = path.stat().st_size
        (self.package / "package.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        checksums = {record["path"]: record["sha256"] for record in manifest["files"]}
        checksums["package.json"] = digest(self.package / "package.json")
        (self.package / "SHA256SUMS").write_text("".join(f"{checksums[name]}  {name}\n" for name in sorted(checksums)),
                                               encoding="utf-8")

    def change_package(self):
        with (self.package / "README.txt").open("ab") as output:
            output.write(b"\nDelivery regression replacement.\n")
        self.save_manifest(self.manifest(), refresh_files=True)

    def installer_module(self):
        # Execute the packaged source without importlib's bytecode cache: a
        # __pycache__ would itself make the exact package inventory invalid.
        source = self.package / "install.py"
        module = types.ModuleType("delivery_fault_installer")
        module.__file__ = str(source)
        exec(compile(source.read_bytes(), str(source), "exec"), module.__dict__)
        return module

    def wait_for(self, condition, seconds=8):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(0.02)
        self.fail("Owned delivery process did not reach the required state")

    def hold_service(self):
        child = subprocess.Popen([str(self.prefix / EXECUTABLE), "serve", "--workspace", str(self.workspace)],
                                 cwd=self.workspace, env=self.env, stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(child)
        responses = queue.Queue()
        threading.Thread(target=lambda: responses.put(child.stdout.readline()), daemon=True).start()
        child.stdin.write(b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocol_version":2}}\n')
        child.stdin.flush()
        self.assertEqual(json.loads(responses.get(timeout=8))["result"]["protocol_version"], 2)
        return child

    def test_package_inventory_hashes_and_native_identity_match(self):
        manifest = self.manifest()
        self.assertEqual((manifest["format"], manifest["product"]), (2, "CodeStudio-Agent"))
        self.assertEqual(manifest["platform"], "windows" if os.name == "nt" else "linux")
        self.assertEqual(manifest["arch"], "x64" if os.name == "nt" else "x86_64")
        paths = [record["path"] for record in manifest["files"]]
        self.assertEqual(len(paths), len(set(paths)))
        self.assertIn(EXECUTABLE, paths)
        self.assertIn("install.py", paths)
        self.assertIn("protocol/task-service.schema.json", paths)
        expected = set(paths) | {"package.json", "SHA256SUMS"}
        actual = {path.relative_to(self.package).as_posix() for path in self.package.rglob("*") if path.is_file()}
        self.assertEqual(actual, expected)
        hashes = {}
        for line in (self.package / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
            checksum, name = line.split("  ", 1)
            self.assertNotIn(name, hashes)
            hashes[name] = checksum
        self.assertEqual(set(hashes), set(paths) | {"package.json"})
        for record in manifest["files"]:
            path = self.package / record["path"]
            self.assertEqual((path.stat().st_size, digest(path)), (record["size"], record["sha256"]))
            if os.name != "nt":
                self.assertEqual(bool(path.stat().st_mode & 0o111), record["executable"])
        self.assertTrue(all(digest(self.package / name) == checksum for name, checksum in hashes.items()))

    def test_installed_doctor_demo_service_and_environment_credential(self):
        self.install()
        doctor = self.agent("doctor")
        self.assertEqual(doctor["state_schema"], 7)
        self.assertEqual(doctor["mcp_transports"], ["stdio", "streamable-http"])
        demo = self.agent("demo", "--workspace", self.workspace)
        self.assertEqual(demo["status"], "completed")
        self.assertEqual((self.workspace / "CODESTUDIO_DEMO.txt").read_bytes(), b"Hello from CodeStudio-Agent!\n")
        self.assertEqual(self.agent("history", "--workspace", self.workspace)[0]["id"], demo["task_id"])
        frames = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocol_version": 2}},
                  {"jsonrpc": "2.0", "id": 2, "method": "tasks/list", "params": {}},
                  {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": {}}]
        response = subprocess.run([str(self.prefix / EXECUTABLE), "serve", "--workspace", str(self.workspace)],
                                  cwd=self.workspace, env=self.env, input="".join(json.dumps(frame) + "\n" for frame in frames).encode(),
                                  capture_output=True, timeout=10)
        self.assertEqual(response.returncode, 0, response.stderr)
        received = {frame["id"]: frame for frame in map(json.loads, response.stdout.splitlines()) if "id" in frame}
        self.assertTrue(received[1]["result"]["capabilities"]["mcp_streamable_http"])
        self.assertEqual(received[2]["result"]["items"][0]["id"], demo["task_id"])
        token = "synthetic-delivery-key-never-emit"
        self.env["CODESTUDIO_DELIVERY_KEY"] = token
        credential = self.agent("credential-status", "--credential", "env:CODESTUDIO_DELIVERY_KEY")
        self.assertEqual(credential["state"], "available")
        self.assertNotIn(token, json.dumps(credential))
        self.assertNotIn(token.encode(), (self.workspace / ".codestudio" / "state.sqlite3").read_bytes())

    def test_status_absent_install_and_same_package_are_idempotent(self):
        self.assertEqual(self.invoke("status")["status"], "absent")
        self.assertFalse(self.prefix.exists())
        self.install()
        before = self.tree_snapshot(self.prefix)
        self.assertEqual(self.invoke("status")["status"], "installed")
        self.assertEqual(self.invoke("install")["status"], "unchanged")
        self.assertEqual(self.invoke("upgrade")["status"], "unchanged")
        self.assertEqual(self.tree_snapshot(self.prefix), before)

    def test_upgrade_replaces_owned_files_and_preserves_sibling_workspace(self):
        self.install()
        self.agent("demo", "--workspace", self.workspace)
        saved = self.tree_snapshot(self.workspace)
        old_binary = digest(self.prefix / EXECUTABLE)
        self.change_package()
        self.assertEqual(self.invoke("upgrade")["status"], "upgraded")
        self.assertEqual((self.prefix / "README.txt").read_bytes(), (self.package / "README.txt").read_bytes())
        self.assertEqual(digest(self.prefix / EXECUTABLE), old_binary)
        self.assertEqual(self.tree_snapshot(self.workspace), saved)
        self.assertEqual(self.invoke("status")["status"], "installed")
        self.assertEqual(self.agent("doctor")["state_schema"], 7)

    def test_downgrade_refused_without_modifying_installed_files(self):
        manifest = self.manifest()
        manifest["version"] = "9.0.0"
        self.save_manifest(manifest)
        self.install()
        before = self.tree_snapshot(self.prefix)
        manifest["version"] = "8.0.0"
        self.save_manifest(manifest)
        self.invoke("upgrade", expected=2)
        self.assertEqual(self.tree_snapshot(self.prefix), before)

    def test_install_different_package_and_upgrade_absent_refused(self):
        self.invoke("upgrade", expected=2)
        self.assertFalse(self.prefix.exists())
        self.install()
        before = self.tree_snapshot(self.prefix)
        self.change_package()
        self.invoke("install", expected=2)
        self.assertEqual(self.tree_snapshot(self.prefix), before)

    def test_uninstall_via_installed_script_preserves_user_state_and_environment(self):
        initial_environment = profile_snapshot()
        self.install()
        self.agent("demo", "--workspace", self.workspace)
        preserved = self.tree_snapshot(self.workspace)
        self.assertEqual(self.invoke("uninstall", script=self.prefix / "install.py")["status"], "uninstalled")
        self.assertFalse(self.prefix.exists())
        self.assertEqual(self.tree_snapshot(self.workspace), preserved)
        self.assertEqual(profile_snapshot(), initial_environment)
        self.assertEqual(self.invoke("uninstall")["status"], "absent")
        self.assertEqual(self.invoke("status")["status"], "absent")

    def test_corrupt_package_payload_rejected_before_install_and_upgrade(self):
        self.install()
        before = self.tree_snapshot(self.prefix)
        with (self.package / "README.txt").open("ab") as output:
            output.write(b"corruption without matching checksum")
        fresh = self.prefix.with_name("fresh-install")
        self.invoke("install", expected=2, prefix=fresh)
        self.assertFalse(fresh.exists())
        self.invoke("upgrade", expected=2)
        self.assertEqual(self.tree_snapshot(self.prefix), before)

    def test_corrupt_checksum_inventory_rejected_without_destination(self):
        path = self.package / "SHA256SUMS"
        original = path.read_bytes()
        for contents in (b"0" * 64 + b"  package.json\n", original + original.splitlines(keepends=True)[0], b""):
            with self.subTest(contents_length=len(contents)):
                path.write_bytes(contents)
                self.invoke("install", expected=2)
                self.assertFalse(self.prefix.exists())

    def test_changed_missing_and_extra_installed_data_never_deleted(self):
        self.install()
        path = self.prefix / "README.txt"
        original = path.read_bytes()
        extra = self.prefix / "user-created-private-data.txt"
        for mutation in ("changed", "missing", "extra"):
            with self.subTest(mutation=mutation):
                if mutation == "changed":
                    path.write_bytes(original + b"user modification")
                elif mutation == "missing":
                    path.unlink()
                else:
                    extra.write_bytes(b"must survive refused uninstall")
                before = self.tree_snapshot(self.prefix)
                self.invoke("status", expected=2)
                self.invoke("upgrade", expected=2)
                self.invoke("uninstall", expected=2)
                self.assertEqual(self.tree_snapshot(self.prefix), before)
                path.write_bytes(original)
                if extra.exists():
                    extra.unlink()

    def test_extra_package_file_cannot_bypass_manifest(self):
        (self.package / "unlisted-executable.txt").write_bytes(b"not in verified inventory")
        self.invoke("install", expected=2)
        self.assertFalse(self.prefix.exists())

    def test_occupied_unowned_prefix_never_overwritten_or_removed(self):
        self.prefix.mkdir()
        data = self.prefix / "existing-user-file"
        data.write_bytes(b"private existing content")
        before = self.tree_snapshot(self.prefix)
        for operation in ("install", "upgrade", "uninstall", "status"):
            with self.subTest(operation=operation):
                self.invoke(operation, expected=2)
                self.assertEqual(self.tree_snapshot(self.prefix), before)

    def test_manifest_paths_cannot_escape_or_use_aliases(self):
        original = self.manifest()
        outside = self.root / "outside-sentinel"
        outside.write_bytes(b"outside installation boundary")
        for path in ("../outside-sentinel", str(outside), "docs/../../outside-sentinel", "docs\\escape.txt", "README.txt:stream", "./README.txt"):
            with self.subTest(path=path):
                manifest = json.loads(json.dumps(original))
                manifest["files"].append({"path": path, "size": 0, "sha256": "0" * 64, "executable": False})
                self.save_manifest(manifest)
                self.invoke("install", expected=2)
                self.assertFalse(self.prefix.exists())
                self.assertEqual(outside.read_bytes(), b"outside installation boundary")

    def test_unsupported_format_foreign_platform_and_duplicate_paths_rejected(self):
        original = self.manifest()
        changed = []
        for key, value in (("format", 1), ("format", 2.0), ("product", "different-product"),
                           ("platform", "linux" if os.name == "nt" else "windows"), ("arch", "arm64")):
            manifest = json.loads(json.dumps(original))
            manifest[key] = value
            changed.append(manifest)
        duplicate = json.loads(json.dumps(original))
        duplicate["files"].append(dict(duplicate["files"][0]))
        changed.append(duplicate)
        for index, manifest in enumerate(changed):
            with self.subTest(index=index):
                self.save_manifest(manifest)
                self.invoke("install", expected=2)
                self.assertFalse(self.prefix.exists())

    def test_modified_ownership_receipt_cannot_redirect_uninstall(self):
        self.install()
        path = self.prefix / RECEIPT
        receipt = json.loads(path.read_text(encoding="utf-8"))
        receipt["prefix"] = str(self.workspace)
        path.write_text(json.dumps(receipt), encoding="utf-8")
        (self.workspace / "protected").write_bytes(b"keep")
        before = self.tree_snapshot(self.prefix)
        self.invoke("uninstall", expected=2)
        self.assertEqual(self.tree_snapshot(self.prefix), before)
        self.assertEqual((self.workspace / "protected").read_bytes(), b"keep")

    def test_hardlinked_package_payload_is_rejected(self):
        payload = self.package / "README.txt"
        external = self.root / "external-readme"
        external.write_bytes(payload.read_bytes())
        payload.unlink()
        os.link(external, payload)
        self.invoke("install", expected=2)
        self.assertFalse(self.prefix.exists())

    def test_linked_destination_is_rejected_without_following_it(self):
        try:
            self.prefix.symlink_to(self.workspace, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("Host does not permit creating a test directory symlink")
        try:
            for operation in ("install", "uninstall", "status"):
                self.invoke(operation, expected=2)
            self.assertTrue(self.prefix.is_symlink())
            self.assertEqual(list(self.workspace.iterdir()), [])
        finally:
            self.prefix.unlink()

    def test_symlinked_parent_cannot_redirect_new_installation(self):
        parent = self.root / "linked-parent"
        try:
            parent.symlink_to(self.workspace, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("Host does not permit creating a test directory symlink")
        self.invoke("install", expected=2, prefix=parent / "CodeStudio-Agent")
        self.assertEqual(list(self.workspace.iterdir()), [])
        self.assertTrue(parent.is_symlink())

    def test_interrupted_installer_lock_requires_inspection_without_mutation(self):
        lock = self.prefix.with_name("." + self.prefix.name + ".codestudio-install.lock")
        lock.write_bytes(b"owned-test-stale-lock")
        self.invoke("install", expected=2)
        self.assertFalse(self.prefix.exists())
        self.assertEqual(lock.read_bytes(), b"owned-test-stale-lock")
        self.assertEqual(set(self.prefix.parent.iterdir()), {lock})

    def test_native_installer_wrapper_forwards_arguments_and_exit_status(self):
        if os.name == "nt":
            if not shutil.which("python"):
                self.skipTest("Wrapper requires python on PATH")
            command = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(self.package / "install.ps1")]
        else:
            if not shutil.which("python3"):
                self.skipTest("Wrapper requires python3 on PATH")
            command = ["sh", str(self.package / "install.sh")]
        for operation, expected in (("status", 0), ("upgrade", 2)):
            with self.subTest(operation=operation):
                args = [*command, operation, "--prefix", str(self.prefix)]
                if operation == "upgrade":
                    args += ["--package", str(self.package)]
                result = subprocess.run(args, cwd=self.root, env=self.env, capture_output=True, timeout=15)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                response = json.loads(result.stdout)
                self.assertEqual(response["operation"], operation)
                self.assertEqual(response["status"], "absent" if expected == 0 else "error")
                self.assertFalse(self.prefix.exists())

    def test_running_installed_service_refuses_upgrade_and_uninstall(self):
        self.install()
        child = self.hold_service()
        before = self.tree_snapshot(self.prefix)
        self.change_package()
        self.invoke("upgrade", expected=2)
        self.invoke("uninstall", expected=2)
        self.assertEqual(self.tree_snapshot(self.prefix), before)
        self.assertIsNone(child.poll())
        child.stdin.close()
        child.stdin = None
        output, error = child.communicate(timeout=8)
        self.assertEqual(child.returncode, 0, output + error)
        self.assertEqual(self.invoke("upgrade")["status"], "upgraded")

    def test_partial_staging_failure_retains_stage_and_previous_installation(self):
        self.install()
        self.agent("demo", "--workspace", self.workspace)
        previous = self.tree_snapshot(self.prefix)
        workspace = self.tree_snapshot(self.workspace)
        self.change_package()
        installer = self.installer_module()
        real_open = Path.open
        opened = []

        def fail_second_staged_file(path, mode="r", *args, **kwargs):
            if mode == "xb" and path.is_relative_to(self.prefix.parent):
                relative = path.relative_to(self.prefix.parent)
                if relative.parts[0].startswith("." + self.prefix.name + ".stage-"):
                    if opened:
                        raise OSError("Synthetic staging write failure")
                    stream = real_open(path, mode, *args, **kwargs)
                    opened.append(path)
                    return stream
            return real_open(path, mode, *args, **kwargs)

        output = io.StringIO()
        with patch.object(Path, "open", fail_second_staged_file), redirect_stdout(output):
            code = installer.main(["upgrade", "--package", str(self.package), "--prefix", str(self.prefix)])
        result = json.loads(output.getvalue())
        self.assertEqual((code, result["operation"], result["status"]), (2, "upgrade", "staging_incomplete"))
        self.assertEqual(len(opened), 1, "Fault must follow a real completed staged file copy")
        retained = Path(result["retained_path"])
        self.assertEqual(retained.parent, self.prefix.parent)
        self.assertTrue(retained.name.startswith("." + self.prefix.name + ".stage-"))
        self.assertTrue(opened[0].is_file())
        name = opened[0].relative_to(retained)
        self.assertEqual(opened[0].read_bytes(), (self.package / name).read_bytes())
        self.assertLess(len(self.tree_snapshot(retained)), len(self.manifest()["files"]))
        self.assertEqual(self.tree_snapshot(self.prefix), previous)
        self.assertEqual(self.tree_snapshot(self.workspace), workspace)
        self.assertFalse(self.prefix.with_name("." + self.prefix.name + ".codestudio-install.lock").exists())
        self.assertEqual(self.invoke("status")["status"], "installed")

    def test_partial_uninstall_failure_keeps_quarantine_without_restoring_broken_prefix(self):
        self.install()
        self.agent("demo", "--workspace", self.workspace)
        previous = self.tree_snapshot(self.prefix)
        workspace = self.tree_snapshot(self.workspace)
        installer = self.installer_module()
        real_unlink = Path.unlink
        removed = []

        def fail_after_first_removal(path, *args, **kwargs):
            if path.is_relative_to(self.prefix.parent):
                relative = path.relative_to(self.prefix.parent)
                if relative.parts[0].startswith("." + self.prefix.name + ".remove-"):
                    if removed:
                        raise OSError("Synthetic uninstall unlink failure")
                    result = real_unlink(path, *args, **kwargs)
                    removed.append(relative)
                    return result
            return real_unlink(path, *args, **kwargs)

        output = io.StringIO()
        with patch.object(Path, "unlink", fail_after_first_removal), redirect_stdout(output):
            code = installer.main(["uninstall", "--prefix", str(self.prefix)])
        result = json.loads(output.getvalue())
        self.assertEqual((code, result["operation"], result["status"]), (2, "uninstall", "cleanup_incomplete"))
        self.assertEqual(len(removed), 1, "Fault must follow an actual successful payload unlink")
        retained = Path(result["retained_path"])
        self.assertEqual(retained.parent, self.prefix.parent)
        self.assertTrue(retained.name.startswith("." + self.prefix.name + ".remove-"))
        self.assertFalse(self.prefix.exists(), "A partially removed tree must not appear as an installed package")
        removed_name = str(Path(*removed[0].parts[1:]))
        self.assertEqual(self.tree_snapshot(retained), {name: value for name, value in previous.items() if name != removed_name})
        self.assertEqual(self.tree_snapshot(self.workspace), workspace)
        self.assertFalse(self.prefix.with_name("." + self.prefix.name + ".codestudio-install.lock").exists())
        self.assertEqual(self.invoke("status")["status"], "absent")
        self.assertTrue(retained.is_dir())

    def test_installed_background_worker_start_stop_or_verified_job_refusal(self):
        self.install()
        self.agent("demo", "--workspace", self.workspace)
        database = self.workspace / ".codestudio" / "state.sqlite3"
        with closing(sqlite3.connect(database)) as connection:
            before = "\n".join(connection.iterdump())
        started = subprocess.run([str(self.prefix / EXECUTABLE), "scheduler-start", "--workspace", str(self.workspace),
                                  "--poll-ms", "25"], env=self.env, capture_output=True, timeout=15)
        result = json.loads(started.stdout)
        if os.name == "nt" and started.returncode == 2 and result.get("error") == "Cannot detach scheduler from the parent Job; run scheduler-run from an independent terminal":
            self.assertEqual(self.agent("scheduler-status", "--workspace", self.workspace)["state"], "stopped")
            with closing(sqlite3.connect(database)) as connection:
                self.assertEqual("\n".join(connection.iterdump()), before)
            self.skipTest("Enclosing Windows Job prevents detached start; installed refusal preserved the database")
        self.assertEqual(started.returncode, 0, started.stdout + started.stderr)
        self.worker_started = True
        self.assertTrue(result["started"])
        self.wait_for(lambda: self.agent("scheduler-status", "--workspace", self.workspace)["state"] == "running")
        self.assertTrue(self.agent("scheduler-stop", "--workspace", self.workspace)["stop_requested"])
        self.wait_for(lambda: self.agent("scheduler-status", "--workspace", self.workspace)["state"] == "stopped")
        self.worker_started = False
        self.assertEqual(self.invoke("uninstall")["status"], "uninstalled")


if __name__ == "__main__":
    unittest.main()
