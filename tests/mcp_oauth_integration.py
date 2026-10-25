"""Real MCP OAuth CLI/runtime against owned loopback authorization fixtures.

Only randomly named synthetic native credentials are written, and all are
removed. Linux success paths require the existing private Secret Service test
wrapper. No browser, real account, remote provider or live model is used.
"""
import base64
import copy
import hashlib
import json
import os
import queue
import sqlite3
import subprocess
import sys
import threading
import time
import unittest
import uuid
from contextlib import closing
from http.server import ThreadingHTTPServer
from urllib.error import HTTPError
from urllib.parse import parse_qs, urlencode, urlsplit
from urllib.request import urlopen

import resume_integration as fixture
from mcp_http_fixture import Handler as McpHandler, PROTOCOL, CURSOR

_arguments = sys.argv[:]
sys.argv[:] = [_arguments[0], str(fixture.EXE)]
try:
    import service_integration as service_fixture
finally:
    sys.argv[:] = _arguments


class OAuthHandler(McpHandler):
    def do_GET(self):
        self.record()
        if "oauth-protected-resource" in self.path:
            metadata = {"resource": self.server.url,
                        "authorization_servers": [self.server.issuer],
                        "scopes_supported": ["tools.read", "tools.write", "admin"]}
            metadata.update(self.server.resource_override)
            if self.server.discovery_redirect:
                self.respond(status=302, headers={"Location": self.server.origin + "/metadata-sink"})
            else:
                self.respond(metadata)
        elif "oauth-authorization-server" in self.path or "openid-configuration" in self.path:
            metadata = {"issuer": self.server.issuer,
                        "authorization_endpoint": self.server.origin + "/authorize",
                        "token_endpoint": self.server.origin + "/token",
                        "response_types_supported": ["code"],
                        "grant_types_supported": ["authorization_code", "refresh_token"],
                        "token_endpoint_auth_methods_supported": ["none"],
                        "code_challenge_methods_supported": ["S256"],
                        "authorization_response_iss_parameter_supported": True}
            metadata.update(self.server.issuer_override)
            self.respond(metadata)
        elif self.path == "/mcp":
            self.respond(status=401, headers={"WWW-Authenticate":
                'Bearer resource_metadata="' + self.server.origin + '/.well-known/oauth-protected-resource/mcp"'})
        else:
            self.respond(status=404)

    def do_POST(self):
        if self.path != "/token":
            authorization = self.headers.get("Authorization")
            if authorization != "Bearer " + self.server.access:
                try:
                    body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
                except (ValueError, UnicodeError):
                    body = None
                self.record(body)
                self.respond({"error": "invalid_token"}, status=401)
                return
            return super().do_POST()
        encoded = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        body = parse_qs(encoded.decode(), keep_blank_values=True)
        self.record(body)
        with self.server.guard:
            self.server.token_calls.append(copy.deepcopy(body))
        if self.server.token_release is not None:
            self.server.token_release.wait(10)
        if self.server.token_error:
            self.respond({"error": "invalid_grant", "error_description": self.server.refresh}, status=400)
            return
        grant = body.get("grant_type", [None])[0]
        if grant == "authorization_code":
            authorization = self.server.authorization
            verifier = body.get("code_verifier", [""])[0]
            challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip("=")
            valid = (body.get("code") == [self.server.code] and authorization and
                     challenge == authorization.get("code_challenge", [None])[0] and
                     body.get("redirect_uri") == authorization.get("redirect_uri") and
                     body.get("client_id") == ["fixture-client"] and
                     body.get("resource") == [self.server.url])
        elif grant == "refresh_token":
            valid = (body.get("refresh_token") == [self.server.refresh] and
                     body.get("client_id") == ["fixture-client"] and
                     body.get("resource") == [self.server.url])
            if valid:
                self.server.access = self.server.next_access
                self.server.refresh = self.server.next_refresh
        else:
            valid = False
        if not valid:
            self.respond({"error": "invalid_grant"}, status=400)
            return
        result = {"access_token": self.server.access, "refresh_token": self.server.refresh,
                  "token_type": "Bearer", "expires_in": self.server.expires_in,
                  "scope": "tools.read"}
        result.update(self.server.token_override)
        self.respond(result)


class OAuthRemote:
    def __init__(self):
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), OAuthHandler)
        self.server.daemon_threads = True
        self.server.guard = threading.Lock()
        self.server.release = threading.Event()
        self.server.records = []
        self.server.calls = self.server.gets = self.server.initializations = 0
        self.server.pending = self.server.output_text = None
        self.server.mode = "json"
        self.server.retry_ms = 120
        self.server.cursor = CURSOR
        self.server.delete_status = 204
        self.server.origin = f"http://127.0.0.1:{self.server.server_port}"
        self.server.url = self.server.origin + "/mcp"
        self.server.issuer = self.server.origin + "/issuer"
        self.server.code = "authorization-code-" + uuid.uuid4().hex
        self.server.access = "oauth-access-" + uuid.uuid4().hex
        self.server.refresh = "oauth-refresh-" + uuid.uuid4().hex
        self.server.next_access = "oauth-next-access-" + uuid.uuid4().hex
        self.server.next_refresh = "oauth-next-refresh-" + uuid.uuid4().hex
        self.server.authorization = None
        self.server.resource_override = {}
        self.server.issuer_override = {}
        self.server.token_override = {}
        self.server.discovery_redirect = False
        self.server.token_error = False
        self.server.token_release = None
        self.server.token_calls = []
        self.server.expires_in = 3600
        self.url = self.server.url
        self.worker = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.02}, daemon=True)
        self.worker.start()

    def records(self, method=None, verb=None):
        with self.server.guard:
            records = copy.deepcopy(self.server.records)
        return [entry for entry in records if (verb is None or entry["verb"] == verb)
                and (method is None or (entry["body"] or {}).get("method") == method)]

    def close(self):
        self.server.release.set()
        if self.server.token_release:
            self.server.token_release.set()
        self.server.shutdown()
        self.server.server_close()
        self.worker.join(timeout=5)


class McpOAuthTests(unittest.TestCase):
    run_args = fixture.ResumeTests.run_args
    resume_args = fixture.ResumeTests.resume_args
    setup_responses = fixture.ResumeTests.setup_responses
    requests = fixture.ResumeTests.requests
    sql_rows = fixture.ResumeTests.sql_rows
    wait_for = fixture.ResumeTests.wait_for
    kill = staticmethod(fixture.ResumeTests.kill)

    def setUp(self):
        fixture.ResumeTests.setUp(self)
        self.workspace = self.workspace.resolve()
        self.database = self.workspace / ".codestudio/state.sqlite3"
        self.remote = OAuthRemote()
        self.name = "test-credential-oauth-" + uuid.uuid4().hex
        self.reference = "system:" + self.name
        self.manifest = self.workspace / "oauth-mcp.json"
        self.secrets = [fixture.TOKEN, self.remote.server.access, self.remote.server.refresh,
                        self.remote.server.next_access, self.remote.server.next_refresh, self.remote.server.code]
        self.login_processes = []
        self.clients = []
        self.configure()

    def tearDown(self):
        errors = []
        def attempt(action):
            try:
                action()
            except Exception as error:
                errors.append(error)

        # Stop token holders before attempting credential removal, including
        # when an assertion failed while a refresh/login was still pending.
        for process, _, _, thread in self.login_processes:
            def close_login(process=process, thread=thread):
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=5)
                thread.join(timeout=5)
                self.assertFalse(thread.is_alive(), "Authorization notice reader did not stop")
                for pipe in (process.stdout, process.stderr):
                    if not pipe.closed:
                        pipe.close()
            attempt(close_login)
        for process in self.children:
            attempt(lambda process=process: self.kill(process))
        self.children.clear()
        for client in self.clients:
            def close_client(client=client):
                client.close()
                for secret in self.secrets:
                    self.assertNotIn(secret.encode(), bytes(client.raw) + bytes(client.stderr))
            attempt(close_client)
        attempt(self.remote.close)

        def delete_oauth():
            # OAuth occupies a namespace separate from ordinary Bearer secrets.
            # Restore the original binding before removing this test's record.
            self.configure()
            result = subprocess.run([str(fixture.EXE), *map(str, self.auth_args("logout"))],
                                    env=self.env, capture_output=True, timeout=15)
            data = json.loads(result.stdout)
            self.assertIn(data.get("state"), ("deleted", "missing", "locked", "unavailable"), result.stdout)
            for secret in self.secrets:
                self.assertNotIn(secret.encode(), result.stdout + result.stderr)
        def delete_ordinary():
            result = subprocess.run([str(fixture.EXE), "credential-delete", "--name", self.name],
                                    env=self.env, capture_output=True, timeout=15)
            self.assertIn(json.loads(result.stdout).get("state"), ("deleted", "missing", "locked", "unavailable"))
        attempt(delete_oauth)
        attempt(delete_ordinary)
        attempt(lambda: fixture.ResumeTests.tearDown(self))
        if errors:
            for error in errors[1:]:
                errors[0].add_note("Additional cleanup error: " + str(error))
            raise errors[0]

    def configure(self, **overrides):
        server = {"id": "oauth", "transport": "streamable-http", "url": self.remote.url,
                  "allow_http_loopback": True, "tools": ["echo"], "timeout_ms": 1500,
                  "oauth": {"issuer": self.remote.server.issuer, "client_id": "fixture-client",
                            "credential": self.reference, "scopes": ["tools.read"]}}
        server.update(overrides)
        self.server_config = server
        self.manifest.write_text(json.dumps({"version": 2, "servers": [server]}), encoding="utf-8")
        return server

    def capture(self, *args, env=None, input=None):
        result = subprocess.run([str(fixture.EXE), *map(str, args)], cwd=self.workspace,
                                env=self.env if env is None else env, input=input, capture_output=True, timeout=20)
        for secret in self.secrets:
            self.assertNotIn(secret.encode(), result.stdout + result.stderr)
        return result

    def invoke(self, *args, expected=0, **kwargs):
        result = self.capture(*args, **kwargs)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def auth_args(self, operation):
        return ["mcp-auth-" + operation, "--mcp-config", self.manifest, "--server", "oauth"]

    def auth(self, operation, expected=0):
        return self.invoke(*self.auth_args(operation), expected=expected)

    def grants(self, grant=True):
        return ["--mcp-config", self.manifest, *(["--allow-mcp", "oauth"] if grant else [])]

    def discover(self, expected=0):
        return self.invoke("mcp-tools", "--workspace", self.workspace, *self.grants(), expected=expected)

    def require_store(self):
        # Native-store availability is checked without leaving a credential.
        probe = self.capture("credential-set", "--name", self.name, "--stdin", input=b"synthetic-probe\n")
        state = json.loads(probe.stdout).get("state")
        if os.name != "nt" and state in ("locked", "unavailable"):
            self.assertEqual(probe.returncode, 2)
            self.skipTest("private Linux Secret Service is " + state)
        self.assertEqual(probe.returncode, 0, probe.stdout + probe.stderr)
        self.invoke("credential-delete", "--name", self.name)

    def start_login(self, *extra):
        process = subprocess.Popen([str(fixture.EXE), *map(str, [*self.auth_args("login"), *extra])],
                                   cwd=self.workspace, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        events = queue.Queue()
        raw = bytearray()
        def reader():
            for line in process.stderr:
                raw.extend(line)
                try:
                    events.put(json.loads(line))
                except ValueError:
                    events.put({"invalid_line": line.decode(errors="replace")})
            events.put(None)
        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        self.login_processes.append((process, events, raw, thread))
        deadline = time.monotonic() + 8
        notice = None
        while time.monotonic() < deadline:
            try:
                event = events.get(timeout=0.2)
            except queue.Empty:
                continue
            if event is None:
                break
            if event.get("type") == "authorization":
                notice = event
                break
        if notice is None:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            thread.join(timeout=5)
            output = process.stdout.read()
            process.stdout.close()
            process.stderr.close()
            self.fail(f"No authorization notice: rc={process.poll()}, stdout={output!r}, stderr={bytes(raw)!r}")
        query = parse_qs(urlsplit(notice["authorization_url"]).query, keep_blank_values=True)
        self.remote.server.authorization = query
        return process, notice, query

    def callback(self, notice, query, **overrides):
        params = {"code": self.remote.server.code, "state": query["state"][0], "iss": self.remote.server.issuer}
        params.update(overrides)
        params = {key: value for key, value in params.items() if value is not None}
        url = notice["redirect_uri"] + "?" + urlencode(params)
        try:
            with urlopen(url, timeout=5) as response:
                return response.status
        except HTTPError as response:
            response.read()
            return response.code

    def finish_login(self, process, expected=0):
        process.wait(timeout=10)
        output = process.stdout.read()
        _, _, raw, thread = next(item for item in self.login_processes if item[0] is process)
        thread.join(timeout=5)
        process.stdout.close()
        process.stderr.close()
        self.assertFalse(thread.is_alive(), "Authorization notice reader did not stop")
        self.assertEqual(process.returncode, expected, output + bytes(raw))
        for secret in self.secrets:
            self.assertNotIn(secret.encode(), output + bytes(raw))
        return json.loads(output)

    def service(self):
        client = service_fixture.Client(self.workspace, self.env)
        self.clients.append(client)
        client.initialize()
        return client

    def login(self, expires_in=3600):
        self.require_store()
        self.remote.server.expires_in = expires_in
        process, notice, query = self.start_login()
        self.assertLess(self.callback(notice, query), 300)
        result = self.finish_login(process)
        self.assertEqual(result["state"], "available")
        return notice, query

    def assert_secrets_absent(self):
        for path in self.workspace.rglob("*"):
            if path.is_file():
                for secret in self.secrets:
                    self.assertNotIn(secret.encode(), path.read_bytes(), str(path))
        if self.database.exists():
            with closing(sqlite3.connect(self.database)) as database:
                dump = "\n".join(database.iterdump())
            for secret in self.secrets:
                self.assertNotIn(secret, dump)

    def test_discovery_uses_pinned_resource_issuer_and_no_credentials(self):
        self.auth("discover")
        records = self.remote.records()
        self.assertTrue(any("oauth-protected-resource" in item["path"] for item in records))
        self.assertTrue(any("oauth-authorization-server" in item["path"] for item in records))
        self.assertTrue(all("authorization" not in item["headers"] for item in records))
        self.assertEqual(self.remote.server.token_calls, [])
        self.assertFalse(self.database.exists())

    def test_discovery_rejects_resource_and_issuer_mixup_before_token_or_mcp(self):
        for target, field, value in (("resource_override", "resource", self.remote.server.origin + "/other"),
                                     ("resource_override", "authorization_servers", [self.remote.server.origin + "/wrong"]),
                                     ("issuer_override", "issuer", self.remote.server.origin + "/wrong"),
                                     ("issuer_override", "code_challenge_methods_supported", ["plain"])):
            with self.subTest(field=field):
                setattr(self.remote.server, target, {field: value})
                self.auth("discover", expected=2)
                setattr(self.remote.server, target, {})
                self.assertEqual(self.remote.server.token_calls, [])
                self.assertEqual(self.remote.records("initialize"), [])

    def test_metadata_redirect_is_not_followed(self):
        self.remote.server.discovery_redirect = True
        self.auth("discover", expected=2)
        self.assertFalse(any(item["path"] == "/metadata-sink" for item in self.remote.records()))
        self.assertEqual(self.remote.server.token_calls, [])

    def test_invalid_oauth_fields_refuse_before_network(self):
        original = copy.deepcopy(self.server_config)
        variants = [dict(original, credential="env:CODESTUDIO_API_KEY"),
                    dict(original, allow_http_loopback=False),
                    dict(original, oauth=dict(original["oauth"], credential="env:CODESTUDIO_API_KEY")),
                    dict(original, oauth=dict(original["oauth"], scopes=["tools.read", "tools.read"])),
                    dict(original, oauth=dict(original["oauth"], client_secret="not-supported"))]
        for server in variants:
            with self.subTest(server=server):
                self.manifest.write_text(json.dumps({"version": 2, "servers": [server]}), encoding="utf-8")
                self.auth("discover", expected=2)
                self.assertEqual(self.remote.records(), [])

    def test_login_pkce_state_resource_and_scope_are_exact_and_tokens_are_private(self):
        notice, query = self.login()
        self.assertEqual(query["response_type"], ["code"])
        self.assertEqual(query["client_id"], ["fixture-client"])
        self.assertEqual(query["resource"], [self.remote.url])
        self.assertEqual(query["scope"], ["tools.read"])
        self.assertEqual(query["code_challenge_method"], ["S256"])
        self.assertGreaterEqual(len(query["state"][0]), 32)
        self.assertEqual(query["redirect_uri"], [notice["redirect_uri"]])
        redirect = urlsplit(notice["redirect_uri"])
        self.assertEqual((redirect.scheme, redirect.hostname, redirect.path), ("http", "127.0.0.1", "/oauth/callback"))
        self.assertEqual(len(self.remote.server.token_calls), 1)
        token_request = self.remote.server.token_calls[0]
        self.assertEqual(token_request["grant_type"], ["authorization_code"])
        self.assertNotIn("client_secret", token_request)
        self.assertNotIn("code_verifier", query)
        self.assertEqual(self.auth("status")["state"], "available")
        self.assertEqual(self.invoke("credential-status", "--credential", self.reference)["state"], "missing")
        self.discover()
        self.assertTrue(all(item["headers"].get("authorization") == "Bearer " + self.remote.server.access
                            for item in self.remote.records("initialize")))
        self.assert_secrets_absent()

    def test_wrong_callback_state_and_issuer_never_exchange_a_code(self):
        self.require_store()
        process, notice, query = self.start_login()
        self.assertGreaterEqual(self.callback(notice, query, state="wrong-state"), 400)
        self.assertEqual(self.remote.server.token_calls, [])
        self.assertGreaterEqual(self.callback(notice, query, iss=self.remote.server.origin + "/wrong"), 400)
        self.assertEqual(self.remote.server.token_calls, [])
        self.assertGreaterEqual(self.callback(notice, query, iss=None), 400)
        self.assertEqual(self.remote.server.token_calls, [])
        self.assertLess(self.callback(notice, query), 300)
        self.finish_login(process)
        self.assertEqual(len(self.remote.server.token_calls), 1)

    def test_login_timeout_closes_callback_and_never_exchanges_code(self):
        self.require_store()
        started = time.monotonic()
        process, notice, query = self.start_login("--login-timeout-ms", "500")
        self.finish_login(process, expected=2)
        self.assertLess(time.monotonic() - started, 4)
        self.assertEqual(self.remote.server.token_calls, [])
        self.assertNotEqual(self.auth("status")["state"], "available")
        with self.assertRaises(OSError):
            urlopen(notice["redirect_uri"] + "?" + urlencode({"code": self.remote.server.code,
                    "state": query["state"][0], "iss": self.remote.server.issuer}), timeout=1)

    def test_scope_escalation_or_non_bearer_token_is_not_persisted(self):
        self.require_store()
        for override in ({"scope": "tools.read admin"}, {"token_type": "MAC"},
                         {"access_token": "a" * 1400, "refresh_token": "r" * 1400}):
            with self.subTest(override=override):
                self.secrets.extend(value for key, value in override.items() if key in ("access_token", "refresh_token"))
                self.remote.server.token_override = override
                process, notice, query = self.start_login()
                self.callback(notice, query)
                self.finish_login(process, expected=2)
                self.assertNotEqual(self.auth("status")["state"], "available")
                self.auth("logout")
        self.assert_secrets_absent()

    def test_relogin_refuses_secret_reflection_in_cli_and_service_authorization_notice(self):
        self.login()
        self.remote.server.issuer_override["authorization_endpoint"] = (
            self.remote.server.origin + "/authorize/" + self.remote.server.refresh)
        process = self.capture(*self.auth_args("login"), "--login-timeout-ms", "500")
        self.assertEqual(process.returncode, 2, process.stdout + process.stderr)
        self.assertNotIn(b'"authorization_url"', process.stderr)
        self.assertEqual(len(self.remote.server.token_calls), 1)
        self.assertEqual(self.auth("status")["state"], "available")

        client = self.service()
        accepted = client.rpc("mcp/auth/login", {"mcp_config": str(self.manifest), "server": "oauth",
                                                "login_timeout_ms": 500})
        done = client.finished(accepted["operation_id"])
        self.assertEqual(done["exit_code"], 2, done)
        self.assertFalse(any(frame.get("method") == "operation/authorization" for frame in client.frames))
        self.assertEqual(len(self.remote.server.token_calls), 1)
        client.close()
        for secret in self.secrets:
            self.assertNotIn(secret.encode(), bytes(client.raw) + bytes(client.stderr))
        self.assert_secrets_absent()

    def test_status_and_logout_are_local_and_do_not_return_or_revoke_tokens(self):
        self.login()
        before = self.remote.records()
        self.assertEqual(self.auth("status")["state"], "available")
        self.assertEqual(self.auth("logout")["state"], "deleted")
        self.assertEqual(self.auth("status")["state"], "missing")
        self.assertEqual(self.auth("logout")["state"], "missing")
        self.assertEqual(self.remote.records(), before)
        self.assert_secrets_absent()

    def test_same_name_bearer_credential_cannot_replace_or_delete_oauth_tokens(self):
        self.login()
        ordinary = "ordinary-bearer-" + uuid.uuid4().hex
        self.secrets.append(ordinary)
        self.invoke("credential-set", "--name", self.name, "--stdin", input=ordinary.encode() + b"\n")
        self.assertEqual(self.auth("status")["state"], "available")
        self.discover()
        self.invoke("credential-delete", "--name", self.name)
        self.assertEqual(self.auth("status")["state"], "available")
        self.discover()
        self.assertEqual(len(self.remote.server.token_calls), 1)
        self.assertTrue(all(item["headers"].get("authorization") == "Bearer " + self.remote.server.access
                            for item in self.remote.records("initialize")))
        self.assert_secrets_absent()

    def test_expired_token_refreshes_once_before_connect_and_rotated_record_is_reused(self):
        self.login(expires_in=1)
        self.remote.server.expires_in = 3600
        self.discover()
        calls = self.remote.server.token_calls
        self.assertEqual([item["grant_type"][0] for item in calls], ["authorization_code", "refresh_token"])
        self.assertEqual(calls[-1]["resource"], [self.remote.url])
        self.assertTrue(all(item["headers"].get("authorization") == "Bearer " + self.remote.server.next_access
                            for item in self.remote.records("initialize")))
        self.discover()
        self.assertEqual(len(self.remote.server.token_calls), 2)
        self.assertEqual(self.auth("status")["state"], "available")
        self.assert_secrets_absent()

    def test_failed_refresh_is_blocked_and_never_retried_implicitly(self):
        self.login(expires_in=1)
        self.remote.server.token_error = True
        self.discover(expected=2)
        self.assertEqual(len(self.remote.server.token_calls), 2)
        self.assertEqual(self.remote.records("initialize"), [])
        self.assertEqual(self.auth("status")["state"], "refresh_blocked")
        self.remote.server.token_error = False
        self.discover(expected=2)
        self.assertEqual(len(self.remote.server.token_calls), 2)
        self.assertEqual(self.remote.records("initialize"), [])
        self.assert_secrets_absent()

    def test_concurrent_refresh_serializes_one_rotating_token_request(self):
        self.login(expires_in=1)
        self.remote.server.expires_in = 3600
        self.remote.server.token_release = threading.Event()
        args = [str(fixture.EXE), "mcp-tools", "--workspace", str(self.workspace), *map(str, self.grants())]
        first = subprocess.Popen(args, cwd=self.workspace, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(first)
        self.wait_for(lambda: len(self.remote.server.token_calls) == 2)
        second = subprocess.Popen(args, cwd=self.workspace, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(second)
        # A contender may wait and reuse the persisted result, or refuse busy;
        # it must never race a second refresh using the same rotating token.
        time.sleep(0.15)
        self.assertEqual(len(self.remote.server.token_calls), 2)
        self.remote.server.token_release.set()
        one = first.communicate(timeout=10)
        two = second.communicate(timeout=10)
        self.assertEqual(first.returncode, 0, one)
        self.assertIn(second.returncode, (0, 2), two)
        self.assertEqual(len(self.remote.server.token_calls), 2)
        self.assertEqual(self.auth("status")["state"], "available")
        for secret in self.secrets:
            self.assertNotIn(secret.encode(), b"".join(one + two))
        self.assert_secrets_absent()

    def test_crash_after_refresh_dispatch_retains_blocked_record_and_never_replays(self):
        self.login(expires_in=1)
        self.remote.server.expires_in = 3600
        self.remote.server.token_release = threading.Event()
        args = [str(fixture.EXE), "mcp-tools", "--workspace", str(self.workspace), *map(str, self.grants())]
        process = subprocess.Popen(args, cwd=self.workspace, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(process)
        self.wait_for(lambda: len(self.remote.server.token_calls) == 2)
        fixture.ResumeTests.kill(process)
        self.remote.server.token_release.set()
        self.assertEqual(self.auth("status")["state"], "refresh_blocked")
        self.discover(expected=2)
        self.assertEqual(len(self.remote.server.token_calls), 2)
        self.assertEqual(self.remote.records("initialize"), [])
        self.assert_secrets_absent()

    def test_changed_configuration_cannot_reuse_credential_or_refresh_elsewhere(self):
        self.login(expires_in=1)
        before = self.remote.records()
        oauth = dict(self.server_config["oauth"], client_id="another-client")
        self.configure(oauth=oauth)
        self.assertEqual(self.auth("status")["state"], "binding_mismatch")
        self.discover(expected=2)
        self.assertEqual(self.remote.records(), before)

    def test_missing_login_and_ungranted_oauth_do_not_contact_network(self):
        self.require_store()
        self.discover(expected=2)
        self.assertEqual(self.remote.records(), [])
        self.setup_responses({"body": fixture.reply()})
        self.invoke(*self.run_args(*self.grants(grant=False)))
        self.assertEqual(self.remote.records(), [])
        self.assertEqual([item["function"]["name"] for item in self.requests()[0]["body"]["tools"]], ["read_file"])

    def test_later_oauth_binding_failure_prevents_earlier_token_refresh(self):
        self.login(expires_in=1)
        first = copy.deepcopy(self.server_config)
        second = copy.deepcopy(first)
        second["id"] = "other"
        second["oauth"]["client_id"] = "different-client"
        self.manifest.write_text(json.dumps({"version": 2, "servers": [first, second]}), encoding="utf-8")
        before = self.remote.records()
        self.invoke("mcp-tools", "--workspace", self.workspace, *self.grants(),
                    "--allow-mcp", "other", expected=2)
        self.assertEqual(self.remote.records(), before)
        self.assertEqual(len(self.remote.server.token_calls), 1)
        self.configure()
        status = self.auth("status")
        self.assertIn(status["state"], ("available", "expired"))
        self.assertTrue(status["refresh_available"])
        self.assert_secrets_absent()

    def test_invalid_run_preflight_does_not_refresh_or_mutate_oauth_record(self):
        self.login(expires_in=1)
        before = self.remote.records()
        cases = [self.run_args(*self.grants(), "--max-turns", "0"),
                 self.run_args(*self.grants(), "--allow-process", "relative-not-authorized"),
                 self.run_args(*self.grants(), api="unsupported-api")]
        for extra in (("--cron", "invalid", "--timezone", "UTC"),
                      ("--cron", "0 9 * * *", "--timezone", "Missing/Timezone"),
                      ("--cron", "0 9 * * *", "--timezone", "UTC", "--max-runs", "-1"),
                      ("--cron", "0 9 * * *", "--timezone", "UTC", "--deadline-ms", "0")):
            arguments = self.run_args(*self.grants(), *extra)
            arguments[0] = "cron-create"
            cases.append(arguments)
        for args in cases:
            with self.subTest(args=args):
                self.invoke(*args, expected=2)
                self.assertEqual(self.remote.records(), before)
                self.assertEqual(len(self.remote.server.token_calls), 1)
                self.assertEqual(self.requests(), [])
                status = self.auth("status")
                self.assertIn(status["state"], ("available", "expired"))
                self.assertTrue(status["refresh_available"])
        self.assert_secrets_absent()

    def test_new_refresh_token_cannot_be_persisted_as_cron_metadata(self):
        self.login(expires_in=1)
        self.remote.server.expires_in = 3600
        arguments = self.run_args(*self.grants(), "--cron", "0 9 * * *", "--timezone", "UTC",
                                  "--name", self.remote.server.next_refresh)
        arguments[0] = "cron-create"
        self.invoke(*arguments, expected=2)
        self.assertEqual(len(self.remote.server.token_calls), 2)
        self.assertEqual(self.remote.records("initialize"), [])
        self.assertEqual(self.requests(), [])
        self.assertEqual(self.sql_rows("SELECT count(*) FROM cron_schedules"), [(0,)])
        self.assertEqual(self.sql_rows("SELECT count(*) FROM tasks"), [(0,)])
        self.assertEqual(self.auth("status")["state"], "available")
        self.assert_secrets_absent()

    def test_changed_resume_policy_does_not_refresh_before_binding_refusal(self):
        self.login()
        self.setup_responses({"status": 503})
        task = self.invoke(*self.run_args(*self.grants()), expected=1)["task_id"]
        # Refresh timing belongs to the new operation; re-login only replaces
        # the same explicit OAuth binding, never the saved task authorization.
        self.remote.server.expires_in = 1
        process, notice, query = self.start_login()
        self.callback(notice, query)
        self.finish_login(process)
        before = self.remote.records()
        self.invoke(*self.resume_args(task, *self.grants(), "--allow-write"), expected=2)
        self.assertEqual(self.remote.records(), before)
        self.assertEqual(len(self.remote.server.token_calls), 2)
        self.assertEqual(len(self.requests()), 1)
        self.assertIn(self.auth("status")["state"], ("available", "expired"))
        # Enrollment records the existing grant; it does not use a live MCP
        # connection and therefore must not rotate an OAuth token either.
        arguments = self.resume_args(task, *self.grants())
        arguments[0] = "schedule-resume"
        self.invoke(*arguments)
        self.assertEqual(self.remote.records(), before)
        self.assertEqual(len(self.remote.server.token_calls), 2)
        self.assertEqual(len(self.requests()), 1)
        self.assertIn(self.auth("status")["state"], ("available", "expired"))
        self.assert_secrets_absent()

    def test_401_tool_result_remains_unknown_without_refresh_or_replay(self):
        self.login()
        self.remote.server.mode = "call-unauthorized"
        self.setup_responses({"body": fixture.reply(tool=("mcp_oauth_echo", {"text": "once"}))})
        result = self.invoke(*self.run_args(*self.grants()), expected=1)
        self.assertEqual(result["status"], "needs_attention")
        detail = self.invoke("show", result["task_id"], "--workspace", self.workspace)
        self.assertEqual((detail["actions"][0]["status"], detail["actions"][0]["effect"]), ("unknown", "unknown"))
        self.assertEqual(len(self.remote.records("tools/call")), 1)
        self.assertEqual(len(self.remote.server.token_calls), 1)
        self.invoke(*self.resume_args(result["task_id"], *self.grants()), expected=2)
        self.assertEqual(len(self.remote.records("tools/call")), 1)
        self.assertEqual(len(self.remote.server.token_calls), 1)
        self.assertEqual(len(self.requests()), 1)
        self.assert_secrets_absent()

    def test_reflected_access_and_refresh_tokens_are_redacted_from_task_and_model(self):
        self.login()
        self.remote.server.output_text = self.remote.server.access + "/" + self.remote.server.refresh
        self.setup_responses({"body": fixture.reply(tool=("mcp_oauth_echo", {"text": "private"}))},
                             {"body": fixture.reply()})
        result = self.invoke(*self.run_args(*self.grants()))
        self.assertEqual(result["status"], "completed")
        self.invoke("show", result["task_id"], "--workspace", self.workspace)
        model_receipt = json.dumps(self.requests()[1]["body"])
        for secret in self.secrets:
            self.assertNotIn(secret, model_receipt)
        self.assert_secrets_absent()

    def test_reflected_refresh_token_in_tool_schema_is_refused_before_model(self):
        self.login()
        self.remote.server.mode = "schema-secret"
        self.remote.server.output_text = self.remote.server.refresh
        self.setup_responses({"body": fixture.reply()})
        self.invoke(*self.run_args(*self.grants()), expected=2)
        self.assertEqual(self.requests(), [])
        self.assertEqual(self.remote.records("tools/call"), [])
        self.assert_secrets_absent()

    def test_service_login_authorization_notification_and_local_status(self):
        self.require_store()
        client = self.service()
        accepted = client.rpc("mcp/auth/login", {"mcp_config": str(self.manifest), "server": "oauth",
                                                "login_timeout_ms": 5000})
        self.assertEqual(accepted["status"], "accepted")
        operation = accepted["operation_id"]
        notice = client.receive(lambda frame: frame.get("method") == "operation/authorization"
                                and frame.get("params", {}).get("operation_id") == operation)["params"]
        query = parse_qs(urlsplit(notice["authorization_url"]).query)
        self.remote.server.authorization = query
        self.assertLess(self.callback(notice, query), 300)
        done = client.finished(operation)
        self.assertEqual(done["exit_code"], 0, done)
        self.assertEqual(done["result"]["state"], "available")
        before = self.remote.records()
        accepted = client.rpc("mcp/auth/status", {"mcp_config": str(self.manifest), "server": "oauth"})
        done = client.finished(accepted["operation_id"])
        self.assertEqual(done["result"]["state"], "available")
        self.assertEqual(self.remote.records(), before)
        self.assert_secrets_absent()

    def test_service_cancel_pending_login_releases_listener_without_token_post(self):
        self.require_store()
        client = self.service()
        accepted = client.rpc("mcp/auth/login", {"mcp_config": str(self.manifest), "server": "oauth",
                                                "login_timeout_ms": 10000})
        operation = accepted["operation_id"]
        notice = client.receive(lambda frame: frame.get("method") == "operation/authorization"
                                and frame.get("params", {}).get("operation_id") == operation)["params"]
        client.rpc("operations/cancel", {"operation_id": operation})
        done = client.finished(operation)
        self.assertNotEqual(done["exit_code"], 0)
        self.assertEqual(self.remote.server.token_calls, [])
        self.assertNotEqual(self.auth("status")["state"], "available")
        with self.assertRaises(OSError):
            urlopen(notice["redirect_uri"] + "?state=stale&code=stale", timeout=1)
        self.assert_secrets_absent()


if __name__ == "__main__":
    unittest.main()
