"""Exercise refusal before creating an unrecordable deep staging path."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path.cwd().resolve()
binary = root / "build/linux/x86_64/release"
evidence = Path(__file__).resolve().parent
group = Path(os.environ["CODESTUDIO_CGROUP_ROOT"])
before = set(group.iterdir())
with tempfile.TemporaryDirectory(prefix="codestudio-resource-depth-") as temporary:
    owned = Path(temporary).resolve()
    workspace = owned / "workspace"
    workspace.mkdir()
    deep = owned.joinpath(*(["a"] * 130))
    deep.mkdir(parents=True)
    env = dict(os.environ, TMPDIR=str(deep), CODESTUDIO_SANDBOX_REGISTRY=str(owned / "registry"))
    attempt = subprocess.run([str(binary / "sandbox-resource-fixture"), "hold",
                              str(binary / "sandbox-fixture"), str(workspace)],
                             input=b"stop\n", capture_output=True, env=env, timeout=20)
    assert attempt.returncode == 3, (attempt.stdout, attempt.stderr)
    error = json.loads(attempt.stdout)["fixture_error"]
    assert "ancestry" in error and "limit" in error, error
    listing = subprocess.run([str(binary / "codestudio-agent"), "sandbox-resources"],
                             capture_output=True, env=env, check=True, timeout=10)
    assert json.loads(listing.stdout)["items"] == [], listing.stdout
    assert not list(deep.iterdir()), "Unrecordable staging resource was created"
    assert set(group.iterdir()) == before, "Preparation left an owned cgroup"
receipt = {"format": 2, "status": "succeeded", "case": "deep_staging_path_refused_before_creation",
           "registry_empty": True, "owned_cgroups_released": True, "temporary_root_removed": True}
(evidence / "linux-depth-probe.json").write_text(json.dumps(receipt, indent=2) + "\n")
print(json.dumps(receipt))
