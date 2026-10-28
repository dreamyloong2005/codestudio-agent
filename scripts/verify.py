"""Native build/test entrypoint. No installations, account changes or live models.

Every invocation gets its own evidence directory. A failed or incomplete run
cannot replace a prior receipt. Debug/Release use the same native cache serially.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = "windows" if sys.platform == "win32" else "linux" if sys.platform.startswith("linux") else "macosx" if sys.platform == "darwin" else "unsupported"
ARCH = "x64" if PLATFORM == "windows" else "arm64" if PLATFORM == "macosx" else "x86_64"
SUFFIX = ".exe" if PLATFORM == "windows" else ""
COMMON = ("native", "schema", "credentials", "provider", "streaming", "cli", "local_models", "mcp", "mcp_http", "mcp_oauth", "plugins", "plugin_sandbox",
          "resume", "sandbox", "sandbox_resources", "contract", "service", "scheduler", "supervisor", "cron", "autostart")


def unsupported_suites():
    if PLATFORM != "macosx":
        return {}
    return {
        "mcp_oauth": "macOS native OAuth storage and refresh locking belong to stage 2",
        "plugin_sandbox": "macOS OS isolation belongs to stage 3",
        "sandbox": "macOS OS sandbox execution belongs to stage 3; refusal is covered by native/CLI tests",
        "autostart": "macOS LaunchAgent integration belongs to stage 2; refusal is covered by native/CLI tests",
        "sandbox_resources": "macOS isolated resource ownership and reclamation belong to stage 3",
        "supervisor": "macOS background worker supervision belongs to stage 2",
        "real_build": "macOS isolated workspace execution belongs to stage 3",
        "delivery": "macOS installation, signing and notarization belong to stage 4",
    }


def shared_library(name):
    if PLATFORM == "windows":
        return name + ".dll"
    return "lib" + name + (".dylib" if PLATFORM == "macosx" else ".so")


def now():
    return datetime.now(timezone.utc).isoformat()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def version(text):
    match = re.search(r"(?<!\d)(\d+)\.(\d+)(?:\.(\d+))?", text)
    if not match:
        raise RuntimeError("Could not determine tool version")
    return tuple(int(item or 0) for item in match.groups())


def source_fingerprint():
    paths = [ROOT / name for name in ("xmake.lua", "xmake-requires.lock", "README.md")]
    for name in ("src", "include", "scripts", "tests", "docs", "protocol", "examples", ".github"):
        paths += [path for path in (ROOT / name).rglob("*") if path.is_file()
                  and "__pycache__" not in path.parts and path.suffix != ".pyc"]
    state = hashlib.sha256()
    for path in sorted(paths, key=lambda item: item.relative_to(ROOT).as_posix()):
        if path.is_symlink():
            raise RuntimeError("Source fingerprint refuses symbolic links")
        state.update(path.relative_to(ROOT).as_posix().encode() + b"\0" + digest(path).encode() + b"\n")
    return {"sha256": state.hexdigest(), "files": len(paths)}


def artifact_fingerprint(mode):
    directory = ROOT / "build" / PLATFORM / ARCH / mode
    names = ("codestudio-agent", "agent-tests", "provider-fixture", "process-fixture", "line-process-fixture",
             "mcp-fixture", "sandbox-fixture", "build-fixture", "sandbox-resource-fixture")
    result = {name + SUFFIX: digest(directory / (name + SUFFIX)) for name in names}
    for name in ("plugin-sandbox-fixture" + SUFFIX,
                 shared_library("plugin-sandbox-library"), shared_library("plugin-sandbox-dependency")):
        result[name] = digest(directory / name)
    if PLATFORM == "macosx":
        result["toolchain-probe"] = digest(directory / "toolchain-probe")
    return result


@contextmanager
def verification_lock():
    parent = ROOT / "build"
    parent.mkdir(exist_ok=True)
    lock = parent / f".verify-{PLATFORM}-{ARCH}.lock"
    token = uuid.uuid4().hex
    try:
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError:
        raise RuntimeError("Another verification or an interrupted verification lock requires inspection") from None
    try:
        with os.fdopen(descriptor, "w", encoding="ascii") as output:
            output.write(token)
        yield
    finally:
        if lock.is_file() and not lock.is_symlink() and lock.read_text(encoding="ascii") == token:
            lock.unlink()


def capture(command, env):
    output = subprocess.run(command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=20, check=True).stdout
    text = re.sub(r"\x1b\[[0-9;]*m", "", output.decode("utf-8", "replace"))
    return text.strip().splitlines()[0][:512]


def llvm_root(env):
    value = env.get("CODESTUDIO_LLVM_ROOT", "")
    root = Path(value)
    if not value or not root.is_absolute() or not (root / "bin/clang++").is_file():
        raise RuntimeError("macOS verification requires CODESTUDIO_LLVM_ROOT pointing to an absolute LLVM installation")
    return root.resolve()


def libcxx_root(env):
    value = env.get("CODESTUDIO_LIBCXX_ROOT", "")
    root = Path(value)
    required = ("include/c++/v1/chrono", "lib/libc++.1.dylib", "lib/libc++abi.1.dylib", "lib/libc++experimental.a")
    if not value or not root.is_absolute() or not all((root / name).is_file() for name in required):
        raise RuntimeError("macOS verification requires CODESTUDIO_LIBCXX_ROOT with timezone-enabled libc++ headers and libraries")
    return root.resolve()


def runtime_fingerprint(env):
    runtime = libcxx_root(env)
    return {name: digest(runtime / "lib" / name)
            for name in ("libc++.1.dylib", "libc++abi.1.dylib", "libc++experimental.a")}


def prepare_test_environment(env):
    if PLATFORM == "macosx":
        # Darwin's usual /var/folders TMPDIR traverses the /var symlink. Native
        # fixtures must use its real directory so product path checks still
        # reject symbolic links rather than granting a testing-only exception.
        directory = Path(env.get("TMPDIR") or tempfile.gettempdir()).resolve(strict=True)
        if not directory.is_dir():
            raise RuntimeError("The macOS test temporary directory is not a directory")
        env["TMPDIR"] = str(directory)
        return {"temporary_directory": str(directory)}
    return {}


def configure_command(mode, env):
    toolchain = "clang-cl" if PLATFORM == "windows" else "llvm" if PLATFORM == "macosx" else "gcc"
    options = ["--system_deps=y"] if PLATFORM == "linux" else []
    if PLATFORM == "macosx":
        options += ["--sdk=" + str(llvm_root(env)), "--macos_libcxx=" + str(libcxx_root(env)),
                    "--target_minver=15.0"]
    return ["xmake", "f", "-p", PLATFORM, "-a", ARCH, "--toolchain=" + toolchain,
            "-m", mode, *options, "-y"]


def preflight(env, build, suites):
    expected_arches = ("arm64", "aarch64") if PLATFORM == "macosx" else ("amd64", "x86_64")
    if PLATFORM == "unsupported" or platform.machine().lower() not in expected_arches:
        raise RuntimeError("Verification requires native Windows/Linux x86-64 or macOS arm64")
    config = json.loads((ROOT / "scripts/toolchains.json").read_text(encoding="utf-8"))
    if sys.version_info[:3] < version(config["python_minimum"]):
        raise RuntimeError("Python 3.11 or newer is required")
    observed = {"python": platform.python_version(), "requirements": config}
    if PLATFORM == "macosx":
        observed["libcxx_root"] = str(libcxx_root(env))
        observed["libcxx_sha256"] = runtime_fingerprint(env)
        observed["macos"] = capture(["sw_vers", "-productVersion"], env)
        observed["deployment_target"] = config["macosx"]["macos_minimum"]
        if version(observed["macos"]) < version(observed["deployment_target"]):
            raise RuntimeError("macOS is below scripts/toolchains.json requirements")
    if "schema" in suites or "contract" in suites or "service" in suites:
        observed["jsonschema"] = importlib.metadata.version("jsonschema")
        if observed["jsonschema"] != config["jsonschema"]:
            raise RuntimeError("Install the pinned tests/requirements.txt in the selected Python environment")
    if build:
        observed["xmake"] = capture(["xmake", "--version"], env)
        compiler = ("clang-cl" if PLATFORM == "windows" else
                    str(llvm_root(env) / "bin/clang++") if PLATFORM == "macosx" else "g++")
        observed["compiler"] = capture([compiler, "--version"], env)
        if version(observed["xmake"]) < version(config["xmake_minimum"]):
            raise RuntimeError("XMake does not meet scripts/toolchains.json requirements")
        if version(observed["compiler"]) < version(config[PLATFORM]["compiler_minimum"]):
            raise RuntimeError("Compiler does not meet the C++26 baseline in scripts/toolchains.json")
        if PLATFORM == "macosx":
            observed["llvm_root"] = str(llvm_root(env))
            observed["sdk"] = capture(["xcrun", "--sdk", "macosx", "--show-sdk-path"], env)
            observed["sdk_version"] = capture(["xcrun", "--sdk", "macosx", "--show-sdk-version"], env)
        if PLATFORM == "linux":
            observed["system_dependencies"] = {}
            for name, minimum in config["linux"]["pkg_config_minimum"].items():
                actual = capture(["pkg-config", "--modversion", name], env)
                if version(actual) < version(minimum):
                    raise RuntimeError("A Linux system dependency is below scripts/toolchains.json requirements")
                observed["system_dependencies"][name] = actual
    isolated = {"native", "credentials", "mcp_http", "plugin_sandbox", "sandbox", "sandbox_resources", "build", "build_proposals", "real_build", "delivery"}
    if PLATFORM == "linux" and isolated.intersection(suites):
        delegation = env.get("CODESTUDIO_CGROUP_ROOT", "")
        if not delegation or not Path(delegation).is_dir() or not Path("/usr/bin/bwrap").is_file():
            raise RuntimeError("Linux isolation tests require bwrap and an existing CODESTUDIO_CGROUP_ROOT delegation")
        controllers = (Path(delegation) / "cgroup.subtree_control").read_text().split()
        if not {"cpu", "memory", "pids"}.issubset(controllers):
            raise RuntimeError("The Linux delegation must already enable cpu/memory/pids controllers")
        observed["isolation_delegation"] = "provided"
    return observed


def commands(mode, package):
    binary = ROOT / "build" / PLATFORM / ARCH / mode
    agent, provider = str(binary / ("codestudio-agent" + SUFFIX)), str(binary / ("provider-fixture" + SUFFIX))
    result = {"native": [str(binary / ("agent-tests" + SUFFIX))],
              "schema": [sys.executable, "tests/contract_schema.py"],
              "provider": [sys.executable, "tests/provider_integration.py", provider],
              "streaming": [sys.executable, "tests/streaming_integration.py", provider, agent],
              "contract": [sys.executable, "tests/client_contract_integration.py", agent],
              "service": [sys.executable, "tests/client_contract_integration.py", agent, "--service-suite"]}
    if PLATFORM == "macosx":
        result = {"toolchain": [str(binary / "toolchain-probe")], **result}
    for name in COMMON:
        if name not in result:
            result[name] = [sys.executable, f"tests/{name}_integration.py", agent]
    if PLATFORM == "windows":
        result["spawn_lifetime"] = [sys.executable, "tests/windows_spawn_lifetime.py", agent]
        for name in ("build", "build_proposals"):
            result[name] = [sys.executable, f"tests/{name}_integration.py", agent]
        result["real_build"] = [sys.executable, "tests/xmake_build_integration.py", agent]
    elif PLATFORM == "linux":
        result["real_build"] = [sys.executable, "tests/linux_build_integration.py", agent]
    result["package"] = [sys.executable, "tests/package_tests.py"]
    result["verification"] = [sys.executable, "tests/verification_tests.py"]
    result["delivery"] = [sys.executable, "tests/delivery_integration.py", agent, "--package", str(package)]
    return {name: command for name, command in result.items() if name not in unsupported_suites()}


def terminate(process):
    if process.poll() is not None:
        return
    if PLATFORM == "windows":
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=20)
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    process.wait(timeout=20)


def step(name, command, directory, env, timeout):
    log = directory / f"{name}.log"
    start = time.monotonic()
    result = {"name": name, "command": command, "log": log.name, "status": "failed"}
    with log.open("wb") as output:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=PLATFORM != "windows",
                                   creationflags=subprocess.CREATE_NO_WINDOW if PLATFORM == "windows" else 0)
        try:
            result["returncode"] = process.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            terminate(process)
            result["returncode"] = process.returncode
            result["interrupted"] = True
            result["cleanup"] = "process_group_terminated_detached_resources_require_inspection"
    result["seconds"] = round(time.monotonic() - start, 3)
    content = log.read_text(encoding="utf-8", errors="replace")
    if result["returncode"] == 0 and not result.get("interrupted"):
        result["status"] = "succeeded"
    if name == "toolchain":
        try:
            probe = json.loads(content)
            if not (probe["cxx26"] is True and probe["cplusplus"] > 202302
                    and probe["format"] == "probe:07" and probe["timezone_offsets"] is True
                    and probe["timezone_dst"] is True and probe["tzdb_version"] and probe["current_zone"]):
                raise ValueError("Incomplete runtime probe")
            result["capabilities"] = probe
        except (KeyError, TypeError, ValueError):
            result["status"] = "failed"
    elif name == "native":
        result["native_groups"] = len(re.findall(r"^PASS ", content, re.MULTILINE))
        if result["native_groups"] != 22 or re.search(r"^FAIL", content, re.MULTILINE):
            result["status"] = "failed"
    elif name == "real_build" and PLATFORM == "windows":
        variants = re.findall(r"^PASS real isolated XMake / (explicit-v2|custom-cache-v2) / "
                              r"C\+\+26 pack indexing / compile / link / execute / atomic export\r?$",
                              content, re.MULTILINE)
        result["native_groups"] = len(variants)
        summary = "PASS 2 real isolated XMake builds: explicit-v2 and custom-cache-v2"
        if (sorted(variants) != ["custom-cache-v2", "explicit-v2"]
                or not content.rstrip().endswith("\n" + summary)
                or re.search(r"^FAIL", content, re.MULTILINE)):
            result["status"] = "failed"
    elif name not in ("configure", "xmake-build"):
        # Every Python suite uses unittest; no summary is not a passing suite.
        counts = re.findall(r"^Ran (\d+) tests? in", content, re.MULTILINE)
        outcomes = re.findall(r"^OK(?: \(skipped=(\d+)\))?\s*\Z", content, re.MULTILINE)
        count = counts[-1] if counts else None
        outcome = outcomes[-1] if outcomes else None
        if count is None or outcome is None:
            result["status"] = "failed"
        else:
            result["cases"] = int(count)
            result["skipped"] = int(outcome or 0)
            result["passed"] = result["cases"] - result["skipped"]
            if result["cases"] <= 0 or not 0 <= result["skipped"] <= result["cases"]:
                result["status"] = "failed"
    print(f"{PLATFORM}/{directory.name.split('-')[0]} {name}: {result['status']}", flush=True)
    if result["status"] != "succeeded":
        print(content[-6000:], flush=True)
    return result


def verify(args, mode):
    available = commands(mode, args.package)
    full = [name for name in available if name != "delivery"]
    selected = args.suite or full
    if any(name not in available for name in selected) or len(set(selected)) != len(selected):
        raise RuntimeError("Invalid, repeated or non-native suite name; use --list")
    if "delivery" in selected and not args.package:
        raise RuntimeError("Delivery verification requires --package pointing to an extracted native package")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    directory = args.report_dir.resolve() / f"{mode}-{stamp}"
    directory.mkdir(parents=True, exist_ok=False)
    report = {"format": 2, "product": "CodeStudio-Agent", "platform": PLATFORM, "arch": ARCH, "mode": mode,
              "started_at": now(), "status": "failed", "selected_suites": selected,
              "omitted_suites": [name for name in available if name not in selected],
              "unsupported_suites": unsupported_suites(),
              "build_requested": not args.skip_build, "steps": []}
    env = dict(os.environ)
    env.update({"PYTHONIOENCODING": "utf-8", "PYTHONUTF8": "1", "XMAKE_COLORTERM": "nocolor"})
    if PLATFORM == "linux" and os.getuid() == 0:
        env["XMAKE_ROOT"] = "y"
    try:
        report["test_environment"] = prepare_test_environment(env)
        report["source"] = source_fingerprint()
        report["toolchain"] = preflight(env, not args.skip_build, selected)
        if PLATFORM == "macosx" and not args.skip_build:
            env["SDKROOT"] = report["toolchain"]["sdk"]
        if args.preflight_only:
            report["status"] = "preflight"
            return 0
        schedule = []
        if not args.skip_build:
            schedule += [("configure", configure_command(mode, env)),
                         ("xmake-build", ["xmake", "build", "-a", "-j", str(args.jobs)])]
        schedule += [(name, available[name]) for name in selected]
        tested = None
        for name, command in schedule:
            if name not in ("configure", "xmake-build") and tested is None:
                tested = artifact_fingerprint(mode)
                report["tested_artifacts"] = tested
            outcome = step(name, command, directory, env, args.timeout)
            report["steps"].append(outcome)
            if outcome["status"] != "succeeded":
                return 1
        binary = ROOT / "build" / PLATFORM / ARCH / mode / ("codestudio-agent" + SUFFIX)
        report["artifact"] = {"path": binary.relative_to(ROOT).as_posix(), "size": binary.stat().st_size,
                              "sha256": digest(binary)}
        if artifact_fingerprint(mode) != tested:
            raise RuntimeError("Built executables changed while tests were running")
        if "libcxx_sha256" in report["toolchain"] and runtime_fingerprint(env) != report["toolchain"]["libcxx_sha256"]:
            raise RuntimeError("C++ runtime changed while tests were running")
        if source_fingerprint() != report["source"]:
            raise RuntimeError("Source changed during verification; the receipt is not valid for a single snapshot")
        report["status"] = "succeeded"
        return 0
    except Exception as error:
        report["error"] = str(error)
        print(f"Verification failed: {error}", file=sys.stderr)
        return 2
    finally:
        report["finished_at"] = now()
        (directory / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Report: {directory / 'verification.json'}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("debug", "release", "all"), default="debug")
    parser.add_argument("--suite", action="append", help="Select a suite; omitted suites stay explicit in the receipt")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--list", action="store_true", help="Read-only native suite inventory")
    parser.add_argument("--package", type=Path)
    parser.add_argument("--report-dir", type=Path, default=ROOT / "build/verification" / PLATFORM)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--timeout", type=int, default=900, help="Per-step wall-clock deadline in seconds")
    args = parser.parse_args()
    if args.list:
        print(json.dumps({"platform": PLATFORM, "arch": ARCH, "suites": list(commands("release", None)),
                          "unsupported_suites": unsupported_suites()}, indent=2))
        return 0
    if not 1 <= args.jobs <= 64 or not 1 <= args.timeout <= 7200:
        parser.error("--jobs must be 1..64; --timeout must be 1..7200")
    if args.package:
        args.package = args.package.resolve()
    try:
        with verification_lock():
            for mode in (("debug", "release") if args.mode == "all" else (args.mode,)):
                code = verify(args, mode)
                if code:
                    return code
        return 0
    except Exception as error:
        print(f"Verification failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
