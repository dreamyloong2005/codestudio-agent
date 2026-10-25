"""Owned loopback Streamable HTTP MCP peer; never contacts an external server."""
import copy
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


PROTOCOL = "2025-11-25"
CURSOR = "cursor-1"
UNICODE_CURSOR = "cursor-一"
PING = {"jsonrpc": "2.0", "id": "server-ping", "method": "ping"}
UNSUPPORTED = {"jsonrpc": "2.0", "id": "server-unsupported", "method": "sampling/createMessage",
               "params": {"messages": [], "maxTokens": 10}}


def event(message, event_id=None, newline="\n"):
    lines = (["id: " + event_id] if event_id is not None else [])
    lines += ["event: message", "data: " + json.dumps(message, ensure_ascii=False)]
    return (newline.join(lines) + newline + newline).encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def record(self, body=None):
        record = {"verb": self.command, "path": self.path,
                  "headers": {key.lower(): value for key, value in self.headers.items()},
                  "body": body, "at": time.monotonic()}
        with self.server.guard:
            self.server.records.append(record)
        return record

    def respond(self, body=b"", status=200, content_type="application/json", headers=None,
                fragmented=False, truncated=False, wait=False):
        if not isinstance(body, bytes):
            body = json.dumps(body, ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(status)
            if content_type:
                self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body) + (100 if truncated or wait else 0)))
            self.send_header("Connection", "close")
            for key, value in (headers or {}).items():
                self.send_header(key, value)
            self.end_headers()
            step = 1 if fragmented else max(len(body), 1)
            for offset in range(0, len(body), step):
                self.wfile.write(body[offset:offset + step])
                self.wfile.flush()
            if wait:
                self.server.release.wait(10)
        except (ConnectionError, OSError):
            pass
        finally:
            self.close_connection = True

    def rpc_result(self, request, result):
        return {"jsonrpc": "2.0", "id": request["id"], "result": result}

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
        except (ValueError, UnicodeError):
            self.record()
            self.respond(status=400)
            return
        self.record(body)
        mode = self.server.mode
        method = body.get("method")
        if method == "notifications/cancelled":
            self.server.release.set()
            self.respond(status=202)
            return
        if method == "initialize":
            if mode == "unauthorized":
                self.respond({"error": "authentication required"}, status=401)
                return
            if mode == "redirect":
                self.respond(status=307, headers={"Location": self.server.url + "/sink"})
                return
            if mode == "hang-initialize":
                self.respond(b": waiting\n\n", content_type="text/event-stream", wait=True)
                return
            with self.server.guard:
                self.server.initializations += 1
                session = "remote-session-" + str(self.server.initializations)
            headers = {} if mode == "stateless" else {"Mcp-Session-Id": session}
            if mode == "invalid-session":
                headers["Mcp-Session-Id"] = "invalid session"
            result = {"protocolVersion": "1900-01-01" if mode == "invalid-protocol" else PROTOCOL,
                      "capabilities": {"tools": {}},
                      "serverInfo": {"name": "owned-http-fixture", "version": "2"}}
            self.respond(self.rpc_result(body, result), headers=headers,
                         content_type="text/plain" if mode == "invalid-content-type" else "application/json")
            return
        if method == "notifications/initialized" or method is None:
            self.respond(status=202)
            return
        if method == "ping":
            self.respond(self.rpc_result(body, {}))
            return
        if method == "tools/list":
            schema = {"type": "object", "properties": {"text": {"type": "string"}},
                      "required": ["text"], "additionalProperties": False}
            if mode == "invalid-schema":
                schema = {"type": "unrecognized-type"}
            tools = [{"name": "echo", "description": "Return the provided text", "inputSchema": schema},
                     {"name": "hidden", "description": "Never granted", "inputSchema": schema}]
            if mode == "schema-secret":
                tools[0]["description"] = "Reflected credential " + self.server.output_text
            response = self.rpc_result(body, {"tools": tools})
            if mode == "batch":
                response = [response]
            if mode == "oversized":
                response["result"]["padding"] = "x" * (2 * 1024 * 1024)
            self.respond(response)
            return
        if method != "tools/call":
            self.respond({"jsonrpc": "2.0", "id": body.get("id"),
                          "error": {"code": -32601, "message": "Unknown method"}})
            return
        with self.server.guard:
            self.server.calls += 1
            count = self.server.calls
        text = self.server.output_text
        if text is None:
            text = body.get("params", {}).get("arguments", {}).get("text", "")
        result = {"content": [{"type": "text", "text": text}],
                  "structuredContent": {"text": text, "calls": count}}
        response = self.rpc_result(body, result)
        with self.server.guard:
            self.server.pending = response
        if mode == "session-expired":
            self.respond(status=404)
        elif mode == "call-unauthorized":
            self.respond({"error": "credential no longer authorized"}, status=401)
        elif mode == "session-changed":
            self.respond(response, headers={"Mcp-Session-Id": "unexpected-new-session"})
        elif mode == "disconnect":
            self.respond(b"", content_type="text/event-stream", truncated=True)
        elif mode == "timeout":
            self.respond(b": waiting\n\n", content_type="text/event-stream", wait=True)
        elif mode == "malformed-json":
            self.respond(b'{"jsonrpc":')
        elif mode == "wrong-id":
            response["id"] = "not-the-request"
            self.respond(response)
        elif mode == "tool-error":
            response["result"]["isError"] = True
            self.respond(response)
        elif mode == "invalid-result":
            response["result"] = {"content": "not-an-array"}
            self.respond(response)
        elif mode == "rpc-error":
            self.respond({"jsonrpc": "2.0", "id": body["id"],
                          "error": {"code": -32603, "message": "fixture error"}})
        elif mode == "sse":
            # CR, LF and CRLF framing, a BOM, comment, retry and an id-only
            # primer precede a JSON document spanning several data fields.
            lines = json.dumps(response, ensure_ascii=False, indent=2).splitlines()
            data = b"\xef\xbb\xbf: comment\r\nretry: 120\r\nid: primer\r\ndata:\r\n\r\n"
            data += ("event: message\rid: result\r" + "\r".join("data: " + line for line in lines) + "\r\r").encode()
            self.respond(data, content_type="text/event-stream; charset=utf-8", fragmented=True)
        elif mode == "ping":
            self.respond(event(PING, "ping") + event(response, "done"), content_type="text/event-stream")
        elif mode == "unsupported-request":
            self.respond(event(UNSUPPORTED, "unsupported") + event(response, "done"), content_type="text/event-stream")
        elif mode == "list-changed":
            changed = {"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}
            self.respond(event(changed, "changed") + event(response, "done"), content_type="text/event-stream")
        elif mode in ("resume", "resume-exhausted", "retry-deadline"):
            retry = 3000 if mode == "retry-deadline" else self.server.retry_ms
            data = ("retry: " + str(retry) + "\n\n").encode() + event(PING, self.server.cursor)
            self.respond(data, content_type="text/event-stream", truncated=True)
        else:
            self.respond(response)

    def do_GET(self):
        self.record()
        with self.server.guard:
            response = copy.deepcopy(self.server.pending)
            self.server.gets += 1
            count = self.server.gets
        if self.server.mode == "resume-exhausted":
            notice = {"jsonrpc": "2.0", "method": "notifications/message",
                      "params": {"level": "info", "data": "Still pending"}}
            self.respond(event(notice, "next-" + str(count)), content_type="text/event-stream", truncated=True)
        else:
            # The original cursor event is deliberately replayed. A correct
            # client acknowledges the server ping only once across reconnect.
            self.respond(event(PING, self.server.cursor) + event(response, "done"), content_type="text/event-stream")

    def do_DELETE(self):
        self.record()
        self.respond(status=self.server.delete_status)


class RemoteMcp:
    def __init__(self):
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.server.guard = threading.Lock()
        self.server.release = threading.Event()
        self.server.records = []
        self.server.calls = 0
        self.server.gets = 0
        self.server.initializations = 0
        self.server.pending = None
        self.server.output_text = None
        self.server.mode = "json"
        self.server.retry_ms = 120
        self.server.cursor = CURSOR
        self.server.delete_status = 204
        self.url = f"http://127.0.0.1:{self.server.server_port}/mcp"
        self.server.url = self.url
        self.worker = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.02}, daemon=True)
        self.worker.start()

    def records(self, method=None, verb=None):
        with self.server.guard:
            records = copy.deepcopy(self.server.records)
        return [record for record in records
                if (verb is None or record["verb"] == verb)
                and (method is None or (record["body"] or {}).get("method") == method)]

    def close(self):
        self.server.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.worker.join(timeout=5)
