"""Native macOS process observations used only by integration fixtures."""
import ctypes
import os
import pathlib
import select
import signal


class BsdInfo(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in (
        "flags", "status", "xstatus", "pid", "ppid", "uid", "gid", "ruid", "rgid", "svuid", "svgid", "reserved")]
    _fields_ += [("comm", ctypes.c_char * 16), ("name", ctypes.c_char * 32)]
    _fields_ += [(name, ctypes.c_uint32) for name in ("nfiles", "pgid", "pjobc", "tdev", "tpgid")]
    _fields_ += [("nice", ctypes.c_int32), ("start_seconds", ctypes.c_uint64), ("start_microseconds", ctypes.c_uint64)]


class VnodeStat(ctypes.Structure):
    _fields_ = [("device", ctypes.c_uint32), ("mode", ctypes.c_uint16), ("nlink", ctypes.c_uint16),
                ("inode", ctypes.c_uint64), ("uid", ctypes.c_uint32), ("gid", ctypes.c_uint32)]
    _fields_ += [(name, ctypes.c_int64) for name in ("atime", "atimens", "mtime", "mtimens", "ctime", "ctimens",
                                                 "birthtime", "birthtimens", "size", "blocks")]
    _fields_ += [("blksize", ctypes.c_int32), ("flags", ctypes.c_uint32), ("gen", ctypes.c_uint32),
                ("rdev", ctypes.c_uint32), ("spare", ctypes.c_int64 * 2)]


class VnodeInfo(ctypes.Structure):
    _fields_ = [("stat", VnodeStat), ("type", ctypes.c_int), ("pad", ctypes.c_int), ("fsid", ctypes.c_int32 * 2)]


class FileInfo(ctypes.Structure):
    _fields_ = [("flags", ctypes.c_uint32), ("status", ctypes.c_uint32), ("offset", ctypes.c_int64),
                ("type", ctypes.c_int32), ("guard", ctypes.c_uint32)]


class VnodePath(ctypes.Structure):
    _fields_ = [("file", FileInfo), ("vnode", VnodeInfo), ("path", ctypes.c_char * 1024)]


class FdInfo(ctypes.Structure):
    _fields_ = [("fd", ctypes.c_int32), ("type", ctypes.c_uint32)]


libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
libproc.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
libproc.proc_pidinfo.restype = ctypes.c_int
libproc.proc_pidfdinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
libproc.proc_pidfdinfo.restype = ctypes.c_int
libproc.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
libproc.proc_pidpath.restype = ctypes.c_int


def process_info(pid):
    info = BsdInfo()
    if libproc.proc_pidinfo(pid, 3, 0, ctypes.byref(info), ctypes.sizeof(info)) != ctypes.sizeof(info):
        raise ProcessLookupError(pid)
    if info.pid != pid or not info.start_seconds:
        raise AssertionError("Invalid native process identity")
    return info


def birth(info):
    return info.start_seconds, info.start_microseconds


def process_identity(pid):
    system = ctypes.CDLL(None, use_errno=True)
    system.sysctlbyname.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t),
                                   ctypes.c_void_p, ctypes.c_size_t]
    buffer = ctypes.create_string_buffer(64)
    size = ctypes.c_size_t(len(buffer))
    if system.sysctlbyname(b"kern.bootsessionuuid", buffer, ctypes.byref(size), None, 0) != 0:
        raise OSError(ctypes.get_errno(), "sysctlbyname")
    info = process_info(pid)
    return f"darwin:{buffer.value.decode().lower()}:{info.start_seconds * 1000000 + info.start_microseconds}"


def vnode_paths(pid):
    """Snapshot native vnode descriptors; disappearing entries are harmless."""
    size = libproc.proc_pidinfo(pid, 1, 0, None, 0)
    if size <= 0:
        raise OSError(ctypes.get_errno(), "proc_pidinfo descriptor count")
    entries = (FdInfo * (size // ctypes.sizeof(FdInfo) + 64))()
    size = libproc.proc_pidinfo(pid, 1, 0, entries, ctypes.sizeof(entries))
    if size <= 0 or size >= ctypes.sizeof(entries):
        raise AssertionError("Could not obtain complete native descriptor snapshot")
    paths = {}
    for entry in entries[:size // ctypes.sizeof(FdInfo)]:
        if entry.type != 1:
            continue
        info = VnodePath()
        count = libproc.proc_pidfdinfo(pid, entry.fd, 2, ctypes.byref(info), ctypes.sizeof(info))
        if count == ctypes.sizeof(info):
            paths[entry.fd] = os.fsdecode(info.path)
        elif entry.fd in (0, 1, 2):
            raise AssertionError("Cannot inspect standard stream")
    return paths


class ProcessWatch:
    """Bind exit observation to a live process, then validate path and birth.

    macOS exposes no pidfd signal operation. Test signals additionally recheck
    the birth identity just before kill; this helper never accepts ledger PIDs
    without validating the expected executable and, where known, direct parent.
    """
    def __init__(self, pid):
        self.pid = pid
        self.queue = select.kqueue()
        self.exited = False
        try:
            self.queue.control([select.kevent(pid, filter=select.KQ_FILTER_PROC,
                flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT, fflags=select.KQ_NOTE_EXIT)], 0)
            self.info = process_info(pid)
            self.birth = birth(self.info)
            path = ctypes.create_string_buffer(4096)
            if libproc.proc_pidpath(pid, path, len(path)) <= 0:
                raise ProcessLookupError(pid)
            self.executable = pathlib.Path(os.fsdecode(path.value))
            if not self.alive():
                raise ProcessLookupError(pid)
        except BaseException:
            self.close()
            raise

    def alive(self):
        if not self.exited:
            self.exited = bool(self.queue.control([], 1, 0))
        return not self.exited

    def signal(self, value):
        if not self.alive():
            return
        current = process_info(self.pid)
        if birth(current) != self.birth:
            raise AssertionError("Refusing to signal a reused process identity")
        if self.alive():
            os.kill(self.pid, value)

    def kill(self):
        self.signal(signal.SIGKILL)

    def close(self):
        self.queue.close()
