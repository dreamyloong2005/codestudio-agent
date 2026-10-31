"""Package a specific successful native receipt twice and extract actual archive bytes."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
import types

ROOT = Path(__file__).resolve().parents[2]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verification", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(exist_ok=False)
    verification = args.verification.resolve(strict=True)
    native = json.loads(verification.read_text())
    trusted = types.ModuleType("native_verify")
    trusted.__file__ = str(ROOT / "scripts/verify.py")
    exec(compile((ROOT / "scripts/verify.py").read_bytes(), trusted.__file__, "exec"), trusted.__dict__)
    assert native["status"] == "succeeded" and native["build_requested"]
    assert native["mode"] == "release" and native["platform"] == "macosx"
    assert native["source"] == trusted.source_fingerprint()
    assert native["tested_artifacts"] == trusted.artifact_fingerprint("release")
    receipt = {"status": "failed", "started_at_utc": datetime.now(timezone.utc).isoformat(),
               "native_receipt": str(verification), "native_receipt_sha256": sha(verification),
               "source": native["source"], "runs": [], "formal_distribution": False}
    try:
        for index in (1, 2):
            command = [sys.executable, "-B", str(ROOT / "scripts/package.py"), "--mode", "release",
                       "--verification", str(verification), "--output", str(work / f"package-{index}")]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
            (work / f"package-{index}.log").write_text(result.stdout + result.stderr)
            assert result.returncode == 0, f"Packaging run {index} failed"
            receipt["runs"].append({"command": command, "result": json.loads(result.stdout)})
        archive = Path(receipt["runs"][0]["result"]["archive"])
        assert sha(archive) == sha(Path(receipt["runs"][1]["result"]["archive"]))
        extracted = work / "extracted"
        extracted.mkdir()
        with tarfile.open(archive, "r:gz") as container:
            members = container.getmembers()
            assert 0 < len(members) <= 512
            assert sum(item.size for item in members) <= 512 * 1024 * 1024
            seen = set()
            for item in members:
                name = PurePosixPath(item.name)
                assert item.isfile() and not name.is_absolute() and ".." not in name.parts
                assert item.name not in seen
                seen.add(item.name)
            container.extractall(extracted, filter="data")
        frozen = trusted.delivery_fingerprint(extracted)
        metadata = json.loads((extracted / "build-metadata.json").read_text())
        assert metadata["source_binary_sha256"] == native["artifact"]["sha256"]
        assert metadata["binary_sha256"] == frozen["files"]["codestudio-agent"]
        assert trusted.source_fingerprint() == native["source"]
        receipt.update(status="succeeded", archive=str(archive), archive_sha256=sha(archive),
                       archive_size=archive.stat().st_size, reproducible_archive=True,
                       extracted=str(extracted), package=frozen,
                       source_binary_sha256=metadata["source_binary_sha256"],
                       relocated_binary_sha256=metadata["binary_sha256"],
                       runtime_records=metadata["macos_bundle"]["runtime_records"])
    finally:
        receipt["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
        (work / "packaging.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"status": receipt["status"], "archive_sha256": receipt["archive_sha256"],
                      "extracted": receipt["extracted"]}))

if __name__ == "__main__":
    main()
