"""Inspect and relocate this project's native macOS arm64 runtime copies.

No system library is copied, and no source binary is modified. Ad-hoc signatures
only make the relocated development payload runnable; they identify no publisher.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import struct
import tempfile


RUNTIMES = ("libc++.1.dylib", "libc++abi.1.dylib")
DEPLOYMENT = "15.0"
DYLIB_COMMANDS = {0xC, 0x80000018, 0x8000001F, 0x20, 0x80000023}


class BundleError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise BundleError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def version(value):
    return value >> 16, (value >> 8) & 255, value & 255


def version_text(value):
    major, minor, patch = version(value)
    return f"{major}.{minor}" + (f".{patch}" if patch else "")


def inspect_macho(data, filetype=None):
    """Read bounded Mach-O load commands; accept one native arm64 slice only."""
    require(len(data) >= 32, "Truncated macOS Mach-O header")
    magic, cpu, subtype, kind, count, size, flags, reserved = struct.unpack_from("<8I", data)
    require(magic == 0xFEEDFACF and cpu == 0x0100000C and subtype == 0,
            "macOS payload must be a thin native arm64 Mach-O image")
    require(kind in (2, 6) and (filetype is None or kind == filetype), "Unexpected Mach-O image type")
    require(0 < count <= 4096 and 32 + size <= len(data), "Invalid Mach-O load-command table")
    dependencies, rpaths, install_id, minimum = [], [], None, None
    cursor = 32
    for _ in range(count):
        require(cursor + 8 <= 32 + size, "Truncated Mach-O load command")
        command, length = struct.unpack_from("<2I", data, cursor)
        require(length >= 8 and length % 8 == 0 and cursor + length <= 32 + size,
                "Invalid Mach-O load-command length")
        body = data[cursor:cursor + length]
        if command in DYLIB_COMMANDS | {0xD, 0x8000001C}:
            require(length >= (24 if command != 0x8000001C else 16), "Truncated Mach-O path command")
            offset, = struct.unpack_from("<I", body, 8)
            require((24 if command != 0x8000001C else 12) <= offset < length, "Invalid Mach-O path offset")
            name, separator, _ = body[offset:].partition(b"\0")
            require(separator and name, "Unterminated Mach-O dependency path")
            try:
                text = name.decode("utf-8")
            except UnicodeError as error:
                raise BundleError("Invalid Mach-O dependency encoding") from error
            require("\n" not in text and "\r" not in text, "Invalid Mach-O dependency path")
            if command == 0x8000001C:
                rpaths.append(text)
            elif command == 0xD:
                require(install_id is None, "Repeated Mach-O install name")
                install_id = text
            else:
                dependencies.append(text)
        elif command == 0x32:
            require(length >= 24 and minimum is None, "Invalid Mach-O deployment declaration")
            platform_id, minimum, sdk, tools = struct.unpack_from("<4I", body, 8)
            require(platform_id == 1 and 24 + tools * 8 <= length,
                    "Mach-O payload must target macOS")
        elif command in (0x24, 0x25, 0x2F, 0x30):
            # All supported builds use LC_BUILD_VERSION. Do not guess the
            # platform or mix an older deployment declaration with it.
            raise BundleError("Legacy or foreign Mach-O deployment declaration")
        elif command == 0x27:
            raise BundleError("Mach-O embedded dynamic-loader environment is not permitted")
        elif command == 0xE:
            require(length >= 16 and struct.unpack_from("<I", body, 8)[0] == 12 and
                    body[12:].split(b"\0", 1)[0] == b"/usr/lib/dyld" and b"\0" in body[12:],
                    "Mach-O names an unexpected dynamic loader")
        cursor += length
    require(cursor == 32 + size and minimum is not None, "Incomplete Mach-O deployment metadata")
    require(len(set(dependencies)) == len(dependencies) and len(set(rpaths)) == len(rpaths),
            "Repeated Mach-O dependency or runtime search path")
    require((kind == 6) == (install_id is not None), "Invalid Mach-O install-name declaration")
    return {"dependencies": dependencies, "rpaths": rpaths, "install_id": install_id,
            "minimum_os": version_text(minimum), "filetype": kind}


def system_dependency(name):
    path = PurePosixPath(name)
    return (name.startswith(("/usr/lib/", "/System/Library/")) and
            str(path) == name and ".." not in path.parts)


def validate_payload(images, deployment=DEPLOYMENT):
    """Validate the complete relocatable Mach-O set, before or after signing."""
    require(set(images) == {"codestudio-agent", *("lib/" + name for name in RUNTIMES)},
            "Incomplete or unexpected macOS runtime payload")
    inspected = {}
    for name, data in images.items():
        info = inspect_macho(data, 2 if name == "codestudio-agent" else 6)
        require(info["minimum_os"] == deployment, "Mach-O minimum macOS version differs from package declaration")
        require(not info["rpaths"], "Packaged Mach-O retains a runtime search path")
        private = [item for item in info["dependencies"] if not system_dependency(item)]
        expected = (["@loader_path/lib/" + item for item in RUNTIMES] if name == "codestudio-agent"
                    else ["@loader_path/libc++abi.1.dylib"] if name == "lib/libc++.1.dylib" else [])
        require(set(private) == set(expected), "Packaged Mach-O has an unapproved runtime dependency")
        if name != "codestudio-agent":
            require(info["install_id"] == "@rpath/" + Path(name).name, "Unexpected packaged dylib install name")
        inspected[name] = info
    return inspected


def runtime_file(root, name, read_file):
    """Resolve only the toolchain's ordinary in-directory version symlinks."""
    library = root / "lib"
    candidate = library / name
    seen = set()
    for _ in range(8):
        require(candidate not in seen, "Cyclic macOS runtime alias")
        seen.add(candidate)
        info = candidate.lstat()
        if not stat.S_ISLNK(info.st_mode):
            return read_file(candidate)
        target = os.readlink(candidate)
        require(target and Path(target).name == target and target not in (".", ".."),
                "macOS runtime alias escapes the runtime directory")
        candidate = library / target
    raise BundleError("Too many macOS runtime aliases")


def prepare(binary_data, runtime_root, read_file, command, temporary_root, report=None):
    """Return payload bytes and provenance while modifying only temporary copies."""
    root = Path(runtime_root)
    require(root.is_absolute(), "macOS runtime root must be absolute")
    # read_file checks every parent for symlinks. Only final version aliases
    # inside this known runtime directory are deliberately resolved above.
    runtimes = {name: runtime_file(root, name, read_file) for name in RUNTIMES}
    original = {"codestudio-agent": binary_data, **{"lib/" + name: data for name, data in runtimes.items()}}
    original_info = {name: inspect_macho(data, 2 if name == "codestudio-agent" else 6)
                     for name, data in original.items()}
    library = str(root / "lib")
    for name, info in original_info.items():
        require(info["minimum_os"] == DEPLOYMENT, "Source runtime minimum macOS version differs from package declaration")
        require(all(item == library for item in info["rpaths"]), "Source Mach-O has an unapproved runtime search path")
        private = [item for item in info["dependencies"] if not system_dependency(item)]
        expected = (["@rpath/" + item for item in RUNTIMES] if name == "codestudio-agent"
                    else ["@rpath/libc++abi.1.dylib"] if name == "lib/libc++.1.dylib" else [])
        require(set(private) == set(expected), "Source Mach-O has an unapproved runtime dependency")
        if name != "codestudio-agent":
            require(info["install_id"] == "@rpath/" + Path(name).name, "Source dylib install name differs from its declared runtime")
    require(original_info["codestudio-agent"]["rpaths"] == [library],
            "Source executable does not resolve the selected runtime directory")
    if report is not None:
        toolchain = report.get("toolchain", {})
        require(isinstance(toolchain, dict) and toolchain.get("deployment_target") == DEPLOYMENT and toolchain.get("libcxx_root") == str(root),
                "Verification report does not bind the selected macOS runtime")
        hashes = toolchain.get("libcxx_sha256", {})
        require(isinstance(hashes, dict) and all(hashes.get(name) == digest(data) for name, data in runtimes.items()),
                "macOS runtime differs from the verified runtime bytes")
    with tempfile.TemporaryDirectory(prefix=".codestudio-macos-relocate-", dir=temporary_root) as temporary:
        work = Path(temporary)
        (work / "lib").mkdir()
        for name, data in original.items():
            path = work / name
            path.write_bytes(data)
            path.chmod(0o755)
            info = original_info[name]
            changes = []
            for dependency in info["dependencies"]:
                if dependency.startswith("@rpath/"):
                    replacement = "@loader_path/" + ("lib/" if name == "codestudio-agent" else "") + dependency[7:]
                    changes += ["-change", dependency, replacement]
            for rpath in info["rpaths"]:
                changes += ["-delete_rpath", rpath]
            if changes:
                command(["/usr/bin/install_name_tool", *changes, str(path)])
        for name in [*("lib/" + item for item in RUNTIMES), "codestudio-agent"]:
            path = work / name
            identifier = "com.codestudio.agent" if name == "codestudio-agent" else "com.codestudio.agent." + Path(name).name
            command(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", "--identifier", identifier, str(path)])
            command(["/usr/bin/codesign", "--verify", "--strict", str(path)])
        images = {name: read_file(work / name) for name in original}
        inspected = validate_payload(images)
        smoke_version = command([str(work / "codestudio-agent"), "version"])
        try:
            doctor = json.loads(command([str(work / "codestudio-agent"), "doctor"]))
        except ValueError as error:
            raise BundleError("Relocated macOS payload returned invalid diagnostics") from error
        require(isinstance(doctor, dict) and doctor.get("product") == "CodeStudio-Agent" and
                smoke_version == "CodeStudio-Agent " + str(doctor.get("version")) and
                doctor.get("cplusplus", 0) > 202302, "Relocated macOS payload identity check failed")
    metadata = {"deployment_target": DEPLOYMENT, "signature": "ad_hoc_not_publisher_verified",
                "source_runtime_root": str(root), "images": inspected,
                "relocated_smoke": {"version": smoke_version, "product": doctor["product"],
                                    "cplusplus": doctor["cplusplus"], "doctor": doctor},
                "runtime_records": [{"path": "lib/" + name, "source_sha256": digest(data),
                                     "sha256": digest(images["lib/" + name]), "size": len(images["lib/" + name])}
                                    for name, data in runtimes.items()]}
    return images, metadata
