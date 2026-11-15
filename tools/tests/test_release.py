#!/usr/bin/env python3
"""Release regressions: ephemeral TEST FIXTURE keys only, never production keys."""
import base64
import copy
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_manifest as protocol
import release_bundle as bundle
import release_pages as pages
import release_ci as transport
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils


def app(version="0.8.32-dev"):
    raw = bytearray(1024)
    raw[0] = 0xE9
    struct.pack_into("<H", raw, 12, 9)
    struct.pack_into("<I", raw, 32, 0xABCD5432)
    for start, text in ((48, version), (80, "Folio"), (144, "v6.1")):
        raw[start:start + len(text)] = text.encode()
    return bytes(raw)


def fixture_candidate(key, version="0.8.32-dev"):
    files = {"folio.bin": app(version), "bootloader.bin": b"B" * 4096, "partition-table.bin": b"P" * 3072, "ota_data_initial.bin": b"\xff" * 8192}
    provenance = {"version": version, "sequence": protocol.version_sequence(version), "source_commit": "1" * 40,
                  "app_sha256": protocol.digest(files["folio.bin"]), "source_sha256": protocol.digest(b"fixture source")}
    files["provenance.json"] = protocol.canonical(provenance)
    files["source.tar.gz"] = b"fixture source"
    files["LICENSE"] = b"TEST FIXTURE MIT License"
    for kind, payload in zip(("ota", "usb"), protocol.payload_pair(version, provenance["sequence"], "preview", files)):
        files[kind + ".json"] = protocol.sign(payload, key, key.public_key())
    bundle.sign_materials(files, key)
    return files


def fixture_acceptance(provenance):
    # The entire record is a TEST FIXTURE, never a committed production acceptance.
    return {"schema": 1, "version": provenance["version"], "source_commit": provenance["source_commit"], "app_sha256": provenance["app_sha256"],
            "hardware": "RDP-G01-W", "layout": protocol.LAYOUT, "approved": True, "reviewer": "test-reviewer", "tested_at": "2026-11-14T12:00:00Z",
            "checks": {name: {"passed": True, "evidence": "Synthetic unit test evidence 0123456789"} for name in bundle.CHECKS}}


class ReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture_key = ec.generate_private_key(ec.SECP256R1())
        cls.public = cls.fixture_key.public_key()

    def payload(self):
        return protocol.payload_pair("0.8.32-dev", 8032000, "preview", {"folio.bin": app(), "bootloader.bin": b"B", "partition-table.bin": b"P", "ota_data_initial.bin": b"\xff" * 8192})[0]

    def test_version_sequence(self):
        for version, expected in {"0.8.32-dev": 8032000, "0.8.32-dev.1": 8032001, "0.8.32-dev.499": 8032499, "0.8.32-rc.1": 8032501, "0.8.32-rc.498": 8032998, "0.8.32": 8032999, "1.0.0": 100000999}.items():
            self.assertEqual(protocol.version_sequence(version), expected)
        for version in ("0.8.31", "0.08.32", "0.100.0", "0.8.1000", "43.0.0", "0.8.32-rc", "0.8.32-dev.0", "0.8.32-dev.500", "0.8.32-rc.499", "../0.8.32", "0.8.32-dev.01"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                protocol.version_sequence(version)

    def test_roundtrip_and_openssl_cross_verification(self):
        envelope = protocol.sign(self.payload(), self.fixture_key, self.public)
        self.assertLessEqual(len(envelope), 4096)
        self.assertEqual(protocol.verify(envelope, self.public), self.payload())
        data = protocol.load_json(envelope)
        raw = base64.b64decode(data["payload"])
        signature = base64.b64decode(data["signature"])
        self.assertEqual(len(signature), 64)
        with tempfile.TemporaryDirectory(prefix="folio-test-fixture-") as tmp:
            root = Path(tmp)
            (root / "public.pem").write_bytes(self.public.public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
            (root / "fixture-only.pem").write_bytes(self.fixture_key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
            (root / "payload").write_bytes(raw)
            (root / "signature.der").write_bytes(utils.encode_dss_signature(int.from_bytes(signature[:32], "big"), int.from_bytes(signature[32:], "big")))
            result = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(root / "public.pem"), "-signature", str(root / "signature.der"), str(root / "payload")], capture_output=True, check=True)
            self.assertIn(b"Verified OK", result.stdout)
            subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(root / "fixture-only.pem"), "-out", str(root / "openssl.der"), str(root / "payload")], check=True)
            r, s = utils.decode_dss_signature((root / "openssl.der").read_bytes())
            data["signature"] = base64.b64encode(r.to_bytes(32, "big") + s.to_bytes(32, "big")).decode()
            self.assertEqual(protocol.verify(protocol.canonical(data), self.public), self.payload())

    def test_tamper_identity_and_unknown_fields(self):
        envelope = protocol.load_json(protocol.sign(self.payload(), self.fixture_key, self.public))
        for field, value in (("key_id", "attacker"), ("signature", base64.b64encode(bytes(64)).decode()), ("payload", base64.b64encode(b"{}").decode())):
            bad = dict(envelope, **{field: value})
            with self.subTest(field=field), self.assertRaises(ValueError):
                protocol.verify(protocol.canonical(bad), self.public)
        bad = dict(envelope, extra=1)
        with self.assertRaises(ValueError):
            protocol.verify(protocol.canonical(bad), self.public)
        with self.assertRaises(ValueError):
            protocol.verify(b" " * 4097, self.public)
        with self.assertRaises(ValueError):
            protocol.sign(self.payload(), ec.generate_private_key(ec.SECP256R1()), self.public)

    def test_payload_rejections(self):
        changes = (("channel", "stable"), ("project", "evil"), ("chip", "esp32"), ("layout", "old"), ("sequence", 8032001), ("sequence", True), ("schema", True), ("notes", "中" * 171))
        for field, value in changes:
            payload = self.payload(); payload[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                protocol.validate_payload(payload)
        for field, value in (("url", protocol.BASE_URL + "/firmware/../folio.bin"), ("url", "https://evil.test/folio.bin"), ("size", 3145729), ("size", True), ("sha256", "A" * 64)):
            payload = self.payload(); payload["image"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                protocol.validate_payload(payload)

    def test_no_usb_storage_font_or_merged(self):
        candidate = fixture_candidate(self.fixture_key)
        payload = protocol.verify(candidate["usb.json"], self.public)
        self.assertEqual([row["offset"] for row in payload["parts"]], [0, 0x8000, 0x10000, 0x910000])
        for offset in (0x410000, 0x9000, 0x920000):
            bad = copy.deepcopy(payload); bad["parts"][3]["offset"] = offset
            with self.assertRaises(ValueError): protocol.validate_payload(bad)
        bad = copy.deepcopy(payload); bad["parts"].append(dict(bad["parts"][0]))
        with self.assertRaises(ValueError): protocol.validate_payload(bad)

    def test_app_debug_mixed_version_size_and_chip(self):
        self.assertEqual(protocol.app_descriptor(app())["version"], "0.8.32-dev")
        bad = bytearray(app()); bad[300:322] = b"USB debug bridge ready"
        with self.assertRaises(ValueError): protocol.app_descriptor(bytes(bad))
        bad = bytearray(app()); bad[12] = 0
        with self.assertRaises(ValueError): protocol.app_descriptor(bytes(bad))
        with self.assertRaises(ValueError): protocol.app_descriptor(app() + bytes(protocol.APP_LIMIT))
        files = fixture_candidate(self.fixture_key); files["folio.bin"] = app("0.8.33-dev")
        with self.assertRaises(ValueError): pages.verify_public_release({name: files[name] for name in pages.PUBLIC_FILES}, self.public)

    def test_release_build_config_debug_and_mixed_sequence_rejected(self):
        config = {"FOLIO_USB_DEBUG": False, "IDF_TARGET": "esp32s3", "ESPTOOLPY_FLASHSIZE": "16MB", "ESPTOOLPY_FLASHFREQ_120M": True, "SPIRAM_SPEED_120M": True}
        built = {"version": "0.8.32-dev", "sequence": 8032000}
        bundle.validate_build_config(config, built, "0.8.32-dev", 8032000)
        for field, value in (("FOLIO_USB_DEBUG", True), ("FOLIO_USB_DEBUG", None), ("ESPTOOLPY_FLASHSIZE", "8MB"), ("ESPTOOLPY_FLASHFREQ_120M", False), ("SECURE_BOOT", True)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                bundle.validate_build_config(dict(config, **{field: value}), built, "0.8.32-dev", 8032000)
        with self.assertRaises(ValueError):
            bundle.validate_build_config(config, dict(built, sequence=8032001), "0.8.32-dev", 8032000)

    def test_real_esp_image_checksum_and_digest(self):
        import hashlib
        # A real ESP32-S3 image container around synthetic descriptor bytes.
        header = bytearray(24); header[0] = 0xE9; header[1] = 1; header[12] = 9; header[23] = 1
        data = app()[32:]
        raw = bytes(header) + struct.pack("<II", 0x3C000020, len(data)) + data
        checksum = 0xEF
        for value in data: checksum ^= value
        raw += bytes((-len(raw) - 1) % 16) + bytes([checksum])
        raw += hashlib.sha256(raw).digest()
        bundle.validate_esp_image(raw)
        broken = bytearray(raw); broken[-1] ^= 1
        with self.assertRaises(ValueError): bundle.validate_esp_image(bytes(broken))
        broken = bytearray(raw); broken[350] ^= 1
        with self.assertRaises(ValueError): bundle.validate_esp_image(bytes(broken))

    def test_source_allowlist_complete_and_private_content_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            names = set(bundle.REQUIRED_SOURCE)
            for name in names:
                path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text("MIT License fixture public source")
            (root / "components").mkdir()
            allowlist = root / "allowlist.txt"; allowlist.write_text("\n".join(sorted(names)))
            self.assertEqual(set(bundle.source_snapshot(root, allowlist)), names)
            (root / "main/omitted.c").write_text("unreviewed source")
            with self.assertRaisesRegex(ValueError, "not reviewed"): bundle.source_snapshot(root, allowlist)
            names.add("main/omitted.c"); allowlist.write_text("\n".join(sorted(names)))
            (root / "main/omitted.c").write_bytes(b"-----BEGIN " + b"PRIVATE KEY-----")
            with self.assertRaisesRegex(ValueError, "Private key/token"): bundle.source_snapshot(root, allowlist)

    def test_relink_instructions_are_auxiliary_not_standalone(self):
        for text in (bundle.RELINK_SCOPE, bundle.RELINK_INSTRUCTIONS.decode()):
            self.assertIn("not a standalone complete relink kit", text)
            self.assertIn("complete corresponding project source", text)
            self.assertIn("pinned ESP-IDF", text)
            self.assertIn("toolchain and public native release defaults", text)
            self.assertIn("does not establish compliance with every LGPL", text)
        instructions = bundle.RELINK_INSTRUCTIONS
        self.assertIn(b"folio/.github/RELEASING.md", instructions)
        self.assertIn(b"not a native sdkconfig input", instructions)
        self.assertIn(b"External SDK libraries and toolchain runtimes are omitted", instructions)
        self.assertNotIn(b"/Users/", instructions)
        self.assertIsNone(bundle.PRIVATE.search(instructions))

    def test_release_guide_is_required_and_packaged(self):
        name = ".github/RELEASING.md"
        self.assertIn(name, bundle.REQUIRED_SOURCE)
        guide = (Path(__file__).resolve().parents[2] / name).read_bytes()
        self.assertNotIn(b"/Users/", guide)
        self.assertNotIn(b".local-secrets/", guide)
        self.assertIsNone(bundle.PRIVATE.search(guide))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for filename in bundle.REQUIRED_SOURCE:
                path = root / filename
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(guide if filename == name else b"MIT License fixture public source")
            allowlist = root / "allowlist.txt"
            allowlist.write_text("\n".join(sorted(bundle.REQUIRED_SOURCE)))
            snapshot = bundle.source_snapshot(root, allowlist)
            with tarfile.open(fileobj=io.BytesIO(bundle.archive_sources(snapshot)), mode="r:gz") as archive:
                self.assertEqual(archive.extractfile("folio/" + name).read(), guide)
            allowlist.write_text("\n".join(sorted(bundle.REQUIRED_SOURCE - {name})))
            with self.assertRaisesRegex(ValueError, "Incomplete"):
                bundle.source_snapshot(root, allowlist)

    def test_release_guide_matches_pinned_build_and_signing_environments(self):
        root = Path(__file__).resolve().parents[2]
        guide = (root / ".github/RELEASING.md").read_text()
        for token in (protocol.IDF_COMMIT, '"$IDF_PATH/install.sh" esp32s3',
                      'mkdir -p "$FOLIO_BUILD_DIR/log"', 'SDKCONFIG_DEFAULTS=',
                      '$FOLIO_SOURCE/sdkconfig.defaults;$FOLIO_SOURCE/.github/sdkconfig.release',
                      'config["FOLIO_USB_DEBUG"] is False', '--no-ccache',
                      'not a standalone complete relink kit', 'Prevent self-review',
                      'FOLIO_SIGNING_KEY_PEM', 'GitHub Actions'):
            self.assertIn(token, guide)
        for workflow, environment in (("release.yml", "release-signing"), ("promote.yml", "release-stable")):
            self.assertIn("environment: " + environment, (root / ".github/workflows" / workflow).read_text())
            self.assertIn("`" + environment + "`", guide)

    def test_environment_protection_is_executable_gate(self):
        protected = {"can_admins_bypass": False, "protection_rules": [{"type": "required_reviewers", "reviewers": ["maintainer"]}], "deployment_branch_policy": {"custom_branch_policies": True}}
        for record in ({}, {"protection_rules": []}, {"protection_rules": [{"type": "required_reviewers", "reviewers": ["maintainer"]}], "deployment_branch_policy": {"custom_branch_policies": True}}, dict(protected, can_admins_bypass=True)):
            with patch.object(transport, "gh", return_value=json.dumps(record)), self.assertRaises(ValueError):
                transport.require_environment("release-signing")
        with patch.object(transport, "gh", return_value=json.dumps(protected)):
            transport.require_environment("release-signing")

    def test_authority_hash_mismatch_never_treated_as_empty_history(self):
        state = {"schema": 1, "generation": 1, "snapshot_sha256": "a" * 64}
        response = json.dumps({"sha": "b" * 40, "content": base64.b64encode(protocol.canonical(state)).decode()})
        with tempfile.TemporaryDirectory() as tmp, patch.object(transport, "gh", return_value=response), patch.object(transport, "download", return_value=b"stale-CDN"):
            with self.assertRaisesRegex(ValueError, "manual recovery"):
                transport.read_state(Path(tmp) / "state")

    def test_json_duplicate_and_noncanonical_refused(self):
        with self.assertRaises(ValueError): protocol.load_json('{"schema":1,"schema":1}')
        raw = json.dumps(self.payload(), indent=2).encode()
        r, s = utils.decode_dss_signature(self.fixture_key.sign(raw, ec.ECDSA(hashes.SHA256())))
        envelope = protocol.canonical({"key_id": protocol.KEY_ID, "payload": base64.b64encode(raw).decode(), "signature": base64.b64encode(r.to_bytes(32, "big") + s.to_bytes(32, "big")).decode()})
        with self.assertRaisesRegex(ValueError, "canonical"): protocol.verify(envelope, self.public)

    def test_missing_production_key_is_not_generated(self):
        with patch.dict(os.environ, {"FOLIO_SIGNING_KEY_PEM": ""}):
            with self.assertRaisesRegex(ValueError, "missing"):
                protocol.private_key()

    def test_source_paths_deny_private_and_traversal_before_open(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("../private", "/etc/passwd", "main/../secret", "main//x.c", "main\\x", ".local-secrets/key.pem", "artifacts/device.bin", "tools/.dsh-meow/data", "main/sdkconfig", "main/backup/a.c", "docs/photo.jpg", "keys/private.pem", "main/test.c~"):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    bundle.source_path(root, name)
            (root / "main").mkdir(); (root / "main/a.c").write_text("public")
            self.assertEqual(bundle.source_path(root, "main/a.c").read_text(), "public")
            (root / "main/b.c").symlink_to(root / "main/a.c")
            with self.assertRaises(ValueError): bundle.source_path(root, "main/b.c")
            private_marker = b"-----BEGIN " + b"PRIVATE KEY-----"
            self.assertTrue(bundle.PRIVATE.search(private_marker))

    def test_immutable_and_stale_workflow_and_retention(self):
        old = fixture_candidate(self.fixture_key)
        first = pages.compose({}, self.public, candidate=old)
        newer = fixture_candidate(self.fixture_key, "0.8.33-dev")
        second = pages.compose(first, self.public, candidate=newer)
        self.assertIn("firmware/0.8.32-dev/folio.bin", second)
        self.assertEqual(second["firmware/0.8.32-dev/folio.bin"], old["folio.bin"])
        for candidate in (old, fixture_candidate(self.fixture_key, "0.8.32-dev.1")):
            with self.assertRaises(ValueError): pages.compose(second, self.public, candidate=candidate)
        self.assertEqual(first["channels/preview.json"], old["ota.json"])
        self.assertEqual(pages.unpack_snapshot(pages.pack_snapshot(second)), second)

    def test_site_update_preserves_all_firmware_and_pointers(self):
        first = pages.compose({}, self.public, candidate=fixture_candidate(self.fixture_key))
        second = pages.compose(first, self.public)
        for path, raw in first.items():
            if path != "release-state.json": self.assertEqual(second[path], raw)
        self.assertEqual(protocol.load_json(second["release-state.json"])["generation"], 2)

    def test_failure_does_not_mutate_previous_state(self):
        first = pages.compose({}, self.public, candidate=fixture_candidate(self.fixture_key))
        original = dict(first)
        bad = fixture_candidate(self.fixture_key, "0.8.33-dev"); bad["folio.bin"] += b"corruption"
        with self.assertRaises(ValueError): pages.compose(first, self.public, candidate=bad)
        self.assertEqual(first, original)

    def test_stable_requires_exact_human_acceptance(self):
        candidate = fixture_candidate(self.fixture_key, "0.8.32")
        provenance = protocol.load_json(candidate["provenance.json"])
        record = fixture_acceptance(provenance)
        bundle.validate_acceptance(protocol.canonical(record), provenance)
        for field, value in (("approved", False), ("app_sha256", "f" * 64), ("source_commit", "f" * 40), ("reviewer", "")):
            bad = dict(record, **{field: value})
            with self.assertRaises(ValueError): bundle.validate_acceptance(protocol.canonical(bad), provenance)
        bad = copy.deepcopy(record); bad["checks"]["physical_display"]["evidence"] = "not tested at all yet"
        with self.assertRaises(ValueError): bundle.validate_acceptance(protocol.canonical(bad), provenance)
        promotion = {"acceptance.json": protocol.canonical(record)}
        for kind in ("ota", "usb"):
            payload = protocol.verify(candidate[kind + ".json"], self.public); payload["channel"] = "stable"
            promotion[kind + ".json"] = protocol.sign(payload, self.fixture_key, self.public)
        first = pages.compose({}, self.public, candidate=candidate)
        stable = pages.compose(first, self.public, promotion=promotion)
        self.assertEqual(stable["firmware/0.8.32/folio.bin"], candidate["folio.bin"])
        self.assertEqual(stable["channels/preview.json"], first["channels/preview.json"])
        with self.assertRaises(ValueError): pages.compose(stable, self.public, promotion=promotion)

    def test_snapshot_malicious_paths_links_and_duplicates(self):
        for name, kind in (("../escape", tarfile.REGTYPE), ("channels/a", tarfile.SYMTYPE), ("unapproved/a", tarfile.REGTYPE)):
            buffer = io.BytesIO()
            with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
                member = tarfile.TarInfo(name); member.type = kind; member.size = 1 if kind == tarfile.REGTYPE else 0
                archive.addfile(member, io.BytesIO(b"x") if member.size else None)
            with self.subTest(name=name), self.assertRaises(ValueError): pages.unpack_snapshot(buffer.getvalue())

    def test_materials_signature_binds_source_license_provenance(self):
        files = fixture_candidate(self.fixture_key)
        bundle.verify_materials(files, self.public)
        for name in ("source.tar.gz", "LICENSE", "provenance.json"):
            bad = dict(files); bad[name] += b"changed"
            with self.assertRaises(ValueError): bundle.verify_materials(bad, self.public)


if __name__ == "__main__":
    unittest.main()
