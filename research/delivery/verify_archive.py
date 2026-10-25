"""Verify the shipped archive by running installed tests from a fresh extraction.

Run with the native test Python in the native build directory. Linux isolation
delegation is supplied by the caller, just as for the regular verify entrypoint.
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
import tempfile
import zipfile

evidence = Path(__file__).resolve().parent
root = Path.cwd().resolve()
platform = "windows" if sys.platform == "win32" else "linux"
arch = "x64" if platform == "windows" else "x86_64"
name = f"codestudio-agent-{platform}-{arch}"
archive = root / "build/packages" / f"{platform}-{arch}" / (name + (".zip" if platform == "windows" else ".tar.gz"))
report_dir = evidence / f"installed-{platform}"
before = set(report_dir.glob("release-*/verification.json"))
payload, modes = {}, {}

def add(name, data, mode):
    path = PurePosixPath(name)
    assert path.as_posix() == name and not path.is_absolute()
    assert name and ".." not in path.parts and "\\" not in name and ":" not in name
    assert name not in payload
    payload[name], modes[name] = data, mode & 0o777

if platform == "windows":
    with zipfile.ZipFile(archive) as source:
        for entry in source.infolist():
            assert not entry.is_dir() and (entry.external_attr >> 16) & 0o170000 == 0o100000
            add(entry.filename, source.read(entry), entry.external_attr >> 16)
else:
    with tarfile.open(archive, "r:gz") as source:
        for entry in source:
            assert entry.isfile()
            add(entry.name, source.extractfile(entry).read(), entry.mode)

manifest = json.loads(payload["package.json"])
assert manifest["format"] == 2 and manifest["platform"] == platform and manifest["arch"] == arch
assert manifest["product"] == "CodeStudio-Agent"
assert set(payload) == {entry["path"] for entry in manifest["files"]} | {"package.json", "SHA256SUMS"}
sums = {}
for line in payload["SHA256SUMS"].decode().splitlines():
    digest, name = line.split("  ", 1)
    assert name not in sums
    sums[name] = digest
assert set(sums) == set(payload) - {"SHA256SUMS"}
for name, digest in sums.items():
    assert hashlib.sha256(payload[name]).hexdigest() == digest
for entry in manifest["files"]:
    assert len(payload[entry["path"]]) == entry["size"]
    assert hashlib.sha256(payload[entry["path"]]).hexdigest() == entry["sha256"]
    assert modes[entry["path"]] == (0o755 if entry["executable"] else 0o644)

with tempfile.TemporaryDirectory(prefix="codestudio-delivery-archive-", dir=root / "build") as temporary:
    extracted = Path(temporary)
    for name, data in payload.items():
        destination = extracted / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        if platform != "windows":
            destination.chmod(modes[name])
    code = subprocess.call([sys.executable, str(root / "scripts/verify.py"), "--mode", "release", "--skip-build",
                            "--suite", "delivery", "--package", str(extracted), "--report-dir", str(report_dir)], cwd=root)
    new_reports = set(report_dir.glob("release-*/verification.json")) - before
    assert len(new_reports) == 1
    report_path = new_reports.pop()
    report = json.loads(report_path.read_text())
    embedded = json.loads(payload["verification.json"])
    assert report["source"] == embedded["source"]
    assert report["artifact"] == embedded["artifact"]

result = {"format": 2, "platform": platform, "archive": str(archive),
          "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
          "payload_files": len(payload), "manifest_and_checksums": "verified",
          "native_extraction_modes": "verified", "tested_from_fresh_archive_extraction": True,
          "extraction_cleaned": not extracted.exists(),
          "verification_receipt": report_path.relative_to(evidence).as_posix(),
          "status": report["status"], "returncode": code}
(evidence / f"archive-check-{platform}.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result, indent=2))
assert code == 0 and report["status"] == "succeeded"
