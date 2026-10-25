"""Run verification inside an already owned private Secret Service, then scan names.

Pass this helper as the command to research/credentials/linux_secrets.py. It
never reads secret blobs, unlocks a collection, deletes entries or touches the
desktop user's session. Root owns executing the verification command.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid
from datetime import datetime, timezone


evidence = Path(__file__).resolve().parent
assert os.name != "nt" and os.environ.get("CODESTUDIO_TEST_PRIVATE_SECRET_SERVICE") == "1"
assert os.environ.get("DBUS_SESSION_BUS_ADDRESS", "").startswith("unix:")
runtime = Path(os.environ["XDG_RUNTIME_DIR"])
assert any(part.startswith("codestudio-secret-service-") for part in runtime.parts)
arguments = sys.argv[1:]
if arguments[:1] == ["--"]:
    arguments.pop(0)
assert arguments, "Expected a verification command"
def verification_paths():
    return {path for folder in ("linux", "installed-linux")
            for path in (evidence / folder).glob("*-*/verification.json")}


before = verification_paths()
stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
receipt = {"format": 2, "platform": "linux", "private_secret_service": True,
           "started_at": datetime.now(timezone.utc).isoformat(), "status": "failed",
           "scope": "Synthetic identifier attributes only; no secret load/unlock/delete or desktop bus access"}


def call(path, method, *parameters):
    response = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.secrets",
                               "--object-path", path, "--method", method, *parameters],
                              capture_output=True, text=True, timeout=10)
    assert response.returncode == 0, "Cannot inspect the private Secret Service metadata"
    assert len(response.stdout.encode()) <= 65536, "Private service metadata exceeds bound"
    return response.stdout


code = 2
try:
    code = subprocess.call(arguments)
    receipt["verification_exit_code"] = code
    listing = call("/org/freedesktop/secrets", "org.freedesktop.Secret.Service.SearchItems",
                   "{'application': 'CodeStudio-Agent'}")
    paths = sorted(set(re.findall(r"'(/org/freedesktop/secrets/[A-Za-z0-9_/]+)'", listing)))
    assert len(paths) <= 1024, "Private service item count exceeds bound"
    ordinary, oauth = [], []
    for path in paths:
        attributes = call(path, "org.freedesktop.DBus.Properties.Get", "org.freedesktop.Secret.Item", "Attributes")
        found = re.search(r"'identifier':\s*(?:<)?'([^']+)'", attributes)
        assert found, "Private service item has no identifier attribute"
        identifier = found[1]
        if identifier.startswith("test-credential-"):
            ordinary.append(identifier)
        elif identifier.startswith("oauth:test-credential-oauth-"):
            oauth.append(identifier)
    receipt.update(ordinary_test_entries=ordinary, oauth_test_entries=oauth,
                   private_items_examined=len(paths), namespace_scan="completed")
    assert not ordinary and not oauth, "Synthetic native credential entries remain in private service"
    receipt["status"] = "succeeded" if code == 0 else "verification_failed_cleanup_verified"
except Exception:
    receipt["cleanup_error"] = "Private credential metadata verification failed; inspect the owned fixture before teardown"
    raise
finally:
    receipt["verification_receipts"] = sorted(path.relative_to(evidence).as_posix()
        for path in verification_paths() - before)
    receipt["finished_at"] = datetime.now(timezone.utc).isoformat()
    destination = evidence / ("credential-cleanup-linux-" + stamp + ".json")
    destination.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"credential_cleanup_receipt": str(destination), "status": receipt["status"]}))
raise SystemExit(code)
