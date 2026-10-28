"""Exercise extracted native deliveries and user installation in owned temp roots.

No default installation, PATH change, startup registration, inference or real
credential lookup is requested. The parent runner builds/packages first.
"""
import argparse
import ctypes
from datetime import datetime, timezone
import errno
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import queue
import shutil
import sqlite3
import stat
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
if "--standalone" not in sys.argv[1:]:
    parser.add_argument("agent", type=Path)
else:
    parser.set_defaults(agent=None)
parser.add_argument("--package", type=Path, required=True)
parser.add_argument("--standalone", action="store_true", help="Validate an extracted package without its original build tree")
parser.add_argument("--receipt", type=Path, help="New external machine-readable result file")
arguments, remaining = parser.parse_known_args()
if arguments.standalone == (arguments.agent is not None):
    parser.error("Use either the native build agent argument or --standalone, not both")
AGENT = arguments.agent.resolve() if arguments.agent is not None else None
PACKAGE = arguments.package.resolve()
sys.argv[:] = [sys.argv[0], *remaining]
EXECUTABLE = "codestudio-agent.exe" if os.name == "nt" else "codestudio-agent"
RECEIPT = ".codestudio-install.json"
TARGET = "windows" if os.name == "nt" else "macosx" if sys.platform == "darwin" else "linux"
ARCH = "x64" if os.name == "nt" else "arm64" if sys.platform == "darwin" else "x86_64"
RESULT_RECEIPT = arguments.receipt.absolute() if arguments.receipt else None
PACKAGE_SNAPSHOT = None
PACKAGE_METADATA = None
SOURCE_BINARY_MATCHED = False
class ReceiptDestination:
    """Pin an external receipt parent and create only a new exact file there."""
    def __init__(self, path, package):
        self.path, self.package = path, package
        self.parent = path.parent.resolve(strict=True)
        self.descriptor = os.open(self.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) |
                                  getattr(os, "O_NOFOLLOW", 0)) if os.name != "nt" else None
        try:
            if path.exists() or path.is_symlink():
                raise ValueError("Receipt already exists")
            self.verify()
        except BaseException:
            self.close()
            raise

    def verify(self):
        if not self.parent.is_dir():
            raise ValueError("Receipt parent is unavailable")
        parents = [self.parent]
        if self.descriptor is not None:
            opened, named = os.fstat(self.descriptor), self.parent.stat()
            if (opened.st_dev, opened.st_ino) != (named.st_dev, named.st_ino):
                raise ValueError("Receipt parent was replaced")
            if sys.platform == "darwin":
                import fcntl
                raw = fcntl.fcntl(self.descriptor, 102, bytes(1024))  # F_GETPATH_NOFIRMLINK
                physical = Path(os.fsdecode(raw.split(b"\0", 1)[0]))
                physical_info = physical.stat()
                if (physical_info.st_dev, physical_info.st_ino) != (opened.st_dev, opened.st_ino):
                    raise ValueError("Physical receipt parent was replaced")
                parents.append(physical)
        try:
            package = self.package.stat()
        except FileNotFoundError:
            return  # Invalid-package runs can still write an external failed receipt.
        for parent in parents:
            for ancestor in (parent, *parent.parents):
                observed = ancestor.stat()
                if (observed.st_dev, observed.st_ino) == (package.st_dev, package.st_ino):
                    raise ValueError("Receipt must be outside the package")

    def write(self, value):
        self.verify()
        if self.descriptor is None:
            with self.path.open("x", encoding="utf-8") as stream:
                json.dump(value, stream, ensure_ascii=False, indent=2)
                stream.write("\n")
            return
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(self.path.name, flags, 0o600, dir_fd=self.descriptor)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write("\n")

    def close(self):
        if self.descriptor is not None:
            os.close(self.descriptor)
            self.descriptor = None


RECEIPT_DESTINATION = None
if RESULT_RECEIPT is not None:
    try:
        RECEIPT_DESTINATION = ReceiptDestination(RESULT_RECEIPT, PACKAGE)
    except (OSError, ValueError):
        parser.error("--receipt must be a new file in an existing directory outside the package")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def package_snapshot(root):
    """Freeze all package bytes and paths, including metadata, before tests.

    Each copied fixture is compared with this snapshot and the original package
    is checked again after the run. A receipt never binds tests to later bytes.
    """
    records, directories, total_bytes, entries = {}, [], 0, 0
    pending = [root]
    while pending:
        directory = pending.pop()
        for path in directory.iterdir():
            entries += 1
            if entries > 2048:
                raise RuntimeError("Delivery evidence inventory exceeds package limit")
            info = path.lstat()
            name = path.relative_to(root).as_posix()
            if path.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
                raise RuntimeError("Delivery evidence cannot include linked package entries")
            if stat.S_ISDIR(info.st_mode):
                directories.append(name)
                pending.append(path)
                continue
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise RuntimeError("Delivery evidence requires ordinary single-link files")
            total_bytes += info.st_size
            if info.st_size > 256 * 1024 * 1024 or total_bytes > 512 * 1024 * 1024:
                raise RuntimeError("Delivery evidence bytes exceed package limit")
            hashed, length = hashlib.sha256(), 0
            flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
            with os.fdopen(os.open(path, flags), "rb") as source:
                opened = os.fstat(source.fileno())
                if (opened.st_dev, opened.st_ino, opened.st_nlink) != (info.st_dev, info.st_ino, 1):
                    raise RuntimeError("Delivery evidence file changed before snapshot")
                for data in iter(lambda: source.read(1024 * 1024), b""):
                    length += len(data)
                    if length > info.st_size:
                        raise RuntimeError("Delivery evidence file grew during snapshot")
                    hashed.update(data)
                after = os.fstat(source.fileno())
                if length != info.st_size or (after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_mode, after.st_nlink) != (
                        info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_mode, 1):
                    raise RuntimeError("Delivery evidence file changed during snapshot")
            records[name] = {"sha256": hashed.hexdigest(), "size": info.st_size,
                             "executable": bool(info.st_mode & 0o111) if os.name != "nt" else None}
    return {"files": records, "directories": sorted(directories)}


class DeliveryResult(unittest.TextTestResult):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.selected_test_ids = []

    def startTest(self, test):
        self.selected_test_ids.append(test.id())
        super().startTest(test)


def darwin_acl(path):
    library = ctypes.CDLL(None, use_errno=True)
    library.acl_get_file.argtypes = [ctypes.c_char_p, ctypes.c_int]
    library.acl_get_file.restype = ctypes.c_void_p
    library.acl_to_text.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ssize_t)]
    library.acl_to_text.restype = ctypes.c_void_p
    library.acl_free.argtypes = [ctypes.c_void_p]
    library.acl_free.restype = ctypes.c_int
    acl = library.acl_get_file(os.fsencode(path), 0x100)
    if not acl:
        if ctypes.get_errno() == getattr(errno, "ENOATTR", 93) or (ctypes.get_errno() == errno.ENOENT and path.exists()):
            return None
        raise OSError(ctypes.get_errno(), "Cannot inspect fixture ACL")
    text = None
    try:
        length = ctypes.c_ssize_t()
        text = library.acl_to_text(acl, ctypes.byref(length))
        if not text:
            raise OSError(ctypes.get_errno(), "Cannot encode fixture ACL")
        return ctypes.string_at(text, length.value)
    finally:
        if text:
            library.acl_free(text)
        library.acl_free(acl)


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
        global PACKAGE_SNAPSHOT, PACKAGE_METADATA, SOURCE_BINARY_MATCHED
        if not PACKAGE.is_dir() or (AGENT is not None and not AGENT.is_file()):
            raise RuntimeError("Provide an extracted native package and, unless standalone, its original build")
        PACKAGE_SNAPSHOT = package_snapshot(PACKAGE)
        cls.original_manifest = json.loads((PACKAGE / "package.json").read_text(encoding="utf-8"))
        metadata = json.loads((PACKAGE / "build-metadata.json").read_text(encoding="utf-8"))
        PACKAGE_METADATA = metadata
        records = PACKAGE_SNAPSHOT["files"]
        expected_names = {record["path"] for record in cls.original_manifest["files"]} | {"package.json", "SHA256SUMS"}
        if set(records) != expected_names:
            raise RuntimeError("The initial delivery package inventory differs from its manifest")
        for record in cls.original_manifest["files"]:
            observed = records[record["path"]]
            if observed["size"] != record["size"] or observed["sha256"] != record["sha256"]:
                raise RuntimeError("The initial delivery package bytes differ from their manifest")
            if os.name != "nt" and observed["executable"] != record["executable"]:
                raise RuntimeError("The initial delivery package executable modes differ from their manifest")
        payload_hash = digest(PACKAGE / EXECUTABLE)
        declared = next(record for record in cls.original_manifest["files"] if record["path"] == EXECUTABLE)
        if payload_hash != declared["sha256"] or payload_hash != metadata["binary_sha256"]:
            raise RuntimeError("The package payload does not match its manifest and build metadata")
        if AGENT is not None:
            source_hash = metadata.get("source_binary_sha256", metadata["binary_sha256"])
            if source_hash != digest(AGENT):
                raise RuntimeError("The package does not identify the native source binary under verification")
            SOURCE_BINARY_MATCHED = True
        if package_snapshot(PACKAGE) != PACKAGE_SNAPSHOT:
            raise RuntimeError("The package changed while its initial evidence was captured")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="codestudio-delivery-")
        self.root = Path(self.temporary.name).resolve()
        self.package = self.root / "extracted package"
        shutil.copytree(PACKAGE, self.package)
        if package_snapshot(self.package) != PACKAGE_SNAPSHOT:
            self.temporary.cleanup()
            raise RuntimeError("The delivery fixture differs from the frozen package snapshot")
        self.prefix = self.root / "user applications" / "CodeStudio-Agent"
        self.prefix.parent.mkdir()
        self.workspace = self.root / "user workspace"
        self.workspace.mkdir()
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(("CODESTUDIO_", "DYLD_"))}
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

    @unittest.skipUnless(sys.platform == "darwin", "Native external receipt aliases")
    def test_receipt_destination_rejects_package_alias_and_replaced_parent(self):
        import fcntl
        descriptor = os.open(self.package, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            raw = fcntl.fcntl(descriptor, 102, bytes(1024))
            physical = Path(os.fsdecode(raw.split(b"\0", 1)[0]))
        finally:
            os.close(descriptor)
        self.assertTrue(os.path.samefile(physical, self.package))
        for parent in (self.package, physical, physical / "docs"):
            target = parent / "must-not-create-receipt.json"
            with self.assertRaises(ValueError):
                ReceiptDestination(target, self.package)
            self.assertFalse(target.exists())
        external = self.root / "external-receipts"
        external.mkdir()
        pinned = ReceiptDestination(external / "receipt.json", self.package)
        try:
            pinned.write({"external": True})
        finally:
            pinned.close()
        self.assertEqual(json.loads((external / "receipt.json").read_text()), {"external": True})
        other = self.root / "replaceable-receipt-parent"
        other.mkdir()
        pinned = ReceiptDestination(other / "receipt.json", self.package)
        moved = self.package / "moved-receipt-parent"
        other.rename(moved)
        other.mkdir()
        try:
            with self.assertRaises(ValueError):
                pinned.write({"must_not_write": True})
        finally:
            pinned.close()
        self.assertEqual(list(other.iterdir()), [])
        self.assertEqual(list(moved.iterdir()), [])

    def test_package_inventory_hashes_and_native_identity_match(self):
        manifest = self.manifest()
        self.assertEqual((manifest["format"], manifest["product"]), (2, "CodeStudio-Agent"))
        self.assertEqual(manifest["platform"], TARGET)
        self.assertEqual(manifest["arch"], ARCH)
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

    @unittest.skipUnless(sys.platform == "darwin", "Native macOS delivery behavior")
    def test_installed_private_runtime_and_iana_dst(self):
        self.install()
        for name in ("libc++.1.dylib", "libc++abi.1.dylib"):
            self.assertTrue((self.prefix / "lib" / name).is_file())
            self.assertFalse((self.prefix / "lib" / name).is_symlink())
        self.assertFalse(any(name.startswith(("DYLD_", "CODESTUDIO_")) for name in self.env))
        self.assertEqual(self.agent("doctor")["state_schema"], 7)
        epoch = lambda value: int(datetime.fromisoformat(value).replace(tzinfo=timezone.utc).timestamp() * 1000)
        folded = self.agent("cron-preview", "--workspace", self.workspace, "--cron", "30 1 * * *",
                            "--timezone", "America/New_York", "--after", epoch("2026-11-01T00:00:00"), "--count", 2)
        self.assertEqual([item["at_ms"] for item in folded["occurrences"]],
                         [epoch("2026-11-01T05:30:00"), epoch("2026-11-01T06:30:00")])
        gap = self.agent("cron-preview", "--workspace", self.workspace, "--cron", "30 2 * * *",
                         "--timezone", "America/New_York", "--after", epoch("2026-03-08T00:00:00"), "--count", 1)
        self.assertEqual(gap["occurrences"][0]["at_ms"], epoch("2026-03-09T06:30:00"))

    @unittest.skipUnless(sys.platform == "darwin", "Native macOS path aliases")
    def test_firmlink_and_unicode_aliases_share_installation_ownership(self):
        self.prefix = self.prefix.with_name("Caf\u00e9 Agent")
        self.install()
        installer = self.installer_module()
        physical = installer.darwin_path(self.prefix, physical=True)
        self.assertTrue(os.path.samefile(physical, self.prefix))
        self.assertEqual(self.invoke("status", prefix=physical)["status"], "installed")
        decomposed = self.prefix.with_name("Cafe\u0301 Agent")
        self.assertTrue(os.path.samefile(decomposed, self.prefix))
        self.assertEqual(self.invoke("status", prefix=decomposed)["status"], "installed")
        uppercase = self.prefix.with_name("CAF\u00c9 AGENT")
        if uppercase.exists():
            self.assertTrue(os.path.samefile(uppercase, self.prefix))
            self.assertEqual(self.invoke("status", prefix=uppercase)["status"], "installed")
        self.change_package()
        self.assertEqual(self.invoke("upgrade", prefix=physical)["status"], "upgraded")
        self.assertEqual(self.invoke("uninstall", prefix=decomposed)["status"], "uninstalled")
        self.assertFalse(self.prefix.exists())

    @unittest.skipUnless(sys.platform == "darwin", "Native macOS path aliases")
    def test_missing_parent_alias_is_rechecked_after_lock_acquisition(self):
        installer = self.installer_module()
        proposed = self.root / "parent" / "Cafe\u0301 Agent"
        canonical_parent = self.root / "Parent"
        original_lock = installer.mutation_lock
        # Reproduce a competing operation creating the parent after preflight
        # but before this invocation takes its installer lock.
        def changed_parent(prefix):
            canonical_parent.mkdir()
            return original_lock(prefix)
        output = io.StringIO()
        with patch.object(installer, "mutation_lock", changed_parent), redirect_stdout(output):
            code = installer.main(["install", "--package", str(self.package), "--prefix", str(proposed)])
        result = json.loads(output.getvalue())
        self.assertEqual((code, result["status"]), (0, "installed"))
        installed = canonical_parent / "Caf\u00e9 Agent"
        # A case-sensitive volume creates distinct parents; the source of this
        # historical failure only exists where the two spellings alias.
        if not installed.exists():
            installed = self.root / "parent" / "Caf\u00e9 Agent"
        self.assertEqual(Path(result["prefix"]), installer.check_chain(installed))
        self.assertEqual(self.invoke("status", prefix=proposed)["status"], "installed")
        self.assertEqual(self.invoke("uninstall", prefix=proposed)["status"], "uninstalled")
        self.assertFalse(installed.exists())

    @unittest.skipUnless(sys.platform == "darwin", "Native macOS path aliases")
    def test_firmlink_source_destination_overlap_is_refused_before_mutation(self):
        installer = self.installer_module()
        physical = installer.darwin_path(self.package, physical=True)
        before = self.tree_snapshot(self.package)
        for destination in (physical, physical / "nested"):
            result = self.invoke("install", prefix=destination, expected=2)
            self.assertIn("must not overlap", result["error"])
            self.assertEqual(self.tree_snapshot(self.package), before)
        # Metadata only: no package traversal occurs after overlap refusal.
        # The physical Data root is absent from /Users and /private spellings.
        data_root = Path("/System/Volumes/Data")
        if physical.is_relative_to(data_root):
            result = self.invoke("install", package=data_root, expected=2)
            self.assertIn("must not overlap", result["error"])
            self.assertFalse(self.prefix.exists())

    @unittest.skipUnless(sys.platform == "darwin", "Native macOS inherited ACLs")
    def test_new_installation_removes_inherited_acl_without_changing_parent(self):
        import pwd
        principal = pwd.getpwuid(os.getuid()).pw_name
        result = subprocess.run(["/bin/chmod", "+a",
            f"user:{principal} allow read,readattr,readextattr,readsecurity,file_inherit,directory_inherit",
            str(self.prefix.parent)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        inherited = self.prefix.parent / "inherited-fixture"
        inherited.write_bytes(b"temporary ACL inheritance proof")
        self.assertIsNotNone(darwin_acl(inherited))
        inherited.unlink()
        parent_acl = darwin_acl(self.prefix.parent)
        self.assertIsNotNone(parent_acl)
        self.install()
        self.assertEqual(darwin_acl(self.prefix.parent), parent_acl)
        for path in (self.prefix, *self.prefix.rglob("*")):
            self.assertIsNone(darwin_acl(path), str(path))
        self.assertEqual(self.invoke("uninstall")["status"], "uninstalled")
        self.assertEqual(darwin_acl(self.prefix.parent), parent_acl)

    @unittest.skipUnless(sys.platform == "darwin", "Native macOS quarantine preservation")
    def test_installation_preserves_package_quarantine_attributes(self):
        quarantine = b"0083;00000000;CodeStudioDeliveryFixture;00000000-0000-0000-0000-000000000000"
        subprocess.run(["/usr/bin/xattr", "-w", "com.apple.quarantine", quarantine.decode(), str(self.package)],
                       check=True, capture_output=True)
        self.install()
        # Do not execute the quarantined fixture; this tests metadata handling,
        # not a Gatekeeper bypass or an interactive trust decision.
        for name in (EXECUTABLE, "README.txt", "lib/libc++.1.dylib"):
            attribute = subprocess.run(["/usr/bin/xattr", "-p", "com.apple.quarantine", str(self.prefix / name)],
                                       check=True, capture_output=True).stdout.rstrip(b"\n")
            self.assertEqual(attribute, quarantine)
            self.assertEqual(digest(self.prefix / name), digest(self.package / name))

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
                           ("platform", "linux" if os.name == "nt" else "windows"), ("arch", "x86_64" if ARCH == "arm64" else "arm64")):
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
        self.assertIn("Stop installed Agent processes", self.invoke("upgrade", expected=2)["error"])
        self.assertIn("Stop installed Agent processes", self.invoke("uninstall", expected=2)["error"])
        self.assertEqual(self.tree_snapshot(self.prefix), before)
        self.assertIsNone(child.poll())
        child.stdin.close()
        child.stdin = None
        output, error = child.communicate(timeout=8)
        self.assertEqual(child.returncode, 0, output + error)
        self.assertEqual(self.invoke("upgrade")["status"], "upgraded")

    @unittest.skipUnless(sys.platform == "darwin", "Native mapped vnode identity")
    def test_process_mapping_identity_does_not_follow_reused_executable_path(self):
        self.install()
        child = self.hold_service()
        installer = self.installer_module()
        binary = self.prefix / EXECUTABLE
        original = binary.stat()
        moved = self.root / "renamed-original-image"
        binary.rename(moved)
        try:
            shutil.copyfile(moved, binary)
            binary.chmod(0o755)
            replacement = binary.stat()
            self.assertNotEqual(original.st_ino, replacement.st_ino)
            self.assertTrue(installer.darwin_mapped_file(child.pid, (original.st_dev, original.st_ino)))
            self.assertFalse(installer.darwin_mapped_file(child.pid, (replacement.st_dev, replacement.st_ino)))
            self.assertIsNone(child.poll())
        finally:
            binary.unlink(missing_ok=True)
            moved.rename(binary)
        self.assertTrue(installer.running_installed_executable(binary))

    @unittest.skipUnless(sys.platform == "darwin", "Native exclusive directory publication")
    def test_publication_and_rollback_preserve_an_unexpected_empty_destination(self):
        self.install()
        previous = self.tree_snapshot(self.prefix)
        self.change_package()
        installer = self.installer_module()
        publish = installer.publish_new_path
        unexpected = []

        def replace_before_publication(source, target):
            if source.name.startswith("." + self.prefix.name + ".stage-") and target == self.prefix:
                self.prefix.mkdir()
                unexpected.append(self.prefix.stat().st_ino)
            return publish(source, target)

        output = io.StringIO()
        with patch.object(installer, "publish_new_path", replace_before_publication), redirect_stdout(output):
            code = installer.main(["upgrade", "--package", str(self.package), "--prefix", str(self.prefix)])
        result = json.loads(output.getvalue())
        self.assertEqual((code, result["status"]), (2, "publication_incomplete"))
        self.assertEqual(len(unexpected), 1)
        self.assertEqual(self.prefix.stat().st_ino, unexpected[0])
        self.assertEqual(list(self.prefix.iterdir()), [])
        retained = Path(result["retained_path"])
        self.assertEqual(self.tree_snapshot(retained), previous)
        installer.verify_install(retained, self.prefix)

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
    started = datetime.now(timezone.utc).isoformat()
    class DeliveryRunner(unittest.TextTestRunner):
        resultclass = DeliveryResult
    result = unittest.main(exit=False, testRunner=DeliveryRunner).result
    succeeded = result.wasSuccessful() and result.testsRun > 0
    package_unchanged = False
    try:
        package_unchanged = PACKAGE_SNAPSHOT is not None and package_snapshot(PACKAGE) == PACKAGE_SNAPSHOT
    except (OSError, ValueError, RuntimeError):
        pass
    if not package_unchanged:
        succeeded = False
        print("Delivery evidence refused: package changed or no initial snapshot was captured.", file=sys.stderr)
    if RESULT_RECEIPT is not None:
        package_evidence = {"path": str(PACKAGE), "inspection": "unavailable"}
        try:
            records = PACKAGE_SNAPSHOT["files"]
            package_evidence = {"path": str(PACKAGE), "inspection": "recorded",
                                "manifest_sha256": records["package.json"]["sha256"],
                                "metadata_sha256": records["build-metadata.json"]["sha256"],
                                "payload_sha256": records[EXECUTABLE]["sha256"],
                                "declared_source_binary_sha256": PACKAGE_METADATA.get("source_binary_sha256", PACKAGE_METADATA["binary_sha256"]),
                                "initial_snapshot": PACKAGE_SNAPSHOT,
                                "unchanged_after_tests": package_unchanged}
        except (OSError, ValueError, KeyError, TypeError):
            succeeded = False
        receipt = {
            "format": 2, "product": "CodeStudio Agent", "scope": "native extracted-package delivery regression",
            "started_at": started, "finished_at": datetime.now(timezone.utc).isoformat(),
            "status": "succeeded" if succeeded else "failed", "standalone": arguments.standalone,
            "source_binary_matched": SOURCE_BINARY_MATCHED,
            "clean_machine_verified": False,
            "clean_machine_requirement": "An operator must independently establish this is a separate clean Mac; this script cannot prove host history.",
            "host": {"platform": sys.platform, "architecture": platform.machine(), "system": platform.platform(),
                     "macos": platform.mac_ver()[0] if sys.platform == "darwin" else None, "python": platform.python_version()},
            "package": package_evidence,
            "tests": {"total": result.testsRun, "passed": max(0, result.testsRun - len(result.failures) - len(result.errors) - len(result.skipped)),
                      "selected_test_ids": result.selected_test_ids,
                      "skipped": len(result.skipped), "failures": len(result.failures), "errors": len(result.errors),
                      "failed_ids": [test.id() for test, _ in result.failures + result.errors],
                      "skipped_cases": [{"id": test.id(), "reason": reason} for test, reason in result.skipped]},
            "limits": ["No default installation, actual login, reboot, or credential mutation is performed.",
                       "A local success is not notarization, Gatekeeper download acceptance, or a clean-machine claim."]}
        try:
            RECEIPT_DESTINATION.write(receipt)
        except (OSError, ValueError):
            succeeded = False
            print("Delivery evidence refused: external receipt destination changed or is unsafe.", file=sys.stderr)
        finally:
            RECEIPT_DESTINATION.close()
    raise SystemExit(0 if succeeded else 1)
