"""Summarize completed diagnostics regression logs without counting reruns."""
import json
from pathlib import Path
import re

logs = Path(__file__).resolve().parent


def unittest_result(path):
    content = path.read_text(encoding="utf-8", errors="replace")
    matches = re.findall(r"^Ran (\d+) tests? in .+$", content, re.MULTILINE)
    assert len(matches) == 1, path
    outcome = re.search(r"^OK(?: \(skipped=(\d+)\))?\s*\Z", content, re.MULTILINE)
    assert outcome, path
    cases = int(matches[0])
    skipped = int(outcome[1] or 0)
    return {"cases": cases, "passed": cases - skipped, "skipped": skipped}


schema = unittest_result(logs / "schema-final.log")
result = {"date": "2026-10-24", "state_schema": 7, "protocol_version": 2,
          "static_schema_tests": schema["passed"], "configurations": {}}
assert schema == {"cases": 11, "passed": 11, "skipped": 0}
for platform in ("windows", "linux"):
    for mode in ("debug", "release"):
        prefix = f"{platform}-{mode}"
        native = (logs / f"{prefix}-native-final.log").read_text(encoding="utf-8")
        groups = len(re.findall(r"^PASS ", native, re.MULTILINE))
        assert groups == 21 and "FAIL" not in native, prefix
        suites = {name: unittest_result(logs / f"{prefix}-{name}-final.log")
                  for name in ("autostart", "supervisor", "scheduler", "cron", "resume", "contract", "service")}
        result["configurations"][prefix] = {
            "native_groups": groups, "suites": suites,
            "python_cases": sum(suite["cases"] for suite in suites.values()),
            "passed": sum(suite["passed"] for suite in suites.values()),
            "skipped": sum(suite["skipped"] for suite in suites.values())}
result["limitations"] = [
    "Only the related suites listed here were rerun; earlier full-platform suites remain historical evidence.",
    "No actual logon, logout, reboot or systemd user-manager reload/start was exercised.",
    "Windows detached-worker success is blocked by the enclosing Job; refusal and lack of side effects are checked.",
    "Tests use temporary workspaces and registrations plus mock HTTP/MCP; no permanent enrollment or real model inference.",
    "Process observation identifies a process at query time; it does not establish health, future liveness or control authority."]
(logs / "verification.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps({key: {name: value[name] for name in ("native_groups", "python_cases", "passed", "skipped")}
                  for key, value in result["configurations"].items()}, indent=2))
