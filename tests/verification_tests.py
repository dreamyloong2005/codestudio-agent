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
            if name not in ("native", "toolchain"):
                self.assertEqual(command[0], sys.executable)
        if "real_build" in commands:
            self.assertNotEqual(commands["real_build"][1], "tests/local_models_integration.py")

    def test_macos_inventory_preserves_base_tests_and_reports_unsupported_suites(self):
        with patch.object(verification, "PLATFORM", "macosx"), patch.object(verification, "ARCH", "arm64"), \
             patch.object(verification, "SUFFIX", ""):
            commands = verification.commands("debug", None)
            unsupported = verification.unsupported_suites()
            for name in ("toolchain", "native", "credentials", "provider", "streaming", "cli",
                         "mcp", "mcp_http", "mcp_oauth", "plugins", "resume", "contract", "service",
                         "scheduler", "supervisor", "autostart", "cron"):
                self.assertIn(name, commands)
            self.assertFalse(set(commands).intersection(unsupported))
            self.assertIn("real_build", unsupported)
            self.assertIn("delivery", unsupported)
            self.assertTrue(all(unsupported.values()))
            self.assertEqual(Path(commands["native"][0]).parts[-4:],
                             ("macosx", "arm64", "debug", "agent-tests"))
            self.assertEqual(verification.shared_library("fixture"), "libfixture.dylib")

    def test_macos_temporary_directory_is_canonical_without_relaxing_product_checks(self):
        actual = self.root / "actual-temp"
        actual.mkdir()
        alias = self.root / "temp-alias"
        try:
            alias.symlink_to(actual, target_is_directory=True)
        except OSError:
            self.skipTest("Host cannot create a temporary-directory symlink")
        env = {"TMPDIR": str(alias), "UNCHANGED": "fixture"}
        with patch.object(verification, "PLATFORM", "macosx"):
            observed = verification.prepare_test_environment(env)
        self.assertEqual(env, {"TMPDIR": str(actual.resolve()), "UNCHANGED": "fixture"})
        self.assertEqual(observed, {"temporary_directory": str(actual.resolve())})
        self.assertTrue(alias.is_symlink())

    def test_macos_configuration_requires_explicit_native_llvm(self):
        installation = self.root / "llvm with spaces"
        (installation / "bin").mkdir(parents=True)
        (installation / "bin/clang++").write_bytes(b"fixture")
        runtime = self.root / "libcxx"
        for name in ("include/c++/v1/chrono", "lib/libc++.1.dylib", "lib/libc++abi.1.dylib", "lib/libc++experimental.a"):
            path = runtime / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture")
        with patch.object(verification, "PLATFORM", "macosx"), patch.object(verification, "ARCH", "arm64"):
            for env in ({}, {"CODESTUDIO_LLVM_ROOT": "relative"},
                        {"CODESTUDIO_LLVM_ROOT": str(self.root / "missing")}):
                with self.subTest(env=env), self.assertRaises(RuntimeError):
                    verification.configure_command("debug", env)
            command = verification.configure_command("release", {"CODESTUDIO_LLVM_ROOT": str(installation),
                                                                   "CODESTUDIO_LIBCXX_ROOT": str(runtime)})
            self.assertIn("--toolchain=llvm", command)
            self.assertIn("--sdk=" + str(installation.resolve()), command)
            self.assertIn("--macos_libcxx=" + str(runtime.resolve()), command)
            self.assertIn("--target_minver=15.0", command)
            self.assertNotIn("--system_deps=y", command)
            self.assertEqual(command[command.index("-a") + 1], "arm64")

    def test_changed_external_runtime_never_yields_successful_receipt(self):
        binary = self.root / "build" / verification.PLATFORM / verification.ARCH / "release" / ("codestudio-agent" + verification.SUFFIX)
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"fixture")
        with patch.object(verification, "ROOT", self.root), \
             patch.object(verification, "source_fingerprint", return_value={"sha256": "a", "files": 1}), \
             patch.object(verification, "artifact_fingerprint", return_value={"agent": "same"}), \
             patch.object(verification, "preflight", return_value={"libcxx_sha256": {"libc++.1.dylib": "old"}}), \
             patch.object(verification, "runtime_fingerprint", return_value={"libc++.1.dylib": "new"}), \
             patch.object(verification, "step", return_value={"name": "schema", "status": "succeeded", "returncode": 0}):
            self.assertEqual(verification.verify(self.args, "release"), 2)
        report = json.loads(next(self.root.glob("*/verification.json")).read_text())
        self.assertEqual(report["status"], "failed")
        self.assertIn("runtime changed", report["error"])

    def test_runtime_probe_requires_executed_format_and_timezone_evidence(self):
        valid = dict(cxx26=True, cplusplus=202400, format="probe:07", timezone_offsets=True,
                     timezone_dst=True, tzdb_version="2026a", current_zone="Asia/Shanghai")
        for payload, expected in ((valid, "succeeded"), ({}, "failed"),
                                  (dict(valid, timezone_dst=False), "failed"),
                                  (dict(valid, cplusplus=202302), "failed")):
            with self.subTest(payload=payload):
                result = self.step("print(" + repr(json.dumps(payload)) + ")", name="toolchain")
                self.assertEqual(result["status"], expected)

    def test_native_wrapper_forwards_arguments_to_selected_python(self):
        wrapper = (["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ROOT / "scripts/verify.ps1")]
                   if os.name == "nt" else ["bash", str(ROOT / ("scripts/verify-macos.sh" if sys.platform == "darwin" else "scripts/verify-linux.sh"))])
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
