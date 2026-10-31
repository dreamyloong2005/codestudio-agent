"""Read-only Windows/Linux test-resource inventory; writes one new JSON receipt.

Use outside verification/delegation/Secret Service processes. No resource is
adopted, deleted, signalled or opened as a credential/database payload. Baseline
comparison reports observations, not a proof of ownership or safe reclamation.
"""
import argparse
import ctypes
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


NAMES = ("codestudio-agent", "agent-tests", "provider-fixture", "process-fixture",
         "line-process-fixture", "mcp-fixture", "sandbox-fixture", "build-fixture",
         "plugin-sandbox-fixture", "sandbox-resource-fixture")
PREFIXES = ("codestudio-sandbox-", "codestudio-isolation-", "codestudio-package-interface-",
            "codestudio-plugin-sandbox-", "codestudio-linux-sandbox-tests-",
            "codestudio-resource-test-", "codestudio-delivery-", "codestudio-package-test-",
            "codestudio-verification-", "codestudio-secret-service-", "codestudio-resume-",
            ".codestudio-package-stage-", ".codestudio-package-backup-", ".verify-")
errors = []


def entries(path):
    try:
        return list(path.iterdir())
    except FileNotFoundError:
        return []
    except OSError as error:
        errors.append({"path": str(path), "operation": "list", "error": type(error).__name__})
        return []


def walk(path):
    try:
        path.stat()
    except FileNotFoundError:
        return
    except OSError as error:
        errors.append({"path": str(path), "operation": "walk", "error": type(error).__name__})
        return
    def failed(error):
        errors.append({"path": str(error.filename), "operation": "walk", "error": type(error).__name__})
    for parent, directories, files in os.walk(path, followlinks=False, onerror=failed):
        directories[:] = [name for name in directories if name not in (".git", ".xmake", "__pycache__")
                          and not (Path(parent) / name).is_symlink()]
        yield Path(parent), directories, files


def key(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=True)


def windows(root):
    from ctypes import wintypes
    class Credential(ctypes.Structure):
        _fields_ = [("Flags", wintypes.DWORD), ("Type", wintypes.DWORD), ("TargetName", wintypes.LPWSTR),
                    ("Comment", wintypes.LPWSTR), ("LastWritten", wintypes.FILETIME),
                    ("CredentialBlobSize", wintypes.DWORD), ("CredentialBlob", ctypes.c_void_p),
                    ("Persist", wintypes.DWORD), ("AttributeCount", wintypes.DWORD),
                    ("Attributes", ctypes.c_void_p), ("TargetAlias", wintypes.LPWSTR), ("UserName", wintypes.LPWSTR)]
    library = ctypes.WinDLL("advapi32", use_last_error=True)
    array_type = ctypes.POINTER(ctypes.POINTER(Credential))
    library.CredEnumerateW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD,
                                     ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(array_type)]
    library.CredEnumerateW.restype = wintypes.BOOL
    library.CredFree.argtypes = [ctypes.c_void_p]
    credentials = []
    for pattern in ("CodeStudio-Agent/credentials/test-credential-*", "CodeStudio-Agent/oauth/test-credential-oauth-*"):
        count, values = wintypes.DWORD(), array_type()
        if library.CredEnumerateW(pattern, 0, ctypes.byref(count), ctypes.byref(values)):
            try:
                credentials += [values[index].contents.TargetName for index in range(count.value)]
            finally:
                library.CredFree(values)
        elif ctypes.get_last_error() != 1168:
            errors.append({"operation": "credential_names", "error": ctypes.get_last_error()})
    command = r'''
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$rootPath = (Resolve-Path '.').Path.TrimEnd('\') + '\'
$names = @('codestudio-agent.exe','agent-tests.exe','provider-fixture.exe','process-fixture.exe','line-process-fixture.exe','mcp-fixture.exe','sandbox-fixture.exe','build-fixture.exe','plugin-sandbox-fixture.exe','sandbox-resource-fixture.exe')
$all = @(Get-CimInstance Win32_Process)
$matched = @($all | Where-Object { $_.Name -in $names -and $_.ExecutablePath -and ($_.ExecutablePath.StartsWith($rootPath,[StringComparison]::OrdinalIgnoreCase) -or $_.ExecutablePath -match '\\codestudio-(delivery|sandbox|package-interface|resume|resource-test|isolation|plugin-sandbox)-[^\\]+\\') } | ForEach-Object { [ordered]@{ pid=$_.ProcessId; name=$_.Name; image=$_.ExecutablePath; born=$_.CreationDate.ToUniversalTime().ToString('o') } })
$unresolved = @($all | Where-Object { -not $_.ExecutablePath -and $_.Name -in $names } | ForEach-Object { [ordered]@{ pid=$_.ProcessId; name=$_.Name; born=$_.CreationDate.ToUniversalTime().ToString('o') } })
$scheduler = New-Object -ComObject 'Schedule.Service'
$scheduler.Connect()
$tasks = @($scheduler.GetFolder('\').GetTasks(1) | Where-Object { $_.Name -like 'CodeStudio-Agent-*' } | ForEach-Object { $_.Path })
[ordered]@{ project_processes=$matched; unresolved_processes=$unresolved; autostart_registrations=$tasks } | ConvertTo-Json -Depth 5
'''
    result = subprocess.run(["powershell", "-NoProfile", "-Command", command], cwd=root,
                            capture_output=True, timeout=40)
    if result.returncode:
        errors.append({"operation": "windows_process_and_autostart_scan", "returncode": result.returncode})
        observed = {"project_processes": [], "unresolved_processes": [], "autostart_registrations": []}
    else:
        observed = json.loads(result.stdout.decode("utf-8-sig"))
    observed["credential_test_names"] = credentials
    observed["sandbox_profiles"] = [str(path) for path in entries(Path(os.environ["LOCALAPPDATA"]) / "Packages")
                                    if re.fullmatch(r"codestudio-sandbox-[0-9a-f]{32}", path.name)]
    return observed, [Path.home() / ".codestudio-agent/sandbox-resources"]


def linux(root):
    processes, unresolved, credentials = [], [], []
    known_comm = {name[:15] for name in NAMES}
    for proc in entries(Path("/proc")):
        if not proc.name.isdigit():
            continue
        try:
            name = (proc / "comm").read_text().strip()
            stat = (proc / "stat").read_text()
            fields = stat[stat.rfind(")") + 2:].split()
            born = fields[19]
            try:
                image = (proc / "exe").resolve(strict=True)
            except (OSError, RuntimeError):
                if name in known_comm and proc.exists():
                    unresolved.append({"pid": int(proc.name), "name": name, "start_ticks": born})
                continue
            record = {"pid": int(proc.name), "name": name, "image": str(image), "start_ticks": born}
            if image.name in NAMES and (image.is_relative_to(root) or
                    any(part.startswith(PREFIXES) for part in image.parts)):
                processes.append(record)
            if image.is_relative_to(Path("/var/tmp/codestudio-credential-fixture")):
                credentials.append(record)
        except (FileNotFoundError, ProcessLookupError):
            continue
        except (OSError, ValueError, IndexError) as error:
            errors.append({"path": str(proc), "operation": "process_identity", "error": type(error).__name__})
    cgroups = []
    for parent, directories, _ in walk(Path("/sys/fs/cgroup")):
        cgroups += [str(parent / name) for name in directories
                    if name.startswith(("codestudio-test-", "codestudio-isolation-"))]
    config = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "systemd/user"
    registrations = []
    for parent, directories, files in walk(config):
        registrations += [str(parent / name) for name in (*directories, *files)
                          if name.startswith("codestudio-agent-") and name.endswith(".service")]
    locks = []
    try:
        locks = [line.split()[-1] for line in Path("/proc/net/unix").read_text().splitlines()
                 if line.split() and line.split()[-1].startswith("@codestudio-oauth.")]
    except OSError as error:
        errors.append({"operation": "oauth_lock_names", "error": type(error).__name__})
    state = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local/state")
    return {"project_processes": processes, "unresolved_processes": unresolved,
            "private_credential_fixture_processes": credentials, "test_cgroups": cgroups,
            "autostart_registrations": registrations, "oauth_abstract_locks": locks}, [state / "CodeStudio-Agent/sandbox-resources"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    platform = "windows" if os.name == "nt" else "linux" if sys.platform.startswith("linux") else None
    if not platform:
        parser.error("Only Windows and Linux are supported")
    if args.output.exists() or not args.output.parent.is_dir():
        parser.error("Output must be a new file in an existing directory")
    baseline = json.loads(args.baseline.read_text(encoding="utf-8")) if args.baseline else None
    if baseline and (baseline.get("format") != 2 or baseline.get("platform") != platform or baseline.get("root") != str(root)):
        parser.error("Baseline must belong to this platform and workspace")
    observed, registries = windows(root) if platform == "windows" else linux(root)
    temporary = Path(tempfile.gettempdir())
    observed["temporary_resources"] = [str(path) for parent in dict.fromkeys((temporary, Path("/tmp") if platform == "linux" else temporary,
                                                                           root / "build", root / "build/packages"))
        for path in entries(parent) if path.name.startswith(PREFIXES)
        or path.name.endswith((".codestudio-package.lock", ".codestudio-install.lock"))]
    if os.environ.get("CODESTUDIO_SANDBOX_REGISTRY"):
        registries.append(Path(os.environ["CODESTUDIO_SANDBOX_REGISTRY"]))
    observed["sandbox_registry_entries"] = [str(entry) for parent in dict.fromkeys(registries) for entry in entries(parent)]
    databases = []
    for parent, _, files in walk(root):
        databases += [str(parent / name) for name in files if name.endswith(
            (".db", ".db-wal", ".db-shm", ".sqlite", ".sqlite-wal", ".sqlite-shm", ".sqlite3", ".sqlite3-wal", ".sqlite3-shm"))]
    observed["project_database_candidates"] = databases
    observed = {name: sorted(values, key=key) for name, values in observed.items()}
    receipt = {"format": 2, "platform": platform, "root": str(root),
               "generated_at": datetime.now(timezone.utc).isoformat(), "observed": observed,
               "registry_roots": [str(path) for path in dict.fromkeys(registries)], "errors": errors,
               "scope": "Names, paths and process birth observations only; no resource mutation or credential/database payload reads. Database detection uses filename extensions. Linux credential values are not inspected; private-service cleanup needs its separate receipt.",
               "zero_total_residue_gate": "failed" if errors or any(observed.values()) else "passed"}
    if baseline:
        before = baseline["observed"]
        added, removed = {}, {}
        for name in sorted(set(observed) | set(before)):
            current = {key(value): value for value in observed.get(name, [])}
            previous = {key(value): value for value in before.get(name, [])}
            added[name] = [current[item] for item in sorted(current.keys() - previous.keys())]
            removed[name] = [previous[item] for item in sorted(previous.keys() - current.keys())]
        receipt.update(baseline=str(args.baseline.resolve()), new_observations=added,
                       no_longer_observed=removed,
                       no_new_residue_gate="failed" if errors or baseline.get("errors") or any(added.values()) else "passed")
    else:
        receipt["no_new_residue_gate"] = "not_evaluated_without_baseline"
    with args.output.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(receipt, output, ensure_ascii=False, indent=2)
        output.write("\n")
    print(json.dumps({"receipt": str(args.output), "errors": len(errors),
                      "no_new_residue_gate": receipt["no_new_residue_gate"],
                      "zero_total_residue_gate": receipt["zero_total_residue_gate"]}))
    return 1 if errors or receipt["no_new_residue_gate"] == "failed" else 0


if __name__ == "__main__":
    raise SystemExit(main())
