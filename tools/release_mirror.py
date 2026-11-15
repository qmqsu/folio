#!/usr/bin/env python3
"""Mirror verified public release assets to the jsDelivr-backed branch.

Only firmware/channel/acceptance files already present in the verified Pages
artifact are copied. No private source, credentials or device data are read.
"""
import argparse
import base64
import json
import subprocess
import time
from pathlib import Path
from urllib.request import urlopen

import release_manifest as protocol
from release_bundle import PRIVATE

REPO = "qmqsu/folio"
BRANCH = "folio-release-assets"
PREFIX = f"https://testingcf.jsdelivr.net/gh/{REPO}@{BRANCH}/"
PURGE_PREFIX = f"https://purge.jsdelivr.net/gh/{REPO}@{BRANCH}/"
ALLOWED = ("channels/", "firmware/", "acceptance/")


def gh(*args, input_text=None):
    result = subprocess.run(["gh", "api", *args], input=input_text, text=True,
                            capture_output=True, check=True)
    return json.loads(result.stdout) if result.stdout.strip() else {}


def mirror_files(root):
    root = root.resolve()
    files = {}
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        if not relative.startswith(ALLOWED):
            continue
        protocol.require(not path.is_symlink(), "Mirror symlink prohibited")
        raw = path.read_bytes()
        protocol.require(len(raw) <= 50 * 1024 * 1024, "Mirror file exceeds bound")
        protocol.require(not PRIVATE.search(raw) and b"/Users/" not in raw,
                         "Private content prohibited in mirror")
        files[relative] = raw
    protocol.require(files, "No public release assets to mirror")
    return files


def download(url, limit=50 * 1024 * 1024):
    with urlopen(url, timeout=30) as response:
        raw = response.read(limit + 1)
    protocol.require(len(raw) <= limit, "Mirror response exceeds bound")
    return raw


def purge_channel(name, branch=BRANCH):
    protocol.require(name.startswith("channels/"), "Only channel pointers need cache purging")
    with urlopen(PURGE_PREFIX + name, timeout=30) as response:
        result = json.loads(response.read(8192))
    protocol.require(result.get("status") == "finished", "CDN channel purge failed: " + name)


def publish(root, branch=BRANCH, downloader=download, retries=12):
    files = mirror_files(root)
    ref = f"refs/heads/{branch}"
    try:
        head = gh(f"repos/{REPO}/git/ref/heads/{branch}")["object"]["sha"]
    except subprocess.CalledProcessError:
        main = gh(f"repos/{REPO}/git/ref/heads/main")["object"]["sha"]
        gh("--method", "POST", f"repos/{REPO}/git/refs", "--input", "-",
           input_text=json.dumps({"ref": ref, "sha": main}))
        head = main
    tree = []
    for name, raw in sorted(files.items()):
        blob = gh("--method", "POST", f"repos/{REPO}/git/blobs", "--input", "-",
                  input_text=json.dumps({"encoding": "base64", "content": base64.b64encode(raw).decode()}))
        tree.append({"path": name, "mode": "100644", "type": "blob", "sha": blob["sha"]})
    tree_sha = gh("--method", "POST", f"repos/{REPO}/git/trees", "--input", "-",
                  input_text=json.dumps({"base_tree": head, "tree": tree}))["sha"]
    commit = gh("--method", "POST", f"repos/{REPO}/git/commits", "--input", "-",
                input_text=json.dumps({"message": "Mirror verified Folio release assets", "tree": tree_sha,
                                       "parents": [head]}))["sha"]
    gh("--method", "PATCH", f"repos/{REPO}/git/refs/heads/{branch}", "--input", "-",
       input_text=json.dumps({"sha": commit, "force": False}))
    for name in files:
        if name.startswith("channels/"):
            purge_channel(name, branch)
    # The branch commit above is the authoritative mirror content. jsDelivr edges
    # serve a cached copy of @branch and can lag for hours after a purge, so a
    # slow edge must not fail the release: Pages stays the primary origin and
    # every downloaded byte is still verified against the signed manifest.
    stale = []
    for name, expected in sorted(files.items()):
        url = PREFIX + name
        # Give a purged channel pointer a short read-through window, then report it.
        attempts = 18 if name.startswith("channels/") else retries
        for attempt in range(attempts):
            try:
                if downloader(url) == expected:
                    break
            except OSError:
                pass
            else:
                pass
            if attempt + 1 == attempts:
                stale.append(name)
                break
            time.sleep(5)
    result = {"branch": branch, "files": len(files), "verified": not stale}
    if stale:
        result["edge_stale"] = stale
        print("CDN edge still serves a cached copy of: " + ", ".join(stale) +
              " (fallback only; Pages remains authoritative and the branch is already updated)")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--branch", default=BRANCH)
    args = parser.parse_args()
    result = publish(args.root, args.branch)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from None
