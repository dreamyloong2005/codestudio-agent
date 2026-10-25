"""Verify receipt integrity and deadlines without building or contacting services."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("verification", ROOT / "scripts/verify.py")
verification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verification)


class VerificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="codestudio-verification-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.args = type("Args", (), dict(package=None, suite=["schema"], report_dir=self.root,
                                        skip_build=True, preflight_only=False, timeout=5, jobs=1))()

    def step(self, source, timeout=5, name="schema"):
        directory = self.root / "release-test"
        directory.mkdir(exist_ok=True)
        return verification.step(name, [sys.executable, "-c", source], directory, dict(os.environ), timeout)

    def test_zero_exit_without_test_evidence_is_not_success(self):
        result = self.step("print('just a message')")
        self.assertEqual(result["returncode"], 0)
        self.assertEqual(result["status"], "failed")

    def test_zero_cases_or_nonterminal_summary_is_not_success(self):
        for source in ("print('Ran 0 tests in 0.1s\\n\\nOK')",
                       "print('Ran 1 test in 0.1s\\n\\nOK\\nnot a complete test run')"):
            self.assertEqual(self.step(source)["status"], "failed")

    def test_concurrent_verification_refuses_before_mutating_lock(self):
        with patch.object(verification, "ROOT", self.root):
            with verification.verification_lock():
                lock = next((self.root / "build").glob("*.lock"))
                content = lock.read_bytes()
                with self.assertRaises(RuntimeError):
                    with verification.verification_lock():
                        self.fail("Concurrent verification acquired lock")
                self.assertEqual(lock.read_bytes(), content)
            self.assertFalse(lock.exists())

    def test_unittest_counts_keep_skipped_cases_visible(self):
        source = "import unittest; T=type('T',(unittest.TestCase,),{'test_a':lambda s:None,'test_b':unittest.skip('fixture')(lambda s:None)}); unittest.main()"
        result = self.step(source)
        self.assertEqual((result["status"], result["cases"], result["passed"], result["skipped"]),
                         ("succeeded", 2, 1, 1))

    def test_failure_with_printed_pass_summary_is_still_failure(self):
        result = self.step("import sys; print('Ran 2 tests in 0.1s\\n\\nOK'); sys.exit(1)")
        self.assertEqual(result["status"], "failed")

    def test_windows_build_counts_completed_variants_not_summary(self):
        records = [f"PASS real isolated XMake / {variant} / C++26 pack indexing / compile / link / execute / atomic export"
                   for variant in ("explicit-v2", "custom-cache-v2")]
        summary = "PASS 2 real isolated XMake builds: explicit-v2 and custom-cache-v2"
        valid = "\n  steps=3, bytes=1024, sha256=fixture\n".join(records) + "\n" + summary + "\n"
        cases = [(valid, 0, "succeeded", 2), (summary, 0, "failed", 0),
                 (valid.replace(records[1], records[0]), 0, "failed", 2),
                 (valid.replace(records[1], ""), 0, "failed", 1),
                 (valid.replace(summary, ""), 0, "failed", 2),
                 (valid + "unfinished\n", 0, "failed", 2),
                 ("FAIL earlier operation\n" + valid, 0, "failed", 2),
                 (valid, 1, "failed", 2)]
        for content, code, status, groups in cases:
            with self.subTest(content=content, code=code):
                def launch(*args, stdout, **kwargs):
                    stdout.write(content.encode())
                    stdout.flush()
                    return SimpleNamespace(wait=lambda timeout: code)
                with patch.object(verification, "PLATFORM", "windows"), \
                     patch.object(verification.subprocess, "CREATE_NO_WINDOW", 0, create=True), \
                     patch.object(verification.subprocess, "Popen", side_effect=launch):
                    result = self.step("captured output", name="real_build")
                self.assertEqual((result["status"], result["native_groups"]), (status, groups))

    def test_timeout_terminates_running_step_and_records_interruption(self):
        result = self.step("import time; time.sleep(30)", timeout=1)
        self.assertTrue(result["interrupted"])
        self.assertEqual(result["status"], "failed")
        self.assertLess(result["seconds"], 10)

    def test_preflight_failure_is_written_without_running_commands(self):
        with patch.object(verification, "source_fingerprint", return_value={"sha256": "a", "files": 1}), \
             patch.object(verification, "preflight", side_effect=RuntimeError("missing tool")), \
             patch.object(verification, "step") as step:
            self.assertEqual(verification.verify(self.args, "release"), 2)
            step.assert_not_called()
        report = json.loads(next(self.root.glob("*/verification.json")).read_text())
        self.assertEqual((report["status"], report["error"], report["steps"]), ("failed", "missing tool", []))

    def test_preflight_only_never_claims_tests_ran(self):
        self.args.preflight_only = True
        with patch.object(verification, "source_fingerprint", return_value={"sha256": "a", "files": 1}), \
             patch.object(verification, "preflight", return_value={}), patch.object(verification, "step") as step:
            self.assertEqual(verification.verify(self.args, "debug"), 0)
            step.assert_not_called()
        report = json.loads(next(self.root.glob("*/verification.json")).read_text())
        self.assertEqual((report["status"], report["steps"]), ("preflight", []))
        self.assertNotIn("artifact", report)

    def test_incomplete_step_receipt_is_failed_and_stops_following_suites(self):
        self.args.suite = ["schema", "provider"]
        with patch.object(verification, "source_fingerprint", return_value={"sha256": "a", "files": 1}), \
             patch.object(verification, "preflight", return_value={}), \
             patch.object(verification, "artifact_fingerprint", return_value={"codestudio-agent": "bytes"}), \
             patch.object(verification, "step", return_value={"name": "schema", "status": "failed", "returncode": 1}) as step:
            self.assertEqual(verification.verify(self.args, "release"), 1)
            self.assertEqual(step.call_count, 1)
        report = json.loads(next(self.root.glob("*/verification.json")).read_text())
        self.assertEqual(report["status"], "failed")
        self.assertEqual(len(report["steps"]), 1)
        self.assertNotIn("artifact", report)

    def test_native_suite_uses_one_python_interpreter(self):
        commands = verification.commands("release", Path("package"))
        for name, command in commands.items():
            if name != "native":
                self.assertEqual(command[0], sys.executable)
        self.assertNotEqual(commands["real_build"][1], "tests/local_models_integration.py")

    def test_native_wrapper_forwards_arguments_to_selected_python(self):
        wrapper = (["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ROOT / "scripts/verify.ps1")]
                   if os.name == "nt" else ["bash", str(ROOT / "scripts/verify-linux.sh")])
        env = dict(os.environ, CODESTUDIO_CONTRACT_PYTHON=sys.executable)
        process = subprocess.run([*wrapper, "release", "--list"], cwd=self.root, env=env,
                                 capture_output=True, text=True, timeout=15)
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        inventory = json.loads(process.stdout)
        self.assertEqual(inventory["platform"], verification.PLATFORM)
        self.assertEqual(inventory["suites"], list(verification.commands("release", None)))

    def test_source_hash_uses_platform_independent_relative_path_order(self):
        files = {"README.md": b"readme", "docs/a.md": b"document",
                 "xmake.lua": b"build", "xmake-requires.lock": b"lock"}
        for name, content in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        expected = hashlib.sha256()
        for name in sorted(files):
            expected.update(name.encode() + b"\0" + hashlib.sha256(files[name]).hexdigest().encode() + b"\n")
        with patch.object(verification, "ROOT", self.root):
            self.assertEqual(verification.source_fingerprint(), {"sha256": expected.hexdigest(), "files": 4})

    def test_changed_source_or_tested_binary_never_yields_successful_receipt(self):
        binary = self.root / "build" / verification.PLATFORM / verification.ARCH / "release" / ("codestudio-agent" + verification.SUFFIX)
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"fixture-not-executed")
        for changed in ("source", "binary"):
            with self.subTest(changed=changed):
                self.args.report_dir = self.root / changed
                source = [{"sha256": "a", "files": 1}, {"sha256": "b" if changed == "source" else "a", "files": 1}]
                artifacts = [{"agent": "a"}, {"agent": "b" if changed == "binary" else "a"}]
                with patch.object(verification, "ROOT", self.root), \
                     patch.object(verification, "source_fingerprint", side_effect=source), \
                     patch.object(verification, "artifact_fingerprint", side_effect=artifacts), \
                     patch.object(verification, "preflight", return_value={}), \
                     patch.object(verification, "step", return_value={"name": "schema", "status": "succeeded", "returncode": 0}):
                    self.assertEqual(verification.verify(self.args, "release"), 2)
                report = json.loads(next(self.args.report_dir.glob("*/verification.json")).read_text())
                self.assertEqual(report["status"], "failed")
                self.assertIn("changed", report["error"])


if __name__ == "__main__":
    unittest.main()
