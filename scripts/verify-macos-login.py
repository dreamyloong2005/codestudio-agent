#!/usr/bin/env python3
"""Prepare and resume native LaunchAgent login acceptance without triggering a login.

prepare registers ordinary and supervised fixtures, but does not load/start them.
After the user logs out and back in, verify requires a new GUI login session and
launchd-owned running processes, checks disable preserves them, then stops them.
Static lint, a manual bootstrap, or a same-session run never counts as acceptance.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(arguments, *, success=True):
    environment = {key: value for key, value in os.environ.items() if not key.startswith("CODESTUDIO_")}
    result = subprocess.run(list(map(str, arguments)), env=environment, capture_output=True,
                            text=True, encoding="utf-8", timeout=30)
    if success:
        require(result.returncode == 0, f"Command failed: {arguments[0]} {arguments[1]}: {result.stderr or result.stdout}")
    return result


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def session():
    output = run(["/bin/launchctl", "print", f"gui/{os.geteuid()}"]).stdout
    require(re.search(r"^\s*type = login$", output, re.M) and re.search(r"^\s*session = Aqua$", output, re.M),
            "A current-user Aqua GUI login session is required")
    identity = re.search(r"^\s*asid = (\d+)$", output, re.M)
    require(identity is not None, "Cannot identify current GUI login session")
    return {"asid": int(identity[1]), "boot_uuid": run(["/usr/sbin/sysctl", "-n", "kern.bootsessionuuid"]).stdout.strip().lower()}


def command(manifest, fixture, action, *options):
    return json.loads(run([manifest["executable"], action, "--workspace", fixture["workspace"],
                           "--state", fixture["database"], *options]).stdout)


def write_receipt(path, value):
    temporary = path.with_suffix(".tmp")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(value, output, ensure_ascii=False, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def validate_registration(manifest, fixture):
    registration = command(manifest, fixture, "scheduler-autostart-status")
    require(registration["registered"] and registration["enabled"] and not registration["conflict"],
            "Fixture registration no longer matches the owned LaunchAgent contract")
    require(registration["task_name"] == fixture["label"] and registration["plist_path"] == fixture["plist_path"],
            "Fixture registration identity changed")
    path = Path(fixture["plist_path"])
    info = path.lstat()
    require(not path.is_symlink() and info.st_uid == os.geteuid() and info.st_nlink == 1 and not info.st_mode & 0o022,
            "Unsafe fixture LaunchAgent file")
    require(digest(path) == fixture["plist_sha256"], "Fixture plist changed after prepare")
    return registration


def prepare(args):
    executable = Path(args.executable).resolve(strict=True)
    root = Path(args.evidence_dir).resolve()
    require(not root.exists(), "Evidence directory already exists; use verify or clean up that receipt before preparing again")
    baseline = session()
    root.mkdir(parents=True, mode=0o700)
    manifest = {"schema": 1, "status": "awaiting_real_login", "uid": os.geteuid(), "prepared_at": time.time(),
                "baseline_session": baseline, "executable": str(executable), "executable_sha256": digest(executable),
                "fixtures": []}
    receipt = root / "login-acceptance.json"
    write_receipt(receipt, manifest)
    try:
        for mode in ("ordinary", "supervised"):
            workspace = root / (mode + " workspace 工作区")
            workspace.mkdir(mode=0o700)
            fixture = {"mode": mode, "workspace": str(workspace), "database": str(workspace / "state.sqlite3")}
            flags = ("--supervise", "--max-restarts", "2", "--restart-delay-ms", "250") if mode == "supervised" else ()
            registration = command(manifest, fixture, "scheduler-autostart-enable", "--poll-ms", "25", *flags)
            fixture.update(label=registration["task_name"], plist_path=registration["plist_path"],
                           plist_sha256=digest(registration["plist_path"]))
            manifest["fixtures"].append(fixture)
            write_receipt(receipt, manifest)
            require(not Path(fixture["database"]).exists(), "Registration unexpectedly started a worker")
            unloaded = run(["/bin/launchctl", "print", f"gui/{os.geteuid()}/{fixture['label']}"], success=False)
            require(unloaded.returncode != 0, "Fixture already loaded in current session; prepare does not accept manual launch")
    except Exception:
        for fixture in manifest["fixtures"]:
            command(manifest, fixture, "scheduler-autostart-disable")
        manifest["status"] = "prepare_failed"
        write_receipt(receipt, manifest)
        raise
    print(json.dumps({"status": manifest["status"], "receipt": str(receipt),
                      "next": "Manually log out of this macOS GUI user and log back in, then run verify with this evidence directory. Do not bootstrap or start the fixtures manually."},
                     ensure_ascii=False, indent=2))


def read_manifest(root):
    receipt = root / "login-acceptance.json"
    info = receipt.lstat()
    require(not receipt.is_symlink() and info.st_uid == os.geteuid() and info.st_nlink == 1 and not info.st_mode & 0o077,
            "Unsafe login acceptance receipt")
    manifest = json.loads(receipt.read_text())
    require(manifest["schema"] == 1 and manifest["uid"] == os.geteuid(), "Receipt belongs to a different user or schema")
    require(digest(manifest["executable"]) == manifest["executable_sha256"], "Executable changed after prepare; prepare a fresh receipt")
    return receipt, manifest


def verify(args):
    root = Path(args.evidence_dir).resolve(strict=True)
    receipt, manifest = read_manifest(root)
    require(manifest["status"] in ("awaiting_real_login", "login_observed_cleanup_pending"),
            "Receipt is not awaiting a real login or its cleanup")
    current = session()
    if manifest["status"] == "login_observed_cleanup_pending":
        require(current == manifest["verified_session"], "Login session changed during cleanup; use cleanup without claiming acceptance")
        finish_verification(receipt, manifest)
        return
    require(current != manifest["baseline_session"], "Still in the original GUI login session; real login acceptance remains pending")
    evidence = []
    for fixture in manifest["fixtures"]:
        validate_registration(manifest, fixture)
        output = run(["/bin/launchctl", "print", f"gui/{os.geteuid()}/{fixture['label']}"]).stdout
        pid_match = re.search(r"^\s*pid = (\d+)$", output, re.M)
        require(pid_match is not None and re.search(r"^\s*state = running$", output, re.M), "Login did not leave the expected LaunchAgent running")
        require(re.search(r"^\s*path = " + re.escape(fixture["plist_path"]) + r"$", output, re.M), "launchd loaded a different plist")
        pid = int(pid_match[1])
        require(run(["/bin/ps", "-p", str(pid), "-o", "ppid="]).stdout.strip() == "1", "LaunchAgent process is not a direct launchd child")
        status = command(manifest, fixture, "scheduler-status")
        require(status["state"] == "running", "Login worker is not running")
        require(status["started_at_ms"] >= manifest["prepared_at"] * 1000 and status["generation"] == 1,
                "Login fixture was already run or restarted outside this acceptance scenario")
        runs = re.search(r"^\s*runs = (\d+)$", output, re.M)
        require(runs is not None and int(runs[1]) == 1, "LaunchAgent did not run exactly once in the new GUI login session")
        if fixture["mode"] == "ordinary":
            require(status["pid"] == pid, "Registered LaunchAgent does not own the running worker")
        else:
            supervisor = status["supervisor"]
            require(supervisor["record"]["pid"] == pid and supervisor["worker_matches"] and
                    supervisor["observation"]["state"] == "matching_process", "LaunchAgent supervisor identity or worker ownership does not match")
            require(run(["/bin/ps", "-p", str(status["pid"]), "-o", "ppid="]).stdout.strip() == str(pid),
                    "Supervised login worker does not belong to its supervisor")
        evidence.append({"mode": fixture["mode"], "label": fixture["label"], "launchd_pid": pid,
                         "worker": status, "launchd_runs": int(runs[1]), "plist_sha256": fixture["plist_sha256"]})
    # Record successful login evidence before cleanup so interruptions remain diagnosable.
    manifest.update(status="login_observed_cleanup_pending", verified_at=time.time(), verified_session=current, evidence=evidence)
    write_receipt(receipt, manifest)
    finish_verification(receipt, manifest)


def finish_verification(receipt, manifest):
    for fixture, observed in zip(manifest["fixtures"], manifest["evidence"]):
        if observed.get("stopped"):
            continue
        registration = command(manifest, fixture, "scheduler-autostart-status")
        if registration["registered"]:
            validate_registration(manifest, fixture)
            removed = command(manifest, fixture, "scheduler-autostart-disable")
            require(removed["removed"], "Owned fixture was not removed")
        state = command(manifest, fixture, "scheduler-status")
        if not observed.get("disable_preserved_worker"):
            original = observed["worker"]
            require(state["state"] == "running" and all(state[key] == original[key] for key in ("pid", "owner", "generation")),
                    "Disabling autostart stopped or replaced its live worker")
            observed["disable_preserved_worker"] = True
            write_receipt(receipt, manifest)
        require(state["generation"] == observed["worker"]["generation"], "Fixture worker generation changed during cleanup")
        if state["state"] != "stopped":
            require(state["pid"] == observed["worker"]["pid"] and state["owner"] == observed["worker"]["owner"],
                    "Fixture worker identity changed during cleanup")
            command(manifest, fixture, "scheduler-stop")
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            state = command(manifest, fixture, "scheduler-status")
            job = run(["/bin/launchctl", "print", f"gui/{os.geteuid()}/{fixture['label']}"])
            runs = re.search(r"^\s*runs = (\d+)$", job.stdout, re.M)
            require(runs is not None and int(runs[1]) == observed["launchd_runs"], "launchd restarted a stopped fixture")
            if state["state"] == "stopped" and not re.search(r"^\s*state = running$", job.stdout, re.M):
                if fixture["mode"] != "supervised" or (state["supervisor"]["record"]["phase"] == "stopped" and
                        state["supervisor"]["observation"]["state"] == "not_running"):
                    break
            time.sleep(0.1)
        else:
            raise RuntimeError("Login fixture did not stop after scheduler-stop")
        observed["stopped"] = True
        observed["launchd_restarted"] = False
        write_receipt(receipt, manifest)
    manifest["status"] = "passed"
    write_receipt(receipt, manifest)
    print(json.dumps({"status": "passed", "receipt": str(receipt), "fixtures": len(manifest["evidence"])}, indent=2))


def cleanup(args):
    root = Path(args.evidence_dir).resolve(strict=True)
    receipt, manifest = read_manifest(root)
    for fixture in manifest["fixtures"]:
        registration = command(manifest, fixture, "scheduler-autostart-status")
        if registration["registered"]:
            validate_registration(manifest, fixture)
            command(manifest, fixture, "scheduler-autostart-disable")
        if Path(fixture["database"]).exists():
            command(manifest, fixture, "scheduler-stop")
    manifest["status"] = "cleaned_up_without_acceptance" if manifest["status"] != "passed" else "passed"
    write_receipt(receipt, manifest)
    print(json.dumps({"status": manifest["status"], "receipt": str(receipt)}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)
    for action in ("prepare", "verify", "cleanup"):
        child = subparsers.add_parser(action)
        child.add_argument("--evidence-dir", required=True)
        if action == "prepare":
            child.add_argument("--executable", required=True)
    args = parser.parse_args()
    require(sys.platform == "darwin", "Real macOS GUI login acceptance requires macOS")
    {"prepare": prepare, "verify": verify, "cleanup": cleanup}[args.action](args)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, KeyError, ValueError) as error:
        print(json.dumps({"status": "not_passed", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        sys.exit(1)
