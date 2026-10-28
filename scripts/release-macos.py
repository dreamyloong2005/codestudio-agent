#!/usr/bin/env python3
"""Prepare a signed macOS delivery, then explicitly submit it for notarization.

Only copies in a new output directory are modified. `prepare --ad-hoc` creates
a local test image, never a publisher-verified release. `notarize` is the only
command that uploads an artifact; credentials are referenced by Keychain profile
name, never accepted as passwords or private keys. A notarized image still does
not establish clean-machine or real-login acceptance.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import subprocess
import sys
import tempfile
import types
import uuid

ROOT = Path(__file__).resolve().parents[1]
PRODUCT = "CodeStudio-Agent"
PACKAGE = "payload/codestudio-agent-macosx-arm64"
IMAGE = "codestudio-agent-macosx-arm64.dmg"
FINAL_IMAGE = "codestudio-agent-macosx-arm64-notarized.dmg"
RECEIPT = "release.json"
IMAGES = ("lib/libc++abi.1.dylib", "lib/libc++.1.dylib", "codestudio-agent")
IDENTIFIERS = {"codestudio-agent": "com.codestudio.agent",
               "lib/libc++.1.dylib": "com.codestudio.agent.libcxx",
               "lib/libc++abi.1.dylib": "com.codestudio.agent.libcxxabi"}


class ReleaseError(Exception):
    def __init__(self, message, *, details=None):
        super().__init__(message)
        self.details = details


def require(condition, message):
    if not condition:
        raise ReleaseError(message)


def now():
    return datetime.now(timezone.utc).isoformat()


def encoded(value):
    return (json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2) + "\n").encode()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load_module(name, path):
    # Load only trusted checkout tools; never execute the supplied package's
    # installer or create a __pycache__ in its exact inventory.
    result = types.ModuleType(name)
    result.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), "exec"), result.__dict__)
    return result


def components():
    return (load_module("release_installer", ROOT / "scripts/install.py"),
            load_module("release_bundle", ROOT / "scripts/macos_bundle.py"))


def run(argv, *, timeout=120, check=True, env=None):
    try:
        result = subprocess.run(list(map(str, argv)), stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=timeout, check=False, env=env)
    except subprocess.TimeoutExpired as error:
        raise ReleaseError("External operation timed out; its effect may be incomplete") from error
    except OSError as error:
        raise ReleaseError("Required macOS release command could not start") from error
    require(len(result.stdout) + len(result.stderr) <= 4 * 1024 * 1024,
            "External release command produced excessive output")
    output = {"returncode": result.returncode,
              "stdout": result.stdout.decode("utf-8", errors="replace"),
              "stderr": result.stderr.decode("utf-8", errors="replace")}
    if check and result.returncode:
        # Caller receipts retain command output where it is actionable. Do not
        # interpolate authentication-related output into a terminal error.
        raise ReleaseError("External release command failed: " + Path(str(argv[0])).name, details=output)
    return output


def read_json(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "Duplicate JSON key in release metadata")
            result[key] = value
        return result
    try:
        result = json.loads(data, object_pairs_hook=unique)
    except (ValueError, UnicodeError) as error:
        raise ReleaseError("Invalid release JSON") from error
    require(isinstance(result, dict), "Release JSON must be an object")
    return result


def signature(path, *, team=None, development=False, executable=False, identifier=None):
    argv = ["/usr/bin/codesign", "--verify", "--strict", "--verbose=2"]
    if not development:
        require(bool(re.fullmatch(r"[A-Z0-9]{10}", team or "")), "Invalid signing Team ID")
        requirement = ('anchor apple generic and certificate leaf[subject.OU] = "' + team +
                       '" and certificate leaf[field.1.2.840.113635.100.6.1.13] exists')
        argv += ["-R", requirement]
    run([*argv, path])
    displayed = run(["/usr/bin/codesign", "--display", "--verbose=4", path])
    text = displayed["stdout"] + displayed["stderr"]
    if identifier:
        require("Identifier=" + identifier + "\n" in text, "Signed artifact has an unstable code identity")
    if development:
        require("Signature=adhoc" in text, "Development image lacks an ad-hoc signature")
    else:
        require("TeamIdentifier=" + team in text and "Authority=Developer ID Application:" in text,
                "Signed artifact has a different publisher identity")
        require("Timestamp=" in text, "Developer ID signature has no secure timestamp")
    hardened = bool(re.search(r"flags=0x[0-9a-f]+\([^\n)]*\bruntime\b", text))
    if executable:
        require(development or hardened, "Developer ID executable lacks hardened runtime")
        entitlement = run(["/usr/bin/codesign", "--display", "--entitlements", "-", "--xml", path])
        if entitlement["stdout"].strip():
            try:
                value = plistlib.loads(entitlement["stdout"].encode())
            except (ValueError, plistlib.InvalidFileException) as error:
                raise ReleaseError("Cannot verify executable entitlements") from error
            require(value == {}, "Release executables must not request extra entitlements")
    return {"kind": "ad_hoc_not_publisher_verified" if development else "developer_id",
            "team_id": None if development else team,
            "hardened_runtime": hardened,
            "details": text.strip()}


def check_identity(identity, team):
    require(bool(re.fullmatch(r"[0-9A-Fa-f]{40}", identity or "")),
            "Specify an exact signing certificate SHA-1, not a display-name selector")
    require(bool(re.fullmatch(r"[A-Z0-9]{10}", team or "")), "Invalid signing Team ID")
    result = run(["/usr/bin/security", "find-identity", "-v", "-p", "codesigning"])
    entries = re.findall(r'\b([0-9A-Fa-f]{40})\s+"([^"\n]+)"', result["stdout"])
    matches = [name for value, name in entries if value.upper() == identity.upper()]
    require(len(matches) == 1 and matches[0].startswith("Developer ID Application: ") and
            matches[0].endswith(" (" + team + ")"),
            "Requested Developer ID identity and Team ID are not available")
    return matches[0]


def write_receipt(root, receipt):
    data = encoded(receipt)
    require(len(data) <= 4 * 1024 * 1024, "Release receipt is too large")
    temporary = root / (".release-next-" + uuid.uuid4().hex + ".json")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(root / RECEIPT)


@contextmanager
def release_lock(root, installer):
    path = root / ".release-operation.lock"
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError as error:
        raise ReleaseError("A release operation or interrupted lock requires inspection") from error
    token = uuid.uuid4().hex.encode()
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(token)
        stream.flush()
        os.fsync(stream.fileno())
    try:
        yield
    finally:
        if installer.read_bounded(path, 128) == token:
            path.unlink()


def verified_input(path, installer, bundle):
    package = installer.verify_package(path)
    document = package["document"]
    require((document["platform"], document["arch"]) == ("macosx", "arm64"),
            "Release requires a native macOS arm64 package")
    images = {name: installer.read_bounded(path / name, installer.MAX_FILE) for name in IMAGES}
    bundle.validate_payload(images)
    metadata = read_json(installer.read_bounded(path / "build-metadata.json", installer.MAX_MANIFEST))
    require(metadata.get("binary_sha256") == sha(images["codestudio-agent"]),
            "Package binary does not match build metadata")
    require(metadata.get("verification", {}).get("state") == "supplied",
            "Signing requires a supplied native Release verification receipt")
    report_data = installer.read_bounded(path / "verification.json", 4 * 1024 * 1024)
    report = read_json(report_data)
    require(metadata["verification"].get("sha256") == sha(report_data),
            "Native verification receipt digest differs from package metadata")
    require(report.get("format") == 2 and report.get("product") == PRODUCT and
            report.get("status") == "succeeded" and report.get("platform") == "macosx" and
            report.get("arch") == "arm64" and report.get("mode") == "release" and
            report.get("build_requested") is True and report.get("omitted_suites") in ([], ["delivery"]),
            "Formal preparation requires a complete successful native Release build receipt")
    require(report.get("artifact", {}).get("sha256") == metadata.get("source_binary_sha256"),
            "Native receipt does not bind the source binary before relocation")
    runtime_records = metadata.get("macos_bundle", {}).get("runtime_records")
    require(isinstance(runtime_records, list) and len(runtime_records) == 2 and
            {item.get("path") for item in runtime_records} == set(IMAGES[:-1]),
            "Runtime provenance inventory is incomplete")
    expected_runtime = report.get("toolchain", {}).get("libcxx_sha256", {})
    for item in runtime_records:
        data = images[item["path"]]
        require(item.get("sha256") == sha(data) and item.get("size") == len(data) and
                item.get("source_sha256") == expected_runtime.get(Path(item["path"]).name),
                "Private runtime does not match native verification provenance")
    steps = report.get("steps")
    require(isinstance(steps, list) and steps and all(s.get("status") == "succeeded" for s in steps) and
            any(s.get("name") == "native" and s.get("native_groups", 0) > 0 for s in steps),
            "Native receipt has no successful native execution evidence")
    return package, metadata


def refresh_package(root, package, metadata, installer, development, team):
    metadata["binary_sha256"] = sha(installer.read_bounded(root / "codestudio-agent", installer.MAX_FILE))
    macos = metadata["macos_bundle"]
    records = macos["runtime_records"]
    require({item.get("path") for item in records} == set(IMAGES[:-1]) and len(records) == 2,
            "Runtime provenance inventory is incomplete")
    for item in records:
        data = installer.read_bounded(root / item["path"], installer.MAX_FILE)
        item.update(sha256=sha(data), size=len(data))
    macos["signature"] = "ad_hoc_not_publisher_verified" if development else "developer_id"
    macos["signing"] = {"team_id": team, "hardened_runtime": not development,
                         "notarization": "see_distribution_receipt" if not development else "not_eligible"}
    explanation = ("This local development payload has ad-hoc signatures without hardened runtime; it identifies no publisher and cannot be notarized."
                   if development else "This payload has Developer ID signatures for Team " + team +
                   ". macOS checks notarization on the enclosing disk image; signatures alone do not establish notarization.")
    dependencies = metadata.get("dependencies")
    require(isinstance(dependencies, dict) and isinstance(dependencies.get("notes"), list) and
            isinstance(dependencies.get("libraries"), list), "Package dependency metadata is incomplete")
    dependencies["notes"] = [note for note in dependencies["notes"]
                             if "ad-hoc" not in note and "development payload" not in note]
    dependencies["notes"].append(explanation)
    (root / "runtime-dependencies.txt").write_text("Native runtime dependencies\n\n" +
        "\n".join(dependencies["libraries"]) + "\n\n" + "\n".join(dependencies["notes"]) + "\n")
    (root / "README.txt").write_text("CodeStudio Agent " + package["document"]["version"] +
        " - macOS arm64\n\n" + explanation + "\n\n" +
        "Read docs/delivery.md for installation, upgrade, removal and supported capabilities.\n"
        "The Agent needs no Python; installation uses Python 3.11 or newer:\n\n"
        "  python3 install.py install --package .\n"
        "  python3 install.py status\n"
        "  python3 install.py upgrade --package /path/to/new/package\n"
        "  python3 install.py uninstall\n\n"
        "Validate this exact extracted package in temporary installation directories:\n\n"
        "  python3 verify-delivery.py --standalone --package . --receipt /outside/new-report.json\n\n"
        "An explicit --prefix selects a user application directory. Stop running Agent instances before changes.\n"
        "Installation preserves workspaces and credentials and does not change PATH or startup registrations.\n"
        "package.json and SHA256SUMS cover these exact payload bytes. Source build verification precedes\n"
        "runtime relocation and signing; build-metadata.json records both the source and shipped digests.\n"
        "Private runtime libraries and license notices are included. System libraries and timezone data come from macOS.\n"
        "No packaging or notarization result establishes real-login or clean-machine acceptance.\n")
    (root / "build-metadata.json").write_bytes(encoded(metadata))
    document = dict(package["document"])
    require({"README.txt", "runtime-dependencies.txt"} <= {item["path"] for item in document["files"]},
            "Package is missing its delivery documentation")
    document["files"] = [installer.file_record(root, item["path"], item["executable"])
                         for item in document["files"]]
    manifest = encoded(document)
    (root / "package.json").write_bytes(manifest)
    hashes = {item["path"]: item["sha256"] for item in document["files"]}
    hashes["package.json"] = sha(manifest)
    (root / "SHA256SUMS").write_text("".join(f"{value}  {name}\n" for name, value in sorted(hashes.items())))
    return installer.verify_package(root)


def verify_image_payload(image, package_hash, installer, parent):
    mount = Path(tempfile.mkdtemp(prefix=".codestudio-dmg-check-", dir=parent))
    attached = False
    detached = False
    try:
        try:
            result = run(["/usr/bin/hdiutil", "attach", "-readonly", "-nobrowse", "-plist",
                          "-mountpoint", mount, image])
        except BaseException as error:
            # A failed client can leave work in the system disk-image helper.
            # Preserve the unique location rather than assuming no mount exists.
            raise ReleaseError("Image attachment is incomplete; inspect retained mount point " + str(mount)) from error
        attached = True
        try:
            response = plistlib.loads(result["stdout"].encode())
        except (ValueError, plistlib.InvalidFileException) as error:
            raise ReleaseError("Mounted image returned invalid identity metadata") from error
        entries = [item for item in response.get("system-entities", []) if item.get("mount-point") == str(mount)]
        require(len(entries) == 1 and re.fullmatch(r"/dev/disk\d+(?:s\d+)*", entries[0].get("dev-entry", "")),
                "Cannot bind the image to its owned mount point")
        inspected = installer.verify_package(mount / Path(PACKAGE).name)
        require(inspected["package_sha256"] == package_hash, "Disk image payload differs from signed package")
    finally:
        if attached:
            try:
                run(["/usr/bin/hdiutil", "detach", mount])
            except BaseException as error:
                raise ReleaseError("Image detach is incomplete; retained mount point " + str(mount)) from error
            detached = True
        if detached:
            mount.rmdir()


def artifact_record(path, installer):
    data = installer.read_bounded(path, installer.MAX_PACKAGE)
    return {"path": path.name, "sha256": sha(data), "size": len(data)}


def prepare(args):
    installer, bundle = components()
    source = installer.check_chain(args.package)
    output = installer.check_chain(args.output)
    require(not output.exists() and output.parent.is_dir(), "Release output must be new, with an existing parent")
    require(not installer.paths_overlap(source, output), "Release source and output must be disjoint")
    development = args.ad_hoc
    require(development != bool(args.identity), "Choose either --ad-hoc or --identity")
    require(not development or args.team_id is None, "Ad-hoc preparation does not take a Team ID")
    publisher = None if development else check_identity(args.identity, args.team_id)
    package, metadata = verified_input(source, installer, bundle)
    source_hash = package["package_sha256"]
    receipt = {"format": 1, "product": PRODUCT, "platform": "macosx", "arch": "arm64",
               "created_at": now(), "status": "preparing", "package": PACKAGE,
               "source_package_sha256": source_hash, "source_binary_sha256": metadata["source_binary_sha256"],
               "signature": {"kind": "ad_hoc_not_publisher_verified" if development else "developer_id",
                             "identity_sha1": None if development else args.identity.upper(),
                             "publisher": publisher, "team_id": args.team_id},
               "stage4_accepted": False, "clean_machine_acceptance": "not_claimed",
               "real_login_acceptance": "not_claimed"}
    output.mkdir(mode=0o700)
    run(["/bin/chmod", "-N", output])
    output.chmod(0o700)
    # All changed files are in the new private output. Do not remove unknown
    # state on failure: the failed receipt identifies the retained work.
    write_receipt(output, receipt)
    try:
        (output / "payload").mkdir(mode=0o700)
        copied = output / PACKAGE
        installer.copy_verified(source, copied, package)
        identity = "-" if development else args.identity
        for name in IMAGES:
            run(["/usr/bin/codesign", "--force", "--sign", identity, "--identifier", IDENTIFIERS[name],
                 *([] if development else ["--options", "runtime"]),
                 "--timestamp=none" if development else "--timestamp", copied / name])
            signature(copied / name, team=args.team_id, development=development, executable=True,
                      identifier=IDENTIFIERS[name])
        # Developer ID's same-Team library validation differs from ad-hoc
        # signing. Exercise the actual re-signed graph before packaging it.
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith(("DYLD_", "CODESTUDIO_"))}
        version = run([copied / "codestudio-agent", "version"], timeout=30, env=environment)["stdout"].strip()
        doctor = read_json(run([copied / "codestudio-agent", "doctor"], timeout=30, env=environment)["stdout"])
        require(version == PRODUCT + " " + package["document"]["version"] and
                doctor.get("product") == PRODUCT and doctor.get("version") == package["document"]["version"] and
                doctor.get("cplusplus", 0) > 202302, "Re-signed payload cannot run with its bundled runtime")
        metadata["macos_bundle"]["signing_smoke"] = {"version": version, "product": doctor["product"],
                                                     "cplusplus": doctor["cplusplus"]}
        signed = refresh_package(copied, package, metadata, installer, development, args.team_id)
        # Recheck the untouched input, so a modified package cannot silently
        # become the source for only some of the signed payload.
        require(installer.verify_package(source)["package_sha256"] == source_hash,
                "Source package changed during release preparation")
        bundle.validate_payload({name: installer.read_bounded(copied / name, installer.MAX_FILE) for name in IMAGES})
        image = output / IMAGE
        run(["/usr/bin/hdiutil", "create", "-fs", "HFS+", "-format", "UDZO", "-volname",
             "CodeStudio Agent", "-srcfolder", output / "payload", image], timeout=180)
        run(["/usr/bin/codesign", "--sign", identity, "--identifier", "com.codestudio.agent.distribution",
             "--timestamp=none" if development else "--timestamp", image])
        signature(image, team=args.team_id, development=development)
        run(["/usr/bin/hdiutil", "verify", image])
        verify_image_payload(image, signed["package_sha256"], installer, output.parent)
        receipt.update(status="development_only" if development else "prepared_not_notarized",
                       package_sha256=signed["package_sha256"], artifact=artifact_record(image, installer),
                       prepared_at=now())
        write_receipt(output, receipt)
        return receipt
    except BaseException as error:
        receipt.update(status="prepare_failed", failed_at=now(),
                       error=str(error) if isinstance(error, ReleaseError) else type(error).__name__)
        if isinstance(error, ReleaseError) and error.details:
            receipt["command_failure"] = error.details
        write_receipt(output, receipt)
        raise


def verified_release(root, installer, bundle):
    receipt = read_json(installer.read_bounded(root / RECEIPT, 4 * 1024 * 1024))
    require((receipt.get("format"), receipt.get("product"), receipt.get("platform"), receipt.get("arch"),
             receipt.get("package")) == (1, PRODUCT, "macosx", "arm64", PACKAGE), "Invalid release receipt")
    candidates = receipt.get("stapling_candidates", [])
    require(isinstance(candidates, list) and len(candidates) <= 16 and
            all(isinstance(item, dict) and re.fullmatch(r"\.codestudio-staple-[0-9a-f]{32}\.dmg", item.get("path", ""))
                for item in candidates), "Invalid retained stapling candidate inventory")
    names = {item["path"] for item in candidates}
    require(len(names) == len(candidates), "Repeated stapling candidate identity")
    expected = {"payload", IMAGE, RECEIPT, ".release-operation.lock", *names}
    if receipt.get("status") in ("publishing", "notarized"):
        expected.add(FINAL_IMAGE)
    require(set(item.name for item in root.iterdir()) <= expected, "Unexpected files in release directory")
    require(set(item.name for item in (root / "payload").iterdir()) == {Path(PACKAGE).name},
            "Unexpected payload directory")
    require(receipt.get("signature", {}).get("kind") == "developer_id",
            "Ad-hoc deliveries cannot be notarized")
    package, _ = verified_input(root / PACKAGE, installer, bundle)
    require(package["package_sha256"] == receipt.get("package_sha256"), "Signed package changed after preparation")
    submitted = receipt.get("submitted_artifact", receipt.get("artifact"))
    require(isinstance(submitted, dict) and submitted.get("path") == IMAGE and
            artifact_record(root / IMAGE, installer) == submitted,
            "Disk image changed after preparation; refusing submission or retry")
    if receipt.get("status") == "notarized":
        require(receipt.get("artifact", {}).get("path") == FINAL_IMAGE and
                artifact_record(root / FINAL_IMAGE, installer) == receipt["artifact"],
                "Published notarized image changed")
    for name in IMAGES:
        signature(root / PACKAGE / name, team=receipt["signature"]["team_id"], executable=True,
                  identifier=IDENTIFIERS[name])
    signature(root / IMAGE, team=receipt["signature"]["team_id"])
    return receipt


def publish_notarized(root, receipt, installer):
    publication = receipt.get("publication", {})
    candidate = publication.get("candidate", "")
    require(candidate in {item["path"] for item in receipt.get("stapling_candidates", [])} and
            publication.get("artifact", {}).get("path") == FINAL_IMAGE,
            "Publication has no verified candidate identity")
    final = root / FINAL_IMAGE
    expected = publication["artifact"]
    if final.exists():
        require(artifact_record(final, installer) == expected,
                "Published image differs from the durable publication intent")
    else:
        value = artifact_record(root / candidate, installer)
        require(dict(value, path=FINAL_IMAGE) == expected, "Verified candidate changed before publication")
        installer.publish_new_path(root / candidate, final)
    # If the process died after rename but before the receipt update, only this
    # exact hash is accepted. The original uploaded image is never replaced.
    require(artifact_record(final, installer) == expected, "Published image changed")
    run(["xcrun", "stapler", "validate", final])
    signature(final, team=receipt["signature"]["team_id"])
    receipt.update(status="notarized", notarized_at=now(), artifact=expected)
    write_receipt(root, receipt)
    return receipt


def finalize_notarization(root, receipt, installer):
    if receipt["status"] == "publishing":
        return publish_notarized(root, receipt, installer)
    submitted = receipt["submitted_artifact"]
    log = receipt.get("notary_log", {})
    require(log.get("status") == "Accepted" and log.get("jobId") == receipt.get("notary", {}).get("id") and
            log.get("sha256") == submitted["sha256"], "No accepted notarization binds this image")
    candidates = receipt.setdefault("stapling_candidates", [])
    require(len(candidates) < 16, "Inspect retained stapling attempts before creating another candidate")
    name = ".codestudio-staple-" + uuid.uuid4().hex + ".dmg"
    entry = {"path": name, "status": "creation_intent", "source_sha256": submitted["sha256"]}
    candidates.append(entry)
    receipt.update(status="stapling", active_candidate=name)
    write_receipt(root, receipt)
    data = installer.read_bounded(root / IMAGE, installer.MAX_PACKAGE)
    require(sha(data) == submitted["sha256"], "Original submitted image changed before stapling")
    candidate = root / name
    descriptor = os.open(candidate, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    entry["status"] = "copied_from_submitted_image"
    write_receipt(root, receipt)
    run(["xcrun", "stapler", "staple", candidate])
    receipt["status"] = "validating"
    write_receipt(root, receipt)
    run(["xcrun", "stapler", "validate", candidate])
    signature(candidate, team=receipt["signature"]["team_id"])
    run(["/usr/bin/hdiutil", "verify", candidate])
    verify_image_payload(candidate, receipt["package_sha256"], installer, root.parent)
    gatekeeper = run(["/usr/sbin/spctl", "--assess", "--type", "open", "--context",
                      "context:primary-signature", "--verbose=4", candidate])
    value = artifact_record(candidate, installer)
    entry.update(status="verified", sha256=value["sha256"], size=value["size"])
    receipt.update(status="publishing", gatekeeper=gatekeeper,
                   publication={"candidate": name, "artifact": dict(value, path=FINAL_IMAGE)})
    write_receipt(root, receipt)
    return publish_notarized(root, receipt, installer)


def notary_response(result):
    response = read_json(result["stdout"])
    value = response.get("id")
    try:
        require(isinstance(value, str) and str(uuid.UUID(value)) == value.lower(), "Invalid notarization submission ID")
    except ValueError as error:
        raise ReleaseError("Invalid notarization submission ID") from error
    return response


def notarize(args):
    installer, bundle = components()
    root = installer.check_chain(args.release)
    profile = args.keychain_profile
    require(isinstance(profile, str) and 0 < len(profile) <= 128 and not profile.startswith("-") and
            all(ord(character) >= 32 and ord(character) != 127 for character in profile),
            "Specify a saved notarytool Keychain profile name")
    require(30 <= args.timeout <= 3600, "Notarization timeout must be between 30 and 3600 seconds")
    if args.submission_id:
        try:
            require(str(uuid.UUID(args.submission_id)) == args.submission_id.lower(), "Invalid submission ID")
        except ValueError as error:
            raise ReleaseError("Invalid submission ID") from error
    with release_lock(root, installer):
        receipt = verified_release(root, installer, bundle)
        status = receipt["status"]
        require(status in ("prepared_not_notarized", "submission_started", "notary_pending", "notary_rejected",
                           "notary_accepted", "stapling", "validating", "publishing", "notarized"),
                "Release state cannot be submitted")
        if status == "notarized":
            run(["xcrun", "stapler", "validate", root / FINAL_IMAGE])
            signature(root / FINAL_IMAGE, team=receipt["signature"]["team_id"])
            return receipt
        if status in ("notary_accepted", "stapling", "validating", "publishing"):
            try:
                return finalize_notarization(root, receipt, installer)
            except BaseException as error:
                receipt.update(last_failure_at=now(),
                               last_error=str(error) if isinstance(error, ReleaseError) else type(error).__name__)
                if isinstance(error, ReleaseError) and error.details:
                    receipt["command_failure"] = error.details
                write_receipt(root, receipt)
                raise
        existing = receipt.get("notary", {}).get("id")
        require(not (existing and args.submission_id and existing != args.submission_id),
                "Submission ID differs from the recorded request")
        submission = existing or args.submission_id
        if status != "prepared_not_notarized":
            require(submission, "Submission outcome is unknown; inspect Apple history and supply the original submission ID")
        require(status != "notary_rejected", "Apple rejected this artifact; prepare a corrected new release")
        # Persist the unknown-effect boundary before sending bytes. A crash or
        # timeout cannot cause a later invocation to upload a second request.
        receipt.update(status="submission_started", submission_started_at=now())
        write_receipt(root, receipt)
        try:
            operation = (["wait", submission] if submission else ["submit", str(root / IMAGE), "--wait"])
            response = run(["xcrun", "notarytool", *operation, "--keychain-profile", profile,
                            "--output-format", "json", "--timeout", str(args.timeout) + "s"],
                           timeout=args.timeout + 30, check=False)
            receipt["notary_command"] = response
            write_receipt(root, receipt)
            value = notary_response(response)
            if submission:
                require(value["id"] == submission, "Apple returned a different submission ID")
            receipt["notary"] = value
            receipt["status"] = "notary_rejected" if value.get("status") == "Invalid" else "notary_pending"
            write_receipt(root, receipt)
            require(response["returncode"] == 0 and value.get("status") == "Accepted",
                    "Apple has not accepted this notarization; inspect the retained receipt")
            log_result = run(["xcrun", "notarytool", "log", value["id"], "--keychain-profile", profile])
            receipt["notary_log_command"] = log_result
            write_receipt(root, receipt)
            log = read_json(log_result["stdout"])
            require(log.get("jobId") == value["id"] and log.get("status") == "Accepted" and
                    log.get("sha256") == receipt["artifact"]["sha256"],
                    "Notary log does not bind acceptance to this exact disk image")
            receipt.update(status="notary_accepted", notary_log=log, submitted_artifact=receipt["artifact"])
            write_receipt(root, receipt)
            # Both the package and pre-staple image must still match the
            # submitted bytes. Stapling is the only permitted byte change.
            verified_release(root, installer, bundle)
            return finalize_notarization(root, receipt, installer)
        except BaseException as error:
            receipt.update(last_failure_at=now(),
                           last_error=str(error) if isinstance(error, ReleaseError) else type(error).__name__)
            if isinstance(error, ReleaseError) and error.details:
                receipt["command_failure"] = error.details
            write_receipt(root, receipt)
            raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    first = commands.add_parser("prepare", help="Sign a copy and create a new disk image; no upload")
    first.add_argument("--package", type=Path, required=True)
    first.add_argument("--output", type=Path, required=True)
    signing = first.add_mutually_exclusive_group(required=True)
    signing.add_argument("--identity", help="Exact Developer ID Application certificate SHA-1")
    signing.add_argument("--ad-hoc", action="store_true", help="Local development only; cannot notarize")
    first.add_argument("--team-id")
    second = commands.add_parser("notarize", help="Explicitly upload the prepared image or resume its recorded submission")
    second.add_argument("--release", type=Path, required=True)
    second.add_argument("--keychain-profile", required=True)
    second.add_argument("--submission-id", help="Original Apple submission ID when the initial response was lost")
    second.add_argument("--timeout", type=int, default=900)
    args = parser.parse_args(argv)
    try:
        require(sys.platform == "darwin" and platform.machine().lower() == "arm64", "Requires native macOS arm64")
        result = prepare(args) if args.command == "prepare" else notarize(args)
        print(json.dumps({"product": PRODUCT, "status": result["status"],
                          "receipt": str((args.output if args.command == "prepare" else args.release) / RECEIPT),
                          "stage4_accepted": False}, indent=2))
        return 0
    except Exception as error:
        message = str(error) if isinstance(error, ReleaseError) else "Release filesystem or metadata operation failed"
        print(json.dumps({"product": PRODUCT, "status": "failed", "error": message}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
