#!/usr/bin/env python3
"""Exercise the shipped client and preserved user state with a verified macOS package.

This research harness uses only new owned temporary paths and synthetic state.
It does not claim independent clean-Mac, real-login, or notarization acceptance.
"""
from __future__ import annotations

import argparse
from contextlib import closing
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import types


ROOT = Path(__file__).resolve().parents[2]


def require(value, message):
    if not value:
        raise RuntimeError(message)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def load(name, path):
    module = types.ModuleType(name)
    module.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), "exec"), module.__dict__)
    return module


def now():
    return datetime.now(timezone.utc).isoformat()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--receipt-dir", type=Path, required=True)
    args = parser.parse_args()
    require(sys.platform == "darwin" and platform.machine() == "arm64", "Native macOS arm64 is required")
    installer = load("package_client_installer", ROOT / "scripts/install.py")
    verifier = load("package_client_verifier", ROOT / "scripts/verify.py")
    package = installer.check_chain(args.package)
    receipt_dir = installer.check_chain(args.receipt_dir)
    require(not receipt_dir.exists() and receipt_dir.parent.is_dir(), "Receipt directory must be new with an existing parent")
    require(not installer.paths_overlap(package, receipt_dir), "Receipt directory must be outside the package")
    interpreter = args.python.absolute()
    require(interpreter.is_file() and os.access(interpreter, os.X_OK), "Python must name an executable")
    source = verifier.source_fingerprint()
    verified = installer.verify_package(package)
    require((verified["document"]["platform"], verified["document"]["arch"]) == ("macosx", "arm64"), "Unexpected package target")
    initial_package = verifier.delivery_fingerprint(package)
    metadata = json.loads((package / "build-metadata.json").read_text())
    native = json.loads((package / "verification.json").read_text())
    require(native["source"] == source and native["status"] == "succeeded", "Native receipt must bind the current tested source")
    require(metadata["verification"]["sha256"] == digest(package / "verification.json"), "Native receipt digest mismatch")
    require(metadata["source_binary_sha256"] == native["artifact"]["sha256"], "Source executable provenance mismatch")
    require(metadata["binary_sha256"] == digest(package / "codestudio-agent"), "Shipped executable digest mismatch")
    receipt_dir.mkdir(mode=0o700)
    temporary = Path(tempfile.mkdtemp(prefix="codestudio-macos-durable-delivery-", dir=Path(tempfile.gettempdir()).resolve()))
    prefix = temporary / "application"
    workspace = temporary / "new-workspace"
    old_workspace = temporary / "legacy-workspace"
    workspace.mkdir()
    old_workspace.mkdir()
    old_database = old_workspace / "state-schema7.sqlite3"
    user_file = old_workspace / "user-data.txt"
    user_file.write_text("Synthetic user-owned text must survive application management.\n", encoding="utf-8")
    with closing(sqlite3.connect(old_database)) as database:
        database.execute("CREATE TABLE preserved(value TEXT)")
        database.execute("INSERT INTO preserved VALUES('synthetic legacy state must remain')")
        database.execute("PRAGMA user_version=7")
        database.commit()
    preserved = {"schema7_database": digest(old_database), "user_file": digest(user_file)}
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("CODESTUDIO_", "DYLD_", "LD_", "PYTHON"))}
    environment["TMPDIR"] = str(Path(tempfile.gettempdir()).resolve())
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    report = {"format": 1, "product": "CodeStudio Agent", "status": "failed", "started_at": now(),
              "scope": "Installed macOS development package, shipped example client, durable restart lookup and synthetic state preservation on the current host",
              "clean_machine_verified": False, "notarization_verified": False, "real_login_verified": False,
              "host": {"macos": platform.mac_ver()[0], "architecture": platform.machine(), "python": platform.python_version()},
              "source": source, "package": initial_package,
              "source_binary_sha256": metadata["source_binary_sha256"], "shipped_binary_sha256": metadata["binary_sha256"],
              "native_verification_sha256": digest(package / "verification.json"),
              "trusted_installer_sha256": digest(ROOT / "scripts/install.py"),
              "shipped_client_sha256": digest(package / "examples/task_client.py"),
              "harness_sha256": digest(Path(__file__)), "temporary_root": str(temporary),
              "commands": [], "checks": [], "preservation": [], "cleanup": "pending"}

    def run(name, command, *, expected=0, input_data=None):
        result = subprocess.run(list(map(str, command)), cwd=temporary, env=environment,
                                input=input_data, capture_output=True, timeout=45)
        require(len(result.stdout) + len(result.stderr) <= 4 * 1024 * 1024, "Command output exceeded the research bound")
        report["commands"].append({"name": name, "command": list(map(str, command)), "returncode": result.returncode,
                                   "stdout_sha256": hashlib.sha256(result.stdout).hexdigest(),
                                   "stderr_sha256": hashlib.sha256(result.stderr).hexdigest()})
        require(result.returncode == expected, "Unexpected return code from " + name)
        return result

    def preserved_at(stage):
        current = {"schema7_database": digest(old_database), "user_file": digest(user_file)}
        require(current == preserved, "Application management modified synthetic legacy state or user data")
        report["preservation"].append({"stage": stage, **current})

    def manage(operation):
        command = [interpreter, "-B", ROOT / "scripts/install.py", operation, "--prefix", prefix]
        if operation in ("install", "upgrade"):
            command += ["--package", package]
        return json.loads(run(operation, command).stdout)

    def query(agent, requests):
        commands = [("initialize", {"protocol_version": 2}), *requests, ("shutdown", {})]
        payload = "".join(json.dumps({"jsonrpc": "2.0", "id": index, "method": method, "params": params}) + "\n"
                          for index, (method, params) in enumerate(commands, 1)).encode()
        output = run("fresh-service-query", [agent, "serve", "--workspace", workspace], input_data=payload)
        frames = [json.loads(line) for line in output.stdout.splitlines()]
        replies = {frame["id"]: frame for frame in frames if "id" in frame}
        require(set(replies) == set(range(1, len(commands) + 1)), "Service response inventory is incomplete")
        require(all("result" in frame and "error" not in frame for frame in replies.values()), "Service query failed")
        return [replies[index]["result"] for index in range(1, len(commands) + 1)]

    try:
        preserved_at("before_install")
        installed = manage("install")
        require(installed["status"] == "installed", "Expected a fresh installation")
        require(installed["package_sha256"] == verified["package_sha256"], "Installed package identity differs")
        agent = Path(installed["executable"])
        require(digest(agent) == metadata["binary_sha256"], "Installed executable differs from the shipped executable")
        report["installed_binary_sha256"] = digest(agent)
        preserved_at("after_install")
        doctor = json.loads(run("installed-doctor", [agent, "doctor"]).stdout)
        require(doctor["state_schema"] == 8 and doctor["task_service"]["durable_requests"] is True, "Installed runtime lacks schema 8 durable requests")
        require(doctor["os_sandbox"] is False and all(doctor[name]["available"] is False
                for name in ("plugin_sandbox", "workspace_build", "sandbox_resources")), "Deferred isolation unexpectedly enabled")
        report["doctor"] = doctor
        database_path = workspace / ".codestudio/state.sqlite3"
        example = package / "examples/task_client.py"
        readonly = run("shipped-client-readonly", [interpreter, "-B", example, "--agent", agent, "--workspace", workspace])
        require(b"current task-list page contains 0 task(s)" in readonly.stdout, "Readonly example did not read the empty task page")
        require(not database_path.exists() and not (workspace / ".codestudio").exists(), "Readonly example created state")
        initialized = query(agent, [])[0]
        require(initialized["database_id"] is None and initialized["capabilities"]["durable_requests"] is True, "Fresh service identity is not readonly")
        require(initialized["capabilities"]["plugin_sandbox"] is False and initialized["capabilities"]["sandbox_resources"] is False,
                "Service advertises deferred isolation")
        require(not database_path.exists(), "Readonly initialize created a database")
        report["readonly_initialize"] = initialized
        report["checks"].append("Shipped example and fresh initialize read without creating a database")
        demo = run("shipped-client-demo", [interpreter, "-B", example, "--agent", agent, "--workspace", workspace, "--demo"])
        require(b"Demo completed; durable request receipt and committed task verified; actions: 2." in demo.stdout,
                "Shipped example did not verify a completed durable offline task")
        target = workspace / "CODESTUDIO_DEMO.txt"
        require(target.read_bytes() == b"Hello from CodeStudio-Agent!\n", "Offline demo output differs")
        with closing(sqlite3.connect(database_path.as_uri() + "?mode=ro", uri=True)) as database:
            require(database.execute("PRAGMA user_version").fetchone() == (8,), "Demo used a different schema")
            identity = database.execute("SELECT database_id FROM database_identity WHERE singleton=1").fetchone()[0]
            rows = database.execute("SELECT request_key,operation_id,session_id,method,state,task_id,completion FROM durable_requests").fetchall()
            require(len(rows) == 1, "Demo did not create exactly one durable request")
            row = rows[0]
            require(row[3:5] == ("tasks/start", "finished"), "Demo request is not durably finished")
            require(database.execute("SELECT count(*) FROM tasks").fetchone() == (1,), "Unexpected task count")
            require(database.execute("SELECT count(*) FROM actions").fetchone() == (2,), "Unexpected action count")
        second, retained, _ = query(agent, [("requests/get", {"database_id": identity, "request_key": row[0]})])
        require(second["session_id"] != row[2] and second["database_id"] == identity, "Lookup did not use a new service session and the original database")
        require((retained["request_key"], retained["operation_id"], retained["session_id"], retained["method"], retained["state"], retained["task_id"]) == row[:6],
                "Restart query did not return the original durable receipt")
        require(retained["completion"] == json.loads(row[6]), "Restart query completion differs from the stored receipt")
        require(retained["completion"]["exit_code"] == 0, "Offline durable completion failed")
        report["restart_receipt"] = {key: retained[key] for key in ("database_id", "request_key", "operation_id", "session_id", "method", "state", "task_id")}
        report["restart_receipt"]["completion_sha256"] = hashlib.sha256(row[6].encode()).hexdigest()
        report["checks"].append("Installed offline demo persisted one schema 8 request and two actions; a new service returned its original completion")
        for command in ("serve", "history", "demo"):
            rejected = run("schema7-" + command, [agent, command, "--workspace", old_workspace, "--state", old_database], expected=2)
            require(b"Unsupported state schema" in rejected.stdout + rejected.stderr, "Old schema was not explicitly rejected")
            if command == "serve":
                require(rejected.stdout == b"", "Old schema service emitted protocol output before refusal")
            preserved_at("after_schema7_" + command + "_refusal")
        require(not (old_workspace / "CODESTUDIO_DEMO.txt").exists(), "Rejected old-state operation wrote a demo output")
        require(not Path(str(old_database) + "-wal").exists() and not Path(str(old_database) + "-shm").exists(), "Old database journal settings were modified")
        current_state = {"database": digest(database_path), "demo_file": digest(target)}
        upgrade = manage("upgrade")
        require(upgrade["status"] == "unchanged", "Same-package upgrade should be idempotent")
        preserved_at("after_same_package_upgrade")
        require({"database": digest(database_path), "demo_file": digest(target)} == current_state, "Idempotent upgrade changed current state")
        report["upgrade_scope"] = "Same-package idempotence only; distinct-package upgrade is covered by the separate delivery suite"
        require(manage("uninstall")["status"] == "uninstalled" and not prefix.exists(), "Owned installation was not removed")
        preserved_at("after_uninstall")
        require({"database": digest(database_path), "demo_file": digest(target)} == current_state, "Uninstall changed current state")
        report["current_state_preserved_sha256"] = current_state
        report["checks"].append("Schema 7 refusal and install/idempotent-upgrade/uninstall preserved old and current synthetic user state")
        require(verifier.delivery_fingerprint(package) == initial_package, "Package changed during the research run")
        require(verifier.source_fingerprint() == source, "Tested source changed during the research run")
        report["package_unchanged"] = True
        # Only this invocation's generated fixture root is removed. The package,
        # receipts, previous evidence and any real user paths are retained.
        shutil.rmtree(temporary)
        report["cleanup"] = "owned_installation_uninstalled_then_owned_synthetic_fixture_root_removed"
        report["status"] = "succeeded"
    except BaseException as error:
        report["error"] = str(error)
        report["cleanup"] = "failed_fixture_retained_for_inspection"
        raise
    finally:
        report["finished_at"] = now()
        (receipt_dir / "verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "receipt": str(receipt_dir / "verification.json"),
                      "package_sha256": verified["package_sha256"], "checks": len(report["checks"])}))


if __name__ == "__main__":
    main()
