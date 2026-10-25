"""Packaging safety/receipt/archive tests; does not build or execute an Agent."""
import importlib.util
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / "scripts" / "package.py"
spec = importlib.util.spec_from_file_location("codestudio_package", MODULE)
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="codestudio-package-test-")
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_manifest_paths_are_portable_and_cannot_escape(self):
        for value in ("../outside", "/absolute", "C:/outside", "a\\b", "a//b", "a/./b", "a/../b",
                      "CON", "a/NUL.txt", "a.", "a ", "", "a:b", "a\x00b"):
            with self.subTest(value=value), self.assertRaises(package.PackageError):
                package.safe_name(value)
        self.assertEqual(package.safe_name("docs/delivery.md"), "docs/delivery.md")

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
        for target, extension in (("linux", ".tar.gz"), ("windows", ".zip")):
            with self.subTest(target=target):
                first, second = self.root / ("first" + extension), self.root / ("second" + extension)
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


if __name__ == "__main__":
    unittest.main()
