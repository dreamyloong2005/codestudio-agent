"""Validate final delivery evidence and copy the verified Linux release payload.

Run under WSL after native verification and archive-extraction installation tests.
This helper runs no binaries and performs no installation or registration. It
never deletes output files. Existing unrelated delivery content is a refusal.
"""
from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tarfile
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = ROOT / "research/resource-lifecycle"
MIRROR = Path("/var/tmp/codestudio-agent-linux")
SOURCE_DIRS = ("src", "include", "scripts", "tests", "docs", "protocol", "examples", ".github")
SOURCE_FILES = ("xmake.lua", "xmake-requires.lock", "README.md")
LIMIT = 512 * 1024 * 1024
PRODUCT = "CodeStudio-Agent"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def linked(info):
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def checked_chain(path):
    path = Path(os.path.abspath(path))
    for component in reversed((path, *path.parents)):
        try:
            info = component.lstat()
        except FileNotFoundError:
            continue
        require(not linked(info), "Linked or reparse-point delivery path")
    return path


def read(path, maximum=LIMIT):
    path = checked_chain(path)
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_size <= maximum,
            "Expected a bounded ordinary single-link delivery file")
    with path.open("rb") as stream:
        value = stream.read(maximum + 1)
        after = os.fstat(stream.fileno())
    require(len(value) == info.st_size and
            (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns) ==
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), "Delivery file changed while reading")
    return value


def unique_object(pairs):
    result = {}
    for name, value in pairs:
        require(name not in result, "Duplicate JSON property in delivery metadata")
        result[name] = value
    return result


def parse(data):
    return json.loads(data.decode("utf-8"), object_pairs_hook=unique_object)


def document(path):
    return parse(read(path, 8 * 1024 * 1024))


def relative_name(name):
    require(isinstance(name, str) and 0 < len(name) <= 240 and "\\" not in name and ":" not in name,
            "Invalid delivery path")
    path = PurePosixPath(name)
    require(not path.is_absolute() and path.as_posix() == name and
            all(re.fullmatch(r"[A-Za-z0-9_.-]+", part) and part not in (".", "..") and not part.endswith(".")
                for part in path.parts), "Invalid delivery path")
    return name


def receipt_path(value):
    # Root's archive check uses paths relative to research/resource-lifecycle. The summary
    # can be written by Windows, where pathlib serializes separators as '\\'.
    require(isinstance(value, str), "Invalid verification receipt path")
    normalized = value.replace("\\", "/")
    if normalized.startswith("research/resource-lifecycle/"):
        normalized = normalized[len("research/resource-lifecycle/"):]
    if normalized.startswith("/"):
        path = checked_chain(Path(normalized))
        require(path.is_relative_to(EVIDENCE), "Receipt must be inside this delivery evidence directory")
        return path
    relative_name(normalized)
    return EVIDENCE / normalized


def file_inventory(directory):
    directory = checked_chain(directory)
    require(directory.is_dir(), "Expected delivery directory")
    result, directories = {}, set()
    for parent, folders, names in os.walk(directory, followlinks=False):
        for name in folders:
            path = Path(parent) / name
            require(not linked(path.lstat()), "Linked delivery directory")
            directories.add(path.relative_to(directory).as_posix())
        for name in names:
            path = Path(parent) / name
            relative = relative_name(path.relative_to(directory).as_posix())
            result[relative] = read(path)
            require(len(result) <= 1024 and sum(map(len, result.values())) <= LIMIT * 3,
                    "Delivery inventory exceeds size limit")
    return result, directories


def source_inventory(root):
    paths = [root / name for name in SOURCE_FILES]
    for name in SOURCE_DIRS:
        directory = root / name
        require(directory.is_dir(), "Missing source directory in delivery snapshot")
        for path in directory.rglob("*"):
            if "__pycache__" in path.parts or path.suffix == ".pyc":
                continue
            require(not path.is_symlink(), "Source snapshot contains a symbolic link")
            if path.is_file():
                paths.append(path)
    records = {path.relative_to(root).as_posix(): sha(read(path)) for path in paths}
    require(len(records) == len(paths), "Repeated file in source snapshot")
    state = hashlib.sha256()
    for name, digest in sorted(records.items()):
        state.update(name.encode() + b"\0" + digest.encode() + b"\n")
    return records, {"sha256": state.hexdigest(), "files": len(records)}


def successful_reports(directory, mode, full):
    result = []
    for path in directory.glob(f"{mode}-*/verification.json"):
        report = document(path)
        if report.get("status") != "succeeded":
            continue
        if full and (report.get("build_requested") is not True or report.get("omitted_suites") != ["delivery"]):
            continue
        if not full and report.get("selected_suites") != ["delivery"]:
            continue
        result.append((datetime.fromisoformat(report["finished_at"]), path, report))
    require(result, "No successful matching verification receipt")
    return max(result, key=lambda item: item[0])[1:]


def validate_report(report, platform, mode, fingerprint):
    arch = "x64" if platform == "windows" else "x86_64"
    require(report.get("format") == 2 and report.get("product") == PRODUCT and
            report.get("platform") == platform and report.get("arch") == arch and
            report.get("mode") == mode and report.get("status") == "succeeded",
            "Verification identity or status differs from delivery target")
    require(report.get("source") == fingerprint, "Verification source snapshot differs from final source")
    require(report.get("steps") and all(step.get("status") == "succeeded" and step.get("returncode") == 0
                                          for step in report["steps"]), "Verification contains an unsuccessful step")
    if report.get("build_requested"):
        selected = set(report.get("selected_suites", []))
        required_suites = {"native", "plugins", "mcp_oauth", "plugin_sandbox", "sandbox_resources"}
        if platform == "windows":
            required_suites.add("spawn_lifetime")
        require(required_suites.issubset(selected), "Full verification omitted this milestone's required suites")
        steps = {step["name"]: step for step in report["steps"]}
        require(len(steps) == len(report["steps"]), "Verification repeats a step name")
        require("native" in steps and type(steps["native"].get("native_groups")) is int and
                steps["native"]["native_groups"] > 0, "Full verification has no successful native groups")
        for name in ("plugins", "mcp_oauth", "plugin_sandbox", "sandbox_resources"):
            require(name in steps and type(steps[name].get("cases")) is int and
                    steps[name]["cases"] > 0 and steps[name].get("passed", 0) > 0 and
                    steps[name]["passed"] + steps[name].get("skipped", 0) == steps[name]["cases"],
                    "Full verification has no successful cases for a required suite")
        fixture = "sandbox-resource-fixture" + (".exe" if platform == "windows" else "")
        tested = report.get("tested_artifacts")
        require(isinstance(tested, dict) and fixture in tested, "Full verification omitted the resource fixture artifact")
        for name, digest in tested.items():
            relative_name(name)
            require("/" not in name and isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{64}", digest),
                    "Invalid verified test artifact")
    return arch


def verify_tested_artifacts(report, platform, mode, base):
    """Bind the complete tested binary/fixture set to the final native output."""
    arch = "x64" if platform == "windows" else "x86_64"
    directory = base / "build" / platform / arch / mode
    for name, digest in report["tested_artifacts"].items():
        relative_name(name)
        require("/" not in name and sha(read(directory / name)) == digest,
                "Verified test artifact has changed: " + name)


def verify_linux_credential_cleanup(expected):
    checks = [(path, document(path)) for path in EVIDENCE.glob("credential-cleanup-linux-*.json")]
    matched = {}
    for verification in expected:
        candidates = []
        for path, record in checks:
            if verification not in record.get("verification_receipts", []):
                continue
            require(record.get("format") == 2 and record.get("platform") == "linux" and
                    record.get("private_secret_service") is True and record.get("status") == "succeeded" and
                    record.get("verification_exit_code") == 0 and record.get("namespace_scan") == "completed" and
                    record.get("ordinary_test_entries") == [] and record.get("oauth_test_entries") == [],
                    "Native verification has an unsuccessful private credential cleanup receipt")
            candidates.append((record["finished_at"], path))
        require(candidates, "Native verification lacks a matching in-session private credential cleanup receipt")
        matched[verification] = max(candidates)[1].relative_to(EVIDENCE).as_posix()
    return matched


def validate_package(tree, platform, arch, release_path, release_report):
    payload, directories = file_inventory(tree)
    require("package.json" in payload and "SHA256SUMS" in payload and "verification.json" in payload,
            "Package metadata or embedded verification is missing")
    manifest = parse(payload["package.json"])
    require(set(manifest) == {"format", "product", "version", "platform", "arch", "files"} and
            manifest["format"] == 2 and manifest["product"] == PRODUCT and manifest["platform"] == platform and
            manifest["arch"] == arch, "Package manifest identity mismatch")
    require(re.fullmatch(r"\d+\.\d+\.\d+", manifest["version"]) and
            isinstance(manifest["files"], list) and 1 <= len(manifest["files"]) <= 512, "Invalid package manifest")
    names, folded, modes = set(), set(), {"package.json": 0o644, "SHA256SUMS": 0o644}
    for entry in manifest["files"]:
        require(set(entry) == {"path", "sha256", "size", "executable"}, "Invalid package file record")
        name = relative_name(entry["path"])
        require(name not in ("package.json", "SHA256SUMS") and name.casefold() not in folded,
                "Repeated or reserved package file")
        require(type(entry["size"]) is int and 0 <= entry["size"] <= 256 * 1024 * 1024 and
                type(entry["executable"]) is bool and re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]),
                "Invalid package file metadata")
        require(name in payload and len(payload[name]) == entry["size"] and sha(payload[name]) == entry["sha256"],
                "Package payload differs from manifest")
        names.add(name)
        folded.add(name.casefold())
        modes[name] = 0o755 if entry["executable"] else 0o644
    require(set(payload) == names | {"package.json", "SHA256SUMS"}, "Package contains unlisted or missing files")
    expected_dirs = {parent.as_posix() for name in payload for parent in PurePosixPath(name).parents
                     if parent.as_posix() != "."}
    require(directories == expected_dirs, "Package contains unlisted directories")
    sums = "".join(f"{sha(data)}  {name}\n" for name, data in sorted(payload.items()) if name != "SHA256SUMS")
    require(payload["SHA256SUMS"] == sums.encode(), "Package SHA256SUMS differs from exact payload")
    require(payload["verification.json"] == read(release_path), "Package does not embed the latest full Release receipt")
    binary = "codestudio-agent.exe" if platform == "windows" else "codestudio-agent"
    require(binary in payload and modes[binary] == 0o755, "Package lacks native executable")
    require(sha(payload[binary]) == release_report["artifact"]["sha256"] and
            len(payload[binary]) == release_report["artifact"]["size"], "Packaged binary differs from verified binary")
    if platform == "linux":
        for name, mode in modes.items():
            require(stat.S_IMODE((tree / name).stat().st_mode) == mode, "Native Linux package mode mismatch")
    return payload, modes, manifest


def validate_archive(path, payload, modes, platform):
    contents, found_modes = {}, {}
    if platform == "windows":
        with zipfile.ZipFile(path) as archive:
            for entry in archive.infolist():
                name = relative_name(entry.filename)
                require(name not in contents and not entry.is_dir() and
                        (entry.external_attr >> 16) & 0o170000 == stat.S_IFREG,
                        "ZIP contains a repeated or non-regular entry")
                require(name in payload and entry.file_size == len(payload[name]), "ZIP size or path mismatch")
                require(entry.date_time == (1980, 1, 1, 0, 0, 0), "ZIP timestamp is not normalized")
                contents[name], found_modes[name] = archive.read(entry), (entry.external_attr >> 16) & 0o7777
    else:
        with tarfile.open(path, "r:gz") as archive:
            for entry in archive:
                name = relative_name(entry.name)
                require(name not in contents and entry.isfile() and name in payload and entry.size == len(payload[name]),
                        "Tar contains a repeated, non-regular or mismatched entry")
                require(entry.mtime == 0 and entry.uid == 0 and entry.gid == 0 and not entry.uname and not entry.gname,
                        "Tar ownership or timestamp is not normalized")
                contents[name], found_modes[name] = archive.extractfile(entry).read(), entry.mode
    require(contents == payload and found_modes == modes, "Archive payload or permissions differ from extracted package")
    return sha(read(path))


def validate_output_marker(output, package_name, archive_name, payload):
    require({item.name for item in output.iterdir()} == {package_name, archive_name, ".codestudio-package-output.json"},
            "Packaging output contains unrelated entries")
    entries = {package_name + "/" + name: data for name, data in payload.items()}
    entries[archive_name] = read(output / archive_name)
    expected = {"format": 2, "product": PRODUCT, "package": package_name, "archive": archive_name,
                "files": [{"path": name, "sha256": sha(data), "size": len(data)} for name, data in sorted(entries.items())]}
    require(document(output / ".codestudio-package-output.json") == expected, "Packaging output ownership receipt mismatch")


def artifact_record(path, base=ROOT):
    data = read(path)
    return {"path": path.relative_to(base).as_posix() if path.is_relative_to(base) else str(path),
            "sha256": sha(data), "bytes": len(data)}


def copy_linux_delivery(payload, modes, archive_path):
    destination = checked_chain(ROOT / "build/linux/x86_64/release")
    expected = {**payload, archive_path.name: read(archive_path)}
    if destination.exists():
        existing, directories = file_inventory(destination)
        require(set(existing).issubset(expected), "Existing Linux delivery contains unrelated files; nothing was copied")
        allowed_dirs = {parent.as_posix() for name in expected for parent in PurePosixPath(name).parents
                        if parent.as_posix() != "."}
        require(directories.issubset(allowed_dirs), "Existing Linux delivery contains unrelated directories")
    destination.mkdir(parents=True, exist_ok=True)
    checked_chain(destination)
    for name, data in sorted(expected.items()):
        target = checked_chain(destination / name)
        target.parent.mkdir(parents=True, exist_ok=True)
        checked_chain(target.parent)
        if target.exists() and read(target) == data:
            continue
        descriptor, temporary_name = tempfile.mkstemp(prefix=".delivery-copy-", dir=target.parent)
        temporary = Path(temporary_name)
        try:
            with os.fdopen(descriptor, "wb") as output:
                output.write(data)
                output.flush()
                os.fsync(output.fileno())
            temporary.chmod(modes.get(name, 0o644))
            require(read(temporary) == data, "Temporary delivery copy differs from verified payload")
            os.replace(temporary, target)
            require(read(target) == data, "Published delivery copy differs from verified payload")
        finally:
            if temporary.exists():
                # This exact tempfile was created above; no recursive cleanup.
                temporary.unlink()
    actual, _ = file_inventory(destination)
    require(actual == expected, "Copied Linux delivery differs from exact verified package and archive")
    return destination


def main():
    source, fingerprint = source_inventory(ROOT)
    mirror, mirror_fingerprint = source_inventory(MIRROR)
    require(source == mirror and fingerprint == mirror_fingerprint, "Native mirror source file set or bytes differ")
    summary = document(EVIDENCE / "verification.json")
    require(summary.get("format") == 2 and set(summary["configurations"]) ==
            {platform + "-" + mode for platform in ("windows", "linux") for mode in ("debug", "release")},
            "Verification summary does not contain all four native configurations")
    reports, report_paths = {}, {}
    for platform in ("windows", "linux"):
        for mode in ("debug", "release"):
            name = platform + "-" + mode
            item = summary["configurations"][name]
            path = receipt_path(item["receipt"])
            report = document(path)
            latest_path, latest = successful_reports(EVIDENCE / platform, mode, full=True)
            require(path == latest_path and report == latest, "Summary did not select the latest successful full verification")
            arch = validate_report(report, platform, mode, fingerprint)
            require(item["source"] == fingerprint and item["artifact"] == report["artifact"], "Summary snapshot differs from receipt")
            base = ROOT if platform == "windows" else MIRROR
            binary = base / "build" / platform / arch / mode / ("codestudio-agent.exe" if platform == "windows" else "codestudio-agent")
            require(report["artifact"] == {"path": binary.relative_to(base).as_posix(), "size": binary.stat().st_size,
                                           "sha256": sha(read(binary))}, "Verified build artifact has changed")
            verify_tested_artifacts(report, platform, mode, base)
            reports[name], report_paths[name] = report, path
    packages, installed, native_linux = {}, {}, None
    for platform, arch in (("windows", "x64"), ("linux", "x86_64")):
        base = ROOT if platform == "windows" else MIRROR
        output = base / "build/packages" / f"{platform}-{arch}"
        name = f"codestudio-agent-{platform}-{arch}"
        archive_name = name + (".zip" if platform == "windows" else ".tar.gz")
        archive = output / archive_name
        release = reports[platform + "-release"]
        payload, modes, manifest = validate_package(output / name, platform, arch, report_paths[platform + "-release"], release)
        archive_sha = validate_archive(archive, payload, modes, platform)
        validate_output_marker(output, name, archive_name, payload)
        check = document(EVIDENCE / f"archive-check-{platform}.json")
        require(check.get("format") == 2 and check.get("platform") == platform and check.get("status") == "succeeded" and
                check.get("returncode") == 0 and check.get("archive_sha256") == archive_sha and
                check.get("payload_files") == len(payload) and check.get("manifest_and_checksums") == "verified" and
                check.get("native_extraction_modes") == "verified" and check.get("tested_from_fresh_archive_extraction") is True and
                check.get("extraction_cleaned") is True, "Fresh archive extraction verification does not match final archive")
        installed_path, installation = successful_reports(EVIDENCE / f"installed-{platform}", "release", full=False)
        require(receipt_path(check["verification_receipt"]) == installed_path, "Archive check does not point to latest installed verification")
        validate_report(installation, platform, "release", fingerprint)
        require(installation["artifact"] == release["artifact"], "Installed verification used a different native source binary")
        steps = [step for step in installation["steps"] if step["name"] == "delivery"]
        require(len(steps) == 1 and type(steps[0].get("cases")) is int and steps[0]["cases"] > 0 and
                steps[0]["passed"] + steps[0]["skipped"] == steps[0]["cases"], "Installed verification count is invalid")
        installed[platform] = {"receipt": installed_path.relative_to(EVIDENCE).as_posix(),
                               "archive_check": f"archive-check-{platform}.json",
                               "cases": steps[0]["cases"], "passed": steps[0]["passed"], "skipped": steps[0]["skipped"],
                               "from_fresh_archive_extraction": True, "temporary_extraction_cleaned": True}
        binary_name = "codestudio-agent.exe" if platform == "windows" else "codestudio-agent"
        packages[platform] = {"archive": artifact_record(archive), "binary": artifact_record(output / name / binary_name),
                              "package_manifest_sha256": sha(payload["package.json"]), "payload_files": len(payload),
                              "archive_bytes_and_modes_match": True, "manifest_and_checksums_match": True,
                              "embedded_full_release_receipt": report_paths[platform + "-release"].relative_to(EVIDENCE).as_posix()}
        reproducibility_path = EVIDENCE / f"package-repro-{platform}.json"
        reproducibility = document(reproducibility_path)
        require(reproducibility.get("format") == 2 and reproducibility.get("platform") == platform and
                reproducibility.get("archive_sha256") == archive_sha and
                reproducibility.get("source_matches_release_report") is True and
                type(reproducibility.get("identical_archive_runs")) is int and
                reproducibility["identical_archive_runs"] == 2 and
                receipt_path(reproducibility["verification_receipt"]) == report_paths[platform + "-release"],
                "Archive reproducibility receipt differs from final package")
        packages[platform]["deterministic_archive_receipt"] = reproducibility_path.name
        packages[platform]["archive_repeated_runs"] = 2
        if platform == "linux":
            native_linux = (payload, modes, archive)
    linux_credential_cleanup = verify_linux_credential_cleanup([
        report_paths["linux-debug"].relative_to(EVIDENCE).as_posix(),
        report_paths["linux-release"].relative_to(EVIDENCE).as_posix(),
        installed["linux"]["receipt"]])
    destination = copy_linux_delivery(*native_linux)
    packages["linux"]["delivered_archive"] = artifact_record(destination / native_linux[2].name)
    packages["linux"]["delivered_binary"] = artifact_record(destination / "codestudio-agent")
    require(packages["linux"]["archive"]["sha256"] == packages["linux"]["delivered_archive"]["sha256"],
            "Copied archive checksum mismatch")
    # A final read prevents a concurrent source edit from acquiring a delivery
    # receipt merely because the earlier snapshot was valid.
    require(source_inventory(ROOT)[1] == fingerprint and source_inventory(MIRROR)[1] == fingerprint,
            "Source changed while finalizing delivery")
    result = {"format": 2, "product": PRODUCT, "generated_at": datetime.now(timezone.utc).isoformat(), "status": "verified",
              "source": fingerprint, "native_mirror_file_set_and_bytes_match": True,
              "full_verification": {name: path.relative_to(EVIDENCE).as_posix() for name, path in report_paths.items()},
              "packages": packages, "installed_verification": installed,
              "linux_private_native_credential_cleanup": linux_credential_cleanup,
              "linux_payload_copied_without_deletion": True,
              "boundaries": ["Windows and Arch Linux WSL are the existing test hosts, not clean machines.",
                             "The enclosing Windows Job limits detached background-success verification; receipt skips remain explicit.",
                             "Archive extraction and user-level install/upgrade/uninstall tests ran in temporary owned prefixes.",
                             "No hosted CI execution, Git publication, signing, real login/reboot or permanent installation is claimed.",
                             "OAuth was verified against synthetic authorization servers and owned native-store entries, not real provider login.",
                             "Plugin distribution tests used owned fixtures and temporary stores; no remote registry or third-party plugin was published.",
                             "Resource recovery is limited to journaled identities; historical unregistered residue is neither adopted nor deleted.",
                             "Delivery verification confirms tested bytes; final-static.json reports current and historical resource residue separately.",
                             "SHA256 verifies bytes and does not establish publisher identity; packages are unsigned.",
                             "Linux extracted payload mode checks occurred on the native filesystem; the archive preserves executable modes."]}
    (EVIDENCE / "delivery.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
