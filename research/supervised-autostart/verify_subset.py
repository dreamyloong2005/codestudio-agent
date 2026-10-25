"""Reproduce this milestone's bounded platform regression with per-suite logs."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--root", type=Path, default=Path.cwd())
parser.add_argument("--logs", type=Path, required=True)
parser.add_argument("--mode", choices=("debug", "release"), required=True)
args = parser.parse_args()
root = args.root.resolve()
logs = args.logs.resolve()
logs.mkdir(parents=True, exist_ok=True)
platform, arch, compiler = ("windows", "x64", "clang-cl") if os.name == "nt" else ("linux", "x86_64", "gcc")
suffix = ".exe" if os.name == "nt" else ""
binary = root / "build" / platform / arch / args.mode
agent = str(binary / ("codestudio-agent" + suffix))
env = dict(os.environ)
if os.name != "nt" and os.getuid() == 0:
    env["XMAKE_ROOT"] = "y"

def run(name, command):
    stage = "final"
    path = logs / f"{platform}-{args.mode}-{name}-{stage}.log"
    with path.open("wb") as output:
        result = subprocess.run(command, cwd=root, env=env, stdout=output, stderr=subprocess.STDOUT)
    print(f"{platform}/{args.mode} {name}: {result.returncode}", flush=True)
    if result.returncode:
        print(path.read_text(encoding="utf-8", errors="replace")[-8000:], flush=True)
        raise SystemExit(result.returncode)

run("config", ["xmake", "f", "-p", platform, "-a", arch, f"--toolchain={compiler}", "-m", args.mode,
               *(["--system_deps=y"] if platform == "linux" else []), "-y"])
run("build", ["xmake", "build", "-a", "-j", "6"])
run("native", [str(binary / ("agent-tests" + suffix))])
for suite in ("autostart", "supervisor", "scheduler"):
    run(suite, [sys.executable, f"tests/{suite}_integration.py", agent])
run("contract", [sys.executable, "tests/client_contract_integration.py", agent])
run("service", [sys.executable, "tests/client_contract_integration.py", agent, "--service-suite"])
