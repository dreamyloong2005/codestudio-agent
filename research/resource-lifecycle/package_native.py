"""Create final native packages twice from one frozen successful Release report."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from finalize_delivery import validate_report, verify_tested_artifacts

root = Path.cwd().resolve()
evidence = Path(__file__).resolve().parent
platform = "windows" if sys.platform == "win32" else "linux"
reports = [(path, json.loads(path.read_text())) for path in (evidence / platform).glob("release-*/verification.json")]
reports = [(path, report) for path, report in reports if report["status"] == "succeeded" and report["build_requested"]
           and report["omitted_suites"] == ["delivery"]]
path, report = max(reports, key=lambda pair: pair[1]["finished_at"])
validate_report(report, platform, "release", report["source"])
verify_tested_artifacts(report, platform, "release", root)
spec = importlib.util.spec_from_file_location("verify", root / "scripts/verify.py")
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)
assert verify.source_fingerprint() == report["source"]
env = dict(os.environ)
if platform == "linux" and os.getuid() == 0:
    env["XMAKE_ROOT"] = "y"
results = []
for attempt in range(2):
    command = [sys.executable, str(root / "scripts/package.py"), "--mode", "release", "--verification", str(path)]
    process = subprocess.run(command, cwd=root, env=env, capture_output=True, timeout=120)
    (evidence / f"package-{platform}-{attempt + 1}.log").write_bytes(process.stdout + process.stderr)
    assert process.returncode == 0, (process.stdout + process.stderr).decode(errors="replace")
    result = json.loads(process.stdout)
    assert result["status"] == "packaged" and "cleanup" not in result and "lock_cleanup" not in result
    results.append(result)
assert verify.source_fingerprint() == report["source"]
assert results[0]["sha256"] == results[1]["sha256"]
verify_tested_artifacts(report, platform, "release", root)
(evidence / f"package-result-{platform}.json").write_text(json.dumps(results[1], indent=2) + "\n", encoding="utf-8")
reproducibility = {"format": 2, "platform": platform, "source_matches_release_report": True,
                   "verification_receipt": path.relative_to(evidence).as_posix(),
                   "archive_sha256": results[1]["sha256"], "identical_archive_runs": 2,
                   "scope": "Same binary, payload, metadata and verification report; not cross-toolchain build reproducibility"}
(evidence / f"package-repro-{platform}.json").write_text(json.dumps(reproducibility, indent=2) + "\n", encoding="utf-8")
print(json.dumps(results[1], indent=2))
