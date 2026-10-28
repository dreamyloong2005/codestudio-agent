"""Release state-machine tests plus a native ad-hoc signature rejection probe.

No Developer ID private key or Apple network service is used by this suite.
"""
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("macos_release", ROOT / "scripts/release-macos.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
IDENTITY = "1" * 40
TEAM = "ABC123DEF4"
SUBMISSION = "60fdf101-6b7c-4c76-8a5a-782b2167eec4"


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="codestudio-macos-release-")
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "input package"
        self.source.mkdir()
        self.output = self.root / "release output"
        self.installer, self.bundle = release.components()
        self.targets = patch.object(self.installer, "native_target", return_value=("macosx", "arm64", "codestudio-agent"))
        self.targets.start()
        self.parts = patch.object(release, "components", return_value=(self.installer, self.bundle))
        self.parts.start()
        self.graph = patch.object(self.bundle, "validate_payload")
        self.graph.start()
        self.mount = patch.object(release, "verify_image_payload")
        self.mount.start()
        self.commands = []
        self.reject_notary = False
        self.notary_timeout = False
        self.wrong_log_hash = False
        self.wrong_signer = False
        self.gatekeeper_failure = False
        self.runtime_failure = False
        self.command_patch = patch.object(release, "run", side_effect=self.external)
        self.command_patch.start()
        self.fixture()

    def tearDown(self):
        self.command_patch.stop()
        self.mount.stop()
        self.graph.stop()
        self.parts.stop()
        self.targets.stop()
        self.temporary.cleanup()

    def fixture(self):
        images = {name: ("synthetic relocated " + name).encode() for name in release.IMAGES}
        report = {"format": 2, "product": release.PRODUCT, "status": "succeeded", "mode": "release",
                  "platform": "macosx", "arch": "arm64", "build_requested": True, "omitted_suites": [],
                  "toolchain": {"libcxx_sha256": {"libc++.1.dylib": "b" * 64, "libc++abi.1.dylib": "b" * 64}},
                  "artifact": {"sha256": "a" * 64}, "steps": [{"name": "native", "status": "succeeded", "native_groups": 22}]}
        report_bytes = release.encoded(report)
        metadata = {"source_binary_sha256": "a" * 64, "binary_sha256": release.sha(images["codestudio-agent"]),
                    "verification": {"state": "supplied", "sha256": release.sha(report_bytes)},
                    "dependencies": {"libraries": ["/usr/lib/libSystem.B.dylib"],
                                     "notes": ["The development payload has ad-hoc signatures."]},
                    "macos_bundle": {"signature": "ad_hoc_not_publisher_verified",
                                     "runtime_records": [{"path": name, "source_sha256": "b" * 64,
                                                          "sha256": release.sha(images[name]), "size": len(images[name])}
                                                         for name in release.IMAGES[:-1]]}}
        files = {**images, "install.py": b"# never executed by the release tool\n",
                 "README.txt": b"Development delivery, ad-hoc signatures only.\n",
                 "runtime-dependencies.txt": b"Ad-hoc development payload.\n",
                 "verification.json": report_bytes, "build-metadata.json": release.encoded(metadata)}
        records = []
        for name, data in files.items():
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            executable = name in release.IMAGES
            path.chmod(0o755 if executable else 0o644)
            records.append({"path": name, "sha256": release.sha(data), "size": len(data), "executable": executable})
        manifest = {"format": 2, "product": release.PRODUCT, "version": "0.1.0",
                    "platform": "macosx", "arch": "arm64", "files": records}
        self.save_package(self.source, manifest)

    def save_package(self, root, manifest):
        for item in manifest["files"]:
            data = (root / item["path"]).read_bytes()
            item.update(sha256=release.sha(data), size=len(data))
        data = release.encoded(manifest)
        (root / "package.json").write_bytes(data)
        hashes = {item["path"]: item["sha256"] for item in manifest["files"]}
        hashes["package.json"] = release.sha(data)
        (root / "SHA256SUMS").write_text("".join(f"{value}  {name}\n" for name, value in sorted(hashes.items())))

    def args(self, development=False):
        return types.SimpleNamespace(package=self.source, output=self.output, ad_hoc=development,
                                     identity=None if development else IDENTITY, team_id=None if development else TEAM)

    def notarize_args(self, submission_id=None):
        return types.SimpleNamespace(release=self.output, keychain_profile="test-profile-no-credentials",
                                     timeout=30, submission_id=submission_id)

    def external(self, arguments, **options):
        argv = list(map(str, arguments))
        self.commands.append(argv)
        result = {"returncode": 0, "stdout": "", "stderr": ""}
        if argv[0] == "/usr/bin/security":
            result["stdout"] = f'1) {IDENTITY} "Developer ID Application: Test Publisher ({TEAM})"\n'
        elif argv[0] == "/usr/bin/codesign":
            target = Path(argv[-1])
            if "--sign" in argv:
                target.write_bytes(target.read_bytes() + b"-signed-test-bytes")
            elif "--display" in argv and "--entitlements" not in argv:
                suffix = str(target).split("codestudio-agent-macosx-arm64/", 1)[-1]
                identifier = release.IDENTIFIERS.get(suffix, "com.codestudio.agent.distribution")
                development = any("--sign" in command and command[command.index("--sign") + 1] == "-"
                                  for command in self.commands)
                details = "Signature=adhoc\n" if development else (
                    "Authority=Developer ID Application: Test Publisher\nTeamIdentifier=" +
                    ("ZZZ123ZZZ4" if self.wrong_signer else TEAM) + "\nTimestamp=test-secure-timestamp\n")
                result["stderr"] = "Identifier=" + identifier + "\nCodeDirectory flags=0x10000(runtime)\n" + details
        elif argv[:2] == ["/usr/bin/hdiutil", "create"]:
            Path(argv[-1]).write_bytes(b"synthetic disk image")
        elif argv[:2] == ["xcrun", "notarytool"]:
            operation = argv[2]
            if operation in ("submit", "wait"):
                before = json.loads((self.output / release.RECEIPT).read_text())
                self.assertEqual(before["status"], "submission_started")
                if self.notary_timeout:
                    raise release.ReleaseError("External operation timed out; its effect may be incomplete")
                result["stdout"] = json.dumps({"id": SUBMISSION, "status": "Invalid" if self.reject_notary else "Accepted"})
            elif operation == "log":
                digest = "f" * 64 if self.wrong_log_hash else release.sha((self.output / release.IMAGE).read_bytes())
                result["stdout"] = json.dumps({"jobId": SUBMISSION, "status": "Accepted", "sha256": digest})
        elif argv[:3] == ["xcrun", "stapler", "staple"]:
            image = Path(argv[-1])
            image.write_bytes(image.read_bytes() + b"-stapled-ticket")
        elif argv[0] == "/usr/sbin/spctl" and self.gatekeeper_failure:
            raise release.ReleaseError("Gatekeeper test rejection")
        elif Path(argv[0]).name == "codestudio-agent":
            self.assertFalse(any(key.startswith(("DYLD_", "CODESTUDIO_")) for key in options["env"]))
            if self.runtime_failure:
                raise release.ReleaseError("Injected runtime dependency load failure")
            result["stdout"] = ("CodeStudio-Agent 0.1.0\n" if argv[1] == "version" else
                                json.dumps({"product": "CodeStudio-Agent", "version": "0.1.0", "cplusplus": 202400}))
        return result

    def test_prepare_signs_copy_refreshes_final_hashes_and_preserves_original(self):
        before = {str(p.relative_to(self.source)): p.read_bytes() for p in self.source.rglob("*") if p.is_file()}
        receipt = release.prepare(self.args())
        self.assertEqual(receipt["status"], "prepared_not_notarized")
        self.assertFalse(receipt["stage4_accepted"])
        self.assertEqual(before, {str(p.relative_to(self.source)): p.read_bytes() for p in self.source.rglob("*") if p.is_file()})
        package = self.installer.verify_package(self.output / release.PACKAGE)
        self.assertEqual(package["package_sha256"], receipt["package_sha256"])
        metadata = json.loads((self.output / release.PACKAGE / "build-metadata.json").read_text())
        self.assertEqual(metadata["source_binary_sha256"], "a" * 64)
        self.assertNotEqual(metadata["binary_sha256"], json.loads(before["build-metadata.json"])["binary_sha256"])
        self.assertTrue(all(item["source_sha256"] == "b" * 64 for item in metadata["macos_bundle"]["runtime_records"]))
        self.assertIn("Developer ID signatures", (self.output / release.PACKAGE / "README.txt").read_text())
        self.assertNotIn("ad-hoc", (self.output / release.PACKAGE / "runtime-dependencies.txt").read_text())
        self.assertFalse(any(command[:2] == ["xcrun", "notarytool"] for command in self.commands))
        signatures = [command for command in self.commands if command[0] == "/usr/bin/codesign" and
                      "--sign" in command and not command[-1].endswith(".dmg")]
        self.assertEqual(len(signatures), 3)
        self.assertTrue(all("--options" in command and "runtime" in command for command in signatures))

    def test_re_signed_runtime_failure_prevents_image_publication(self):
        self.runtime_failure = True
        with self.assertRaises(release.ReleaseError):
            release.prepare(self.args())
        receipt = json.loads((self.output / release.RECEIPT).read_text())
        self.assertEqual(receipt["status"], "prepare_failed")
        self.assertFalse((self.output / release.IMAGE).exists())

    def test_existing_output_and_aliased_source_output_are_preserved(self):
        self.output.mkdir()
        (self.output / "important").write_bytes(b"preserve")
        with self.assertRaises(release.ReleaseError):
            release.prepare(self.args())
        self.assertEqual((self.output / "important").read_bytes(), b"preserve")
        args = self.args()
        args.output = self.source / "nested-release"
        with self.assertRaises(release.ReleaseError):
            release.prepare(args)
        self.assertFalse(args.output.exists())

    def test_wrong_signer_retains_failed_preparation_and_does_not_upload(self):
        self.wrong_signer = True
        with self.assertRaises(release.ReleaseError):
            release.prepare(self.args())
        self.assertEqual(json.loads((self.output / release.RECEIPT).read_text())["status"], "prepare_failed")
        self.assertFalse((self.output / release.IMAGE).exists())
        self.assertFalse(any(command[:2] == ["xcrun", "notarytool"] for command in self.commands))

    def test_ad_hoc_delivery_cannot_be_notarized(self):
        receipt = release.prepare(self.args(development=True))
        self.assertEqual(receipt["status"], "development_only")
        metadata = json.loads((self.output / release.PACKAGE / "build-metadata.json").read_text())
        self.assertFalse(metadata["macos_bundle"]["signing"]["hardened_runtime"])
        self.assertFalse(any("--options" in command for command in self.commands))
        with self.assertRaises(release.ReleaseError):
            release.notarize(self.notarize_args())
        self.assertFalse(any(command[:2] == ["xcrun", "notarytool"] for command in self.commands))

    def test_changed_image_is_refused_before_network_request(self):
        release.prepare(self.args())
        (self.output / release.IMAGE).write_bytes(b"replacement")
        with self.assertRaises(release.ReleaseError):
            release.notarize(self.notarize_args())
        self.assertFalse(any(command[:2] == ["xcrun", "notarytool"] for command in self.commands))

    def test_modified_package_is_refused_before_network_request(self):
        release.prepare(self.args())
        (self.output / release.PACKAGE / "install.py").write_bytes(b"replacement")
        with self.assertRaises(Exception):
            release.notarize(self.notarize_args())
        self.assertFalse(any(command[:2] == ["xcrun", "notarytool"] for command in self.commands))

    def test_apple_rejection_does_not_staple_or_become_success(self):
        release.prepare(self.args())
        self.reject_notary = True
        with self.assertRaises(release.ReleaseError):
            release.notarize(self.notarize_args())
        self.assertEqual(json.loads((self.output / release.RECEIPT).read_text())["status"], "notary_rejected")
        with self.assertRaises(release.ReleaseError):
            release.notarize(self.notarize_args())
        self.assertEqual(sum(command[:3] == ["xcrun", "notarytool", "submit"] for command in self.commands), 1)
        self.assertFalse(any(command[:3] == ["xcrun", "stapler", "staple"] for command in self.commands))

    def test_unknown_submission_effect_is_not_replayed(self):
        release.prepare(self.args())
        self.notary_timeout = True
        with self.assertRaises(release.ReleaseError):
            release.notarize(self.notarize_args())
        self.assertEqual(json.loads((self.output / release.RECEIPT).read_text())["status"], "submission_started")
        self.notary_timeout = False
        with self.assertRaises(release.ReleaseError):
            release.notarize(self.notarize_args())
        self.assertEqual(sum(command[:3] == ["xcrun", "notarytool", "submit"] for command in self.commands), 1)
        receipt = release.notarize(self.notarize_args(SUBMISSION))
        self.assertEqual(receipt["status"], "notarized")
        self.assertTrue(any(command[:3] == ["xcrun", "notarytool", "wait"] for command in self.commands))

    def test_notary_acceptance_must_bind_exact_submitted_digest(self):
        release.prepare(self.args())
        self.wrong_log_hash = True
        with self.assertRaises(release.ReleaseError):
            release.notarize(self.notarize_args())
        self.assertNotEqual(json.loads((self.output / release.RECEIPT).read_text())["status"], "notarized")
        self.assertFalse(any(command[:3] == ["xcrun", "stapler", "staple"] for command in self.commands))

    def test_success_records_final_stapled_bytes_but_not_clean_machine_acceptance(self):
        before = release.prepare(self.args())["artifact"]
        receipt = release.notarize(self.notarize_args())
        self.assertEqual(receipt["status"], "notarized")
        self.assertEqual(receipt["submitted_artifact"], before)
        self.assertNotEqual(receipt["artifact"]["sha256"], before["sha256"])
        self.assertEqual(receipt["artifact"]["sha256"], release.sha((self.output / release.FINAL_IMAGE).read_bytes()))
        self.assertEqual(before["sha256"], release.sha((self.output / release.IMAGE).read_bytes()))
        self.assertFalse(receipt["stage4_accepted"])
        self.assertEqual(receipt["clean_machine_acceptance"], "not_claimed")

    def test_post_staple_gatekeeper_failure_retries_from_immutable_image_without_upload(self):
        before = release.prepare(self.args())["artifact"]
        self.gatekeeper_failure = True
        with self.assertRaises(release.ReleaseError):
            release.notarize(self.notarize_args())
        retained = json.loads((self.output / release.RECEIPT).read_text())
        self.assertEqual(retained["status"], "validating")
        first = self.output / retained["active_candidate"]
        self.assertTrue(first.is_file())
        self.assertNotEqual(release.sha(first.read_bytes()), before["sha256"])
        self.assertEqual(release.sha((self.output / release.IMAGE).read_bytes()), before["sha256"])
        self.gatekeeper_failure = False
        receipt = release.notarize(self.notarize_args())
        self.assertEqual(receipt["status"], "notarized")
        self.assertTrue(first.is_file())
        self.assertEqual(sum(command[:3] == ["xcrun", "notarytool", "submit"] for command in self.commands), 1)
        self.assertFalse(any(command[:3] == ["xcrun", "notarytool", "wait"] for command in self.commands))

    def test_publication_receipt_failure_recovers_exact_final_image_without_upload(self):
        release.prepare(self.args())
        original = release.write_receipt
        def crash_after_publication(root, receipt):
            if receipt["status"] == "notarized":
                raise OSError("injected receipt persistence failure")
            return original(root, receipt)
        with patch.object(release, "write_receipt", side_effect=crash_after_publication), self.assertRaises(OSError):
            release.notarize(self.notarize_args())
        pending = json.loads((self.output / release.RECEIPT).read_text())
        self.assertEqual(pending["status"], "publishing")
        self.assertTrue((self.output / release.FINAL_IMAGE).is_file())
        expected = release.sha((self.output / release.FINAL_IMAGE).read_bytes())
        receipt = release.notarize(self.notarize_args())
        self.assertEqual(receipt["status"], "notarized")
        self.assertEqual(receipt["artifact"]["sha256"], expected)
        self.assertEqual(sum(command[:3] == ["xcrun", "notarytool", "submit"] for command in self.commands), 1)

    def test_replaced_final_image_is_not_adopted_after_publication_failure(self):
        release.prepare(self.args())
        original = release.write_receipt
        def crash_after_publication(root, receipt):
            if receipt["status"] == "notarized":
                raise OSError("injected receipt persistence failure")
            return original(root, receipt)
        with patch.object(release, "write_receipt", side_effect=crash_after_publication), self.assertRaises(OSError):
            release.notarize(self.notarize_args())
        (self.output / release.FINAL_IMAGE).write_bytes(b"other signed image")
        with self.assertRaises(release.ReleaseError):
            release.notarize(self.notarize_args())
        self.assertEqual((self.output / release.FINAL_IMAGE).read_bytes(), b"other signed image")
        self.assertEqual(sum(command[:3] == ["xcrun", "notarytool", "submit"] for command in self.commands), 1)

    def test_native_report_may_defer_only_post_package_delivery_suite(self):
        manifest = json.loads((self.source / "package.json").read_text())
        report = json.loads((self.source / "verification.json").read_text())
        report["omitted_suites"] = ["delivery"]
        report_bytes = release.encoded(report)
        (self.source / "verification.json").write_bytes(report_bytes)
        metadata = json.loads((self.source / "build-metadata.json").read_text())
        metadata["verification"]["sha256"] = release.sha(report_bytes)
        (self.source / "build-metadata.json").write_bytes(release.encoded(metadata))
        self.save_package(self.source, manifest)
        self.assertEqual(release.prepare(self.args())["status"], "prepared_not_notarized")

    def test_partial_native_receipt_cannot_be_signed(self):
        manifest = json.loads((self.source / "package.json").read_text())
        report = json.loads((self.source / "verification.json").read_text())
        report["omitted_suites"] = ["native"]
        report_bytes = release.encoded(report)
        (self.source / "verification.json").write_bytes(report_bytes)
        metadata = json.loads((self.source / "build-metadata.json").read_text())
        metadata["verification"]["sha256"] = release.sha(report_bytes)
        (self.source / "build-metadata.json").write_bytes(release.encoded(metadata))
        self.save_package(self.source, manifest)
        with self.assertRaises(release.ReleaseError):
            release.prepare(self.args())
        self.assertFalse(self.output.exists())

    def test_extra_entitlements_are_rejected(self):
        original = self.external
        def with_entitlements(argv, **kwargs):
            result = original(argv, **kwargs)
            if "--entitlements" in argv:
                import plistlib
                result["stdout"] = plistlib.dumps({"com.apple.security.cs.disable-library-validation": True}).decode()
            return result
        # Patch the active mock rather than changing the test fixture's signing
        # code: an unexpected code entitlement is a boundary violation.
        release.run.side_effect = with_entitlements
        with self.assertRaises(release.ReleaseError):
            release.prepare(self.args())

    def test_duplicate_json_and_invalid_submission_identity_are_rejected(self):
        with self.assertRaises(release.ReleaseError):
            release.read_json('{"status":"Accepted","status":"Invalid"}')
        with self.assertRaises(release.ReleaseError):
            release.notary_response({"stdout": '{"id":"not-a-uuid","status":"Accepted"}'})


@unittest.skipUnless(sys.platform == "darwin" and platform.machine().lower() == "arm64", "Native macOS arm64 only")
class NativeSignatureTests(unittest.TestCase):
    def test_real_ad_hoc_hardened_signature_and_tamper_refusal(self):
        if not shutil.which("xcrun"):
            self.skipTest("Xcode command-line tools are unavailable")
        with tempfile.TemporaryDirectory(prefix="codestudio-signature-native-") as directory:
            root = Path(directory).resolve()
            source, binary = root / "fixture.c", root / "fixture"
            source.write_text("int main(void) { return 0; }\n")
            subprocess.run(["xcrun", "clang", "-arch", "arm64", "-mmacosx-version-min=15.0",
                            str(source), "-o", str(binary)], check=True, capture_output=True, timeout=30)
            release.run(["/usr/bin/codesign", "--force", "--sign", "-", "--identifier", "com.codestudio.agent.test",
                         "--options", "runtime", "--timestamp=none", binary])
            result = release.signature(binary, development=True, executable=True, identifier="com.codestudio.agent.test")
            self.assertEqual(result["kind"], "ad_hoc_not_publisher_verified")
            self.assertEqual(subprocess.run([str(binary)], timeout=5).returncode, 0)
            data = bytearray(binary.read_bytes())
            data[0] ^= 1
            binary.write_bytes(data)
            with self.assertRaises(release.ReleaseError):
                release.signature(binary, development=True, executable=True)

    def test_real_ad_hoc_disk_image_contains_exact_verified_package(self):
        installer, _ = release.components()
        with tempfile.TemporaryDirectory(prefix="codestudio-dmg-native-") as directory:
            root = Path(directory).resolve()
            payload = root / "payload"
            package = payload / Path(release.PACKAGE).name
            package.mkdir(parents=True)
            records = []
            for name, data, executable in (("codestudio-agent", b"not executed by this image test", True),
                                           ("install.py", b"# never executed\n", False)):
                path = package / name
                path.write_bytes(data)
                path.chmod(0o755 if executable else 0o644)
                records.append({"path": name, "sha256": release.sha(data), "size": len(data), "executable": executable})
            manifest = {"format": 2, "product": release.PRODUCT, "version": "0.1.0",
                        "platform": "macosx", "arch": "arm64", "files": records}
            data = release.encoded(manifest)
            (package / "package.json").write_bytes(data)
            hashes = {item["path"]: item["sha256"] for item in records}
            hashes["package.json"] = release.sha(data)
            (package / "SHA256SUMS").write_text("".join(f"{value}  {name}\n" for name, value in sorted(hashes.items())))
            package_hash = installer.verify_package(package)["package_sha256"]
            image = root / release.IMAGE
            release.run(["/usr/bin/hdiutil", "create", "-fs", "HFS+", "-format", "UDZO", "-volname",
                         "CodeStudio Test", "-srcfolder", payload, image])
            release.run(["/usr/bin/codesign", "--sign", "-", "--identifier", "com.codestudio.agent.test.image",
                         "--timestamp=none", image])
            release.signature(image, development=True)
            release.run(["/usr/bin/hdiutil", "verify", image])
            release.verify_image_payload(image, package_hash, installer, root)
            self.assertEqual(list(root.glob(".codestudio-dmg-check-*")), [])


if __name__ == "__main__":
    unittest.main()
