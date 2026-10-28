"""Current-host evidence from the actual final development DMG, not clean-Mac acceptance."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
work = Path(__file__).resolve().parent
release_root = work / "dmg-development"
release = json.loads((release_root / "release.json").read_text())
image = release_root / release["artifact"]["path"]
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
assert release["status"] == "development_only"
assert digest(image) == release["artifact"]["sha256"]
mount = Path(tempfile.mkdtemp(prefix="mounted-dmg-", dir=work))
cwd = Path(tempfile.mkdtemp(prefix="standalone-cwd-", dir=Path(tempfile.gettempdir()).resolve()))
receipt = {"status": "failed", "started_at": datetime.now(timezone.utc).isoformat(),
           "scope": "Actual final development DMG on current build host; not independent clean Mac",
           "clean_machine_verified": False, "image": str(image), "image_sha256": digest(image),
           "mount": str(mount), "cwd": str(cwd), "detached": False}
owned = False

def run(command):
    result = subprocess.run(command, capture_output=True, check=True, timeout=90)
    return result.stdout

def owned_mapping():
    info = plistlib.loads(run(["/usr/bin/hdiutil", "info", "-plist"]))
    matches = []
    for item in info.get("images", []):
        if Path(item.get("image-path", "/missing")).resolve() == image.resolve():
            assert item.get("writeable") is False
            matches.extend(entity for entity in item.get("system-entities", [])
                           if entity.get("mount-point") == str(mount))
    if len(matches) != 1 or not re.fullmatch(r"/dev/disk\d+(?:s\d+)*", matches[0].get("dev-entry", "")):
        raise RuntimeError("Cannot bind the final development image to the owned mount")
    return matches[0]

try:
    attached = run(["/usr/bin/hdiutil", "attach", "-readonly", "-nobrowse", "-plist", "-mountpoint", str(mount), str(image)])
    (work / "dmg-attach.plist").write_bytes(attached)
    mapping = owned_mapping()
    response = plistlib.loads(attached)
    owned = True
    receipt["device"] = mapping["dev-entry"]
    assert any(item.get("dev-entry") == mapping["dev-entry"] and item.get("mount-point") == str(mount)
               for item in response["system-entities"])
    package = mount / "codestudio-agent-macosx-arm64"
    assert digest(package / "package.json") == release["package_sha256"]
    environment = {key: value for key, value in os.environ.items() if not key.startswith(("CODESTUDIO_", "DYLD_"))}
    environment["TMPDIR"] = str(Path(tempfile.gettempdir()).resolve())
    command = [sys.executable, "-B", str(package / "verify-delivery.py"), "--standalone",
               "--package", str(package), "--receipt", str(work / "dmg-delivery.json")]
    receipt["command"] = command
    with (work / "dmg-delivery.log").open("w") as log:
        result = subprocess.run(command, cwd=cwd, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=240)
    receipt["returncode"] = result.returncode
    if result.returncode:
        raise RuntimeError("Mounted DMG standalone delivery failed; see dmg-delivery.log")
    tests = json.loads((work / "dmg-delivery.json").read_text())
    assert tests["status"] == "succeeded" and tests["clean_machine_verified"] is False
    assert digest(image) == receipt["image_sha256"]
    receipt["status"] = "succeeded"
finally:
    if owned:
        assert owned_mapping()["dev-entry"] == receipt["device"]
        run(["/usr/bin/hdiutil", "detach", str(mount)])
        mount.rmdir()
        receipt["detached"] = True
    if not any(cwd.iterdir()):
        cwd.rmdir()
    receipt["finished_at"] = datetime.now(timezone.utc).isoformat()
    (work / "dmg-mounted-verification.json").write_text(json.dumps(receipt, indent=2) + "\n")
print(json.dumps(receipt))
