"""Real XMake -> clang-cl -> lld-link -> freshly built program inside LPAC.

Only installed tools are copied. No downloads, host compiler executions, or
fallback are used. Run with CLI path and optional xmake/clang-cl executable paths.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
from contextlib import closing


def main():
    if os.name != "nt":
        print("SKIP Windows isolated XMake integration")
        return
    cli = Path(sys.argv[1]).resolve()
    xmake = Path(sys.argv[2] if len(sys.argv) > 2 else shutil.which("xmake")).resolve()
    llvm = Path(sys.argv[3] if len(sys.argv) > 3 else shutil.which("clang-cl")).resolve()
    project = Path(__file__).resolve().parent / "fixtures" / "xmake-build"
    for variant in ("explicit-v2", "custom-cache-v2"):
        verify_build(cli, xmake, llvm, project, variant)
    print("PASS 2 real isolated XMake builds: explicit-v2 and custom-cache-v2")


def verify_build(cli, xmake, llvm, project, variant):
    with tempfile.TemporaryDirectory(prefix=f"codestudio-xmake-{variant}-") as temporary:
        root = Path(temporary).resolve()
        for filename in ("xmake.lua", "main.cpp"):
            shutil.copyfile(project / filename, root / filename)
        source_before = {p.name: p.read_bytes() for p in root.iterdir()}
        config = json.loads((project / "build.windows.json").read_text(encoding="utf-8"))
        tool_directories = {"@XMAKE_DIRECTORY@": str(xmake.parent),
                            "@LLVM_BIN_DIRECTORY@": str(llvm.parent)}
        for mount in config["mounts"]:
            mount["source"] = tool_directories[mount["source"]]
        assert config["version"] == 2
        if variant == "custom-cache-v2":
            # Both variants carry every tool-specific setting explicitly.
            config["environment"]["XMAKE_PKG_INSTALLDIR"] = "${home}/custom-packages"
        manifest = root / "build.json"
        manifest.write_text(json.dumps(config), encoding="utf-8")
        command = [str(cli), "build", "--workspace", str(root), "--build-config", str(manifest)]
        run = subprocess.run(command, text=True, encoding="utf-8", errors="replace", capture_output=True, timeout=150)
        if run.stderr:
            print(run.stderr, file=sys.stderr)
        response = json.loads(run.stdout)
        assert "task_id" in response, response
        show = subprocess.run([str(cli), "show", response["task_id"], "--workspace", str(root)],
                              text=True, encoding="utf-8", capture_output=True, check=True)
        task = json.loads(show.stdout)
        action = task["actions"][0]
        output = action["result"]["output"]
        if run.returncode != 0:
            print(json.dumps(action["result"], ensure_ascii=False, indent=2))
        assert run.returncode == 0, action["result"]
        assert response["status"] == "completed"
        assert len(task["actions"]) == 1
        assert [step["exit_code"] for step in output["steps"]] == [0, 0, 0]
        assert all((root / name).read_bytes() == value for name, value in source_before.items())
        assert not (root / "out").exists() and not (root / "build").exists()
        artifact = output["artifacts"]
        assert artifact["file_count"] == 1
        entry = artifact["files"][0]
        data = (Path(artifact["directory"]) / entry["artifact_path"]).read_bytes()
        assert data[:2] == b"MZ"
        assert hashlib.sha256(data).hexdigest() == entry["sha256"]
        with closing(sqlite3.connect(root / ".codestudio" / "state.sqlite3")) as database:
            binding = json.loads(database.execute("SELECT binding FROM task_checkpoints WHERE task_id=?",
                                                  (response["task_id"],)).fetchone()[0])["configuration"]["build"]
        assert binding["version"] == 2
        assert "plugin" not in binding
        expanded = binding["recipe"]
        assert expanded["version"] == 2 and "plugin" not in expanded
        assert expanded["mounts"] == config["mounts"]
        assert expanded["inputs"] == config["inputs"]
        assert expanded["outputs"] == config["outputs"]
        assert expanded["artifact_directory"] == config["artifact_directory"]
        assert expanded["limits"] == config["limits"]
        assert len(expanded["steps"]) == 3
        assert expanded["steps"] == config["steps"]
        assert expanded["environment"] == config["environment"]
        assert expanded["environment"]["XMAKE_PROGRAM_DIR"] == "${xmake}"
        assert expanded["environment"]["XMAKE_GLOBALDIR"] == "${home}/.xmake"
        expected_cache = "custom-packages" if variant == "custom-cache-v2" else "packages"
        assert expanded["environment"]["XMAKE_PKG_INSTALLDIR"] == "${home}/" + expected_cache
        print(f"PASS real isolated XMake / {variant} / C++26 pack indexing / compile / link / execute / atomic export")
        print(f"  steps=3, bytes={len(data)}, sha256={entry['sha256']}")


if __name__ == "__main__":
    main()
