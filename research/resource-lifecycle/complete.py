"""Assemble the reviewed milestone outcome from final verification receipts."""
import json
from datetime import datetime, timezone
from pathlib import Path

base = Path(__file__).resolve().parent
def read(name):
    return json.loads((base / name).read_text(encoding="utf-8-sig"))

verification = read("verification.json")
delivery = read("delivery.json")
static = read("final-static.json")
doctors = read("delivered-doctors.json")
assert delivery["status"] == "verified"
assert static["no_new_residue_gate"] == "passed"
assert doctors["status"] == "succeeded"
assert read("linux-depth-probe.json")["status"] == "succeeded"
assert len(verification["configurations"]) == 4
assert all(item["source"] == delivery["source"] for item in verification["configurations"].values())
result = {
    "format": 2,
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "status": "completed",
    "source": delivery["source"],
    "implemented": ["private native sandbox resource journals and leases", "task correlation",
                    "read-only bounded pagination", "exact-id preview and explicit reclaim",
                    "CLI and asynchronous task-service interfaces", "native identity and liveness refusal",
                    "ordinary Windows child assignment at process creation"],
    "configurations": {name: {key: item[key] for key in
                               ("native_groups", "python_passed", "python_skipped", "receipt")}
                       for name, item in verification["configurations"].items()},
    "installed_verification": delivery["installed_verification"],
    "delivery": "delivery.json",
    "resource_checks": "final-static.json",
    "no_new_residue_gate": static["no_new_residue_gate"],
    "zero_total_residue_gate": static["zero_residue_gate"],
    "historical_cleanup_block": "../plugin-isolation/cleanup-blocked.json",
    "initial_fixture_metadata_archive": "retained-journals/d7d5c4e8a70fd4237381bfff8903480f/archive-receipt.json",
    "boundaries": ["Historical unregistered directories and the previously policy-blocked fixture remain untouched.",
                   "One initial Linux test journal from a prior WSL boot was archived as evidence after lease/identity/hash/absence checks; this was metadata maintenance, not successful product reclamation.",
                   "Uncaptured resource identities and interrupted final journal removal are refused rather than inferred.",
                   "Resource cleanup preserves task database/checkpoint contents and does not replay unknown effects.",
                   "The early Windows Job membership test is supplemental; it does not deterministically reproduce the former admission gap.",
                   "Existing Windows and WSL hosts were tested; no clean VM, real login/reboot, live model or permanent installation acceptance.",
                   "This milestone has not been committed or pushed."]
}
(base / "completion.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"status": result["status"], "source": result["source"],
                  "no_new_residue_gate": result["no_new_residue_gate"],
                  "zero_total_residue_gate": result["zero_total_residue_gate"]}, indent=2))
