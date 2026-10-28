"""Archive one identified initial-test journal; never delete payload resources.

This is evidence maintenance for the exact fixture below, not a product reclaim
implementation. A prior-boot native identity cannot be asserted to match now.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess

evidence = Path(__file__).resolve().parent
resource_id = "d7d5c4e8a70fd4237381bfff8903480f"
registry = Path("/root/.local/state/CodeStudio-Agent/sandbox-resources")
expected_hash = "ca06b77c63a845c69d6da449d63b52630fd6a0f306cfdec0824e14316e51b8dc"
binary = "/var/tmp/codestudio-agent-linux/build/linux/x86_64/release/codestudio-agent"
handles = []

def pin_directory(path):
    current = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    handles.append(current)
    for part in path.parts[1:]:
        current = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=current)
        handles.append(current)
    return current

def absent_without_following(path):
    current = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    transient = [current]
    try:
        for part in path.parts[1:]:
            try:
                current = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=current)
            except FileNotFoundError:
                return True
            transient.append(current)
        return False
    finally:
        for descriptor in reversed(transient):
            os.close(descriptor)

def identity(value):
    return value.st_dev, value.st_ino, value.st_mode, value.st_uid, value.st_nlink

def secure(value, mode, directory=False):
    assert value.st_uid == os.geteuid() and stat.S_IMODE(value.st_mode) == mode
    assert stat.S_ISDIR(value.st_mode) if directory else stat.S_ISREG(value.st_mode) and value.st_nlink == 1

preview = subprocess.run([binary, "sandbox-reclaim", "--id", resource_id, "--dry-run"], capture_output=True, timeout=10)
assert preview.returncode == 1
blocked = json.loads(preview.stdout)
assert blocked["state"] == "blocked"
try:
    parent = pin_directory(registry)
    secure(os.fstat(parent), 0o700, True)
    entry = os.open(resource_id, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=parent)
    handles.append(entry)
    entry_stat = os.fstat(entry)
    secure(entry_stat, 0o700, True)
    assert sorted(os.listdir(entry)) == ["lease", "record.json"]
    files = {}
    for name in ("lease", "record.json"):
        descriptor = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=entry)
        handles.append(descriptor)
        value = os.fstat(descriptor)
        secure(value, 0o600)
        if name == "lease":
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        data = os.read(descriptor, 65537)
        assert len(data) == value.st_size and len(data) <= 65536
        files[name] = (descriptor, value, data)
    data = files["record.json"][2]
    assert hashlib.sha256(data).hexdigest() == expected_hash
    assert files["lease"][2] == b""
    record = json.loads(data)
    assert record["format"] == 2 and record["id"] == resource_id
    assert record["workspace"] == "/tmp/codestudio-linux-sandbox-tests-3921242775"
    assert record["owner"] == {"identity": "linux:fe95aa82-855a-4243-a87c-b07dccc2a96a:5645", "pid": 826, "uid": 0}
    boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    assert boot != record["owner"]["identity"].split(":")[1]
    paths = [Path(item["path"]) for item in record["resources"]]
    assert all(absent_without_following(path) for path in paths)
    archive = evidence / "retained-journals" / resource_id
    archive.mkdir(parents=True, exist_ok=False)
    for name, (_, _, contents) in files.items():
        with (archive / name).open("xb") as output:
            output.write(contents)
            output.flush()
            os.fsync(output.fileno())
        assert (archive / name).read_bytes() == contents
    receipt = {"format": 2, "action": "initial_fixture_metadata_archived", "product_reclaimed": False,
               "resource_id": resource_id, "current_boot": boot, "record_sha256": expected_hash,
               "product_preview": blocked, "payload_paths_observed_absent": [str(path) for path in paths],
               "payload_paths_modified": [], "source_registry": str(registry),
               "boundary": "Prior native identities do not match the current boot; this is not evidence of product reclamation across historical mounts."}
    with (archive / "archive-receipt.json").open("x", encoding="utf-8") as output:
        output.write(json.dumps(receipt, indent=2) + "\n")
        output.flush()
        os.fsync(output.fileno())
    assert json.loads((archive / "archive-receipt.json").read_text()) == receipt
    assert identity(os.stat(resource_id, dir_fd=parent, follow_symlinks=False)) == identity(entry_stat)
    assert sorted(os.listdir(entry)) == ["lease", "record.json"]
    assert all(absent_without_following(path) for path in paths)
    for name, (descriptor, original, contents) in files.items():
        assert identity(os.stat(name, dir_fd=entry, follow_symlinks=False)) == identity(original)
        assert os.pread(descriptor, 65537, 0) == contents
    for name in ("record.json", "lease"):
        os.unlink(name, dir_fd=entry)
    assert os.listdir(entry) == []
    os.rmdir(resource_id, dir_fd=parent)
    os.fsync(parent)
    print(json.dumps({"action": receipt["action"], "resource_id": resource_id, "archive": str(archive), "product_reclaimed": False}))
finally:
    for descriptor in reversed(handles):
        os.close(descriptor)
