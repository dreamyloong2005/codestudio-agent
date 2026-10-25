"""Run Linux verification in a disposable cgroup delegation (test setup only).

Requires root and pre-enabled cpu/memory/pids in the current cgroup root.
No global controllers, accounts, systemd units or login settings are changed.
Usage: python3 scripts/linux_test_delegation.py [--uid UID] -- COMMAND ...
"""
import argparse
import os
from pathlib import Path
import subprocess
import time
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--uid", type=int)
parser.add_argument("command", nargs=argparse.REMAINDER)
args = parser.parse_args()
command = args.command
if command[:1] == ["--"]:
    command = command[1:]
if os.geteuid() != 0 or not command:
    parser.error("root and a verification command are required")
parent = Path("/sys/fs/cgroup")
required = {"cpu", "memory", "pids"}
if not required.issubset(set((parent / "cgroup.subtree_control").read_text().split())):
    parser.error("the host has not delegated cpu/memory/pids; global controls were not changed")
delegation = parent / ("codestudio-test-" + uuid.uuid4().hex)
delegation.mkdir()
runner = None
try:
    (delegation / "cgroup.subtree_control").write_text("+cpu +memory +pids")
    env = os.environ.copy()
    env["CODESTUDIO_CGROUP_ROOT"] = str(delegation)
    if args.uid is not None:
        for path in [delegation, *(delegation / name for name in
                    ("cgroup.procs", "cgroup.threads", "cgroup.subtree_control"))]:
            os.chown(path, args.uid, args.uid)
        # The delegated user can migrate its children only within this subtree.
        # Enter an owned leaf while still privileged, then drop to the requested
        # existing UID without altering any host cgroup permission.
        runner = delegation / "verification-runner"
        runner.mkdir()
        command = ["setpriv", "--reuid", str(args.uid), "--regid", str(args.uid),
                   "--clear-groups", "--no-new-privs", "--", *command]
    def enter_runner():
        (runner / "cgroup.procs").write_text("0")
    result = subprocess.run(command, env=env, preexec_fn=enter_runner if runner else None)
    raise SystemExit(result.returncode)
finally:
    # Only this invocation's unique child is eligible for cleanup. Fail visibly
    # if runtime cleanup left members; do not touch sibling cgroups.
    children = sorted((path for path in delegation.rglob("*") if path.is_dir()),
                      key=lambda path: len(path.parts), reverse=True)
    leaked = [path for path in children if path != runner]
    if leaked:
        print("WARNING: test left cgroups:", ", ".join(str(path) for path in leaked), flush=True)
    (delegation / "cgroup.kill").write_text("1")
    for path in [*children, delegation]:
        for attempt in range(100):
            try:
                path.rmdir()
                break
            except OSError:
                if attempt == 99:
                    raise
                time.sleep(0.01)
    if leaked:
        raise RuntimeError("Verification left owned isolation cgroups; cleaned up but verification is not accepted")
