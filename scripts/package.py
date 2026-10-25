#!/usr/bin/env python3
"""Package an already built native Release binary without building or publishing.

Python 3.10+ is used only by release/installation tooling. The C++ Agent does
not require Python. Only an empty output or an intact output from this tool may
be replaced; repository data and previous milestone receipts are never touched.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import uuid
import zipfile


PRODUCT = "CodeStudio-Agent"
FORMAT = 2
FILE_LIMIT = 256 * 1024 * 1024
TOTAL_LIMIT = 512 * 1024 * 1024
COUNT_LIMIT = 512
OUTPUT_MARKER = ".codestudio-package-output.json"
ROOT = Path(__file__).resolve().parents[1]
PATH_COMPONENT = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]*\Z")
RESERVED = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)), *(f"lpt{i}" for i in range(1, 10))}


class PackageError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise PackageError(message)


def encoded(value):
    return (json.dumps(value, indent=2, ensure_ascii=True, sort_keys=True) + "\n").encode("utf-8")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def native_target():
    require(platform.machine().lower() in ("amd64", "x86_64"), "Only native x86-64 packages are supported")
    if os.name == "nt":
        return "windows", "x64", ".exe"
    require(sys.platform == "linux", "Packaging is supported only on Windows and Linux")
    return "linux", "x86_64", ""


def safe_name(value):
    require(isinstance(value, str) and 0 < len(value) <= 240, "Invalid package path")
    parts = value.split("/")
    require(all(PATH_COMPONENT.fullmatch(part) and not part.endswith(".") and
                part.split(".", 1)[0].lower() not in RESERVED for part in parts), "Unsafe package path")
    require(str(PurePosixPath(value)) == value, "Noncanonical package path")
    return value


def linked(info):
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def checked_path(path):
    path = Path(os.path.abspath(path))
    for parent in reversed((path, *path.parents)):
        try:
            info = parent.lstat()
        except FileNotFoundError:
            continue
        require(not linked(info), "Symlink or reparse-point path is not allowed")
    return path


def read_file(path, maximum=FILE_LIMIT):
    checked_path(path)
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "Package sources must be ordinary, unlinked files")
    require(info.st_size <= maximum, "Package file exceeds size limit")
    with path.open("rb") as source:
        data = source.read(maximum + 1)
        after = os.fstat(source.fileno())
    require(len(data) == info.st_size and (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) ==
            (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns), "Package source changed while reading")
    return data


def tree_files(root):
    checked_path(root)
    require(root.is_dir(), "Package output is not a directory")
    found = {}
    for directory, folders, files in os.walk(root, followlinks=False):
        for name in folders:
            info = (Path(directory) / name).lstat()
            require(stat.S_ISDIR(info.st_mode) and not linked(info), "Unexpected linked package directory")
        for name in files:
            path = Path(directory) / name
            relative = path.relative_to(root).as_posix()
            if relative != OUTPUT_MARKER:
                safe_name(relative)
            found[relative] = read_file(path)
            require(len(found) <= COUNT_LIMIT + 4 and sum(map(len, found.values())) <= TOTAL_LIMIT * 3,
                    "Package output exceeds limits")
    return found


def remove_owned_tree(path, parent):
    """Only for a unique staging/backup path whose resolved parent is known."""
    path = checked_path(path)
    require(path.parent == checked_path(parent) and path.name.startswith(".codestudio-package-"),
            "Refusing cleanup outside owned package staging directory")
    tree_files(path)
    # Do not follow links, including a link swapped in after the tree check.
    for directory, folders, files in os.walk(path, topdown=False, followlinks=False):
        for name in files:
            candidate = Path(directory) / name
            info = candidate.lstat()
            require(stat.S_ISREG(info.st_mode) and not linked(info), "Staging file changed during cleanup")
            candidate.unlink()
        for name in folders:
            candidate = Path(directory) / name
            require(not linked(candidate.lstat()), "Staging directory changed during cleanup")
            candidate.rmdir()
    path.rmdir()


@contextmanager
def output_lock(output):
    output.parent.mkdir(parents=True, exist_ok=True)
    checked_path(output.parent)
    lock = output.with_name("." + output.name + ".codestudio-package.lock")
    try:
        descriptor = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        raise PackageError("Another packaging operation or interrupted output lock requires inspection") from None
    token = uuid.uuid4().hex.encode("ascii")
    cleanup = {}
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(token)
            stream.flush()
            os.fsync(stream.fileno())
        yield cleanup
    finally:
        try:
            if lock.exists():
                if read_file(lock, 128) == token:
                    lock.unlink()
                else:
                    cleanup.update(lock_cleanup="modified_lock_retained", lock_path=str(lock))
        except (OSError, PackageError):
            cleanup.update(lock_cleanup="lock_cleanup_incomplete", lock_path=str(lock))


def command(argv):
    try:
        result = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise PackageError("Required build metadata command could not complete") from error
    require(result.returncode == 0 and len(result.stdout) <= 1024 * 1024,
            "Required build metadata command failed")
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", result.stdout.decode("utf-8", errors="replace")).strip()


def dependency_metadata(binary, target):
    if target == "linux":
        # The source is our already validated native build, never an arbitrary
        # downloaded ELF. ldd is not suitable for inspecting untrusted binaries.
        listing = command(["ldd", str(binary)])
        require("not found" not in listing, "A native runtime dependency is missing")
        listing = re.sub(r"\s+\(0x[0-9a-fA-F]+\)", "", listing)
        libraries = sorted({line.strip() for line in listing.splitlines() if line.strip()})
        return {"inspection": "ldd", "libraries": libraries,
                "notes": ["Distribution runtime libraries are required and are not bundled.",
                          "This build-host resolution is not proof of compatibility with another distribution.",
                          "Sandbox execution additionally needs bubblewrap and delegated cgroup v2 controllers.",
                          "System credentials need an existing accessible Secret Service; the package does not create one."]}
    inspect = shutil.which("llvm-readobj")
    if inspect:
        listing = command([inspect, "--coff-imports", str(binary)])
        libraries = sorted(set(re.findall(r"^\s*Name:\s*(\S+\.dll)\s*$", listing, re.MULTILINE | re.IGNORECASE)), key=str.casefold)
        backend = "llvm-readobj --coff-imports"
    else:
        inspect = shutil.which("dumpbin")
        require(inspect is not None, "Windows packaging requires llvm-readobj or dumpbin for runtime dependency inspection")
        listing = command([inspect, "/dependents", str(binary)])
        libraries = sorted(set(re.findall(r"^\s*(\S+\.dll)\s*$", listing, re.MULTILINE | re.IGNORECASE)), key=str.casefold)
        backend = "dumpbin /dependents"
    require(libraries, "Cannot determine Windows runtime dependencies")
    return {"inspection": backend, "libraries": libraries,
            "notes": ["Windows system/runtime DLLs are not bundled.",
                      "If VCRUNTIME or MSVCP imports are listed, install the matching Microsoft x64 Visual C++ runtime.",
                      "Current-user credentials use Windows Credential Manager; installation does not create credentials."]}


def validate_verification(data, target, arch, binary_data):
    try:
        report = json.loads(data)
    except (ValueError, UnicodeError) as error:
        raise PackageError("Invalid verification report JSON") from error
    require(isinstance(report, dict) and report.get("format") == FORMAT and report.get("product") == PRODUCT and
            report.get("platform") == target and report.get("arch") == arch and report.get("mode") == "release" and
            report.get("status") == "succeeded", "Verification report does not describe a successful native Release verification")
    artifact = report.get("artifact")
    require(isinstance(artifact, dict) and artifact.get("sha256") == digest(binary_data) and
            artifact.get("size") == len(binary_data), "Verification report does not match the packaged binary")
    require(isinstance(report.get("selected_suites"), list) and report["selected_suites"] and
            all(isinstance(value, str) for value in report["selected_suites"]), "Verification report has no declared suite scope")
    require(isinstance(report.get("omitted_suites"), list), "Verification report has no omitted suite scope")
    return {"state": "supplied", "sha256": digest(data), "selected_suites": report["selected_suites"],
            "omitted_suites": report["omitted_suites"]}


def adapted_document(data, relative, shipped):
    """Keep bundle links working without pretending to ship source evidence."""
    text = data.decode("utf-8")
    parent = PurePosixPath(relative).parent

    def replace(match):
        label, destination = match.group(1), match.group(2)
        destination = destination.strip()
        if destination.startswith(("#", "http://", "https://", "mailto:")):
            return match.group(0)
        target = destination.split("#", 1)[0]
        components = []
        for item in (parent / target).parts:
            if item == "..":
                if not components:
                    return f"{label} (source repository: `{destination}`)"
                components.pop()
            elif item != ".":
                components.append(item)
        normalized = "/".join(components)
        if normalized in shipped or any(path.startswith(normalized.rstrip("/") + "/") for path in shipped):
            return match.group(0)
        return f"{label} (source repository: `{destination}`)"

    return re.sub(r"(?<!!)\[([^\]]+)\]\(([^)]+)\)", replace, text).encode("utf-8")


def write_payload(root, contents):
    folded = set()
    require(1 <= len(contents) <= COUNT_LIMIT, "Package contains too many files")
    require(sum(len(data) for data, _ in contents.values()) <= TOTAL_LIMIT, "Package exceeds total size limit")
    for name, (data, executable) in sorted(contents.items()):
        safe_name(name)
        require(name.casefold() not in folded and len(data) <= FILE_LIMIT, "Package path collision or oversized file")
        folded.add(name.casefold())
        destination = root / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("xb") as output:
            output.write(data)
        destination.chmod(0o755 if executable else 0o644)


def make_archive(path, contents, target):
    if target == "windows":
        with zipfile.ZipFile(path, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, (data, executable) in sorted(contents.items()):
                item = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                item.create_system = 3
                item.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
                item.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(item, data, compresslevel=9)
        with zipfile.ZipFile(path) as archive:
            require(set(archive.namelist()) == set(contents), "Archive file set mismatch")
            for name, (data, _) in contents.items():
                require(archive.read(name) == data, "Archive content mismatch")
    else:
        with path.open("xb") as raw:
            with gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0, compresslevel=9) as compressed:
                with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as archive:
                    import io
                    for name, (data, executable) in sorted(contents.items()):
                        item = tarfile.TarInfo(name)
                        item.size, item.mode = len(data), 0o755 if executable else 0o644
                        item.mtime, item.uid, item.gid = 0, 0, 0
                        item.uname = item.gname = ""
                        archive.addfile(item, io.BytesIO(data))
        with tarfile.open(path, "r:gz") as archive:
            require(set(archive.getnames()) == set(contents), "Archive file set mismatch")
            for name, (data, executable) in contents.items():
                item = archive.getmember(name)
                require(item.isfile() and item.mode == (0o755 if executable else 0o644) and
                        archive.extractfile(item).read() == data, "Archive content or mode mismatch")


def output_records(files):
    return [{"path": name, "sha256": digest(data), "size": len(data)} for name, data in sorted(files.items())]


def validate_output(path, package_name, archive_name):
    if not path.exists():
        return
    files = tree_files(path)
    if not files and not any(path.iterdir()):
        return
    require(OUTPUT_MARKER in files, "Output directory is not owned by this packaging tool")
    try:
        marker = json.loads(files.pop(OUTPUT_MARKER))
    except (ValueError, UnicodeError) as error:
        raise PackageError("Invalid package output ownership record") from error
    require(marker == {"format": FORMAT, "product": PRODUCT, "package": package_name,
                       "archive": archive_name, "files": output_records(files)},
            "Package output was modified or has an incompatible ownership record")
    require(files and archive_name in files and all(name == archive_name or name.startswith(package_name + "/")
                                                  for name in files), "Unexpected file in package output")
    expected_dirs = {"."}
    for name in files:
        expected_dirs.update(str(parent) for parent in PurePosixPath(name).parents)
    for directory, folders, _ in os.walk(path):
        for folder in folders:
            require((Path(directory) / folder).relative_to(path).as_posix() in expected_dirs,
                    "Unexpected directory in package output")


def package_locked(args):
    target, arch, suffix = native_target()
    output = checked_path(args.output or ROOT / "build" / "packages" / f"{target}-{arch}")
    require(output not in (ROOT, Path.home(), Path(output.anchor)) and output not in ROOT.parents,
            "Unsafe package output directory")
    package_name = f"codestudio-agent-{target}-{arch}"
    archive_name = package_name + (".zip" if target == "windows" else ".tar.gz")
    validate_output(output, package_name, archive_name)
    binary = ROOT / "build" / target / arch / "release" / ("codestudio-agent" + suffix)
    binary_data = read_file(binary)
    if target == "windows":
        require(binary_data.startswith(b"MZ"), "Release binary is not a Windows executable")
        require(len(binary_data) >= 64, "Release binary has a truncated Windows header")
        pe = int.from_bytes(binary_data[60:64], "little")
        require(binary_data[pe:pe + 4] == b"PE\0\0" and binary_data[pe + 4:pe + 6] == b"\x64\x86",
                "Release binary is not a Windows x86-64 executable")
    else:
        require(binary_data[:6] == b"\x7fELF\x02\x01" and binary_data[18:20] == b"\x3e\x00",
                "Release binary is not a Linux x86-64 executable")
    xmake_source = read_file(ROOT / "xmake.lua").decode("utf-8")
    version_match = re.search(r'^set_version\("(\d+\.\d+\.\d+)"\)', xmake_source, re.MULTILINE)
    require(version_match is not None, "Cannot read project version")
    version = version_match[1]
    require(command([str(binary), "version"]) == f"{PRODUCT} {version}", "Release binary version does not match project")
    try:
        doctor = json.loads(command([str(binary), "doctor"]))
    except ValueError as error:
        raise PackageError("Release binary has invalid diagnostics") from error
    require(doctor.get("product") == PRODUCT and doctor.get("version") == version and
            isinstance(doctor.get("compiler"), str), "Release binary identity mismatch")
    xmake = command(["xmake", "--version"])
    xmake_match = re.search(r"\bxmake v?(\d+\.\d+\.\d+[^\s,]*)", xmake, re.IGNORECASE)
    require(xmake_match is not None, "Cannot determine XMake version")
    dependencies = dependency_metadata(binary, target)
    metadata = {"format": FORMAT, "product": PRODUCT, "version": version, "platform": target, "arch": arch,
                "mode": "release", "language": "c++26", "compiler": doctor["compiler"], "xmake": xmake_match[1],
                "binary_sha256": digest(binary_data), "dependencies": dependencies,
                "verification": {"state": "not_supplied"}}
    contents = {binary.name: (binary_data, True)}
    for name in ("install.py", "install.ps1", "install.sh"):
        contents[name] = (read_file(ROOT / "scripts" / name), name == "install.sh")
    files = [ROOT / "README.md", *sorted((ROOT / "docs").glob("*.md")),
             *sorted((ROOT / "protocol").glob("*.md")), ROOT / "protocol/task-service.schema.json",
             *sorted((ROOT / "examples").glob("*.py")), *sorted((ROOT / "examples").glob("*.json"))]
    require((ROOT / "docs/delivery.md") in files, "Delivery documentation is missing")
    for source in files:
        contents[source.relative_to(ROOT).as_posix()] = (read_file(source), False)
    if args.verification:
        report = read_file(checked_path(args.verification), 4 * 1024 * 1024)
        metadata["verification"] = validate_verification(report, target, arch, binary_data)
        contents["verification.json"] = (report, False)
    else:
        print("No verification report supplied; this package makes no test-pass claim.", file=sys.stderr)
    dependency_text = "Native runtime dependencies\n\n" + "\n".join(dependencies["libraries"]) + "\n\n" + "\n".join(dependencies["notes"]) + "\n"
    contents["runtime-dependencies.txt"] = (dependency_text.encode(), False)
    contents["build-metadata.json"] = (encoded(metadata), False)
    readme = f"""{PRODUCT} {version} - {target} {arch}

This native C++26 Release package does not install or update system components.
Read docs/delivery.md before using the user-level install/upgrade/uninstall tool.
The Agent binary has no Python dependency; the installation tool requires Python 3.10+.

  python install.py install --package .
  python install.py status
  python install.py upgrade --package /path/to/new/extracted/package
  python install.py uninstall

An explicit --prefix selects a different application directory. The default is
LOCALAPPDATA/Programs/CodeStudio-Agent on Windows and ~/.local/share/codestudio-agent
on Linux. Installation never changes PATH, startup registration, workspaces or
credential stores. Use the installed executable by its full path; enable startup
separately only after checking docs/scheduler.md. Stop running instances before
upgrading or uninstalling. Unknown or modified installation files are preserved
by refusing the operation, not by deleting them.

package.json and SHA256SUMS cover the payload. They detect corruption, not a
malicious publisher: obtain this unsigned package from a trusted source.
No historical state conversion or database migration is included.

Runtime libraries are listed in runtime-dependencies.txt. Linux binary compatibility
depends on its build distribution and library versions; this is not a universal
Linux executable. Build details and supplied test scope are in build-metadata.json.
If verification.json is absent, no verification receipt was supplied for this package.
No packaging result establishes real login/reboot or clean-machine validation.

Bundled Markdown documents retain links to bundled files. References to source-only
test fixtures and research receipts are labeled 'source repository' and need the
source checkout. MCP configuration is in docs/mcp.md; credential conditions are in
docs/credentials.md. No secrets or existing task state are included.
"""
    contents["README.txt"] = (readme.encode(), False)
    for name, (data, executable) in list(contents.items()):
        if name.endswith(".md"):
            contents[name] = (adapted_document(data, name, contents), executable)
    manifest = {"format": FORMAT, "product": PRODUCT, "version": version, "platform": target, "arch": arch,
                "files": [{"path": name, "sha256": digest(data), "size": len(data), "executable": executable}
                          for name, (data, executable) in sorted(contents.items())]}
    contents["package.json"] = (encoded(manifest), False)
    sums = "".join(f"{digest(data)}  {name}\n" for name, (data, _) in sorted(contents.items()))
    contents["SHA256SUMS"] = (sums.encode(), False)
    output.parent.mkdir(parents=True, exist_ok=True)
    checked_path(output.parent)
    stage = Path(tempfile.mkdtemp(prefix=".codestudio-package-stage-", dir=output.parent))
    backup = output.parent / (".codestudio-package-backup-" + uuid.uuid4().hex)
    published = False
    retained = None
    try:
        tree = stage / package_name
        tree.mkdir()
        write_payload(tree, contents)
        make_archive(stage / archive_name, contents, target)
        record = {"format": FORMAT, "product": PRODUCT, "package": package_name,
                  "archive": archive_name, "files": output_records(tree_files(stage))}
        (stage / OUTPUT_MARKER).write_bytes(encoded(record))
        validate_output(stage, package_name, archive_name)
        validate_output(output, package_name, archive_name)
        if output.exists():
            output.rename(backup)
        try:
            stage.rename(output)
            published = True
        except OSError:
            if backup.exists():
                backup.rename(output)
            raise
        if backup.exists():
            try:
                remove_owned_tree(backup, output.parent)
            except (OSError, PackageError):
                # The new package is already published. Do not label it a
                # failed publication or pretend a partly cleaned backup is a
                # complete rollback copy.
                retained = str(backup)
    finally:
        if not published and stage.exists():
            remove_owned_tree(stage, output.parent)
    archive = output / archive_name
    result = {"format": FORMAT, "product": PRODUCT, "status": "packaged", "platform": target, "arch": arch,
              "package": str(output / package_name), "archive": str(archive),
              "sha256": digest(read_file(archive)), "bytes": archive.stat().st_size,
              "verification": metadata["verification"]}
    if retained:
        result.update(cleanup="previous_output_cleanup_incomplete", retained_path=retained)
    return result


def package(args):
    target, arch, _ = native_target()
    output = checked_path(args.output or ROOT / "build" / "packages" / f"{target}-{arch}")
    require(output not in (ROOT, Path.home(), Path(output.anchor)) and output not in ROOT.parents,
            "Unsafe package output directory")
    with output_lock(output) as cleanup:
        result = package_locked(args)
    result.update(cleanup)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("release",), default="release")
    parser.add_argument("--output", type=Path, help="Owned output directory, not the extracted package directory")
    parser.add_argument("--verification", type=Path, help="Successful matching native Release verification.json")
    args = parser.parse_args(argv)
    try:
        print(json.dumps(package(args), indent=2))
        return 0
    except (PackageError, OSError, ValueError) as error:
        message = str(error) if isinstance(error, PackageError) else "Package filesystem or metadata operation failed"
        print(json.dumps({"product": PRODUCT, "status": "failed", "error": message}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
