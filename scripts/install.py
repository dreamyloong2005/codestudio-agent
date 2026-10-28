#!/usr/bin/env python3
"""Verified user-level installation for an extracted CodeStudio-Agent package.

Python 3.10+ and the standard library are required. This tool never edits PATH,
shell profiles, startup registrations, credential stores or workspace data.
Checksums establish package integrity, not publisher identity: obtain the package
from a trusted source. Stop Agent processes before upgrading or uninstalling.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import stat
import sys
import unicodedata
import uuid

PRODUCT = "CodeStudio-Agent"
FORMAT = 2
RECEIPT = ".codestudio-install.json"
MAX_MANIFEST = 1024 * 1024
MAX_FILE = 256 * 1024 * 1024
MAX_PACKAGE = 512 * 1024 * 1024
RESERVED = {"package.json", "SHA256SUMS", RECEIPT}


class InstallError(Exception):
    """An intentionally non-sensitive error suitable for command output."""

    def __init__(self, message, *, status="error", **details):
        super().__init__(message)
        self.status = status
        self.details = details


def require(condition, message):
    if not condition:
        raise InstallError(message)


def native_target():
    machine = platform.machine().lower()
    if sys.platform == "darwin":
        require(machine == "arm64", "Only native macOS arm64 installations are supported")
        return "macosx", "arm64", "codestudio-agent"
    require(machine in {"amd64", "x86_64"}, "Only native x86-64 installations are supported")
    if os.name == "nt":
        return "windows", "x64", "codestudio-agent.exe"
    require(sys.platform == "linux", "Only Windows and Linux installations are supported")
    return "linux", "x86_64", "codestudio-agent"


def default_prefix():
    if os.name == "nt":
        local = os.environ.get("LOCALAPPDATA", "")
        require(bool(local) and Path(local).is_absolute(), "LOCALAPPDATA is unavailable; provide an explicit prefix")
        return Path(local) / "Programs" / PRODUCT
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "CodeStudio Agent"
    return Path.home() / ".local" / "share" / "codestudio-agent"


def linked(info):
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def absolute_path(value):
    # Do not resolve first: that would hide a symlink or Windows junction.
    if sys.platform == "darwin":
        require(".." not in Path(value).parts, "Parent traversal is not permitted in installation paths")
    return Path(os.path.abspath(os.fspath(value)))


def darwin_path(path, physical=False):
    """Resolve native spelling only after every existing component is pinned.

    F_GETPATH_NOFIRMLINK also exposes physical ancestors hidden by APFS
    firmlinks. Missing destination tails remain explicit and need not exist.
    """
    import fcntl
    path = absolute_path(path)
    descriptors = [os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)]
    names = []
    tail = []
    try:
        parts = path.parts[1:]
        for index, name in enumerate(parts):
            try:
                child = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
                                dir_fd=descriptors[-1])
            except FileNotFoundError:
                tail = [unicodedata.normalize("NFC", part) for part in parts[index:]]
                break
            descriptors.append(child)
            names.append(name)
        raw = fcntl.fcntl(descriptors[-1], 102 if physical else 50, bytes(1024))
        native = Path(unicodedata.normalize("NFC", os.fsdecode(raw.split(b"\0", 1)[0])))
        require(native.is_absolute(), "Cannot resolve native installation path")
        for index, name in enumerate(names):
            named = os.stat(name, dir_fd=descriptors[index], follow_symlinks=False)
            opened = os.fstat(descriptors[index + 1])
            require(stat.S_ISDIR(named.st_mode) and
                    (named.st_dev, named.st_ino) == (opened.st_dev, opened.st_ino),
                    "Installation path changed during inspection")
        return native.joinpath(*tail)
    finally:
        for descriptor in reversed(descriptors):
            os.close(descriptor)


def check_chain(path):
    path = absolute_path(path)
    if sys.platform == "darwin":
        return darwin_path(path)
    for part in reversed((path, *path.parents)):
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        require(not linked(info), "Symlink or reparse-point paths are not permitted")
        require(stat.S_ISDIR(info.st_mode), "Installation path contains a non-directory component")
    return path


def paths_overlap(left, right):
    if sys.platform == "darwin":
        def key(path):
            return tuple(unicodedata.normalize("NFD", part).casefold()
                         for part in darwin_path(path, physical=True).parts)
        a, b = key(left), key(right)
        return a[:len(b)] == b or b[:len(a)] == a
    return left == right or left in right.parents or right in left.parents


def checked_prefix(value):
    path = check_chain(value)
    require(path != Path(path.anchor) and path != check_chain(Path.home()), "Refusing an unsafe installation prefix")
    # Refuse commonly selected container directories even when they are empty.
    forbidden = {absolute_path(Path(sys.prefix))}
    if os.name == "nt":
        for variable in ("WINDIR", "SystemRoot", "ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA", "TEMP", "TMP"):
            if os.environ.get(variable):
                forbidden.add(absolute_path(os.environ[variable]))
    else:
        forbidden.update(Path(item) for item in ("/usr", "/usr/local", "/opt", "/etc", "/var", "/tmp", "/home"))
    if sys.platform == "darwin":
        forbidden.update(Path(item) for item in ("/Applications", "/Library", "/System", "/Users"))
        forbidden.update(Path.home() / item for item in ("Library", "Library/Application Support", "Applications"))
        native_forbidden = set()
        for candidate in forbidden:
            try:
                native_forbidden.add(check_chain(candidate))
            except OSError:
                # A symlink container is already refused by check_chain.
                pass
        forbidden |= native_forbidden
    require(path not in forbidden, "Refusing an unsafe installation prefix")
    require(path.name not in {".", "..", ""}, "Invalid installation prefix")
    return path


def path_name(value):
    require(isinstance(value, str) and 0 < len(value) <= 240, "Invalid package path")
    parts = value.split("/")
    require(all(re.fullmatch(r"[A-Za-z0-9_.+-]+", part) and part not in {".", ".."}
                and not part.endswith((".", " ")) for part in parts), "Invalid package path")
    # Apply Windows's alias rules on both platforms so one manifest has one name.
    devices = {"CON", "PRN", "AUX", "NUL", *("COM" + str(i) for i in range(1, 10)),
               *("LPT" + str(i) for i in range(1, 10))}
    require(all(part.split(".", 1)[0].upper() not in devices for part in parts), "Invalid package path")
    require(str(PurePosixPath(value)) == value, "Invalid package path")
    return value


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "Duplicate JSON key in package metadata")
        result[key] = value
    return result


def regular_info(path):
    info = path.lstat()
    require(not linked(info) and stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
            "Package and installation files must be ordinary single-link files")
    return info


def read_bounded(path, limit):
    info = regular_info(path)
    require(info.st_size <= limit, "Package metadata exceeds its size limit")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, "rb") as stream:
        opened = os.fstat(stream.fileno())
        require((opened.st_dev, opened.st_ino) == (info.st_dev, info.st_ino) and opened.st_nlink == 1,
                "Package file changed during verification")
        data = stream.read(limit + 1)
    require(len(data) <= limit and len(data) == info.st_size, "Package metadata changed or exceeds its size limit")
    return data


def json_file(path):
    data = read_bounded(path, MAX_MANIFEST)
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=unique_object)
    except (ValueError, UnicodeError):
        raise InstallError("Invalid package metadata JSON") from None
    return value, data


def file_record(root, name, executable):
    path = root / name
    info = regular_info(path)
    require(info.st_size <= MAX_FILE, "Package file exceeds its size limit")
    digest = hashlib.sha256()
    count = 0
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    with os.fdopen(os.open(path, flags), "rb") as stream:
        opened = os.fstat(stream.fileno())
        require((opened.st_dev, opened.st_ino) == (info.st_dev, info.st_ino) and opened.st_nlink == 1,
                "Package file changed during verification")
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            count += len(chunk)
            require(count <= MAX_FILE, "Package file exceeds its size limit")
            digest.update(chunk)
        after = os.fstat(stream.fileno())
        require((opened.st_size, opened.st_mtime_ns, opened.st_ctime_ns) ==
                (after.st_size, after.st_mtime_ns, after.st_ctime_ns), "Package file changed during verification")
    require(count == info.st_size, "Package file changed during verification")
    if os.name != "nt":
        require(bool(info.st_mode & 0o111) == executable, "Package executable permissions differ from the manifest")
        require(not info.st_mode & (stat.S_ISUID | stat.S_ISGID), "Privileged package file modes are not permitted")
    return {"path": name, "sha256": digest.hexdigest(), "size": count, "executable": executable}


def inventory(root):
    check_chain(root)
    require(root.is_dir(), "Package or installation directory is missing")
    files, directories = set(), set()
    pending = [root]
    while pending:
        directory = pending.pop()
        for child in directory.iterdir():
            name = child.relative_to(root).as_posix()
            info = child.lstat()
            require(not linked(info), "Symlink or reparse-point entries are not permitted")
            if stat.S_ISDIR(info.st_mode):
                directories.add(name)
                pending.append(child)
            else:
                require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                        "Package and installation files must be ordinary single-link files")
                files.add(name)
            require(len(files) + len(directories) <= 2048, "Package directory exceeds its entry limit")
    return files, directories


def expected_directories(names):
    result = set()
    for name in names:
        result.update(str(parent) for parent in PurePosixPath(name).parents if str(parent) != ".")
    return result


def validate_records(records):
    require(isinstance(records, list) and 1 <= len(records) <= 512, "Invalid package file list")
    names, total = set(), 0
    for record in records:
        require(isinstance(record, dict) and set(record) == {"path", "sha256", "size", "executable"},
                "Invalid package file record")
        name = path_name(record["path"])
        require(name.casefold() not in {item.casefold() for item in RESERVED} and name.casefold() not in names,
                "Duplicate or reserved package path")
        names.add(name.casefold())
        require(isinstance(record["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", record["sha256"]),
                "Invalid package checksum")
        require(type(record["size"]) is int and 0 <= record["size"] <= MAX_FILE and type(record["executable"]) is bool,
                "Invalid package file size or mode")
        total += record["size"]
    require(total <= MAX_PACKAGE, "Package exceeds its total size limit")
    return records


def version_tuple(value):
    require(isinstance(value, str) and re.fullmatch(r"(?:0|[1-9][0-9]{0,5})\.(?:0|[1-9][0-9]{0,5})\.(?:0|[1-9][0-9]{0,5})", value),
            "Invalid package version")
    return tuple(map(int, value.split(".")))


def verify_package(root, installed=False):
    root = check_chain(root)
    document, data = json_file(root / "package.json")
    require(isinstance(document, dict) and set(document) == {"format", "product", "version", "platform", "arch", "files"},
            "Invalid package manifest fields")
    require(type(document["format"]) is int and document["format"] == FORMAT and document["product"] == PRODUCT,
            "Unsupported package format or product")
    target, arch, binary = native_target()
    require(document["platform"] == target and document["arch"] == arch, "Package does not match the native platform and architecture")
    version_tuple(document["version"])
    records = validate_records(document["files"])
    expected = {record["path"] for record in records}
    require(binary in expected and "install.py" in expected, "Package is missing its executable or installer")
    require(next(record["executable"] for record in records if record["path"] == binary), "Agent executable mode must be declared")
    names = expected | {"package.json", "SHA256SUMS"} | ({RECEIPT} if installed else set())
    found, directories = inventory(root)
    require(found == names and directories == expected_directories(names), "Package contains missing or unowned files or directories")
    for record in records:
        require(file_record(root, record["path"], record["executable"]) == record, "Package payload checksum mismatch")
    package_hash = hashlib.sha256(data).hexdigest()
    raw_checksums = read_bounded(root / "SHA256SUMS", MAX_MANIFEST)
    try:
        lines = raw_checksums.decode("ascii").splitlines()
    except UnicodeError:
        raise InstallError("Invalid SHA256SUMS encoding") from None
    checksums = {}
    for line in lines:
        require(len(line) > 66 and re.fullmatch(r"[0-9a-f]{64}", line[:64]) and line[64:66] == "  ", "Invalid SHA256SUMS line")
        name = path_name(line[66:])
        require(name not in checksums, "Duplicate SHA256SUMS path")
        checksums[name] = line[:64]
    require(checksums == {**{record["path"]: record["sha256"] for record in records}, "package.json": package_hash},
            "SHA256SUMS does not exactly match the package")
    all_records = records + [file_record(root, "package.json", False), file_record(root, "SHA256SUMS", False)]
    return {"document": document, "package_sha256": package_hash, "records": all_records}


def verify_install(root, expected_prefix=None):
    package = verify_package(root, installed=True)
    receipt, _ = json_file(root / RECEIPT)
    expected = {"format": FORMAT, "product": PRODUCT, "prefix": str(expected_prefix or root),
                "package_sha256": package["package_sha256"], "files": package["records"]}
    require(receipt == expected, "Installation ownership receipt is invalid or belongs to another prefix")
    package["records"] = package["records"] + [file_record(root, RECEIPT, False)]
    return package


class DarwinRegionInfo(ctypes.Structure):
    _fields_=[(name,ctypes.c_uint32) for name in ('protection','max_protection','inheritance','flags')]
    _fields_ += [('offset',ctypes.c_uint64)]
    _fields_ += [(name,ctypes.c_uint32) for name in ('behavior','user_wired_count','user_tag','pages_resident','pages_shared_now_private','pages_swapped_out','pages_dirtied','ref_count','shadow_depth','share_mode','private_pages_resident','shared_pages_resident','obj_id','depth')]
    _fields_ += [('address',ctypes.c_uint64),('size',ctypes.c_uint64)]
class DarwinVinfoStat(ctypes.Structure):
    _fields_=[('dev',ctypes.c_uint32),('mode',ctypes.c_uint16),('nlink',ctypes.c_uint16),('ino',ctypes.c_uint64),('uid',ctypes.c_uint32),('gid',ctypes.c_uint32)]
    _fields_ += [(name,ctypes.c_int64) for name in ('atime','atimensec','mtime','mtimensec','ctime','ctimensec','birthtime','birthtimensec','size','blocks')]
    _fields_ += [('blksize',ctypes.c_int32),('flags',ctypes.c_uint32),('gen',ctypes.c_uint32),('rdev',ctypes.c_uint32),('qspare',ctypes.c_int64*2)]
class DarwinVnodeInfo(ctypes.Structure):
    _fields_=[('stat',DarwinVinfoStat),('type',ctypes.c_int),('pad',ctypes.c_int),('fsid',ctypes.c_int32*2)]
class DarwinVnodePath(ctypes.Structure):
    _fields_=[('vnode',DarwinVnodeInfo),('path',ctypes.c_char*1024)]
class DarwinRegionPath(ctypes.Structure):
    _fields_=[('region',DarwinRegionInfo),('vnodepath',DarwinVnodePath)]


def darwin_mapped_file(pid, identity):
    """Compare kernel-reported mapped vnodes, never stat a process pathname."""
    import select
    import time
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    library.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
    library.proc_pidinfo.restype = ctypes.c_int
    require(ctypes.sizeof(DarwinRegionPath) == 1272, "Unexpected native process-region ABI")
    queue = select.kqueue()
    try:
        try:
            queue.control([select.kevent(pid, filter=select.KQ_FILTER_PROC, flags=select.KQ_EV_ADD,
                           fflags=select.KQ_NOTE_EXIT | select.KQ_NOTE_EXEC)], 0)
        except ProcessLookupError:
            return False
        address, deadline = 0, time.monotonic() + 2
        matched = False
        for _ in range(16384):
            require(time.monotonic() < deadline, "Process mapping inspection exceeded its deadline")
            region = DarwinRegionPath()
            ctypes.set_errno(0)
            size = library.proc_pidinfo(pid, 8, address, ctypes.byref(region), ctypes.sizeof(region))
            error = ctypes.get_errno()
            if size != ctypes.sizeof(region):
                if error in (errno.EINVAL, errno.ESRCH):
                    break
                raise InstallError("Cannot inspect mapped Agent image identity")
            observed = region.vnodepath.vnode.stat
            if observed.ino and (observed.dev, observed.ino) == identity:
                matched = True
                break
            following = region.region.address + region.region.size
            require(address < following < 2**64, "Invalid native process mapping range")
            address = following
        else:
            raise InstallError("Process mapping inspection exceeded its region bound")
        events = queue.control([], 8, 0)
        if any(event.fflags & select.KQ_NOTE_EXIT for event in events):
            return False
        require(not any(event.fflags & select.KQ_NOTE_EXEC for event in events),
                "Agent executable changed during process inspection")
        return matched
    finally:
        queue.close()


def darwin_running_executable(binary):
    # Name/path selects native Agent candidates; the actual mapped vnode is
    # compared below. This is read-only and not a lock against later exec.
    expected = binary.stat()
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    library.proc_listpids.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_int]
    library.proc_listpids.restype = ctypes.c_int
    library.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    library.proc_pidpath.restype = ctypes.c_int
    library.proc_name.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    library.proc_name.restype = ctypes.c_int
    count = library.proc_listpids(4, os.getuid(), None, 0)  # PROC_UID_ONLY
    require(0 < count < 4 * 1024 * 1024, "Cannot inspect running Agent processes")
    for _ in range(3):
        processes = (ctypes.c_int * (count // ctypes.sizeof(ctypes.c_int) + 128))()
        size = ctypes.sizeof(processes)
        returned = library.proc_listpids(4, os.getuid(), processes, size)
        require(returned >= 0, "Cannot inspect running Agent processes")
        if returned < size:
            break
        count = size * 2
        require(count < 4 * 1024 * 1024, "Process observation exceeds its bound")
    else:
        raise InstallError("Process list changed too quickly to inspect safely")
    for pid in processes[:returned // ctypes.sizeof(ctypes.c_int)]:
        if pid <= 0:
            continue
        buffer = ctypes.create_string_buffer(4096)
        ctypes.set_errno(0)
        length = library.proc_pidpath(pid, buffer, len(buffer))
        error = ctypes.get_errno()
        if length <= 0:
            if error == errno.ESRCH:
                continue
            name = ctypes.create_string_buffer(1024)
            ctypes.set_errno(0)
            length = library.proc_name(pid, name, len(name))
            if length <= 0 and ctypes.get_errno() == errno.ESRCH:
                continue
            require(length > 0 and os.fsdecode(name.value).casefold() != binary.name.casefold(),
                    "Cannot verify whether an Agent process uses this installation")
            continue
        image = Path(os.fsdecode(buffer.value))
        # Package inventory forbids linked/renamed payload files. Include every
        # standard Agent name, and unusual names still within this installation.
        parts = lambda path: tuple(unicodedata.normalize("NFD", part).casefold() for part in path.parts)
        installation = parts(binary.parent)
        if image.name.casefold() != binary.name.casefold() and parts(image)[:len(installation)] != installation:
            continue
        if darwin_mapped_file(pid, (expected.st_dev, expected.st_ino)):
            return True
    return False


def running_installed_executable(binary):
    if sys.platform == "darwin":
        return darwin_running_executable(binary)
    if os.name != "nt":
        expected = binary.stat()
        for process in Path("/proc").iterdir():
            if not process.name.isdigit():
                continue
            try:
                observed = (process / "exe").stat()
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                continue
            if (observed.st_dev, observed.st_ino) == (expected.st_dev, expected.st_ino):
                return True
        return False
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL

    class Entry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG), ("dwFlags", wintypes.DWORD),
                    ("szExeFile", wintypes.WCHAR * 260)]
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    require(snapshot not in (None, ctypes.c_void_p(-1).value), "Cannot inspect running Agent processes")
    try:
        entry = Entry()
        entry.dwSize = ctypes.sizeof(entry)
        available = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        require(bool(available) or ctypes.get_last_error() == 18, "Cannot inspect running Agent processes")
        while available:
            if entry.szExeFile.casefold() == binary.name.casefold():
                handle = kernel.OpenProcess(0x1000, False, entry.th32ProcessID)
                require(bool(handle), "Cannot verify whether an Agent process uses this installation")
                try:
                    count = wintypes.DWORD(32768)
                    buffer = ctypes.create_unicode_buffer(count.value)
                    require(bool(kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(count))),
                            "Cannot verify whether an Agent process uses this installation")
                    # Compare file identity as well as accepting native path
                    # spellings (drive case, extended paths and 8.3 aliases).
                    if os.path.samefile(binary, buffer.value):
                        return True
                finally:
                    kernel.CloseHandle(handle)
            available = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        require(ctypes.get_last_error() == 18, "Cannot inspect running Agent processes")
        return False
    finally:
        kernel.CloseHandle(snapshot)


def private_new_descriptor(descriptor, mode):
    if sys.platform == "darwin":
        library = ctypes.CDLL(None, use_errno=True)
        library.acl_init.argtypes = [ctypes.c_int]
        library.acl_init.restype = ctypes.c_void_p
        library.acl_set_fd.argtypes = [ctypes.c_int, ctypes.c_void_p]
        library.acl_set_fd.restype = ctypes.c_int
        library.acl_free.argtypes = [ctypes.c_void_p]
        library.acl_free.restype = ctypes.c_int
        acl = library.acl_init(0)
        require(bool(acl), "Cannot create a private installation ACL")
        try:
            require(library.acl_set_fd(descriptor, acl) == 0, "Cannot clear inherited installation ACL")
        finally:
            library.acl_free(acl)
        os.fchmod(descriptor, mode)


def create_directories(path):
    if sys.platform != "darwin":
        path.mkdir(parents=True, exist_ok=True)
        return
    # Existing user directories are never chmod'ed or stripped of their ACLs.
    for directory in reversed((path, *path.parents)):
        try:
            directory.mkdir(mode=0o700)
        except FileExistsError:
            check_chain(directory)
            continue
        descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            private_new_descriptor(descriptor, 0o700)
        finally:
            os.close(descriptor)


def quarantine_value(path):
    if sys.platform != "darwin":
        return None
    library = ctypes.CDLL(None, use_errno=True)
    library.getxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p,
                                ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
    library.getxattr.restype = ctypes.c_ssize_t
    encoded_path, name = os.fsencode(path), b"com.apple.quarantine"
    size = library.getxattr(encoded_path, name, None, 0, 0, 1)  # XATTR_NOFOLLOW
    if size < 0:
        error = ctypes.get_errno()
        if error in (getattr(errno, "ENOATTR", 93), errno.ENOTSUP):
            return None
        raise OSError(error, "Cannot inspect package quarantine")
    require(size <= 4096, "Package quarantine attribute exceeds its limit")
    value = ctypes.create_string_buffer(max(size, 1))
    returned = library.getxattr(encoded_path, name, value, size, 0, 1)
    require(returned == size, "Package quarantine changed during inspection")
    return value.raw[:size]


def set_quarantine(path, value):
    library = ctypes.CDLL(None, use_errno=True)
    library.setxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p,
                                ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
    library.setxattr.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(value)
    require(library.setxattr(os.fsencode(path), b"com.apple.quarantine", buffer, len(value), 0, 1) == 0,
            "Cannot preserve package quarantine")


@contextmanager
def mutation_lock(prefix):
    # This coordinates installer invocations only, not new Agent launches.
    create_directories(prefix.parent)
    prefix = check_chain(prefix)
    lock = prefix.with_name("." + prefix.name + ".codestudio-install.lock")
    try:
        descriptor = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        raise InstallError("Another installation operation or interrupted installer lock requires inspection") from None
    token = uuid.uuid4().hex.encode("ascii")
    try:
        with os.fdopen(descriptor, "wb") as stream:
            private_new_descriptor(stream.fileno(), 0o600)
            stream.write(token)
            stream.flush()
            os.fsync(stream.fileno())
        yield check_chain(prefix)
    finally:
        if lock.exists() and read_bounded(lock, 128) == token:
            lock.unlink()


def copy_verified(source, destination, package):
    destination.mkdir(mode=0o700)
    if sys.platform == "darwin":
        descriptor = os.open(destination, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            private_new_descriptor(descriptor, 0o700)
        finally:
            os.close(descriptor)
    root_quarantine = quarantine_value(source)
    for record in package["records"]:
        source_path = source / record["path"]
        target = destination / record["path"]
        create_directories(target.parent)
        regular_info(source_path)
        digest = hashlib.sha256()
        total = 0
        flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
        with os.fdopen(os.open(source_path, flags), "rb") as incoming, target.open("xb") as outgoing:
            private_new_descriptor(outgoing.fileno(), 0o600)
            for chunk in iter(lambda: incoming.read(1024 * 1024), b""):
                total += len(chunk)
                require(total <= record["size"], "Package changed while staging")
                digest.update(chunk)
                outgoing.write(chunk)
            outgoing.flush()
            os.fsync(outgoing.fileno())
        require(total == record["size"] and digest.hexdigest() == record["sha256"], "Package changed while staging")
        target.chmod(0o755 if record["executable"] else 0o644)
        quarantine = quarantine_value(source_path)
        if quarantine is None:
            quarantine = root_quarantine
        if quarantine is not None:
            set_quarantine(target, quarantine)


def remove_verified_tree(root, records):
    # No recursive delete: every unlink uses a previously verified exact file.
    # Refuse extra files rather than treating them as disposable application data.
    names = {record["path"] for record in records}
    found, directories = inventory(root)
    require(found == names and directories == expected_directories(names), "Unowned content prevents cleanup")
    for record in records:
        require(file_record(root, record["path"], record["executable"]) == record,
                "Modified content prevents cleanup")
    for record in records:
        path = root / record["path"]
        regular_info(path)
        path.unlink()
    for name in sorted(directories, key=lambda item: item.count("/"), reverse=True):
        (root / name).rmdir()
    root.rmdir()


def response(operation, status, prefix, package=None, **extra):
    result = {"product": PRODUCT, "operation": operation, "status": status, "prefix": str(prefix)}
    if package is not None:
        result.update(version=package["document"]["version"], package_sha256=package["package_sha256"],
                      executable=str(prefix / native_target()[2]))
    result.update(extra)
    return result


def publish_new_path(source, target):
    if sys.platform != "darwin":
        source.rename(target)
        return
    library = ctypes.CDLL(None, use_errno=True)
    library.renameatx_np.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                                    ctypes.c_char_p, ctypes.c_uint]
    library.renameatx_np.restype = ctypes.c_int
    # RENAME_EXCL also refuses an empty directory created after preflight.
    if library.renameatx_np(-2, os.fsencode(source), -2, os.fsencode(target), 4) != 0:
        raise OSError(ctypes.get_errno(), "Exclusive installation publication failed")


def install(operation, source, prefix):
    source = check_chain(source)
    require(not paths_overlap(source, prefix),
            "Package source and installation prefix must not overlap")
    package = verify_package(source)
    with mutation_lock(prefix) as prefix:
        previous = verify_install(prefix) if prefix.exists() else None
        if previous and previous["package_sha256"] == package["package_sha256"]:
            return response(operation, "unchanged", prefix, previous)
        require(operation != "upgrade" or previous is not None, "Installation is absent; use install")
        require(operation != "install" or previous is None, "A different package is installed; use upgrade")
        if previous:
            require(version_tuple(package["document"]["version"]) >= version_tuple(previous["document"]["version"]),
                    "Package downgrade is not permitted")
            require(not running_installed_executable(prefix / native_target()[2]), "Stop installed Agent processes before upgrade")
        stage = prefix.with_name("." + prefix.name + ".stage-" + uuid.uuid4().hex)
        backup = prefix.with_name("." + prefix.name + ".backup-" + uuid.uuid4().hex)
        staged_records = None
        try:
            copy_verified(source, stage, package)
            staged = verify_package(stage)
            receipt = {"format": FORMAT, "product": PRODUCT, "prefix": str(prefix),
                       "package_sha256": staged["package_sha256"], "files": staged["records"]}
            with (stage / RECEIPT).open("x", encoding="utf-8", newline="\n") as output:
                private_new_descriptor(output.fileno(), 0o600)
                json.dump(receipt, output, sort_keys=True, indent=2)
                output.write("\n")
                output.flush()
                os.fsync(output.fileno())
            (stage / RECEIPT).chmod(0o644)
            staged_records = verify_install(stage, prefix)["records"]
            if previous:
                verify_install(prefix)
                require(not running_installed_executable(prefix / native_target()[2]), "Stop installed Agent processes before upgrade")
                publish_new_path(prefix, backup)
            try:
                publish_new_path(stage, prefix)
            except BaseException:
                if previous:
                    try:
                        publish_new_path(backup, prefix)
                    except OSError:
                        raise InstallError("Package publication and rollback failed; the previous installation is retained for recovery",
                                           status="publication_incomplete", retained_path=str(backup), prefix=str(prefix)) from None
                raise
            if previous:
                try:
                    remove_verified_tree(backup, previous["records"])
                except (OSError, InstallError):
                    return response(operation, "upgraded", prefix, package,
                                    cleanup="previous_package_retained", retained_path=str(backup))
            return response(operation, "upgraded" if previous else "installed", prefix, package)
        except (OSError, InstallError):
            if stage.exists() and staged_records is None:
                raise InstallError("Package staging failed; retained files require inspection and the previous installation was not replaced",
                                   status="staging_incomplete", retained_path=str(stage), prefix=str(prefix)) from None
            raise
        finally:
            if stage.exists() and staged_records is not None:
                try:
                    remove_verified_tree(stage, staged_records)
                except (OSError, InstallError):
                    raise InstallError("Temporary package cleanup is incomplete; retained files require inspection",
                                       status="cleanup_incomplete", retained_path=str(stage), prefix=str(prefix)) from None


def uninstall(prefix):
    if not prefix.exists():
        return response("uninstall", "absent", prefix)
    with mutation_lock(prefix) as prefix:
        package = verify_install(prefix)
        require(not running_installed_executable(prefix / native_target()[2]), "Stop installed Agent processes before uninstall")
        removal = prefix.with_name("." + prefix.name + ".remove-" + uuid.uuid4().hex)
        publish_new_path(prefix, removal)
        try:
            # Recheck after the atomic rename. Unknown content is never deleted.
            verify_install(removal, prefix)
            remove_verified_tree(removal, package["records"])
        except BaseException:
            # Unlinks may already have succeeded. Never present a partially
            # removed directory as a restored usable installation. Preserve the
            # quarantine and explicitly report the incomplete cleanup instead.
            raise InstallError("Installation was removed from its active prefix but cleanup is incomplete; retained files require inspection",
                               status="cleanup_incomplete", retained_path=str(removal), prefix=str(prefix)) from None
        return response("uninstall", "uninstalled", prefix)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="operation", required=True)
    for name in ("install", "upgrade", "status", "uninstall"):
        command = commands.add_parser(name)
        command.add_argument("--prefix", type=Path)
        if name in {"install", "upgrade"}:
            command.add_argument("--package", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        native_target()
        prefix = checked_prefix(args.prefix if args.prefix is not None else default_prefix())
        if args.operation in {"install", "upgrade"}:
            result = install(args.operation, absolute_path(args.package), prefix)
        elif args.operation == "uninstall":
            result = uninstall(prefix)
        else:
            result = response("status", "installed", prefix, verify_install(prefix)) if prefix.exists() else response("status", "absent", prefix)
        print(json.dumps(result, ensure_ascii=True, indent=2))
        return 0
    except InstallError as error:
        message = str(error)
        failure = {"status": error.status, **error.details}
    except (OSError, ValueError, TypeError, KeyError, RuntimeError):
        message = "Installation filesystem or metadata operation failed; existing files were not authorized for removal"
        failure = {"status": "error"}
    print(json.dumps({"product": PRODUCT, "operation": args.operation, **failure, "error": message}, indent=2))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
