"""Summarize observed follow-up receipts; never substitute preparation for acceptance."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import types

ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = Path(__file__).resolve().parent
read = lambda path: json.loads(path.read_text())
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--debug", type=Path, required=True)
    parser.add_argument("--release", type=Path, required=True)
    parser.add_argument("--delivery", type=Path, required=True)
    parser.add_argument("--packaging", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    args = parser.parse_args()
    verifier = types.ModuleType("followup_verifier")
    verifier.__file__ = str(ROOT / "scripts/verify.py")
    exec(compile((ROOT / "scripts/verify.py").read_bytes(), verifier.__file__, "exec"), verifier.__dict__)
    source = verifier.source_fingerprint()
    reports = {mode: read(getattr(args, mode)) for mode in ("debug", "release", "delivery")}
    native = {}
    for mode, report in reports.items():
        assert report["status"] == "succeeded" and report["source"] == source
        assert report["tested_artifacts"] == verifier.artifact_fingerprint("release" if mode == "delivery" else mode)
        native[mode] = {"receipt": str(getattr(args, mode).resolve().relative_to(EVIDENCE)),
                        "receipt_sha256": sha(getattr(args, mode)), "status": report["status"],
                        "native_groups": sum(s.get("native_groups", 0) for s in report["steps"]),
                        "python_passed": sum(s.get("passed", 0) for s in report["steps"]),
                        "python_skipped": sum(s.get("skipped", 0) for s in report["steps"]),
                        "selected_suites": report["selected_suites"],
                        "unsupported_suites": report["unsupported_suites"],
                        "artifact": report["artifact"]}
    packaging, client = read(args.packaging), read(args.client)
    assert packaging["status"] == client["status"] == "succeeded"
    assert packaging["source"] == client["source"] == source
    assert sha(Path(packaging["archive"])) == packaging["archive_sha256"]
    assert verifier.delivery_fingerprint(Path(packaging["extracted"])) == packaging["package"] == reports["delivery"]["delivery_package"] == client["package"]
    runtime = read(EVIDENCE / "login/runtime-manifest.json")
    for name, expected in runtime["files"].items():
        assert sha(Path(runtime["path"]) / name) == expected
    login = read(Path(runtime["login_receipt"]))
    assert login["executable_sha256"] == runtime["files"]["codestudio-agent"]
    assert login["status"] in ("awaiting_real_login", "passed")
    ci = read(EVIDENCE / "ci.json") if (EVIDENCE / "ci.json").exists() else read(EVIDENCE / "ci-preflight.json")
    after = read(EVIDENCE / "residue-after.json")
    summary = {
        "format": 1, "product": "CodeStudio Agent", "recorded_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": "local_followup_complete_external_acceptance_pending", "all_plan_items_accepted": False,
        "source": source,
        "M0": {"status": "completed_against_pushed_git_snapshot", "receipt": "source-sync.json",
               "environment": "environment.json", "windows_live_checkout_observed": False,
               "scope": "Mac synchronized from committed origin/main; no fresh Windows execution or current Windows working tree assertion"},
        "M1": {"status": "succeeded", "debug": native["debug"], "release": native["release"]},
        "M2": {"status": "succeeded", "archive": packaging["archive"],
               "archive_sha256": packaging["archive_sha256"], "reproducible_archive": packaging["reproducible_archive"],
               "package_manifest_sha256": packaging["package"]["manifest_sha256"],
               "source_binary_sha256": packaging["source_binary_sha256"],
               "relocated_binary_sha256": packaging["relocated_binary_sha256"],
               "runtime_records": packaging["runtime_records"], "delivery": native["delivery"],
               "installed_client_receipt": str(args.client.resolve().relative_to(EVIDENCE)),
               "client_checks": client["checks"], "clean_machine_verified": False,
               "formal_signing_or_notarization": "deferred_by_user"},
        "M3": {"status": login["status"], "runtime_manifest": "login/runtime-manifest.json",
               "receipt": runtime["login_receipt"], "historical_schema7_probe": "preserved_for_separate_acceptance",
               "operator_requirement": "Save other work, log out of the GUI user and log back in; never manually bootstrap/start either fixture. Then verify both owned historical and current receipts separately.",
               "real_login_verified": login["status"] == "passed"},
        "M4": {"status": "deferred_by_user_no_available_runner", "observed_ci_status": ci["status"],
               "receipt": "ci.json" if (EVIDENCE / "ci.json").exists() else "ci-preflight.json",
               "native_ci_verified": False, "artifact_bytes_validated": False},
        "residue": {"before": "residue-before.json", "after": "residue-after.json",
                    "process_candidates_after": len(after["processes"]["candidates"]),
                    "scope": after["scope"], "historical_or_global_cleanliness": "not_claimed"},
        "failure_history": [{"receipt": "native/debug-20261031T174223Z-a792c05d/verification.json",
                             "reason": "Two new test assumptions were incorrect: APFS aliases are canonicalized by libc++, and schema7 service construction fails before RPC.",
                             "repair": "Test assertions corrected; production identity checks unchanged; full final native runs use a new source fingerprint."}],
        "limits": ["Only macOS 26.5.2 native arm64 executed; macOS 15.0 is the deployment target, not a separately tested host.",
                   "Native OS/plugin/workspace/resource isolation remains disabled by user choice; no Linux VM.",
                   "Developer ID, Apple notarization, independent clean Mac and Intel/Universal remain deferred.",
                   "These shared test/verifier changes were not rerun on Windows/Linux in this Mac session."]
    }
    (EVIDENCE / "completion.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"source": source, "native": {mode: {k:v for k,v in data.items() if k in ("native_groups", "python_passed", "python_skipped")} for mode,data in native.items()}, "login": login["status"], "ci": ci["status"]}))

if __name__ == "__main__":
    main()
