"""Capture a bounded read-only native fixture inventory; never clean or signal."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DIRECTORY_LIMIT = 4096
SCAN_LIMIT = 131072


def run(argv):
    return subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=30,
                          env={k: v for k, v in os.environ.items() if not k.startswith(("CODESTUDIO_", "DYLD_"))})


def metadata(path):
    value = path.lstat()
    return {"path": str(path), "uid": value.st_uid, "mode": oct(stat.S_IMODE(value.st_mode)),
            "device": value.st_dev, "inode": value.st_ino, "size": value.st_size,
            "kind": "directory" if stat.S_ISDIR(value.st_mode) else "file" if stat.S_ISREG(value.st_mode) else "other"}


def entries(path, predicate=lambda p: True):
    try:
        if path.is_symlink():
            return {"status": "not_inspected_symlink", "entries": []}
        if not path.exists():
            return {"status": "absent", "entries": []}
        names = []
        for index, child in enumerate(path.iterdir()):
            if index >= SCAN_LIMIT:
                return {"status": "scan_bound_exceeded", "entries": []}
            if predicate(child):
                names.append(child)
                if len(names) > DIRECTORY_LIMIT:
                    return {"status": "matching_entry_bound_exceeded", "entries": []}
        result = []
        for child in sorted(names):
            item = metadata(child)
            if child == ROOT:
                item["classification"] = "repository_not_test_fixture"
            elif child == Path.home() / "codestudio-lite":
                item["classification"] = "preexisting_unrelated_repository_not_test_fixture"
            result.append(item)
        return {"status": "observed", "entries": result}
    except OSError as error:
        return {"status": "unreadable", "error_type": type(error).__name__, "entries": []}


def capture(helper):
    canonical_temp = Path(tempfile.gettempdir()).resolve(strict=True)
    native_temp = run(["/usr/bin/getconf", "DARWIN_USER_TEMP_DIR"])
    directories = {str(path): entries(path, lambda item: item.name.startswith("codestudio-"))
                   for path in sorted({canonical_temp, Path.home()})}
    oauth_locks = {"status": "native_temp_unavailable"}
    if native_temp.returncode == 0 and native_temp.stdout.strip():
        lock_directory = Path(native_temp.stdout.strip()).resolve(strict=True) / "org.codestudio.Agent.oauth-locks"
        oauth_locks = {"path": str(lock_directory), **entries(lock_directory),
                       "ownership": "Directory may include historical or non-test lock names; no ownership or deletion inferred from naming alone.",
                       "contents_read": False}
    launch_directory = Path.home() / "Library/LaunchAgents"
    launches = entries(launch_directory, lambda item: item.name.startswith("com.codestudio.agent.") and item.suffix == ".plist")
    for item in launches["entries"]:
        path = Path(item["path"])
        if item["kind"] != "file" or item["size"] > 1024 * 1024:
            item["inspection"] = "not_regular_or_oversized"
            continue
        item["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        label = path.name[:-6]
        loaded = run(["/bin/launchctl", "print", f"gui/{os.getuid()}/{label}"])
        item.update(label=label, launchctl_returncode=loaded.returncode, loaded=loaded.returncode == 0)
        for field in ("pid", "state", "runs"):
            match = re.search(r"^\s*" + field + r" = (.*)$", loaded.stdout, re.M)
            if match:
                item[field] = match[1]
    processes = run(["/bin/ps", "-axo", "pid=,ppid=,comm="])
    candidates = []
    for line in processes.stdout.splitlines():
        fields = line.strip().split(None, 2)
        if len(fields) != 3:
            continue
        executable = Path(fields[2]).name
        if executable == "codestudio-agent" or "fixture" in executable or executable in ("agent-tests", "toolchain-probe"):
            candidates.append({"pid": int(fields[0]), "ppid": int(fields[1]), "executable": fields[2]})
    keychain = run([helper])
    keychain_inventory = json.loads(keychain.stdout) if keychain.returncode == 0 else {"status": "helper_failed", "returncode": keychain.returncode}
    return {"captured_at_utc": datetime.now(timezone.utc).isoformat(),
            "scope_version": 1, "scope": "Read-only snapshots of immediate codestudio-* entries in canonical TMPDIR/home, OAuth lock metadata, current-user CodeStudio LaunchAgent files, scoped test Keychain attributes, and Agent/fixture executable processes.",
            "fixture_directories": directories, "oauth_locks": oauth_locks,
            "launch_agents": launches, "processes": {"returncode": processes.returncode, "candidates": candidates, "arguments_read": False},
            "keychain": keychain_inventory,
            "history": "Historical ownership and total system residue are unknown; only comparison with this run's before snapshot may establish additions.",
            "mutations": "No deletes, signals, launchctl mutations, Keychain writes, search-list/default changes, or authentication prompts."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    receipt = capture(args.helper.resolve(strict=True))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x") as stream:
        json.dump(receipt, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(json.dumps({"output": str(args.output),
                      "keychain_osstatus": [s["osstatus"] for s in receipt["keychain"].get("services", [])],
                      "launch_agents": len(receipt["launch_agents"]["entries"]),
                      "process_candidates": len(receipt["processes"]["candidates"])}))


if __name__ == "__main__":
    main()
