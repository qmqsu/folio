#!/usr/bin/env python3
"""Assemble one atomic Pages artifact; immutable firmware and monotonic pointers."""
import argparse
import io
import tarfile
from pathlib import Path

import release_bundle as bundle
import release_manifest as protocol
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, utils

PUBLIC_FILES = set(protocol.PARTS.values()) | {"ota.json", "usb.json", "provenance.json", "materials.json", "materials.sig"}
SNAPSHOT_LIMIT = 400 * 1024 * 1024


def unpack_snapshot(raw):
    protocol.require(len(raw) <= SNAPSHOT_LIMIT, "Previous snapshot too large")
    files, total = {}, 0
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:gz") as archive:
        for member in archive:
            bundle.safe_relative(member.name)
            protocol.require(member.isfile() and member.name not in files, "Snapshot links/directories/duplicates prohibited")
            protocol.require(member.name.startswith(("firmware/", "channels/", "acceptance/")) or member.name == "release-state.json", "Snapshot unexpected path")
            total += member.size
            protocol.require(total <= SNAPSHOT_LIMIT and member.size >= 0, "Expanded snapshot too large")
            files[member.name] = archive.extractfile(member).read()
    return files


def pack_snapshot(files):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
        for name, raw in sorted(files.items()):
            member = tarfile.TarInfo(name)
            member.size, member.mtime, member.mode = len(raw), 0, 0o644
            archive.addfile(member, io.BytesIO(raw))
    return buffer.getvalue()


def verify_public_release(files, key):
    protocol.require(set(files) == PUBLIC_FILES, "Public release files incomplete or unknown")
    signature = files["materials.sig"]
    protocol.require(len(signature) == 64, "Invalid materials signature")
    key.verify(utils.encode_dss_signature(int.from_bytes(signature[:32], "big"), int.from_bytes(signature[32:], "big")), files["materials.json"], ec.ECDSA(hashes.SHA256()))
    inventory = protocol.load_json(files["materials.json"])
    for name in PUBLIC_FILES - {"materials.json", "materials.sig"}:
        protocol.require(inventory.get(name) == protocol.digest(files[name]), "Public release material mismatch")
    ota, usb = [protocol.verify(files[kind + ".json"], key) for kind in ("ota", "usb")]
    for field in ("version", "sequence", "channel", "project", "layout"):
        protocol.require(ota[field] == usb[field], "Mixed OTA/USB release")
    protocol.require(ota["channel"] == "preview", "Immutable version must originate as preview")
    for part in usb["parts"]:
        raw = files[protocol.PARTS[part["offset"]]]
        protocol.require(len(raw) == part["size"] and protocol.digest(raw) == part["sha256"], "Firmware bytes mismatch")
    app = next(part for part in usb["parts"] if part["offset"] == 0x10000)
    protocol.require(ota["image"] == {key: value for key, value in app.items() if key != "offset"}, "OTA image differs from USB application")
    protocol.require(protocol.app_descriptor(files["folio.bin"])["version"] == ota["version"], "Mixed descriptor version")
    provenance = protocol.load_json(files["provenance.json"])
    protocol.require(provenance["version"] == ota["version"] and provenance["sequence"] == ota["sequence"] and provenance["app_sha256"] == ota["image"]["sha256"], "Mixed provenance")
    return ota


def validate_snapshot(files, key):
    if not files:
        return {"schema": 1, "generation": 0, "versions": []}
    state = protocol.load_json(files["release-state.json"])
    protocol.require(set(state) == {"schema", "generation", "versions"} and state["schema"] == 1 and type(state["generation"]) is int and state["generation"] > 0, "Invalid snapshot state")
    expected = {"release-state.json"}
    seen = set()
    for version in state["versions"]:
        protocol.version_ok(version)
        protocol.require(version not in seen, "Duplicate snapshot version")
        seen.add(version)
        prefix = "firmware/" + version + "/"
        release = {name: files[prefix + name] for name in PUBLIC_FILES}
        protocol.require(verify_public_release(release, key)["version"] == version, "Wrong immutable directory version")
        expected.update(prefix + name for name in PUBLIC_FILES)
    for channel in ("preview", "stable"):
        ota_path, usb_path = f"channels/{channel}.json", f"channels/{channel}-usb.json"
        if ota_path not in files:
            protocol.require(usb_path not in files, "Half-published channel")
            continue
        expected.update((ota_path, usb_path))
        for kind, path in (("ota", ota_path), ("usb", usb_path)):
            payload = protocol.verify(files[path], key)
            protocol.require(payload["channel"] == channel and payload["kind"] == kind and payload["version"] in seen, "Channel target missing")
            original = protocol.verify(files[f"firmware/{payload['version']}/{kind}.json"], key)
            original["channel"] = channel
            protocol.require(original == payload, "Channel differs from immutable version")
        if channel == "stable":
            acceptance_path = f"acceptance/{payload['version']}.json"
            expected.add(acceptance_path)
            provenance = protocol.load_json(files[f"firmware/{payload['version']}/provenance.json"])
            bundle.validate_acceptance(files[acceptance_path], provenance)
    # Preserve earlier stable acceptance documents, but never arbitrary extra paths.
    for name in files:
        if name.startswith("acceptance/"):
            version = name[len("acceptance/"):-len(".json")]
            protocol.require(name.endswith(".json") and version in seen, "Unknown acceptance path")
            bundle.validate_acceptance(files[name], protocol.load_json(files[f"firmware/{version}/provenance.json"]))
            expected.add(name)
    protocol.require(set(files) == expected, "Unexpected snapshot files")
    return state


def compose(previous, key, candidate=None, promotion=None):
    files = dict(previous)
    state = validate_snapshot(files, key)
    if candidate is not None:
        bundle.verify_materials(candidate, key)
        public = {name: candidate[name] for name in PUBLIC_FILES}
        payload = verify_public_release(public, key)
        version, sequence = payload["version"], payload["sequence"]
        protocol.require(version not in state["versions"], "Immutable version already published; duplicate release refused")
        previous_sequences = [protocol.verify(files[f"firmware/{v}/ota.json"], key)["sequence"] for v in state["versions"]]
        protocol.require(sequence > max(previous_sequences, default=protocol.BASELINE - 1), "Old/duplicate workflow cannot replace newer release")
        for name, raw in public.items():
            files[f"firmware/{version}/{name}"] = raw
        state["versions"].append(version)
        files["channels/preview.json"] = public["ota.json"]
        files["channels/preview-usb.json"] = public["usb.json"]
    if promotion is not None:
        protocol.require(candidate is None, "Cannot release and promote together")
        protocol.require(set(promotion) == {"ota.json", "usb.json", "acceptance.json"}, "Invalid promotion files")
        payload = protocol.verify(promotion["ota.json"], key)
        version = payload["version"]
        protocol.require(payload["channel"] == "stable" and version in state["versions"], "Promotion target not published")
        if "channels/stable.json" in files:
            previous_stable = protocol.verify(files["channels/stable.json"], key)
            protocol.require(payload["sequence"] > previous_stable["sequence"], "Duplicate/backward stable promotion")
        provenance = protocol.load_json(files[f"firmware/{version}/provenance.json"])
        bundle.validate_acceptance(promotion["acceptance.json"], provenance)
        files["channels/stable.json"] = promotion["ota.json"]
        files["channels/stable-usb.json"] = promotion["usb.json"]
        files[f"acceptance/{version}.json"] = promotion["acceptance.json"]
    state["generation"] += 1
    files["release-state.json"] = protocol.canonical(state)
    validate_snapshot(files, key)
    protocol.require(sum(map(len, files.values())) <= SNAPSHOT_LIMIT, "Pages history limit reached; do not silently delete previous versions")
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", required=True, type=Path)
    parser.add_argument("--previous", type=Path)
    parser.add_argument("--expected-sha256")
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--promotion", type=Path)
    parser.add_argument("--public-key", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    previous = {}
    if args.previous:
        raw = args.previous.read_bytes()
        protocol.require(args.expected_sha256 and protocol.digest(raw) == args.expected_sha256, "Snapshot does not match authoritative state; stop rather than downgrade")
        previous = unpack_snapshot(raw)
    else:
        protocol.require(not args.expected_sha256, "Missing authoritative previous snapshot")
    files = compose(previous, protocol.public_key(args.public_key), bundle.read_files(args.candidate) if args.candidate else None,
                    bundle.read_files(args.promotion) if args.promotion else None)
    site = bundle.read_files(args.site)
    protocol.require(not any(name.startswith(("firmware/", "channels/", "acceptance/")) or name.startswith("release-state") for name in site), "Static site must not supply release state")
    site.update(files)
    snapshot = pack_snapshot(files)
    site["release-state.tar.gz"] = snapshot
    site["release-state.sha256"] = (protocol.digest(snapshot) + "\n").encode()
    bundle.write_new(args.output, site)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError) as error:
        raise SystemExit(str(error)) from None
