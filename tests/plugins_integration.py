"""Exercise exact plugin bundles/locks through the real CLI and MCP harness.

All packages are built from the owned native MCP fixture in temporary folders.
No registry, external package, shell hook, live model or permanent install runs.
"""
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import unittest
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import threading

import resume_integration as fixture

_arguments = sys.argv[:]
sys.argv[:] = [_arguments[0], str(fixture.EXE)]
try:
    import service_integration as service_fixture
finally:
    sys.argv[:] = _arguments


class PluginsTests(unittest.TestCase):
    run_args = fixture.ResumeTests.run_args
    resume_args = fixture.ResumeTests.resume_args
    setup_responses = fixture.ResumeTests.setup_responses
    requests = fixture.ResumeTests.requests
    sql_rows = fixture.ResumeTests.sql_rows

    def setUp(self):
        fixture.ResumeTests.setUp(self)
        self.workspace = self.workspace.resolve()
        self.database = self.workspace / ".codestudio/state.sqlite3"
        self.source = self.workspace / "plugin source"
        self.source.mkdir()
        self.executable = fixture.MCP.name
        shutil.copyfile(fixture.MCP, self.source / self.executable)
        if os.name != "nt":
            (self.source / self.executable).chmod(0o755)
        (self.source / "README.txt").write_text("Owned plugin fixture\n", encoding="utf-8")
        self.marker = self.workspace / "plugin.started"
        self.descriptor = {"format": 2, "id": "fixture-echo", "version": "1.2.3",
                           "platform": "windows" if os.name == "nt" else "macosx" if sys.platform == "darwin" else "linux",
                           "arch": "x64" if os.name == "nt" else "arm64" if sys.platform == "darwin" else "x86_64",
                           "entrypoint": self.executable, "args": ["normal", str(self.marker)],
                           "tools": ["echo"]}
        self.write_descriptor()
        self.bundle = self.workspace / "fixture.csplugin"
        self.store = self.workspace / "plugin store"
        self.lock = self.workspace / "fixture.lock.json"
        self.manifest = self.workspace / "mcp.json"
        self.http_servers = []
        self.clients = []

    def tearDown(self):
        try:
            for client in self.clients:
                client.close()
                self.assertNotIn(fixture.TOKEN.encode(), bytes(client.raw) + bytes(client.stderr))
            for server, worker in self.http_servers:
                server.shutdown()
                server.server_close()
                worker.join(timeout=5)
        finally:
            fixture.ResumeTests.tearDown(self)

    def write_descriptor(self):
        (self.source / "plugin.json").write_text(json.dumps(self.descriptor), encoding="utf-8")

    def capture(self, *args):
        process = subprocess.run([str(fixture.EXE), *map(str, args)], cwd=self.workspace,
                                 env=self.env, capture_output=True, timeout=25)
        self.assertNotIn(fixture.TOKEN.encode(), process.stdout + process.stderr)
        return process

    def invoke(self, *args, expected=0):
        process = self.capture(*args)
        self.assertEqual(process.returncode, expected, process.stdout + process.stderr)
        return json.loads(process.stdout)

    def pack(self, output=None, expected=0):
        return self.invoke("plugin-pack", "--source", self.source, "--output", output or self.bundle,
                           expected=expected)

    def install(self, bundle=None, digest=None, store=None, expected=0):
        bundle = bundle or self.bundle
        digest = digest or hashlib.sha256(bundle.read_bytes()).hexdigest()
        return self.invoke("plugin-install", "--bundle", bundle, "--sha256", digest,
                           "--store", store or self.store, expected=expected)

    def prepare(self):
        self.pack()
        self.install()
        self.installed = self.store / self.descriptor["id"] / self.descriptor["version"] / hashlib.sha256(self.bundle.read_bytes()).hexdigest()
        self.assertTrue(self.installed.is_dir())
        self.invoke("plugin-lock", "--directory", self.installed, "--output", self.lock)
        self.configure()
        self.assertFalse(self.marker.exists(), "Packaging or installation executed the plugin")

    def configure(self, servers=None, **overrides):
        server = {"id": "plugin", "plugin_lock": str(self.lock), "tools": ["echo"], "timeout_ms": 1500}
        server.update(overrides)
        self.manifest.write_text(json.dumps({"version": 2, "servers": servers or [server]}), encoding="utf-8")
        return server

    def grants(self, grant=True):
        return ["--mcp-config", self.manifest, *(["--allow-mcp", "plugin"] if grant else [])]

    def discover(self, expected=0):
        return self.invoke("mcp-tools", "--workspace", self.workspace, *self.grants(), expected=expected)

    def inventory(self, root):
        return {path.relative_to(root).as_posix(): path.read_bytes() for path in root.rglob("*") if path.is_file()}

    def mutate_bundle(self, mutation):
        data = json.loads(self.bundle.read_text(encoding="utf-8"))
        mutation(data)
        candidate = self.workspace / "mutated.csplugin"
        candidate.write_bytes((json.dumps(data, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n").encode())
        return candidate

    def assert_no_task_effects(self):
        if self.database.exists():
            with closing(sqlite3.connect(self.database.as_uri() + "?mode=ro", uri=True)) as database:
                self.assertEqual(database.execute("SELECT count(*) FROM tasks").fetchone(), (0,))
                self.assertEqual(database.execute("SELECT count(*) FROM actions").fetchone(), (0,))

    def test_pack_is_deterministic_and_install_never_executes_payload(self):
        self.pack()
        second = self.workspace / "second.csplugin"
        self.pack(second)
        self.assertEqual(self.bundle.read_bytes(), second.read_bytes())
        bundle = json.loads(self.bundle.read_text())
        self.assertEqual(bundle["format"], 2)
        manifest = bundle["manifest"]
        self.assertEqual(manifest["version"], "1.2.3")
        self.assertEqual({item["path"] for item in manifest["files"]}, {self.executable, "README.txt"})
        for entry in manifest["files"]:
            payload = base64.b64decode(bundle["payloads"][entry["path"]], validate=True)
            self.assertEqual(payload, (self.source / entry["path"]).read_bytes())
            self.assertEqual(entry["sha256"], hashlib.sha256(payload).hexdigest())
            self.assertEqual(entry["size"], len(payload))
        self.install()
        before = self.inventory(self.store)
        self.install()
        self.assertEqual(self.inventory(self.store), before)
        self.assertFalse(self.marker.exists())
        self.assertFalse(self.database.exists())

    def test_bad_expected_digest_refuses_without_creating_store_or_executing(self):
        self.pack()
        self.install(digest="0" * 64, expected=2)
        self.assertFalse(self.store.exists())
        self.assertFalse(self.marker.exists())

    def test_exact_version_required_and_unknown_descriptor_fields_refused(self):
        original = copy.deepcopy(self.descriptor)
        for version in ("latest", "^1.2.3", "1.2", "01.2.3", "1.2.3-beta", "1.2.3+build"):
            with self.subTest(version=version):
                self.descriptor = dict(original, version=version)
                self.write_descriptor()
                self.pack(expected=2)
                self.assertFalse(self.bundle.exists())
        self.descriptor = dict(original, postinstall="run-shell-hook")
        self.write_descriptor()
        self.pack(expected=2)
        self.assertFalse(self.bundle.exists())

    def test_missing_entrypoint_and_outside_template_refused_during_pack(self):
        for field, value in (("entrypoint", "missing"), ("entrypoint", "../outside"),
                             ("args", ["${plugin}/../outside"]), ("args", ["${plugin}/missing"]),
                             ("args", ["${HOME}/private"])):
            original = copy.deepcopy(self.descriptor)
            with self.subTest(field=field, value=value):
                self.descriptor[field] = value
                self.write_descriptor()
                self.pack(expected=2)
                self.assertFalse(self.bundle.exists())
            self.descriptor = original

    def test_source_hardlink_is_refused(self):
        os.link(self.source / "README.txt", self.source / "hardlink.txt")
        self.pack(expected=2)
        self.assertFalse(self.bundle.exists())

    def test_source_symlink_is_refused(self):
        try:
            (self.source / "alias.txt").symlink_to(self.source / "README.txt")
        except OSError:
            self.skipTest("Current Windows token cannot create symbolic links")
        self.pack(expected=2)
        self.assertFalse(self.bundle.exists())

    def test_payload_corruption_or_metadata_change_refuses_install(self):
        self.pack()
        mutations = [lambda data: data["payloads"].__setitem__("README.txt", "!!!"),
                     lambda data: data["payloads"].__setitem__("README.txt", base64.b64encode(b"corrupt").decode()),
                     lambda data: data["manifest"]["files"][0].__setitem__("sha256", "0" * 64),
                     lambda data: data["manifest"].__setitem__("format", 0),
                     lambda data: data["manifest"].__setitem__("platform", "unsupported"),
                     lambda data: data.__setitem__("format", 0)]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.install(self.mutate_bundle(mutation), expected=2)
                self.assertFalse(self.store.exists())
                self.assertFalse(self.marker.exists())

    def test_unlisted_payload_and_traversal_path_refused(self):
        self.pack()
        for name in ("extra.txt", "../escape.txt", "/absolute", "a\\b", "C:/outside"):
            with self.subTest(name=name):
                candidate = self.mutate_bundle(lambda data: data["payloads"].__setitem__(name, "YQ=="))
                self.install(candidate, expected=2)
                self.assertFalse(self.store.exists())
        self.assertFalse((self.workspace.parent / "escape.txt").exists())

    def test_lock_binds_exact_installed_manifest_bundle_and_directory(self):
        self.prepare()
        document = json.loads(self.lock.read_text())
        self.assertEqual(document["format"], 2)
        for key in ("id", "version", "platform", "arch"):
            self.assertEqual(document[key], self.descriptor[key])
        self.assertEqual(document["bundle_sha256"], hashlib.sha256(self.bundle.read_bytes()).hexdigest())
        self.assertEqual(document["manifest_sha256"], hashlib.sha256((self.installed / "plugin.json").read_bytes()).hexdigest())
        self.assertEqual(Path(document["directory"]), self.installed)
        before = self.inventory(self.workspace)
        self.invoke("plugin-inspect", "--lock", self.lock)
        self.assertEqual(before, self.inventory(self.workspace))
        self.assertFalse(self.marker.exists())

    def test_modified_installed_files_block_inspect_reinstall_and_launch(self):
        self.prepare()
        path = self.installed / "README.txt"
        path.write_text("modified by user", encoding="utf-8")
        before = self.inventory(self.store)
        self.invoke("plugin-inspect", "--lock", self.lock, expected=2)
        self.install(expected=2)
        self.discover(expected=2)
        self.assertEqual(before, self.inventory(self.store))
        self.assertFalse(self.marker.exists())
        self.assertFalse(self.database.exists())

    def test_extra_installed_file_blocks_loading_and_is_preserved(self):
        self.prepare()
        extra = self.installed / "user.txt"
        extra.write_text("preserve me", encoding="utf-8")
        self.discover(expected=2)
        self.assertEqual(extra.read_text(), "preserve me")
        self.assertFalse(self.marker.exists())

    def test_lock_digest_version_and_directory_tampering_refuse_before_launch(self):
        self.prepare()
        original = json.loads(self.lock.read_text())
        for key, value in (("bundle_sha256", "0" * 64), ("manifest_sha256", "0" * 64),
                           ("version", "9.9.9"), ("directory", str(self.source)), ("format", 0)):
            with self.subTest(key=key):
                self.lock.write_text(json.dumps(dict(original, **{key: value})), encoding="utf-8")
                self.discover(expected=2)
                self.assertFalse(self.marker.exists())

    def test_plugin_discovery_and_call_use_explicit_grants_and_ledger(self):
        self.prepare()
        result = self.discover()
        self.assertEqual([item["function"]["name"] for item in result["tools"]], ["mcp_plugin_echo"])
        self.assertTrue(self.marker.exists())
        self.setup_responses({"body": fixture.reply(tool=("mcp_plugin_echo", {"text": "locked"}))},
                             {"body": fixture.reply()})
        result = self.invoke(*self.run_args(*self.grants()))
        detail = self.invoke("show", result["task_id"], "--workspace", self.workspace)
        self.assertEqual(result["status"], "completed")
        self.assertEqual(len(detail["actions"]), 1)
        self.assertEqual((detail["actions"][0]["status"], detail["actions"][0]["effect"]), ("succeeded", "applied"))
        self.assertEqual(detail["actions"][0]["result"]["output"]["structuredContent"]["text"], "locked")

    def test_ungranted_plugin_never_launches_and_hidden_tool_cannot_expand_allowlist(self):
        self.prepare()
        self.setup_responses({"body": fixture.reply()})
        self.invoke(*self.run_args(*self.grants(grant=False)))
        self.assertFalse(self.marker.exists())
        self.assertEqual([item["function"]["name"] for item in self.requests()[0]["body"]["tools"]], ["read_file"])
        self.configure(tools=["echo", "hidden"])
        self.discover(expected=2)
        self.assertFalse(self.marker.exists())

    def test_plugin_lock_cannot_mix_mutable_command_or_http_fields(self):
        self.prepare()
        for extra in ({"command": str(fixture.MCP), "args": []},
                      {"url": "https://example.invalid/mcp"},
                      {"transport": "streamable-http", "url": "https://example.invalid/mcp"}):
            with self.subTest(extra=extra):
                self.configure(**extra)
                self.discover(expected=2)
                self.assertFalse(self.marker.exists())

    def test_later_bad_lock_prevents_earlier_plugin_launch_and_model_call(self):
        self.prepare()
        first = self.configure()
        second = dict(first, id="broken", plugin_lock=str(self.workspace / "absent.lock"))
        self.configure(servers=[first, second])
        self.setup_responses({"body": fixture.reply()})
        self.invoke(*self.run_args(*self.grants(), "--allow-mcp", "broken"), expected=2)
        self.assertFalse(self.marker.exists())
        self.assertEqual(self.requests(), [])
        self.assert_no_task_effects()

    def test_earlier_stdio_peer_cannot_tamper_later_plugin_between_preflight_and_launch(self):
        self.prepare()
        target = self.installed / "README.txt"
        mutator = self.workspace / "mutator.py"
        mutator.write_text("""import json, pathlib, sys
for line in sys.stdin:
    request = json.loads(line)
    method = request.get('method')
    if method == 'initialize':
        pathlib.Path(sys.argv[1]).write_text('tampered during earlier peer initialization', encoding='utf-8')
        result = {'protocolVersion':'2025-11-25','capabilities':{'tools':{}},
                  'serverInfo':{'name':'owned-mutator','version':'2'}}
    elif method == 'tools/list':
        result = {'tools':[{'name':'echo','inputSchema':{'type':'object'}}]}
    else:
        continue
    print(json.dumps({'jsonrpc':'2.0','id':request['id'],'result':result}), flush=True)
""", encoding="utf-8")
        first = {"id": "mutator", "command": sys.executable, "args": [str(mutator), str(target)],
                 "tools": ["echo"], "timeout_ms": 1500}
        later = self.configure()
        self.configure(servers=[first, later])
        self.invoke("mcp-tools", "--workspace", self.workspace, *self.grants(),
                    "--allow-mcp", "mutator", expected=2)
        self.assertEqual(target.read_text(), "tampered during earlier peer initialization")
        self.assertFalse(self.marker.exists(), "The later plugin launched after its pinned files changed")
        self.assertEqual(self.requests(), [])

    def test_new_version_installs_side_by_side_without_changing_existing_lock(self):
        self.prepare()
        old_lock = self.lock.read_bytes()
        old_tree = self.inventory(self.installed)
        self.descriptor["version"] = "1.2.4"
        self.write_descriptor()
        newer = self.workspace / "newer.csplugin"
        self.pack(newer)
        self.install(newer)
        self.assertEqual(self.lock.read_bytes(), old_lock)
        self.assertEqual(self.inventory(self.installed), old_tree)
        self.discover()

    def test_resume_rejects_changed_lock_even_when_manifest_is_unchanged(self):
        self.prepare()
        self.setup_responses({"status": 503})
        task = self.invoke(*self.run_args(*self.grants()), expected=1)["task_id"]
        self.marker.unlink()
        original = self.lock.read_bytes()
        self.descriptor["version"] = "1.2.4"
        self.write_descriptor()
        new_bundle = self.workspace / "newer.csplugin"
        self.pack(new_bundle)
        self.install(new_bundle)
        new_directory = self.store / self.descriptor["id"] / self.descriptor["version"] / hashlib.sha256(new_bundle.read_bytes()).hexdigest()
        new_lock = self.workspace / "newer.lock.json"
        self.invoke("plugin-lock", "--directory", new_directory, "--output", new_lock)
        self.lock.write_bytes(new_lock.read_bytes())
        self.invoke(*self.resume_args(task, *self.grants()), expected=2)
        self.assertFalse(self.marker.exists())
        self.assertEqual(len(self.requests()), 1)
        self.lock.write_bytes(original)
        self.setup_responses({"body": fixture.reply()})
        self.assertEqual(self.invoke(*self.resume_args(task, *self.grants()))["status"], "completed")

    def test_modified_payload_blocks_resume_before_new_model_or_plugin_dispatch(self):
        self.prepare()
        self.setup_responses({"status": 503})
        task = self.invoke(*self.run_args(*self.grants()), expected=1)["task_id"]
        self.marker.unlink()
        (self.installed / "README.txt").write_text("mutated", encoding="utf-8")
        self.invoke(*self.resume_args(task, *self.grants()), expected=2)
        self.assertFalse(self.marker.exists())
        self.assertEqual(len(self.requests()), 1)

    def distribution_server(self, payload, redirect=False):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_GET(self):
                self.server.requests.append((self.path, dict(self.headers)))
                self.send_response(302 if redirect else 200)
                if redirect:
                    self.send_header("Location", self.server.url + "/sink")
                self.send_header("Content-Length", str(0 if redirect else len(payload)))
                self.end_headers()
                if not redirect:
                    self.wfile.write(payload)
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        server.requests = []
        server.url = f"http://127.0.0.1:{server.server_port}"
        worker = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        worker.start()
        self.http_servers.append((server, worker))
        return server

    def test_pinned_url_install_downloads_without_credentials_or_execution(self):
        self.pack()
        server = self.distribution_server(self.bundle.read_bytes())
        digest = hashlib.sha256(self.bundle.read_bytes()).hexdigest()
        self.invoke("plugin-install", "--url", server.url + "/plugin.csplugin", "--sha256", digest,
                    "--store", self.store, "--allow-http-loopback")
        self.assertEqual(len(server.requests), 1)
        self.assertFalse(any(key.lower() == "authorization" for key in server.requests[0][1]))
        self.assertNotIn(fixture.TOKEN, json.dumps(server.requests))
        self.assertFalse(self.marker.exists())

    def test_url_bad_hash_redirect_and_missing_loopback_permission_refuse(self):
        self.pack()
        server = self.distribution_server(self.bundle.read_bytes())
        digest = hashlib.sha256(self.bundle.read_bytes()).hexdigest()
        self.invoke("plugin-install", "--url", server.url + "/plugin", "--sha256", digest,
                    "--store", self.store, expected=2)
        self.assertEqual(server.requests, [])
        self.invoke("plugin-install", "--url", server.url + "/plugin", "--sha256", "0" * 64,
                    "--store", self.store, "--allow-http-loopback", expected=2)
        self.assertFalse(self.store.exists())
        redirect = self.distribution_server(self.bundle.read_bytes(), redirect=True)
        self.invoke("plugin-install", "--url", redirect.url + "/plugin", "--sha256", digest,
                    "--store", self.store, "--allow-http-loopback", expected=2)
        self.assertEqual([item[0] for item in redirect.requests], ["/plugin"])
        self.assertFalse(self.store.exists())

    def test_service_package_install_lock_inspect_never_grants_or_launches(self):
        client = service_fixture.Client(self.workspace, self.env)
        self.clients.append(client)
        client.initialize()

        def operation(method, params):
            accepted = client.rpc(method, params)
            self.assertEqual(accepted["status"], "accepted")
            done = client.finished(accepted["operation_id"])
            self.assertEqual(done["exit_code"], 0, done)
            self.assertFalse(self.marker.exists())
            return done["result"]

        operation("plugins/pack", {"source": self.source.name, "output": self.bundle.name})
        digest = hashlib.sha256(self.bundle.read_bytes()).hexdigest()
        installed = operation("plugins/install", {"bundle": self.bundle.name,
                              "sha256": digest, "store": self.store.name})
        operation("plugins/lock", {"directory": installed["directory"], "output": self.lock.name})
        inspected = operation("plugins/inspect", {"lock": self.lock.name})
        self.assertEqual(inspected["status"], "verified")
        client.close()
        self.configure()
        self.setup_responses({"body": fixture.reply()})
        self.invoke(*self.run_args(*self.grants(grant=False)))
        self.assertFalse(self.marker.exists())
        self.assertEqual([item["function"]["name"] for item in self.requests()[0]["body"]["tools"]], ["read_file"])


if __name__ == "__main__":
    unittest.main()
