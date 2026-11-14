#!/usr/bin/env python3
"""GitHub-only publication transport. Never called by host tests; no implicit writes."""
import argparse
import base64
import io
import json
import os
import re
import subprocess
import time
import urllib.request
import urllib.error
import zipfile
from pathlib import Path

import release_bundle as bundle
import release_manifest as protocol

REPO = "qmqsu/folio"
STATE_BRANCH = "folio-pages-state"
STATE_PATH = ".github/pages-state.json"


def gh(*args):
    return subprocess.check_output(["gh", *args], text=True)


def download(url, limit=450 * 1024 * 1024):
    protocol.require(url.startswith(protocol.BASE_URL + "/"), "Download must use publisher Pages origin")
    request = urllib.request.Request(url, headers={"Cache-Control": "no-cache", "Pragma": "no-cache"})
    with urllib.request.urlopen(request, timeout=90) as response:
        protocol.require(response.geturl().startswith(protocol.BASE_URL + "/"), "Cross-origin redirect prohibited")
        raw = response.read(limit + 1)
    protocol.require(len(raw) <= limit, "Download exceeds bound")
    return raw


def read_state(output):
    protocol.require(not output.exists(), "State output already exists")
    output.mkdir(parents=True)
    response = protocol.load_json(gh("api", f"repos/{REPO}/contents/{STATE_PATH}?ref={STATE_BRANCH}"))
    state = protocol.load_json(base64.b64decode(response["content"]))
    protocol.require(state.get("schema") == 1 and type(state.get("generation")) is int, "Invalid authoritative deployment state")
    (output / "authority.json").write_bytes(protocol.canonical({"blob_sha": response["sha"], "state": state}))
    if state["generation"] == 0:
        protocol.require(state.get("snapshot_sha256") is None, "Invalid bootstrap state")
        # Do not silently bootstrap over an already deployed snapshot after state loss.
        try:
            download(protocol.BASE_URL + "/release-state.sha256?bootstrap=" + str(time.time_ns()), limit=256)
        except urllib.error.HTTPError as error:
            protocol.require(error.code == 404, "Bootstrap requires an absent site snapshot (HTTP 404)")
        else:
            raise ValueError("Existing deployed snapshot with zero authority; manual recovery required")
        return
    expected = state["snapshot_sha256"]
    protocol.require(protocol.HASH.fullmatch(expected), "Invalid state snapshot hash")
    query = f"?run={os.environ.get('GITHUB_RUN_ID', 'local')}-{time.time_ns()}"
    raw = download(protocol.BASE_URL + "/release-state.tar.gz" + query)
    protocol.require(protocol.digest(raw) == expected, "Live Pages differs from authoritative state; manual recovery required, refusing downgrade")
    (output / "previous.tar.gz").write_bytes(raw)


def commit_state(authority, site):
    old = protocol.load_json(authority.read_bytes())
    raw = (site / "release-state.tar.gz").read_bytes()
    expected = protocol.digest(raw)
    state = protocol.load_json((site / "release-state.json").read_bytes())
    protocol.require(state["generation"] == old["state"]["generation"] + 1, "Deployment generation conflict")
    # Verify the actual URL, not merely deploy-pages success; never commit a stale CDN snapshot.
    for attempt in range(12):
        try:
            live = download(protocol.BASE_URL + "/release-state.tar.gz?verify=" + str(time.time_ns()))
            if protocol.digest(live) == expected:
                break
        except (OSError, ValueError):
            pass
        if attempt == 11:
            raise ValueError("Deployed snapshot not visible; authority left unchanged; manual recovery required")
        time.sleep(10)
    next_state = {"schema": 1, "generation": state["generation"], "snapshot_sha256": expected,
                  "run_id": os.environ["GITHUB_RUN_ID"]}
    body = {"message": "Record verified atomic Pages deployment", "branch": STATE_BRANCH, "sha": old["blob_sha"],
            "content": base64.b64encode(protocol.canonical(next_state)).decode()}
    subprocess.run(["gh", "api", "--method", "PUT", f"repos/{REPO}/contents/{STATE_PATH}", "--input", "-"],
                   input=json.dumps(body), text=True, check=True, stdout=subprocess.DEVNULL)


def require_environment(name):
    protocol.require(name in ("release-signing", "release-stable"), "Unknown signing environment")
    data = protocol.load_json(gh("api", f"repos/{REPO}/environments/{name}"))
    rules = data.get("protection_rules", [])
    protocol.require(any(row.get("type") == "required_reviewers" and row.get("reviewers") for row in rules), "Signing environment requires configured human reviewers")
    policy = data.get("deployment_branch_policy")
    protocol.require(isinstance(policy, dict) and (policy.get("protected_branches") or policy.get("custom_branch_policies")), "Signing environment requires branch/tag restrictions")


def extract_zip(path, output):
    files, total = {}, 0
    with zipfile.ZipFile(path) as archive:
        for row in archive.infolist():
            bundle.safe_relative(row.filename)
            protocol.require(not row.is_dir() and row.filename not in files and (row.external_attr >> 16) & 0o170000 != 0o120000, "Unsafe offline zip member")
            total += row.file_size
            protocol.require(total <= 800 * 1024 * 1024, "Offline archive expansion exceeds bound")
            files[row.filename] = archive.read(row)
    bundle.write_new(output, files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("read-state"); p.add_argument("--output", required=True, type=Path)
    p = sub.add_parser("commit-state"); p.add_argument("--authority", required=True, type=Path); p.add_argument("--site", required=True, type=Path)
    p = sub.add_parser("extract"); p.add_argument("--zip", required=True, type=Path); p.add_argument("--output", required=True, type=Path)
    p = sub.add_parser("environment"); p.add_argument("name")
    p = sub.add_parser("version"); p.add_argument("tag")
    args = parser.parse_args()
    if args.command == "read-state":
        read_state(args.output)
    elif args.command == "commit-state":
        commit_state(args.authority, args.site)
    elif args.command == "extract":
        extract_zip(args.zip, args.output)
    elif args.command == "environment":
        require_environment(args.name)
    else:
        protocol.require(args.tag.startswith("v"), "Version tag must start with v")
        version = args.tag[1:]
        print(json.dumps({"version": version, "sequence": protocol.version_sequence(version)}))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from None
