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

def accepted_login(receipt, current_session, executable_sha256):
    assert receipt["status"] == "passed"
    assert receipt["verified_session"] == current_session != receipt["baseline_session"]
    assert receipt["executable_sha256"] == executable_sha256
    assert sha(Path(receipt["executable"])) == executable_sha256
    assert len(receipt["fixtures"]) == len(receipt["evidence"]) == 2
    assert {item["mode"] for item in receipt["fixtures"]} == {"ordinary", "supervised"}
    for fixture, observed in zip(receipt["fixtures"], receipt["evidence"]):
        assert observed["mode"] == fixture["mode"] and observed["label"] == fixture["label"]
        assert observed["plist_sha256"] == fixture["plist_sha256"]
        assert observed["launchd_runs"] == 1 and observed["worker"]["generation"] == 1
        assert observed["worker"]["started_at_ms"] >= receipt["prepared_at"] * 1000
        assert observed["disable_preserved_worker"] is True and observed["stopped"] is True
        assert observed["launchd_restarted"] is False


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
    m3 = {"status": login["status"], "runtime_manifest": "login/runtime-manifest.json",
          "receipt": runtime["login_receipt"], "historical_schema7_probe": "preserved_for_separate_acceptance",
          "operator_requirement": "Save other work, log out of the GUI user and log back in; never manually bootstrap/start either fixture. Then verify both owned historical and current receipts separately.",
          "real_login_verified": login["status"] == "passed"}
    residue_before, residue_after = "residue-before.json", "residue-after.json"
    post_login_path = EVIDENCE / "login/post-login-check.json"
    if post_login_path.exists():
        post = read(post_login_path)
        assert post["status"] == "passed" and post["source"] == source
        assert post["runtime_files_verified"] == len(runtime["files"]) == 52
        assert post["historical_runtime_verified"] is True
        assert post["schema8_receipt"] == "login/accepted.json"
        assert post["historical_schema7_receipt"] == "login/historical-schema7-accepted.json"
        assert post["residue_comparison"] == "login/residue-comparison.json"
        accepted = read(EVIDENCE / post["schema8_receipt"])
        historical = read(EVIDENCE / post["historical_schema7_receipt"])
        assert accepted == login
        assert historical == read(ROOT / "build/macos-login-acceptance/login-acceptance.json")
        accepted_login(accepted, post["current_session"], runtime["files"]["codestudio-agent"])
        accepted_login(historical, post["current_session"], runtime["historical_schema7_runtime_preserved_sha256"])
        credentials_path = (EVIDENCE / post["credentials_receipt"]).resolve()
        assert credentials_path.name == "verification.json"
        assert credentials_path.parent.parent == (EVIDENCE / "login/post-login-native").resolve()
        credentials = read(credentials_path)
        assert credentials["status"] == "succeeded" and credentials["source"] == source
        assert credentials["mode"] == "release" and credentials["build_requested"] is False
        assert credentials["tested_artifacts"] == reports["release"]["tested_artifacts"]
        assert credentials["artifact"] == reports["release"]["artifact"]
        assert credentials["selected_suites"] == ["credentials", "mcp_oauth"]
        assert [step["name"] for step in credentials["steps"]] == credentials["selected_suites"]
        for step in credentials["steps"]:
            assert step["status"] == "succeeded" and step["returncode"] == 0
            assert step["cases"] == step["passed"] + step["skipped"] and step["passed"] > 0
            assert step["skipped"] >= 0
        comparison = read(EVIDENCE / post["residue_comparison"])
        assert comparison["status"] == "passed"
        residue_before, residue_after = "login/residue-before-credentials.json", "residue-after-login.json"
        before, after = read(EVIDENCE / residue_before), read(EVIDENCE / residue_after)
        assert before["scope"] == after["scope"]
        assert not after["processes"]["candidates"]
        m3 = {"status": "passed", "runtime_manifest": "login/runtime-manifest.json",
              "receipt": post["schema8_receipt"], "receipt_sha256": sha(EVIDENCE / post["schema8_receipt"]),
              "acceptance_check": "login/post-login-check.json", "verified_session": post["current_session"],
              "real_login_verified": True, "runtime_files_verified": post["runtime_files_verified"],
              "historical_schema7_probe": {"status": "passed", "receipt": post["historical_schema7_receipt"],
                                           "receipt_sha256": sha(EVIDENCE / post["historical_schema7_receipt"]),
                                           "counts_as_schema8_acceptance": False},
              "post_login_credentials": {"status": credentials["status"], "receipt": post["credentials_receipt"],
                                         "receipt_sha256": sha(credentials_path), "scope": "native_release",
                                         "python_passed": sum(step["passed"] for step in credentials["steps"]),
                                         "python_skipped": sum(step["skipped"] for step in credentials["steps"]),
                                         "suites": [{key: step[key] for key in ("name", "status", "cases", "passed", "skipped")}
                                                    for step in credentials["steps"]]},
              "residue_comparison": post["residue_comparison"]}
    after = read(EVIDENCE / residue_after)
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
        "M3": m3,
        "M4": {"status": "deferred_by_user_no_available_runner", "observed_ci_status": ci["status"],
               "receipt": "ci.json" if (EVIDENCE / "ci.json").exists() else "ci-preflight.json",
               "implementation_commit": ci.get("head_sha", ci.get("source_head")),
               "runs": [{key: run.get(key) for key in ("id", "head_sha", "status", "conclusion", "html_url")}
                        for run in ci.get("runs", [])],
               "native_ci_verified": False, "artifact_bytes_validated": False},
        "residue": {"before": residue_before, "after": residue_after,
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
