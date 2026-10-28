#!/usr/bin/env python3
"""Run small self-owned macOS resource-limit experiments, without privileges."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import tempfile
import time


def run(command, timeout=5):
    start = time.monotonic()
    child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, start_new_session=True)
    timed_out = False
    try:
        stdout, stderr = child.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        # Only the isolated session created above belongs to this runner.
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = child.communicate(timeout=5)
    records = []
    for line in stdout.splitlines():
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return {"command": [str(arg) for arg in command], "returncode": child.returncode,
            "timed_out": timed_out, "seconds": round(time.monotonic() - start, 6),
            "stdout": stdout, "stderr": stderr, "records": records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="JSON destination (default: stdout)")
    args = parser.parse_args()
    if platform.system() != "Darwin" or os.geteuid() == 0:
        parser.error("run on native macOS as an unprivileged user")
    source = Path(__file__).resolve().parent
    repository = source.parents[2]
    scratch = repository / "build" / "macos-stage3-probes" / "limits"
    scratch.mkdir(parents=True, exist_ok=True)
    files = ["rlimits.c", "private_limits.c", "virtualization.swift", "run.py"]
    evidence = {
        "schema_version": 1,
        "captured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": {"system": platform.system(), "macos": platform.mac_ver()[0],
                 "kernel": platform.release(), "machine": platform.machine(),
                 "effective_uid": os.geteuid(), "python": platform.python_version()},
        "sources_sha256": {name: hashlib.sha256((source / name).read_bytes()).hexdigest()
                           for name in files},
        "sdk": run(["/usr/bin/xcrun", "--show-sdk-path"]),
        "compiler": run(["/usr/bin/clang", "--version"]),
        "compilations": [], "experiments": [],
    }
    with tempfile.TemporaryDirectory(prefix="run-", dir=scratch) as temporary:
        directory = Path(temporary)
        for name, modes in [("rlimits", ["as", "rss", "nproc", "cpu", "footprint"]),
                            ("private_limits", ["memory", "thread", "newthread",
                                                "process", "clear", "fork"])]:
            binary = directory / name
            compilation = run(["/usr/bin/clang", "-Wall", "-Wextra", "-Werror", "-O1",
                               str(source / f"{name}.c"), "-o", str(binary)], timeout=60)
            evidence["compilations"].append(compilation)
            if compilation["returncode"] != 0:
                continue
            for mode in modes:
                result = run([str(binary), mode])
                result["fixture"], result["mode"] = name, mode
                evidence["experiments"].append(result)
        binary = directory / "virtualization"
        compilation = run(["/usr/bin/xcrun", "swiftc", str(source / "virtualization.swift"),
                           "-o", str(binary)], timeout=60)
        evidence["compilations"].append(compilation)
        if compilation["returncode"] == 0:
            result = run([str(binary)])
            result["fixture"], result["mode"] = "virtualization", "host_capabilities_only"
            evidence["experiments"].append(result)
    formatted = json.dumps(evidence, indent=2) + "\n"
    if args.output:
        args.output.write_text(formatted)
    else:
        print(formatted, end="")
    expected = lambda experiment: (experiment["returncode"] ==
                                   (-signal.SIGXCPU if experiment["mode"] == "cpu" else 0))
    healthy = (len(evidence["experiments"]) == 12 and
               all(item["returncode"] == 0 for item in evidence["compilations"]) and
               all(expected(item) and not item["timed_out"] for item in evidence["experiments"]))
    return 0 if healthy else 1


if __name__ == "__main__":
    raise SystemExit(main())
