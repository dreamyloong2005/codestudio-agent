#!/usr/bin/env python3
"""Capture public GitHub Actions state for an exact commit; never dispatch a job.

Usage: python3 research/macos-durable-requests/capture_ci.py \
  --sha <40-character-commit> --output <new-evidence-file.json>

This records API metadata only. Success metadata is not a downloaded artifact
validation or a native local test receipt. Existing snapshots are not replaced.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
from urllib.error import HTTPError
from urllib.request import Request, urlopen

BASE = "https://api.github.com/repos/dreamyloong2005/codestudio-agent"
WORKFLOW = ".github/workflows/native.yml"
RUN_FIELDS = ("id", "name", "path", "event", "status", "conclusion", "workflow_id",
              "head_branch", "head_sha", "html_url", "created_at", "updated_at", "run_attempt")
JOB_FIELDS = ("id", "name", "status", "conclusion", "head_sha", "started_at", "completed_at",
              "runner_id", "runner_name", "labels", "html_url", "steps")
ARTIFACT_FIELDS = ("id", "name", "size_in_bytes", "digest", "expired", "created_at",
                   "updated_at", "expires_at", "workflow_run")


def get(path: str) -> tuple[int, dict]:
    request = Request(BASE + path, headers={"Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "CodeStudio-Agent-CI-Evidence"})
    try:
        response = urlopen(request, timeout=20)
    except HTTPError as error:
        response = error
    with response:
        data = response.read(4 * 1024 * 1024 + 1)
        if len(data) > 4 * 1024 * 1024:
            raise RuntimeError("GitHub metadata response exceeded the evidence bound")
        value = json.loads(data)
        if not isinstance(value, dict):
            raise RuntimeError("Expected an object response from GitHub")
        return response.status, value


def select(value: dict, fields: tuple[str, ...]) -> dict:
    return {key: value[key] for key in fields if key in value}


def collect(sha: str) -> dict:
    report = {"format": 1, "product": "CodeStudio Agent", "head_sha": sha,
              "captured_at_utc": datetime.now(timezone.utc).isoformat(),
              "scope": "Public GitHub Actions metadata for the exact requested commit",
              "workflow": WORKFLOW, "api_access": "unauthenticated_public_read_only",
              "status": "not_observed", "native_ci_verified": False,
              "artifact_bytes_downloaded_or_validated": False, "runs": []}
    path = f"/actions/runs?head_sha={sha}&per_page=100"
    status, data = get(path)
    report["query"] = {"url": BASE + path, "http_status": status}
    if status != 200:
        report.update(status="api_read_failed", api_error=data.get("message"))
        return report
    if data.get("total_count", 0) > 100:
        raise RuntimeError("Too many runs for one commit; explicit pagination is required")
    matches = [run for run in data.get("workflow_runs", [])
               if run.get("head_sha") == sha and run.get("path") == WORKFLOW]
    for run in sorted(matches, key=lambda item: (item["id"], item.get("run_attempt", 0))):
        run_id = run["id"]
        if not isinstance(run_id, int) or run_id <= 0:
            raise RuntimeError("Invalid GitHub run identity")
        result = select(run, RUN_FIELDS)
        for endpoint, collection, fields in (("jobs", "jobs", JOB_FIELDS),
                                             ("artifacts", "artifacts", ARTIFACT_FIELDS)):
            subpath = f"/actions/runs/{run_id}/{endpoint}?per_page=100"
            substatus, subdata = get(subpath)
            record = {"url": BASE + subpath, "http_status": substatus}
            if substatus == 200:
                if subdata.get("total_count", 0) > 100:
                    raise RuntimeError("Too many job/artifact records; explicit pagination is required")
                record["total_count"] = subdata.get("total_count")
                record[collection] = [select(item, fields) for item in subdata.get(collection, [])]
            else:
                record["error"] = subdata.get("message")
            result[collection] = record
        report["runs"].append(result)
    if matches:
        report["status"] = "remote_runs_observed_not_artifact_verified"
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.sha):
        parser.error("--sha must be the exact lowercase 40-character commit ID")
    if not args.output.parent.is_dir() or args.output.exists():
        parser.error("--output requires an existing parent and a new destination")
    report = collect(args.sha)
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    print(json.dumps({"status": report["status"], "head_sha": args.sha,
                      "runs": [{key: run.get(key) for key in ("id", "status", "conclusion", "html_url")}
                               for run in report["runs"]], "output": str(args.output)}))
    return 0 if report["query"]["http_status"] == 200 else 1


if __name__ == "__main__":
    raise SystemExit(main())
