"""Read compiled capabilities from the two final delivered executables."""
import hashlib
import json
from pathlib import Path
import subprocess

base = Path(__file__).resolve().parent
root = base.parents[1]
delivery = json.loads((base / "delivery.json").read_text())
executables = {
    "windows": root / "build/packages/windows-x64/codestudio-agent-windows-x64/codestudio-agent.exe",
    "linux": root / "build/linux/x86_64/release/codestudio-agent",
}
receipts = {}
for platform, executable in executables.items():
    expected = delivery["packages"][platform]["binary" if platform == "windows" else "delivered_binary"]["sha256"]
    digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    assert digest == expected
    if platform == "windows":
        command = [str(executable), "doctor"]
    else:
        translated = "/mnt/" + executable.drive[0].lower() + executable.as_posix()[2:]
        command = ["wsl", "-d", "archlinux", "--", translated, "doctor"]
    response = subprocess.run(command, capture_output=True, check=True, timeout=20)
    doctor = json.loads(response.stdout)
    assert doctor["sandbox_resources"] == {"available": True, "reclaim": "explicit-id", "runtime_probe": "not-run"}
    assert doctor["state_schema"] == 7 and doctor["task_service"]["protocol_version"] == 2
    assert doctor["plugin_sandbox"]["available"] and doctor["plugin_locks"] and doctor["plugin_distribution"]
    assert hashlib.sha256(executable.read_bytes()).hexdigest() == expected
    receipts[platform] = {"path": str(executable), "sha256": digest, "doctor": doctor}
result = {"format": 2, "status": "succeeded", "scope": "Compiled capability declarations; runtime tests are in verification.json", "executables": receipts}
(base / "delivered-doctors.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps({"status": result["status"], "platforms": list(receipts)}))
