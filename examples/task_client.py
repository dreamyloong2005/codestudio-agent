"""Minimal parent-process client for CodeStudio-Agent's stdio task service.

Standard library only. The default reads one task-list page without starting a
task. --demo writes CODESTUDIO_DEMO.txt in the existing workspace, so use a
dedicated directory. It discovers the session and capabilities, verifies the
terminal operation receipt with a read-only lookup, then queries durable task
state. It never automatically resends a command after losing a response. Model
text, prompts, receipts and stderr are not printed.

python examples/task_client.py --agent build/windows/x64/release/codestudio-agent.exe \
    --workspace D:/YourExistingDemoDirectory --demo
"""
import argparse
from collections import deque
import json
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time


class ClientError(Exception):
    """A short, payload-free diagnostic suitable for display."""


ERROR_KINDS = frozenset({
    "parse_error", "invalid_request", "invalid_params", "method_not_found",
    "not_initialized", "already_initialized", "busy", "operation_not_found",
    "session_mismatch", "command_failed", "operation_failed", "internal_error",
})


class Client:
    def __init__(self, agent, workspace):
        self.deadline = time.monotonic() + 30
        self.process = subprocess.Popen(
            [str(agent), "serve", "--workspace", str(workspace)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        self.frames = queue.Queue(maxsize=128)
        self.overflow = threading.Event()
        self.next_id = 1
        self.session_id = None
        self.operation_id = None
        self.completion = None
        self.finished_tasks = deque(maxlen=256)
        self.shutdown_sent = False
        self.stdout_reader = threading.Thread(target=self._read_stdout, daemon=True)
        self.stderr_reader = threading.Thread(target=self._read_stderr, daemon=True)
        self.stdout_reader.start()
        self.stderr_reader.start()

    def _enqueue(self, frame):
        try:
            self.frames.put_nowait(frame)
        except queue.Full:
            # Keep draining the pipe even if the client cannot keep up.
            self.overflow.set()

    def _read_stdout(self):
        try:
            while line := self.process.stdout.readline(4 * 1024 * 1024 + 1):
                if len(line) > 4 * 1024 * 1024 or not line.endswith(b"\n"):
                    raise ClientError("Service output exceeded the frame limit.")
                frame = json.loads(line.decode("utf-8"))
                if not isinstance(frame, dict) or frame.get("jsonrpc") != "2.0":
                    raise ClientError("Service emitted an invalid protocol frame.")
                self._enqueue(frame)
        except (OSError, UnicodeError, ValueError, ClientError):
            self._enqueue(ClientError("Cannot decode the service output."))
        finally:
            self._enqueue(ClientError("Service output closed before the expected response."))

    def _read_stderr(self):
        # stderr is a separate diagnostic channel. Always drain it to avoid
        # deadlock; this example deliberately does not display sensitive text.
        try:
            while self.process.stderr.read(4096):
                pass
        except OSError:
            pass

    def remaining(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise ClientError("The 30-second client deadline expired.")
        return remaining

    def send(self, method, params=None):
        request_id = self.next_id
        self.next_id += 1
        frame = {"jsonrpc": "2.0", "id": request_id, "method": method,
                 "params": {} if params is None else params}
        try:
            self.process.stdin.write((json.dumps(frame) + "\n").encode("utf-8"))
            self.process.stdin.flush()
        except (OSError, ValueError) as error:
            raise ClientError("Cannot send a request to the service.") from error
        return request_id

    def receive(self):
        if self.overflow.is_set():
            raise ClientError("Client notification queue exceeded its limit.")
        try:
            frame = self.frames.get(timeout=self.remaining())
        except queue.Empty as error:
            raise ClientError("The 30-second client deadline expired.") from error
        if isinstance(frame, Exception):
            raise frame
        return frame

    def observe(self, frame):
        method = frame.get("method")
        params = frame.get("params", {})
        if method == "events/batch":
            for event in params.get("items", []):
                if event.get("kind") == "task.finished":
                    self.finished_tasks.append(event.get("task_id"))
        elif method == "operation/finished" and params.get("operation_id") == self.operation_id:
            self.completion = self.completion_metadata(params)
        elif method == "events/error":
            raise ClientError("The service could not continue the event subscription.")
        # operation/text is provisional display data; this small example ignores it.

    def completion_metadata(self, params):
        if (params.get("session_id") != self.session_id or
                params.get("operation_id") != self.operation_id):
            raise ClientError("Completion belongs to a different service session or operation.")
        result = params.get("result")
        if not isinstance(result, dict):
            result = {}
        # Retain only status metadata, never the prompt or tool payload.
        return {"exit_code": params.get("exit_code"), "has_error": "error" in params,
                "task_id": result.get("task_id"), "status": result.get("status")}

    def rpc(self, method, params=None):
        self.remaining()
        request_id = self.send(method, params)
        while True:
            frame = self.receive()
            if frame.get("id") == request_id:
                if "error" in frame:
                    error = frame["error"]
                    code = error.get("code")
                    kind = error.get("data", {}).get("kind")
                    # Do not echo the service's potentially sensitive message.
                    suffix = str(code) if isinstance(code, int) else "unknown"
                    kind = kind if kind in ERROR_KINDS else "unknown"
                    raise ClientError(f"Service rejected {method} ({kind}, code {suffix}); no command was retried.")
                if "result" not in frame:
                    raise ClientError("Service response has no result.")
                return frame["result"]
            self.observe(frame)

    def initialize(self):
        initialized = self.rpc("initialize", {"protocol_version": 2})
        if initialized.get("protocol_version") != 2:
            raise ClientError("The service protocol version does not match this example.")
        capabilities = initialized.get("capabilities", {})
        if not (capabilities.get("operation_lookup") and capabilities.get("structured_errors")):
            raise ClientError("The service lacks operation lookup or structured errors.")
        self.session_id = initialized.get("session_id")
        if not isinstance(self.session_id, str) or not self.session_id:
            raise ClientError("Initialization did not identify the service session.")
        return initialized

    def demo(self):
        self.rpc("events/subscribe", {"after": 0})
        accepted = self.rpc("tasks/start", {"mode": "demo", "options": {}})
        if (accepted.get("status") != "accepted" or not accepted.get("operation_id") or
                accepted.get("session_id") != self.session_id):
            raise ClientError("The demo operation was not accepted.")
        self.operation_id = accepted["operation_id"]
        # Completion and committed ledger notifications may arrive in either
        # order. Wait for both before querying the same task through the reader.
        while (self.completion is None or
               self.completion.get("task_id") not in self.finished_tasks):
            if self.completion is not None and not self.completion.get("task_id"):
                raise ClientError("The demo failed before creating a task.")
            self.observe(self.receive())
        if self.completion["exit_code"] != 0 or self.completion["status"] != "completed":
            raise ClientError("The demo did not complete successfully; inspect it with the CLI.")
        # The ID is scoped to this process session, and the cache is bounded.
        # A lookup failure after eviction/restart is not proof of no effects;
        # reconcile the durable task/events instead of resending tasks/start.
        retained = self.rpc("operations/get", {
            "session_id": self.session_id, "operation_id": self.operation_id,
        })
        if (retained.get("session_id") != self.session_id or
                retained.get("operation_id") != self.operation_id or
                retained.get("method") != "tasks/start" or retained.get("state") != "finished" or
                self.completion_metadata(retained.get("completion", {})) != self.completion):
            raise ClientError("The retained receipt did not confirm the operation completion.")
        detail = self.rpc("tasks/get", {"task_id": self.completion["task_id"]})
        if detail.get("status") != "completed" or not isinstance(detail.get("action_count"), int):
            raise ClientError("Task detail did not confirm successful completion.")
        print(f"Demo completed; session receipt and committed task verified; actions: {detail['action_count']}.")

    def shutdown(self):
        self.rpc("shutdown")
        self.shutdown_sent = True
        self.process.wait(timeout=self.remaining())
        if self.process.returncode != 0:
            raise ClientError("The service exited with an error.")

    def close(self):
        # Stop only the process this client owns. Graceful cancellation/EOF lets
        # the service record outcomes and release its locks before forced exit.
        try:
            if self.process.poll() is None:
                try:
                    if self.operation_id and self.completion is None:
                        self.send("operations/cancel", {"operation_id": self.operation_id})
                    if not self.shutdown_sent:
                        self.send("shutdown")
                except ClientError:
                    pass
                try:
                    self.process.stdin.close()
                except (OSError, ValueError):
                    pass
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
        finally:
            self.stdout_reader.join(timeout=2)
            self.stderr_reader.join(timeout=2)
            for pipe in (self.process.stdin, self.process.stdout, self.process.stderr):
                try:
                    pipe.close()
                except (OSError, ValueError):
                    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--agent", required=True, type=Path, help="Built agent executable, absolute or relative")
    parser.add_argument("--workspace", required=True, type=Path, help="Existing workspace directory")
    parser.add_argument("--demo", action="store_true", help="Write/read CODESTUDIO_DEMO.txt in this workspace")
    args = parser.parse_args()
    try:
        agent = args.agent.resolve(strict=True)
        workspace = args.workspace.resolve(strict=True)
    except OSError:
        parser.error("--agent and --workspace must already exist")
    if not agent.is_file() or not workspace.is_dir():
        parser.error("--agent must name a file and --workspace must name a directory")
    client = None
    try:
        client = Client(agent, workspace)
        client.initialize()
        tasks = client.rpc("tasks/list", {"limit": 20})
        print(f"Connected; current task-list page contains {len(tasks['items'])} task(s).")
        if args.demo:
            client.demo()
        client.shutdown()
        print("Service closed.")
        return 0
    except ClientError as error:
        print(f"Client error: {error}", file=sys.stderr)
        return 1
    except (OSError, subprocess.TimeoutExpired, KeyError, TypeError, AttributeError):
        print("Client error: service startup, protocol or shutdown failed.", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Client cancelled.", file=sys.stderr)
        return 130
    finally:
        if client is not None:
            client.close()


if __name__ == "__main__":
    raise SystemExit(main())
