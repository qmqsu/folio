#!/usr/bin/env python3
"""Strict Folio release envelopes. No network access and no key generation."""
import argparse
import base64
import hashlib
import json
import os
import re
import struct
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

KEY_ID = "folio-release-2026-3"
BASE_URL = "https://qmqsu.github.io/folio"
BASELINE = 8032000
APP_LIMIT = 3 * 1024 * 1024
FLASH_SIZE = 16777216
IDF_COMMIT = "fff9895c82d744c7237be8847347bdd1b07c6643"
LAYOUT = "folio-16m-factory-storage-ota-v2"
PARTS = {0: "bootloader.bin", 0x8000: "partition-table.bin", 0x10000: "folio.bin", 0x910000: "ota_data_initial.bin"}
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(?:-(?:dev|rc)(?:\.[0-9]+)?)?\Z")
HASH = re.compile(r"[0-9a-f]{64}\Z")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "Duplicate JSON field")
        result[key] = value
    return result


def load_json(raw):
    return json.loads(raw, object_pairs_hook=unique_object,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Nonfinite JSON")))


def canonical(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def version_ok(version):
    require(isinstance(version, str) and len(version) <= 31 and VERSION.fullmatch(version), "Invalid release version")


def version_sequence(version):
    version_ok(version)
    core, _, suffix = version.partition("-")
    major, minor, patch = map(int, core.split("."))
    require(core == f"{major}.{minor}.{patch}" and minor <= 99 and patch <= 999, "Version numeric range/canonical form invalid")
    if not suffix:
        stage = 999
    elif suffix == "dev":
        stage = 0
    elif suffix.startswith("dev."):
        number = suffix[4:]
        require(number.isdigit() and str(int(number)) == number and 1 <= int(number) <= 499, "Invalid dev sequence")
        stage = int(number)
    elif suffix.startswith("rc."):
        number = suffix[3:]
        require(number.isdigit() and str(int(number)) == number and 1 <= int(number) <= 498, "Invalid rc sequence")
        stage = 500 + int(number)
    else:
        raise ValueError("Use numbered rc.N releases")
    sequence = major * 100000000 + minor * 1000000 + patch * 1000 + stage
    require(BASELINE <= sequence <= 0xFFFFFFFF, "Version sequence outside allowed uint32 release range")
    return sequence


def validate_payload(payload):
    require(isinstance(payload, dict), "Payload must be an object")
    common = {"schema", "kind", "project", "chip", "hardware", "layout", "channel", "version", "sequence"}
    kind = payload.get("kind")
    require(kind in ("ota", "usb"), "Invalid manifest kind")
    required = common | ({"image"} if kind == "ota" else {"flash_size", "parts"})
    require(required <= payload.keys() and payload.keys() <= required | {"notes"}, "Missing or unknown payload fields")
    for key, value in {"schema": 1, "project": "Folio", "chip": "esp32s3", "hardware": "RDP-G01-W", "layout": LAYOUT}.items():
        require(payload.get(key) == value, "Wrong release identity: " + key)
    require(type(payload["schema"]) is int, "Invalid schema type")
    version_ok(payload["version"])
    require(payload["channel"] in ("preview", "stable"), "Invalid channel")
    require(payload["channel"] != "stable" or "-" not in payload["version"], "Prerelease cannot enter stable")
    require(type(payload["sequence"]) is int and payload["sequence"] == version_sequence(payload["version"]), "Invalid release sequence")
    if "notes" in payload:
        require(isinstance(payload["notes"], str) and len(payload["notes"].encode("utf-8")) <= 512, "Notes exceed 512 UTF-8 bytes")
    prefix = BASE_URL + "/firmware/" + payload["version"] + "/"
    if kind == "ota":
        rows = [(payload["image"], "folio.bin", APP_LIMIT, {"url", "size", "sha256"})]
    else:
        require(type(payload["flash_size"]) is int and payload["flash_size"] == FLASH_SIZE, "Wrong flash size")
        require(isinstance(payload["parts"], list) and len(payload["parts"]) == 4, "USB requires exactly four parts")
        require(all(isinstance(row, dict) and type(row.get("offset")) is int for row in payload["parts"]), "Invalid USB offsets")
        require([row["offset"] for row in payload["parts"]] == list(PARTS), "USB offsets must be ordered and exact")
        limits = {0: 0x8000, 0x8000: 0x1000, 0x10000: APP_LIMIT, 0x910000: 0x2000}
        rows = [(row, PARTS[row["offset"]], limits[row["offset"]], {"offset", "url", "size", "sha256"}) for row in payload["parts"]]
    for row, filename, limit, fields in rows:
        require(isinstance(row, dict) and set(row) == fields, "Invalid image fields")
        require(row["url"] == prefix + filename, "Image URL is not the immutable same-origin path")
        require(type(row["size"]) is int and 0 < row["size"] <= limit, "Image size out of range")
        require(isinstance(row["sha256"], str) and HASH.fullmatch(row["sha256"]), "Invalid SHA256")
    return payload


def public_key(path):
    raw = Path(path).read_bytes()
    if raw.startswith(b"-----BEGIN PUBLIC KEY-----"):
        key = serialization.load_pem_public_key(raw)
    else:
        key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), raw)
    require(isinstance(key, ec.EllipticCurvePublicKey) and isinstance(key.curve, ec.SECP256R1), "Public key must be P-256")
    return key


def private_key(path=None):
    # Environment PEM is consumed in memory, never written or printed.
    raw = Path(path).read_bytes() if path else os.environ.get("FOLIO_SIGNING_KEY_PEM", "").encode()
    require(bool(raw), "Production signing key missing; release remains unpublished")
    try:
        key = serialization.load_pem_private_key(raw, password=None)
    except (ValueError, TypeError):
        raise ValueError("Invalid signing key; key contents suppressed") from None
    require(isinstance(key, ec.EllipticCurvePrivateKey) and isinstance(key.curve, ec.SECP256R1), "Signing key must be P-256")
    return key


def sign(payload, key, trusted_key):
    validate_payload(payload)
    require(key.public_key().public_numbers() == trusted_key.public_numbers(), "Signing key differs from pinned publisher public key")
    raw = canonical(payload)
    r, s = utils.decode_dss_signature(key.sign(raw, ec.ECDSA(hashes.SHA256())))
    result = canonical({"key_id": KEY_ID, "payload": base64.b64encode(raw).decode(),
                        "signature": base64.b64encode(r.to_bytes(32, "big") + s.to_bytes(32, "big")).decode()})
    require(len(result) <= 4096, "Envelope exceeds 4096 bytes")
    verify(result, trusted_key)
    return result


def verify(raw, key):
    require(0 < len(raw) <= 4096, "Envelope exceeds 4096 bytes")
    envelope = load_json(raw)
    require(isinstance(envelope, dict) and set(envelope) == {"key_id", "payload", "signature"}, "Invalid envelope fields")
    require(envelope["key_id"] == KEY_ID, "Unknown publisher key_id")
    try:
        payload_raw = base64.b64decode(envelope["payload"], validate=True)
        signature = base64.b64decode(envelope["signature"], validate=True)
    except (ValueError, TypeError):
        raise ValueError("Invalid envelope base64") from None
    require(len(signature) == 64, "ECDSA signature must be raw 64-byte r||s")
    try:
        key.verify(utils.encode_dss_signature(int.from_bytes(signature[:32], "big"), int.from_bytes(signature[32:], "big")),
                   payload_raw, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        raise ValueError("Invalid publisher signature") from None
    payload = validate_payload(load_json(payload_raw))
    require(canonical(payload) == payload_raw, "Release payload is not canonical UTF-8 JSON")
    return payload


def app_descriptor(raw):
    require(288 <= len(raw) <= APP_LIMIT, "Application must fit both 3 MiB OTA slots")
    require(raw[0] == 0xE9 and struct.unpack_from("<H", raw, 12)[0] == 9, "Application is not ESP32-S3")
    require(struct.unpack_from("<I", raw, 32)[0] == 0xABCD5432, "Invalid application descriptor")
    def string(start):
        field = raw[start:start + 32]
        require(b"\0" in field, "Unterminated application descriptor")
        return field.split(b"\0", 1)[0].decode("ascii")
    desc = {"version": string(48), "project": string(80), "idf": string(144)}
    version_ok(desc["version"])
    require(desc["project"] == "Folio", "Application project mismatch")
    require(desc["idf"] in ("v6.1", "v6.1.0"), "Application not built with pinned ESP-IDF v6.1")
    require(b"USB debug bridge ready" not in raw and b"@FOLIO SCREEN" not in raw, "Debug bridge found in application")
    return desc


def payload_pair(version, sequence, channel, files, notes=None):
    common = {"schema": 1, "project": "Folio", "chip": "esp32s3", "hardware": "RDP-G01-W", "layout": LAYOUT,
              "version": version, "sequence": sequence, "channel": channel}
    if notes is not None:
        common["notes"] = notes
    def image(name):
        raw = files[name]
        return {"url": BASE_URL + "/firmware/" + version + "/" + name, "size": len(raw), "sha256": digest(raw)}
    ota = dict(common, kind="ota", image=image("folio.bin"))
    usb = dict(common, kind="usb", flash_size=FLASH_SIZE, parts=[dict(image(name), offset=offset) for offset, name in PARTS.items()])
    for item in (ota, usb):
        validate_payload(item)
    return ota, usb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("envelope", type=Path)
    parser.add_argument("--public-key", required=True, type=Path)
    args = parser.parse_args()
    payload = verify(args.envelope.read_bytes(), public_key(args.public_key))
    print(f"Verified {payload['kind']} {payload['version']} {payload['channel']} sequence={payload['sequence']}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError) as error:
        raise SystemExit(str(error)) from None
