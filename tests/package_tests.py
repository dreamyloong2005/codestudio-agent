"""Packaging safety/receipt/archive tests; does not build or execute an Agent."""
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import tempfile
import types
import unittest
from unittest.mock import patch

MODULE = Path(__file__).resolve().parents[1] / "scripts" / "package.py"
spec = importlib.util.spec_from_file_location("codestudio_package", MODULE)
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="codestudio-package-test-")
        self.root = Path(self.temporary.name).resolve()

    def tearDown(self):
        self.temporary.cleanup()

    def test_manifest_paths_are_portable_and_cannot_escape(self):
        for value in ("../outside", "/absolute", "C:/outside", "a\\b", "a//b", "a/./b", "a/../b",
                      "CON", "a/NUL.txt", "a.", "a ", "", "a:b", "a\x00b"):
            with self.subTest(value=value), self.assertRaises(package.PackageError):
                package.safe_name(value)
        self.assertEqual(package.safe_name("docs/delivery.md"), "docs/delivery.md")
        self.assertEqual(package.safe_name("lib/libc++.1.dylib"), "lib/libc++.1.dylib")

    def test_casefold_collision_and_oversized_file_rejected(self):
        with self.assertRaises(package.PackageError):
            package.write_payload(self.root, {"A.txt": (b"a", False), "a.txt": (b"b", False)})

    def test_hardlinked_sources_are_rejected(self):
        source = self.root / "source"
        source.write_bytes(b"original")
        os.link(source, self.root / "linked")
        with self.assertRaises(package.PackageError):
            package.read_file(source)

    def test_symlink_sources_are_rejected(self):
        source = self.root / "source"
        source.write_bytes(b"original")
        link = self.root / "linked"
        try:
            link.symlink_to(source)
        except OSError:
            self.skipTest("Creating symlinks requires unavailable host privileges")
        with self.assertRaises(package.PackageError):
            package.read_file(link)

    def test_archives_are_reproducible_and_match_payload(self):
        contents = {"codestudio-agent": (b"synthetic-binary", True), "docs/guide.md": (b"guide", False)}
        for target, extension in (("linux", ".tar.gz"), ("windows", ".zip"), ("macosx", ".tar.gz")):
            with self.subTest(target=target):
                first, second = self.root / (target + "-first" + extension), self.root / (target + "-second" + extension)
                package.make_archive(first, contents, target)
                package.make_archive(second, dict(reversed(list(contents.items()))), target)
                self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_output_requires_ownership_and_preserves_unrelated_data(self):
        (self.root / "important.txt").write_bytes(b"keep")
        with self.assertRaises(package.PackageError):
            package.validate_output(self.root, "codestudio-agent-linux-x86_64", "codestudio-agent-linux-x86_64.tar.gz")
        self.assertEqual((self.root / "important.txt").read_bytes(), b"keep")

    def owned_output(self):
        name, archive = "codestudio-agent-linux-x86_64", "codestudio-agent-linux-x86_64.tar.gz"
        contents = {"codestudio-agent": (b"binary", True)}
        tree = self.root / name
        tree.mkdir()
        package.write_payload(tree, contents)
        package.make_archive(self.root / archive, contents, "linux")
        marker = {"format": 2, "product": "CodeStudio-Agent", "package": name, "archive": archive,
                  "files": package.output_records(package.tree_files(self.root))}
        (self.root / package.OUTPUT_MARKER).write_bytes(package.encoded(marker))
        return name, archive

    def test_modified_output_cannot_be_replaced(self):
        name, archive = self.owned_output()
        package.validate_output(self.root, name, archive)
        (self.root / name / "codestudio-agent").write_bytes(b"modified")
        with self.assertRaises(package.PackageError):
            package.validate_output(self.root, name, archive)

    def test_untracked_empty_directory_is_not_discarded(self):
        name, archive = self.owned_output()
        (self.root / "notes").mkdir()
        with self.assertRaises(package.PackageError):
            package.validate_output(self.root, name, archive)

    def test_verification_requires_same_release_binary_and_success(self):
        binary = b"built-agent"
        report = {"format": 2, "product": "CodeStudio-Agent", "platform": "linux", "arch": "x86_64",
                  "mode": "release", "status": "succeeded", "selected_suites": ["native"], "omitted_suites": ["credentials"],
                  "artifact": {"sha256": package.digest(binary), "size": len(binary)}}
        result = package.validate_verification(package.encoded(report), "linux", "x86_64", binary)
        self.assertEqual(result["omitted_suites"], ["credentials"])
        for key, value in (("status", "failed"), ("mode", "debug"), ("platform", "windows"),
                           ("format", 1), ("selected_suites", [])):
            with self.subTest(key=key), self.assertRaises(package.PackageError):
                package.validate_verification(package.encoded(dict(report, **{key: value})), "linux", "x86_64", binary)
        with self.assertRaises(package.PackageError):
            package.validate_verification(package.encoded(report), "linux", "x86_64", b"different-agent")

    def test_bundle_document_links_do_not_point_at_missing_evidence(self):
        source = b"[included](credentials.md) [evidence](../research/run.json) [web](https://example.com)"
        adapted = package.adapted_document(source, "docs/delivery.md", {"docs/credentials.md"})
        self.assertIn(b"[included](credentials.md)", adapted)
        self.assertIn(b"evidence (source repository: `../research/run.json`)", adapted)
        self.assertIn(b"[web](https://example.com)", adapted)

    def test_output_lock_refuses_concurrent_publisher_and_cleans_own_token(self):
        output = self.root / "published"
        lock = self.root / ".published.codestudio-package.lock"
        with package.output_lock(output):
            original = lock.read_bytes()
            with self.assertRaises(package.PackageError):
                with package.output_lock(output):
                    self.fail("A second packaging invocation acquired the same output")
            self.assertEqual(lock.read_bytes(), original)
            self.assertFalse(output.exists())
        self.assertFalse(lock.exists())


class MacOSBundleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bundle = package.macos_backend()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="codestudio-macos-package-test-")
        self.root = Path(self.temporary.name).resolve()

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def image(dependencies, *, kind=2, rpaths=(), minimum=15 << 16, install_id=None, cpu=0x0100000C):
        commands = [struct.pack("<6I", 0x32, 24, 1, minimum, 26 << 16, 0)]
        for command, text in [(0xC, name) for name in dependencies] + [(0x8000001C, name) for name in rpaths] + (
                [(0xD, install_id)] if install_id else []):
            offset = 12 if command == 0x8000001C else 24
            raw = text.encode() + b"\0"
            length = (offset + len(raw) + 7) // 8 * 8
            commands.append(struct.pack("<3I", command, length, offset) + bytes(offset - 12) + raw + bytes(length - offset - len(raw)))
        return struct.pack("<8I", 0xFEEDFACF, cpu, 0, kind, len(commands), sum(map(len, commands)), 0, 0) + b"".join(commands)

    def payload(self):
        return {"codestudio-agent": self.image(["@loader_path/lib/libc++.1.dylib", "@loader_path/lib/libc++abi.1.dylib", "/usr/lib/libSystem.B.dylib"]),
                "lib/libc++.1.dylib": self.image(["@loader_path/libc++abi.1.dylib", "/usr/lib/libSystem.B.dylib"], kind=6, install_id="@rpath/libc++.1.dylib"),
                "lib/libc++abi.1.dylib": self.image(["/usr/lib/libSystem.B.dylib"], kind=6, install_id="@rpath/libc++abi.1.dylib")}

    def test_native_target_refuses_rosetta_or_other_architecture(self):
        with patch.object(package.sys, "platform", "darwin"), patch.object(package.platform, "machine", return_value="arm64"):
            self.assertEqual(package.native_target(), ("macosx", "arm64", ""))
        with patch.object(package.sys, "platform", "darwin"), patch.object(package.platform, "machine", return_value="x86_64"):
            with self.assertRaises(package.PackageError):
                package.native_target()

    def test_runtime_closure_rejects_external_paths_and_missing_library(self):
        original = self.payload()
        self.bundle.validate_payload(original)
        for dependency in ("/opt/homebrew/lib/libextra.dylib", "@rpath/libc++.1.dylib", "@loader_path/../outside.dylib",
                           "/usr/lib/../local/unsafe.dylib", "/usr/lib//libSystem.B.dylib"):
            invalid = dict(original, **{"codestudio-agent": self.image([dependency])})
            with self.subTest(dependency=dependency), self.assertRaises(self.bundle.BundleError):
                self.bundle.validate_payload(invalid)
        del original["lib/libc++abi.1.dylib"]
        with self.assertRaises(self.bundle.BundleError):
            self.bundle.validate_payload(original)

    def test_universal_foreign_deployment_and_truncated_headers_are_rejected(self):
        valid = self.payload()["codestudio-agent"]
        cases = [valid[:31], valid[:-1], b"\xca\xfe\xba\xbe" + valid[4:],
                 self.image([], cpu=0x01000007), self.image([], minimum=16 << 16),
                 self.image([], minimum=14 << 16), self.image([], rpaths=["/build/private/lib"])]
        for invalid in cases:
            with self.subTest(data=invalid[:32]), self.assertRaises(self.bundle.BundleError):
                self.bundle.validate_payload(dict(self.payload(), **{"codestudio-agent": invalid}))

    def test_malformed_load_command_does_not_hide_trailing_dependency(self):
        valid = bytearray(self.payload()["codestudio-agent"])
        struct.pack_into("<I", valid, 16, 1)  # A forged count cannot hide the remaining command bytes.
        with self.assertRaises(self.bundle.BundleError):
            self.bundle.inspect_macho(valid)
        valid = bytearray(self.payload()["codestudio-agent"])
        struct.pack_into("<I", valid, 36, 0)
        with self.assertRaises(self.bundle.BundleError):
            self.bundle.inspect_macho(valid)

    def test_runtime_version_symlink_must_stay_inside_runtime_directory(self):
        library = self.root / "lib"
        library.mkdir()
        real = library / "libc++.1.0.dylib"
        real.write_bytes(b"owned-runtime")
        alias = library / "libc++.1.dylib"
        try:
            alias.symlink_to(real.name)
        except OSError:
            self.skipTest("Creating symlinks requires unavailable host privileges")
        self.assertEqual(self.bundle.runtime_file(self.root, alias.name, package.read_file), b"owned-runtime")
        alias.unlink()
        alias.symlink_to("../outside")
        with self.assertRaises(self.bundle.BundleError):
            self.bundle.runtime_file(self.root, alias.name, package.read_file)

    def test_verified_runtime_tampering_is_refused_before_any_tool_runs(self):
        library = self.root / "lib"
        library.mkdir()
        runtimes = {"libc++.1.dylib": self.image(["@rpath/libc++abi.1.dylib", "/usr/lib/libSystem.B.dylib"], kind=6, install_id="@rpath/libc++.1.dylib"),
                    "libc++abi.1.dylib": self.image(["/usr/lib/libSystem.B.dylib"], kind=6, install_id="@rpath/libc++abi.1.dylib")}
        for name, data in runtimes.items():
            (library / name).write_bytes(data)
        binary = self.image(["@rpath/libc++.1.dylib", "@rpath/libc++abi.1.dylib"], rpaths=[str(library)])
        report = {"format": 2, "product": "CodeStudio-Agent", "platform": "macosx", "arch": "arm64",
                  "mode": "release", "status": "succeeded", "selected_suites": ["native"], "omitted_suites": [],
                  "artifact": {"sha256": package.digest(binary), "size": len(binary)},
                  "toolchain": {"deployment_target": "15.0", "libcxx_root": str(self.root),
                                "libcxx_sha256": {name: "0" * 64 for name in runtimes}}}
        built = self.root / "build/macosx/arm64/release/codestudio-agent"
        built.parent.mkdir(parents=True)
        built.write_bytes(binary)
        (self.root / "xmake.lua").write_text('set_version("0.1.0")\n')
        receipt = self.root / "verification.json"
        receipt.write_bytes(package.encoded(report))
        def forbidden(argv):
            self.fail("Runtime binding failure executed a tool: " + repr(argv))
        arguments = types.SimpleNamespace(output=self.root / "output", verification=receipt, macos_libcxx=self.root)
        with patch.object(package, "ROOT", self.root), patch.object(package, "macos_backend", return_value=self.bundle), \
                patch.object(package, "native_target", return_value=("macosx", "arm64", "")), \
                patch.object(package, "command", side_effect=forbidden), \
                self.assertRaisesRegex(package.PackageError, "verified runtime"):
            package.package_locked(arguments)


if __name__ == "__main__":
    unittest.main()
