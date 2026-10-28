"""Validate the published service contract without starting an Agent.

Install tests/requirements.txt in a test environment, then run this file.
The Contract helper is shared by the real-process integration suite. Standard
Draft 2020-12 validation is supplemented only for UTF-8 byte bounds and the
service's integer JSON token rule (2.0 is not an integer token).
"""
import copy
import json
import pathlib
import re
import unittest

from jsonschema import Draft202012Validator, ValidationError, validators

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCHEMA_PATH = ROOT / "protocol" / "task-service.schema.json"


def _max_bytes(validator, maximum, instance, schema):
    if isinstance(instance, str):
        try:
            size = len(instance.encode("utf-8"))
        except UnicodeEncodeError:
            yield ValidationError("String contains an invalid Unicode surrogate")
            return
        if size > maximum:
            yield ValidationError(f"UTF-8 string uses {size} bytes; maximum is {maximum}")


WireValidator = validators.extend(
    Draft202012Validator,
    validators={"x-maxBytes": _max_bytes},
    type_checker=Draft202012Validator.TYPE_CHECKER.redefine(
        "integer", lambda checker, instance: type(instance) is int),
)


class Contract:
    """Request/response correlation belongs to the caller, not this validator."""

    def __init__(self, path=SCHEMA_PATH):
        self.schema = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
        self.methods = self.schema["x-methods"]
        self.notifications = self.schema["x-notifications"]
        self._validator = WireValidator(self.schema)

    def _validate(self, schema, value):
        self._validator.evolve(schema=schema).validate(value)

    def _definition(self, name, value):
        self._validate({"$ref": "#/$defs/" + name}, value)

    def validate_request(self, frame):
        self._definition("request", frame)

    def validate_response(self, method, frame):
        if "error" in frame:
            self._definition("errorResponse", frame)
            return
        self._definition("successResponse", frame)
        if method not in self.methods:
            raise ValidationError(f"No declared response contract for {method!r}")
        self._validate(self.methods[method]["result"], frame["result"])
        if method == "operations/get" and frame["result"]["state"] == "finished":
            snapshot = frame["result"]
            self.validate_completion(snapshot["method"], snapshot["completion"])
            for key in ("session_id", "operation_id"):
                if snapshot[key] != snapshot["completion"][key]:
                    raise ValidationError(f"Completion {key} differs from its snapshot")

    def validate_completion(self, method, completion):
        self._definition("operationCompletion", completion)
        if method not in self.methods or self.methods[method]["execution"] != "operation":
            raise ValidationError(f"Method {method!r} has no foreground operation")
        if "result" in completion and not completion.get("result_omitted", False):
            self._validate(self.methods[method]["completion_result"], completion["result"])

    def validate_notification(self, frame, operation_method=None):
        self._definition("notification", frame)
        if operation_method is not None and frame["method"] == "operation/finished":
            self.validate_completion(operation_method, frame["params"])

    def validate_frame(self, frame, method=None):
        if isinstance(frame, dict) and "method" in frame:
            if "id" in frame:
                self.validate_request(frame)
            else:
                self.validate_notification(frame, operation_method=method)
        elif isinstance(frame, dict) and "error" in frame:
            self._definition("errorResponse", frame)
        elif method is not None:
            self.validate_response(method, frame)
        else:
            self._definition("successResponse", frame)


def _walk(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from _walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk(child)


def _request(method, params):
    return {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}


class StaticContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.contract = Contract()

    def test_draft_schema_and_every_reference_are_local_and_resolvable(self):
        schema = self.contract.schema
        Draft202012Validator.check_schema(schema)
        for node in _walk(schema):
            if "$ref" not in node:
                continue
            reference = node["$ref"]
            self.assertTrue(reference.startswith("#/"), reference)
            target = schema
            for component in reference[2:].split("/"):
                target = target[component.replace("~1", "/").replace("~0", "~")]
            self.assertIsInstance(target, dict)

    def test_dispatch_and_notification_catalogs_match_runtime(self):
        source = (ROOT / "src/application/service.cpp").read_text(encoding="utf-8")
        # Every currently dispatched method uses an explicit method comparison.
        # This is a deliberate tripwire: adding/renaming a method requires its
        # schema metadata, request branch and retry classification together.
        dispatched = set(re.findall(r'\bmethod\s*==\s*"([^"]+)"', source))
        self.assertEqual(set(self.contract.methods), dispatched)
        emitted = set(re.findall(r'notification\(io,\s*"([^"]+)"', source))
        emitted.update(re.findall(r'\{\s*"method"\s*,\s*"([^"]+)"\s*\}', source))
        self.assertEqual(set(self.contract.notifications), emitted)
        branches = self.contract.schema["$defs"]["request"]["oneOf"]
        self.assertEqual({entry["properties"]["method"]["const"] for entry in branches}, dispatched)
        self.assertEqual(len(branches), len(dispatched))

    def test_retry_metadata_never_authorizes_mutation_replay(self):
        queries = {"tasks/list", "tasks/get", "builds/review", "events/list",
                   "operations/status", "operations/get", "schedules/list", "schedules/get",
                   "scheduler/status", "crons/list", "crons/get", "crons/preview",
                   "scheduler/autostart/status", "scheduler/supervisor/status", "scheduler/supervisor/events", "sandboxes/list"}
        operations = {"tasks/start", "tasks/resume", "crons/create", "builds/approve", "builds/reject",
                      "mcp/auth/discover", "mcp/auth/status", "mcp/auth/login", "mcp/auth/logout",
                      "plugins/pack", "plugins/install", "plugins/lock", "plugins/inspect", "sandboxes/reclaim"}
        for name, spec in self.contract.methods.items():
            self.assertEqual(spec["retry"], "query_only" if name in queries else "never", name)
            expected = "query" if name in queries else "operation" if name in operations else "control"
            self.assertEqual(spec["execution"], expected, name)
            self.assertEqual("completion_result" in spec, name in operations, name)

    def test_extension_requests_and_authorization_notices_have_exact_shapes(self):
        valid = [("mcp/auth/login", {"mcp_config":"mcp.json", "server":"remote", "login_timeout_ms":100}),
                 ("plugins/install", {"url":"https://plugins.example/test.csplugin", "sha256":"a"*64, "store":"plugins"}),
                 ("plugins/pack", {"source":"source", "output":"echo.csplugin"})]
        for method, params in valid:
            frame={"jsonrpc":"2.0", "id":1, "method":method, "params":params}
            self.contract.validate_request(frame)
            frame["params"]={**params, "unexpected":True}
            with self.assertRaises(ValidationError): self.contract.validate_request(frame)
        for params in ({"store":"plugins", "sha256":"a"*64},
                       {"bundle":"x", "url":"https://x.example", "store":"plugins", "sha256":"a"*64},
                       {"bundle":"x", "store":"plugins", "sha256":"latest"}):
            with self.assertRaises(ValidationError):
                self.contract.validate_request({"jsonrpc":"2.0", "id":1,"method":"plugins/install","params":params})
        notice={"jsonrpc":"2.0", "method":"operation/authorization", "params":{
            "operation_id":"op", "type":"authorization", "authorization_url":"https://auth.example/authorize",
            "redirect_uri":"http://127.0.0.1:12345/callback"}}
        self.contract.validate_notification(notice)
        notice["params"]["access_token"]="never include tokens"
        with self.assertRaises(ValidationError): self.contract.validate_notification(notice)

    def test_request_shape_and_option_boundaries(self):
        valid = [
            ("initialize", {"protocol_version": 2}),
            ("tasks/start", {"mode": "demo"}),
            ("tasks/start", {"mode": "build", "options": {"build_config": "build.json"}}),
            ("tasks/start", {"mode": "run", "options": {"max_turns": 100, "timeout_ms": 300000,
             "process_timeout_ms": 100, "max_output_tokens": 1000000, "sandbox": "linux-namespace"}}),
            ("tasks/resume", {"task_id": "t", "options": {"stream": False}}),
            ("schedules/create", {"task_id": "t", "delay_ms": 0, "deadline_ms": 604800000,
             "retry_delay_ms": 100, "max_attempts": 100}),
            ("crons/create", {"expression": "* * * * *", "max_runs": 0}),
            ("crons/preview", {"expression": "* * * * *", "count": 100}),
            ("operations/get", {"session_id": "s", "operation_id": "o"}),
            ("scheduler/start", {"poll_ms": 20}),
        ]
        for name, params in valid:
            with self.subTest(method=name, params=params):
                self.contract.validate_request(_request(name, params))
                bad = copy.deepcopy(params); bad["unexpected"] = True
                with self.assertRaises(ValidationError):
                    self.contract.validate_request(_request(name, bad))
        invalid = [
            ("tasks/start", {"mode": "demo", "options": {"allow_write": True}}),
            ("tasks/start", {"mode": "build"}),
            ("tasks/start", {"mode": "build", "options": {"build_config": "b", "prompt": "p"}}),
            ("tasks/resume", {"task_id": "t", "options": {"stream": True}}),
            ("tasks/resume", {"task_id": "t", "options": {"prompt": "replace"}}),
            ("tasks/start", {"mode": "run", "options": {"max_turns": 101}}),
            ("tasks/start", {"mode": "run", "options": {"timeout_ms": 99}}),
            ("tasks/start", {"mode": "run", "options": {"max_output_tokens": 0}}),
            ("tasks/start", {"mode": "run", "options": {"allow_process": ["tool"] * 129}}),
            ("tasks/start", {"mode": "run", "options": {"api_key": "must-not-send"}}),
            ("tasks/list", {"limit": 101}), ("events/list", {"after": -1}),
            ("operations/get", {"operation_id": "o"}),
            ("scheduler/start", {"poll_ms": 5001}),
            ("crons/preview", {"expression": "* * * * *", "count": 0}),
        ]
        for name, params in invalid:
            with self.subTest(method=name, params=params), self.assertRaises(ValidationError):
                self.contract.validate_request(_request(name, params))

    def test_json_integer_tokens_and_utf8_bytes_match_runtime(self):
        frame = _request("tasks/get", {"task_id": "界" * 42})
        self.contract.validate_request(frame)
        frame["params"]["task_id"] += "界"
        with self.assertRaises(ValidationError):
            self.contract.validate_request(frame)
        for invalid in (True, None, 1.0, "", 9223372036854775808, -9223372036854775809):
            frame = _request("tasks/list", {}); frame["id"] = invalid
            with self.subTest(id=invalid), self.assertRaises(ValidationError):
                self.contract.validate_request(frame)
        for invalid in (True, 2.0):
            with self.subTest(version=invalid), self.assertRaises(ValidationError):
                self.contract.validate_request(_request("initialize", {"protocol_version": invalid}))
        with self.assertRaises(ValidationError):
            self.contract.validate_request(_request("tasks/get", {"task_id": "a\0b"}))

    def test_scheduler_supervision_requires_explicit_opt_in_and_bounded_limits(self):
        valid = [{}, {"supervise": False}, {"supervise": True},
                 {"supervise": True, "poll_ms": 20, "max_restarts": 0, "restart_delay_ms": 100},
                 {"supervise": True, "poll_ms": 5000, "max_restarts": 100, "restart_delay_ms": 60000}]
        for method in ("scheduler/start", "scheduler/autostart/enable"):
            for params in valid:
                with self.subTest(method=method, valid=params):
                    self.contract.validate_request(_request(method, params))
        invalid = [{"supervise": 1}, {"supervise": "true"}, {"max_restarts": 3},
                   {"restart_delay_ms": 1000}, {"supervise": False, "max_restarts": 0},
                   {"supervise": False, "restart_delay_ms": 100},
                   {"supervise": True, "max_restarts": -1}, {"supervise": True, "max_restarts": 101},
                   {"supervise": True, "max_restarts": True}, {"supervise": True, "max_restarts": 3.0},
                   {"supervise": True, "restart_delay_ms": 99},
                   {"supervise": True, "restart_delay_ms": 60001},
                   {"supervise": True, "restart_delay_ms": 1000.0}]
        for method in ("scheduler/start", "scheduler/autostart/enable"):
            for params in invalid + [{"poll_ms": 19}, {"poll_ms": 5001}, {"poll_ms": 100.0}]:
                with self.subTest(method=method, invalid=params), self.assertRaises(ValidationError):
                    self.contract.validate_request(_request(method, params))
        for method in ("scheduler/autostart/status", "scheduler/autostart/disable"):
            self.contract.validate_request(_request(method, {}))
            for params in ({"supervise": True}, {"max_restarts": 3}, {"restart_delay_ms": 1000}, {"poll_ms": 100}):
                with self.subTest(method=method, params=params), self.assertRaises(ValidationError):
                    self.contract.validate_request(_request(method, params))

    def test_autostart_receipt_declares_mode_and_conditional_limits(self):
        ordinary = {"available": True, "registered": True, "enabled": True, "conflict": False,
                    "supervised": False, "poll_ms": 100, "workspace": "/work", "state_database": "/work/state",
                    "task_name": "CodeStudio-Agent-test", "executable": "/bin/agent", "started": False,
                    "trigger": "current_user_logon", "user_sid": "S-1-5-21-test"}
        for method in ("scheduler/autostart/status", "scheduler/autostart/enable", "scheduler/autostart/disable"):
            frame = {"jsonrpc": "2.0", "id": 1, "result": ordinary}
            self.contract.validate_response(method, frame)
            supervised = {**ordinary, "supervised": True, "max_restarts": 3, "restart_delay_ms": 1000}
            self.contract.validate_response(method, {**frame, "result": supervised})
            for key in ("supervised", "max_restarts", "restart_delay_ms"):
                bad = dict(supervised); del bad[key]
                with self.subTest(method=method, missing=key), self.assertRaises(ValidationError):
                    self.contract.validate_response(method, {**frame, "result": bad})
            for key, value in (("supervised", False), ("max_restarts", -1), ("max_restarts", 101),
                               ("max_restarts", True), ("restart_delay_ms", 99),
                               ("restart_delay_ms", 60001), ("restart_delay_ms", 1000.0)):
                with self.subTest(method=method, field=key, value=value), self.assertRaises(ValidationError):
                    self.contract.validate_response(method, {**frame, "result": {**supervised, key: value}})
            for key, value in (("max_restarts", 3), ("restart_delay_ms", 1000)):
                with self.subTest(method=method, ordinary_extra=key), self.assertRaises(ValidationError):
                    self.contract.validate_response(method, {**frame, "result": {**ordinary, key: value}})

    def test_supervised_start_receipt_is_complete_and_distinguishes_processes(self):
        ordinary = {"state": "running", "owner": "w", "pid": 12, "generation": 1, "stop_epoch": 0,
                    "started_at_ms": 10, "heartbeat_at_ms": 10, "started": True}
        frame = {"jsonrpc": "2.0", "id": 1, "result": ordinary}
        self.contract.validate_response("scheduler/start", frame)
        supervised = {**ordinary, "supervised": True, "supervisor_pid": 11,
                      "max_restarts": 3, "restart_delay_ms": 1000}
        self.contract.validate_response("scheduler/start", {**frame, "result": supervised})
        for key in ("supervised", "supervisor_pid", "max_restarts", "restart_delay_ms"):
            bad = dict(supervised); del bad[key]
            with self.subTest(missing=key), self.assertRaises(ValidationError):
                self.contract.validate_response("scheduler/start", {**frame, "result": bad})
        for key, value in (("supervised", False), ("supervisor_pid", 0), ("supervisor_pid", 11.0),
                           ("max_restarts", 101), ("restart_delay_ms", 99)):
            with self.subTest(field=key, value=value), self.assertRaises(ValidationError):
                self.contract.validate_response("scheduler/start", {**frame, "result": {**supervised, key: value}})

    def test_credential_references_are_bounded_and_never_inline_secrets(self):
        for name in ("runOptions", "resumeOptions"):
            base = {"prompt": "test"} if name == "runOptions" else {}
            for reference in ("env:CODESTUDIO_API_KEY", "env:_TEST", "system:my-provider.1", "system:" + "a" * 128):
                with self.subTest(name=name, reference=reference):
                    self.contract._definition(name, {**base, "credential": reference})
            for reference in ("", "token-value", "file:secret", "env:9NAME", "system:../file", "env:NAME\n",
                              "system:" + "a" * 129, 123, None):
                with self.subTest(name=name, invalid=reference), self.assertRaises(ValidationError):
                    self.contract._definition(name, {**base, "credential": reference})
            with self.assertRaises(ValidationError):
                self.contract._definition(name, {**base, "api_key": "not-allowed"})

    def test_supervisor_queries_are_bounded_and_read_only(self):
        for params in ({}, {"after": 0, "limit": 1}, {"after": 9223372036854775807, "limit": 100}):
            self.contract.validate_request(_request("scheduler/supervisor/events", params))
        for params in ({"after": -1}, {"limit": 0}, {"limit": 101}, {"after": True},
                       {"after": 1.0}, {"limit": "1"}, {"run_id": "r"}):
            with self.subTest(params=params), self.assertRaises(ValidationError):
                self.contract.validate_request(_request("scheduler/supervisor/events", params))
        self.contract.validate_request(_request("scheduler/supervisor/status", {}))
        with self.assertRaises(ValidationError):
            self.contract.validate_request(_request("scheduler/supervisor/status", {"pid": 10}))
        status = {"record": None, "observation": {"state": "not_checked", "observed_at_ms": 1}, "worker_matches": False}
        self.contract.validate_response("scheduler/supervisor/status", {"jsonrpc": "2.0", "id": 1, "result": status})
        for missing in status:
            bad = dict(status); del bad[missing]
            with self.subTest(missing=missing), self.assertRaises(ValidationError):
                self.contract.validate_response("scheduler/supervisor/status", {"jsonrpc": "2.0", "id": 1, "result": bad})
        page = {"items": [], "next_after": 0, "has_more": False}
        self.contract.validate_response("scheduler/supervisor/events", {"jsonrpc": "2.0", "id": 1, "result": page})

    def test_operation_completion_omission_and_state_are_explicit(self):
        completion = {"session_id": "s", "operation_id": "o", "exit_code": 0, "result": None,
                      "result_omitted": True, "result_bytes": 262145, "result_refs": {"task_id": "t"}}
        self.contract.validate_completion("tasks/start", completion)
        snapshot = {"session_id": "s", "operation_id": "o", "method": "tasks/start",
                    "state": "finished", "cancel_requested": False, "completion": completion}
        frame = {"jsonrpc": "2.0", "id": 1, "result": snapshot}
        self.contract.validate_response("operations/get", frame)
        for key, value in [("state", "running"), ("operation_id", "different")]:
            bad = copy.deepcopy(frame); bad["result"][key] = value
            with self.subTest(field=key), self.assertRaises(ValidationError):
                self.contract.validate_response("operations/get", bad)
        for key in ("result_omitted", "result_bytes", "result_refs", "session_id"):
            bad = copy.deepcopy(completion); del bad[key]
            with self.subTest(missing=key), self.assertRaises(ValidationError):
                self.contract.validate_completion("tasks/start", bad)

    def test_errors_and_notifications_have_structured_envelopes(self):
        error = {"code": -32000, "message": "unknown", "data": {"kind": "operation_failed",
                 "recovery": "query_state", "outcome": "unknown", "session_id": "s"}}
        self.contract.validate_response("tasks/start", {"jsonrpc": "2.0", "id": 1, "error": error})
        self.contract.validate_notification({"jsonrpc": "2.0", "method": "operation/finished", "params": {
            "session_id": "s", "operation_id": "o", "exit_code": 2, "error": error}})
        event_error = copy.deepcopy(error); event_error["data"]["kind"] = "command_failed"
        self.contract.validate_notification({"jsonrpc": "2.0", "method": "events/error", "params": {
            "after": 10, "message": "unknown", "error": event_error}})
        for replacement in ({"code": -32602}, {"data": {**error["data"], "outcome": "not_started"}},
                            {"message": "界" * 2731}):
            with self.subTest(error_change=list(replacement)), self.assertRaises(ValidationError):
                self.contract.validate_response("tasks/start", {"jsonrpc": "2.0", "id": 1,
                    "error": {**error, **replacement}})
        with self.assertRaises(ValidationError):
            self.contract.validate_notification({"jsonrpc": "2.0", "method": "operation/text", "params": {
                "operation_id": "o", "text": "draft", "provisional": False}})
        with self.assertRaises(ValidationError):
            self.contract.validate_response("tasks/start", {"jsonrpc": "2.0", "id": 1,
                "error": {"code": -32000, "message": "missing structured data"}})


if __name__ == "__main__":
    unittest.main()
