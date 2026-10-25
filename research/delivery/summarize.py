"""Summarize final native receipts without counting abandoned runs as passes."""
import json
from pathlib import Path

root = Path(__file__).resolve().parent
summary = {"date": "2026-10-25", "format": 2, "configurations": {}}
for platform in ("windows", "linux"):
    for mode in ("debug", "release"):
        candidates = []
        for path in (root / platform).glob(f"{mode}-*/verification.json"):
            report = json.loads(path.read_text())
            if report["status"] == "succeeded" and report["build_requested"]:
                candidates.append((path, report))
        assert candidates, (platform, mode)
        path, report = max(candidates, key=lambda item: item[1]["finished_at"])
        assert report["omitted_suites"] == ["delivery"], report["omitted_suites"]
        steps = {step["name"]: step for step in report["steps"]}
        assert all(step["status"] == "succeeded" for step in steps.values())
        assert steps["native"]["native_groups"] == 22
        result = {"receipt": str(path.relative_to(root)), "source": report["source"], "artifact": report["artifact"],
                  "native_groups": steps["native"]["native_groups"],
                  "python_cases": sum(item.get("cases", 0) for item in steps.values()),
                  "python_passed": sum(item.get("passed", 0) for item in steps.values()),
                  "python_skipped": sum(item.get("skipped", 0) for item in steps.values()),
                  "suite_results": {name: {key: value for key, value in step.items()
                                           if key in ("native_groups", "cases", "passed", "skipped")}
                                    for name, step in steps.items() if name not in ("configure", "xmake-build")}}
        summary["configurations"][platform + "-" + mode] = result
summary["boundaries"] = [
    "Each native platform ran Debug then Release with the same unified entrypoint; all default suites selected.",
    "Python totals include static schema, packaging and verification-tool tests; Windows real XMake builds count separately as two PASS groups.",
    "Local model tests use only synthetic HTTP fixtures; live inference remains paused.",
    "Linux tests use an existing WSL host plus temporary cgroup and private Secret Service wrappers, not a clean machine or login session.",
    "Prepared-runner CI definition was locally checked but no hosted workflow, Git publication, real login/reboot or signing was performed.",
    "Installation validation runs separately against actual release packages; unknown/changed files are preserved by refusal."
]
(root / "verification.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
print(json.dumps({name: {key: result[key] for key in ("native_groups", "python_cases", "python_passed", "python_skipped")}
                  for name, result in summary["configurations"].items()}, indent=2))
