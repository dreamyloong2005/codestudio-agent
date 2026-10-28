"""Supplemental private-Job ownership check during early child lifetime.

DEBUG_PROCESS observes the target child's creation event. The spawning host
may already have returned from CreateProcess and assigned its Job by the time
this event is handled, so this check does not prove atomic Job admission.
Checking the specific KILL_ON_JOB_CLOSE Job avoids false positives from a CI
runner's enclosing Job. Cleanup is limited to this debugger's own processes.

Usage: python tests/windows_spawn_lifetime.py path/to/codestudio-agent.exe
"""
import ctypes
from ctypes import wintypes as w
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

EXE = Path(sys.argv.pop(1)).resolve()
HOST = EXE.parent / "sandbox-resource-fixture.exe"
CHILD = EXE.parent / "process-fixture.exe"

if os.name == "nt":
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    native = ctypes.WinDLL("ntdll")
    ULONG_PTR = ctypes.c_size_t

    class HandleEntry(ctypes.Structure):
        _fields_ = [("handle", w.HANDLE), ("handle_count", ULONG_PTR),
                    ("pointer_count", ULONG_PTR), ("access", w.DWORD),
                    ("object_type", w.DWORD), ("attributes", w.DWORD),
                    ("reserved", w.DWORD)]

    class JobLimits(ctypes.Structure):
        _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                    ("flags", w.DWORD), ("minimum_set", ULONG_PTR),
                    ("maximum_set", ULONG_PTR), ("active_process_limit", w.DWORD),
                    ("affinity", ULONG_PTR), ("priority", w.DWORD),
                    ("scheduling_class", w.DWORD)]

    class ExtendedJobLimits(ctypes.Structure):
        _fields_ = [("basic", JobLimits), ("io_counters", ctypes.c_uint64 * 6),
                    ("process_memory_limit", ULONG_PTR), ("job_memory_limit", ULONG_PTR),
                    ("peak_process_memory", ULONG_PTR), ("peak_job_memory", ULONG_PTR)]

    class CreateProcessEvent(ctypes.Structure):
        _fields_ = [("file", w.HANDLE), ("process", w.HANDLE), ("thread", w.HANDLE),
                    ("base", w.LPVOID), ("debug_offset", w.DWORD), ("debug_size", w.DWORD),
                    ("thread_local", w.LPVOID), ("start", w.LPVOID),
                    ("image_name", w.LPVOID), ("unicode", w.WORD)]

    class ExceptionRecord(ctypes.Structure):
        _fields_ = [("code", w.DWORD), ("flags", w.DWORD), ("record", w.LPVOID),
                    ("address", w.LPVOID), ("count", w.DWORD),
                    ("information", ULONG_PTR * 15)]

    class ExceptionEvent(ctypes.Structure):
        _fields_ = [("record", ExceptionRecord), ("first_chance", w.DWORD)]

    class EventData(ctypes.Union):
        _fields_ = [("create", CreateProcessEvent), ("exception", ExceptionEvent),
                    ("file", w.HANDLE), ("exit_code", w.DWORD)]

    class DebugEvent(ctypes.Structure):
        _fields_ = [("code", w.DWORD), ("pid", w.DWORD), ("tid", w.DWORD),
                    ("data", EventData)]

    def api(name, result, *arguments):
        function = getattr(kernel, name)
        function.restype = result
        function.argtypes = arguments
        return function

    close_handle = api("CloseHandle", w.BOOL, w.HANDLE)
    current_process = api("GetCurrentProcess", w.HANDLE)
    open_process = api("OpenProcess", w.HANDLE, w.DWORD, w.BOOL, w.DWORD)
    duplicate_handle = api("DuplicateHandle", w.BOOL, w.HANDLE, w.HANDLE,
                           w.HANDLE, ctypes.POINTER(w.HANDLE), w.DWORD, w.BOOL, w.DWORD)
    query_job = api("QueryInformationJobObject", w.BOOL, w.HANDLE, ctypes.c_int,
                    w.LPVOID, w.DWORD, ctypes.POINTER(w.DWORD))
    in_job = api("IsProcessInJob", w.BOOL, w.HANDLE, w.HANDLE, ctypes.POINTER(w.BOOL))
    wait_event = api("WaitForDebugEvent", w.BOOL, ctypes.POINTER(DebugEvent), w.DWORD)
    continue_event = api("ContinueDebugEvent", w.BOOL, w.DWORD, w.DWORD, w.DWORD)
    terminate = api("TerminateProcess", w.BOOL, w.HANDLE, w.UINT)
    final_path = api("GetFinalPathNameByHandleW", w.DWORD, w.HANDLE, w.LPWSTR, w.DWORD, w.DWORD)
    process_image = api("QueryFullProcessImageNameW", w.BOOL, w.HANDLE, w.DWORD,
                        w.LPWSTR, ctypes.POINTER(w.DWORD))
    native.NtQueryInformationProcess.restype = ctypes.c_long
    native.NtQueryInformationProcess.argtypes = [w.HANDLE, ctypes.c_int, w.LPVOID,
                                                w.ULONG, ctypes.POINTER(w.ULONG)]

    def event_image(event):
        buffer = ctypes.create_unicode_buffer(32768)
        if event.data.create.file:
            count = final_path(event.data.create.file, buffer, len(buffer), 0)
            if not count:
                raise ctypes.WinError(ctypes.get_last_error())
            if count >= len(buffer):
                raise AssertionError("Debuggee image path exceeds bound")
        else:
            count = w.DWORD(len(buffer))
            if not process_image(event.data.create.process, 0, buffer, ctypes.byref(count)):
                raise ctypes.WinError(ctypes.get_last_error())
        path = buffer.value
        if path.startswith("\\\\?\\UNC\\"):
            path = "\\\\" + path[8:]
        elif path.startswith("\\\\?\\"):
            path = path[4:]
        return Path(path).resolve()

    def private_job_membership(host, child):
        """Snapshot only this host, with a strict handle/count allocation bound."""
        size = 4096
        while True:
            buffer = ctypes.create_string_buffer(size)
            required = w.ULONG()
            status = native.NtQueryInformationProcess(host, 51, buffer, size, ctypes.byref(required))
            if status == 0:
                break
            if status & 0xFFFFFFFF not in (0xC0000004, 0xC0000023):
                raise AssertionError(f"Process handle snapshot failed: 0x{status & 0xFFFFFFFF:08x}")
            size = max(size * 2, required.value)
            if size > 1024 * 1024:
                raise AssertionError("Fixture handle snapshot exceeded one MiB")
        count = ULONG_PTR.from_buffer(buffer).value
        offset = 2 * ctypes.sizeof(ULONG_PTR)
        if count > 4096 or offset + count * ctypes.sizeof(HandleEntry) > size:
            raise AssertionError("Invalid or excessive fixture handle count")
        memberships = []
        diagnostics = {}
        def record(entry, operation, outcome):
            key = (entry.object_type, operation, outcome)
            diagnostics[key] = diagnostics.get(key, 0) + 1
        for index in range(count):
            entry = HandleEntry.from_buffer(buffer, offset + index * ctypes.sizeof(HandleEntry))
            copy = w.HANDLE()
            if not duplicate_handle(host, entry.handle, current_process(), ctypes.byref(copy), 0, False, 2):
                error = ctypes.get_last_error()
                # Some non-Job handle types cannot be duplicated across
                # processes. Job handles support duplication; the assertion
                # below still requires exactly one private Job to be found.
                if error == 50:  # ERROR_NOT_SUPPORTED
                    record(entry, "duplicate_error", error)
                    continue
                raise ctypes.WinError(error)
            try:
                limits = ExtendedJobLimits()
                queried = query_job(copy, 9, ctypes.byref(limits), ctypes.sizeof(limits), None)
                record(entry, "job_flags" if queried else "query_error",
                       limits.basic.flags if queried else ctypes.get_last_error())
                if queried and limits.basic.flags & 0x2000:
                    member = w.BOOL()
                    if not in_job(child, copy, ctypes.byref(member)):
                        raise ctypes.WinError(ctypes.get_last_error())
                    memberships.append(bool(member.value))
            finally:
                close_handle(copy)
        if not memberships:
            raise AssertionError(f"No private Job found among {count} host handles; "
                                 f"limit structure sizes={ctypes.sizeof(JobLimits)}/{ctypes.sizeof(ExtendedJobLimits)}; "
                                 f"handle-type/query diagnostic counts={diagnostics}")
        return memberships


@unittest.skipUnless(os.name == "nt", "Windows early child Job ownership")
class SpawnLifetimeTests(unittest.TestCase):
    def check_spawn(self, mode):
        self.assertTrue(HOST.is_file(), "Build sandbox-resource-fixture")
        self.assertTrue(CHILD.is_file(), "Build process-fixture")
        with tempfile.TemporaryDirectory(prefix="codestudio-spawn-") as workspace:
            host = subprocess.Popen([str(HOST), mode, str(CHILD), workspace],
                                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL, creationflags=0x1 | 0x08000000)
            # PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION. Debug events own
            # their handles until EXIT_PROCESS has been continued; this handle
            # remains ours independently throughout cleanup.
            inspection = open_process(0x40 | 0x400, False, host.pid)
            # Track the initial process before receiving even its first event,
            # so an inspection-handle failure still has bounded cleanup.
            live = {host.pid: None}
            observed = None
            seen_images = []
            failure = None
            stopping = False
            deadline = time.monotonic() + 15
            try:
                if not inspection:
                    raise ctypes.WinError(ctypes.get_last_error())
                while True:
                    event = DebugEvent()
                    if not wait_event(ctypes.byref(event), 100):
                        if ctypes.get_last_error() != 121:
                            raise ctypes.WinError(ctypes.get_last_error())
                        if time.monotonic() >= deadline:
                            raise AssertionError(f"{mode}: debugger deadline; live PIDs {list(live)}")
                        continue
                    status = 0x00010002  # DBG_CONTINUE
                    try:
                        if event.code == 3:  # CREATE_PROCESS_DEBUG_EVENT
                            live[event.pid] = event.data.create.process
                            try:
                                image = event_image(event)
                            finally:
                                if event.data.create.file:
                                    close_handle(event.data.create.file)
                            if len(seen_images) < 16:
                                seen_images.append(str(image))
                            # Windows can create a console/loader helper before
                            # fixture main. Only the exact requested image is
                            # evidence about the product's process-spawn path.
                            if image == CHILD:
                                try:
                                    observed = private_job_membership(inspection, event.data.create.process)
                                except Exception as error:
                                    failure = error
                                stopping = True
                        elif event.code == 6:  # LOAD_DLL_DEBUG_EVENT
                            if event.data.file:
                                close_handle(event.data.file)
                        elif event.code == 5:  # EXIT_PROCESS_DEBUG_EVENT
                            live.pop(event.pid, None)
                            if event.pid == host.pid:
                                stopping = True
                        elif event.code == 1 and event.data.exception.record.code != 0x80000003:
                            status = 0x80010001  # DBG_EXCEPTION_NOT_HANDLED
                        if stopping:
                            # Terminate only processes observed in this debug
                            # session, including a suspended unassigned child.
                            for handle in live.values():
                                if handle:
                                    terminate(handle, 99)
                                else:
                                    host.kill()
                    finally:
                        if not continue_event(event.pid, event.tid, status):
                            raise ctypes.WinError(ctypes.get_last_error())
                    if stopping and not live:
                        break
                host.wait(timeout=5)
                if failure:
                    raise failure
                self.assertEqual(observed, [True],
                                 f"{mode}: child must belong to the host's single private "
                                 f"KILL_ON_JOB_CLOSE Job when observed in early lifetime; images={seen_images}")
            finally:
                if inspection:
                    close_handle(inspection)
                for handle in live.values():
                    if handle:
                        terminate(handle, 99)
                    else:
                        host.kill()
                # Do not leave paused debuggees after a test infrastructure
                # error. Drain their exits before releasing the Popen handle.
                if live:
                    cleanup_deadline = time.monotonic() + 5
                    while live and time.monotonic() < cleanup_deadline:
                        event = DebugEvent()
                        if not wait_event(ctypes.byref(event), 100):
                            continue
                        if event.code == 3:
                            live[event.pid] = event.data.create.process
                            if event.data.create.file:
                                close_handle(event.data.create.file)
                            terminate(event.data.create.process, 99)
                        elif event.code == 5:
                            live.pop(event.pid, None)
                        elif event.code == 6 and event.data.file:
                            close_handle(event.data.file)
                        continue_event(event.pid, event.tid, 0x00010002)
                if host.poll() is None:
                    host.kill()
                host.wait(timeout=5)

    def test_run_process_is_owned_in_early_lifetime(self):
        self.check_spawn("plain-run")

    def test_line_process_is_owned_in_early_lifetime(self):
        self.check_spawn("plain-line")


if __name__ == "__main__":
    unittest.main()
