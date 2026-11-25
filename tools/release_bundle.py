#!/usr/bin/env python3
"""Fail-closed release preparation, signing and stable promotion (local only)."""
import argparse
import io
import hashlib
import json
import re
import shutil
import subprocess
import tarfile
import zipfile
from pathlib import Path, PurePosixPath

import package as development_package
import release_manifest as protocol
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

ROOT_FILES = {"CMakeLists.txt", "sdkconfig.defaults", "partitions_16M.csv", "dependencies.lock", "LICENSE", "LICENSES.md", "README.md", ".gitignore", "AGENTS.md"}
ROOT_DIRS = {"main", "components", "tools", "licenses", "docs", "site", "keys", ".github"}
DENIED_PARTS = {".local-secrets", ".dsh-meow", ".git", "artifacts", "dist", "node_modules", "__pycache__", ".astro", ".pnpm-store", "backup", "backups", "photos", "screenshots", "证据", "备份", "照片"}
DENIED_SUFFIXES = {".pyc", ".pyo", ".bak", ".old", ".orig", ".log", ".pem", ".key", ".p12", ".pfx", ".zstd", ".elf", ".map"}
PRIVATE = re.compile(rb"-----BEGIN (?:EC |RSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----\r?\n[A-Za-z0-9+/]{16,}|gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}")
CHECKS = {"device_identity", "flash_identity", "usb_four_parts", "ota_signed_upgrade", "ota_reject_invalid", "boot_rollback", "nvs_storage_preserved", "reading_smoke", "physical_display", "thermal_stability", "font_fallback"}
REQUIRED_SOURCE = (ROOT_FILES - {".gitignore", "AGENTS.md"}) | {
    "keys/firmware-release-public.pem", "main/assets/fonts/NOTICE.txt", "main/assets/fonts/source-manifest.json",
    "main/assets/fonts/manifest.json", "licenses/NotoSerifSC-OFL.txt", "licenses/LGPL-3.0.txt", "licenses/GPL-3.0.txt",
    "tools/idf.sh", "tools/folio_release_version.cmake", ".github/sdkconfig.release", ".github/release-source-files.txt",
    ".github/release-requirements.txt", ".github/release-legal.json", ".github/RELEASING.md", "tools/release_sdk_notices.json"}
RELINK_SCOPE = (
    "Auxiliary archives, linker fragments, ELF and reference configuration only; not a standalone complete relink kit. "
    "Rebuilding and replacing EPDiy requires the complete corresponding project source plus the pinned ESP-IDF, "
    "recursive submodules, toolchain and public native release defaults. "
    "Successful rebuilding does not establish compliance with every LGPL or other license obligation.\n")
RELINK_INSTRUCTIONS = (RELINK_SCOPE +
    "Follow folio/.github/RELEASING.md inside source.tar.gz. Replace/modify components/epdiy, then rebuild/relink "
    "using the original source build scripts and sdkconfig.defaults plus .github/sdkconfig.release. "
    "The JSON configuration here is reference output, not a native sdkconfig input. "
    "External SDK libraries and toolchain runtimes are omitted; this auxiliary archive does not contain all required linker inputs. "
    "Archives retain symbols and relocations; only debugging data was stripped. "
    "The supported firmware profile disables secure boot; publisher OTA signatures are a separate policy.\n").encode()


def safe_relative(name):
    protocol.require(isinstance(name, str) and name and "\\" not in name and not any(ord(c) < 32 for c in name), "Unsafe relative path")
    path = PurePosixPath(name)
    protocol.require(not path.is_absolute() and ".." not in path.parts and str(path) == name and path.parts[0] not in ("", "."), "Unsafe relative path")
    return path


def source_path(root, name):
    relative = safe_relative(name)
    protocol.require(name in ROOT_FILES or relative.parts[0] in ROOT_DIRS, "Source path outside release allowlist roots")
    protocol.require(not any(part.lower() in DENIED_PARTS or part.startswith(".env") for part in relative.parts), "Private source path prohibited")
    protocol.require(relative.name not in ("sdkconfig", "sdkconfig.old") and not relative.name.endswith("~"), "Local configuration/backup prohibited")
    protocol.require(relative.suffix.lower() not in DENIED_SUFFIXES or name == "keys/firmware-release-public.pem", "Private or generated source suffix prohibited")
    protocol.require(not (relative.suffix.lower() in (".png", ".jpg", ".jpeg", ".webp", ".heic") and relative.parts[0] != "site" and not name.startswith("tools/tests/fixtures/")), "Private images prohibited")
    current = root
    for part in relative.parts:
        current = current / part
        protocol.require(not current.is_symlink(), "Source symlink prohibited")
    protocol.require(current.is_file() and current.resolve().is_relative_to(root.resolve()), "Source file missing or escapes checkout")
    return current


def source_snapshot(root, allowlist):
    names = [line.strip() for line in allowlist.read_text().splitlines() if line.strip() and not line.startswith("#")]
    protocol.require(names and len(names) == len(set(names)), "Empty or duplicate source allowlist")
    protocol.require(REQUIRED_SOURCE <= set(names), "Incomplete source/license/publisher/build-script allowlist")
    # A whitelist must cover source and notices, not silently omit linked code.
    for directory in ("main", "components", "licenses"):
        for path in (root / directory).rglob("*"):
            if path.is_file() or path.is_symlink():
                name = path.relative_to(root).as_posix()
                protocol.require(name in names, "Source tree has a file not reviewed in the allowlist: " + name)
    snapshot = {}
    for name in names:
        raw = source_path(root, name).read_bytes()
        protocol.require(not PRIVATE.search(raw), "Private key/token detected in approved source file")
        snapshot[name] = raw
    protocol.require(b"MIT License" in snapshot["LICENSE"], "Project MIT license not finalized")
    protocol.require(b"\xe5\xb0\x9a\xe6\x9c\xaa\xe6\x8c\x87\xe5\xae\x9a" not in snapshot["LICENSES.md"], "Project licensing remains undecided")
    return snapshot


def archive_sources(snapshot):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
        for name, raw in sorted(snapshot.items()):
            info = tarfile.TarInfo("folio/" + name)
            info.size, info.mode, info.mtime = len(raw), 0o644, 0
            archive.addfile(info, io.BytesIO(raw))
    return buffer.getvalue()


def legal_review(raw):
    legal = protocol.load_json(raw)
    for field, expected in {"schema": 1, "publisher": "qmqsu", "repository": "https://github.com/qmqsu/folio", "project_license": "MIT"}.items():
        protocol.require(legal.get(field) == expected, "Release legal identity missing: " + field)
    pending = legal.get("pending")
    protocol.require(isinstance(pending, list) and len(pending) <= 64, "Release pending must be a bounded list")
    protocol.require(all(isinstance(item, str) and 0 < len(item.encode("utf-8")) <= 512 for item in pending), "Release pending entries must be short strings")
    return legal


# Publisher-authorized exceptions are pinned per source/image, never caller options.
WAIVER_VERSION = "0.8.39"
WAIVER_COMMIT = "783257a0f8f2a6696605cd74e570371fa19f7f04"
WAIVER_APP_SHA256 = "f9de0be26d89e0cb2408f9bdd1abe637fcd39611708066173ef980825728125a"
WAIVER_0840_VERSION = "0.8.40"
WAIVER_0840_COMMIT = "d845c25b463229744adcf1663f5ac9a57be1330b"
WAIVER_0840_APP_SHA256 = "adf76bad4160215289449d0b850093cd35ffda688538fa567c62d335002946a5"


def validate_decision_time(value):
    from datetime import datetime, timezone, timedelta
    protocol.require(isinstance(value, str) and re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", value), "UTC release decision timestamp required")
    when = datetime.fromisoformat(value.replace("Z", "+00:00"))
    protocol.require(when <= datetime.now(timezone.utc) + timedelta(minutes=5), "Release decision timestamp is in the future")


def validate_acceptance(raw, provenance):
    record = protocol.load_json(raw)
    if "kind" in record:
        candidates = [
            {"version": WAIVER_VERSION, "source_commit": WAIVER_COMMIT, "app_sha256": WAIVER_APP_SHA256},
            {"version": "0.8.46", "source_commit": "c26ad619651dc85e25856c35b7754c6b11c1b0a6", "app_sha256": "09547ffb7702a8c8b56603a8143e9fbe637aebaf8d4103617e4b49094e986139"},
            {"version": "0.9.0", "source_commit": "63674392438a9f00f94501061f6fd4540757f2ba", "app_sha256": "0a224de2db1072d84ac51b6b113144a7a6d005ff148c69d919ae8d572894c0bd"},
            {"version": "0.9.2", "source_commit": "8918d8dc94542d61642f33d670db34f4c87f2751", "app_sha256": "9e941bc484a27cf14cfb867643c1e69a8fc77ed8faf9f924988b726754fc90f0"},
            {"version": "0.9.3", "source_commit": "706b24e0055800d38911c59a1ec1f731839419c7", "app_sha256": "44584af700ed785c9d465efb519c8003f8af28745fcee58b40e19395227683f9"},
            {"version": WAIVER_0840_VERSION, "source_commit": WAIVER_0840_COMMIT, "app_sha256": WAIVER_0840_APP_SHA256},
        ]
        identity = next((candidate for candidate in candidates if all(provenance.get(k) == v for k, v in candidate.items())), None)
        protocol.require(identity is not None, "No waiver for this candidate")
        protocol.require(re.fullmatch(r"[0-9a-f]{64}", identity["app_sha256"]), "Invalid waiver application digest")
        expected = {**identity, "schema": 2, "kind": "maintainer-waiver", "hardware": "RDP-G01-W", "layout": protocol.LAYOUT,
                    "maintainer": "qmqsu", "hardware_tested": False, "reason": "维护者决定本版本不经实机验收直接发布。"}
        protocol.require(set(record) == set(expected) | {"authorized_at"} and
                         all(type(record[k]) is type(v) and record[k] == v for k, v in expected.items()), "Invalid maintainer waiver")
        validate_decision_time(record["authorized_at"])
        return record
    expected = {"schema": 1, "version": provenance["version"], "source_commit": provenance["source_commit"],
                "app_sha256": provenance["app_sha256"], "hardware": "RDP-G01-W", "layout": protocol.LAYOUT, "approved": True}
    for key, value in expected.items():
        protocol.require(record.get(key) == value and type(record.get(key)) is type(value), "Acceptance mismatch: " + key)
    protocol.require(isinstance(record.get("reviewer"), str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]{0,38}", record["reviewer"]), "Named human reviewer required")
    validate_decision_time(record.get("tested_at"))
    public_record = protocol.canonical(record)
    protocol.require(not PRIVATE.search(public_record) and b"/Users/" not in public_record and b".local-secrets/" not in public_record, "Acceptance contains private material")
    protocol.require(isinstance(record.get("checks"), dict) and set(record["checks"]) == CHECKS, "All hardware acceptance checks required")
    for name, check in record["checks"].items():
        protocol.require(isinstance(check, dict) and check.get("passed") is True and isinstance(check.get("evidence"), str) and 16 <= len(check["evidence"]) <= 1024, "Hardware evidence missing: " + name)
        protocol.require(not re.search(r"(?i)todo|pending|not.tested|fixture|example|待测|未测|示例", check["evidence"]), "Unverified acceptance evidence")
    return record


def read_files(directory):
    result = {}
    for path in directory.rglob("*"):
        protocol.require(not path.is_symlink(), "Release symlinks prohibited")
        if path.is_file():
            name = path.relative_to(directory).as_posix()
            safe_relative(name)
            result[name] = path.read_bytes()
    return result


def write_new(directory, files):
    protocol.require(not directory.exists(), "Output already exists; immutable releases cannot be overwritten")
    directory.mkdir(parents=True)
    for name, raw in files.items():
        safe_relative(name)
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)


def validate_build_config(config, built_version, version, sequence):
    protocol.require(config.get("FOLIO_USB_DEBUG") is False, "USB debug must explicitly be disabled in release build")
    protocol.require(built_version == {"version": version, "sequence": sequence} and sequence == protocol.version_sequence(version), "Built version/sequence differs from release request")
    protocol.require(config.get("IDF_TARGET") == "esp32s3" and config.get("ESPTOOLPY_FLASHSIZE") == "16MB", "Build flash/chip mismatch")
    protocol.require(config.get("ESPTOOLPY_FLASHFREQ_120M") is True and config.get("SPIRAM_SPEED_120M") is True, "Unexpected board clock profile; review required")
    protocol.require(not config.get("SECURE_BOOT") and not config.get("SECURE_FLASH_ENC_ENABLED"), "Secure boot/encrypted flash requires a separate release process")


def validate_esp_image(raw):
    from esptool.bin_image import LoadFirmwareImage
    try:
        image = LoadFirmwareImage("esp32s3", raw)
        image.verify()
        protocol.require(image.checksum == image.calculate_checksum(), "ESP image checksum mismatch")
        protocol.require(image.append_digest and image.stored_digest == image.calc_digest, "ESP image SHA256 mismatch/missing")
        protocol.require(raw[0] == 0xE9 and int.from_bytes(raw[12:14], "little") == 9, "Wrong ESP image chip")
    except (ValueError, OSError, EOFError) as error:
        raise ValueError("Invalid ESP firmware image") from error


def prepare(root, build, output, version, sequence, commit):
    root, build = root.resolve(), build.resolve()
    protocol.version_ok(version)
    protocol.require(re.fullmatch(r"[0-9a-f]{40}", commit), "Exact source commit required")
    snapshot = source_snapshot(root, root / ".github/release-source-files.txt")
    tree = subprocess.check_output(["git", "-C", str(root), "ls-tree", "-rz", "--full-tree", commit])
    committed = {}
    for entry in tree.split(b"\0"):
        if entry:
            metadata, name = entry.split(b"\t", 1)
            mode, kind, oid = metadata.split()
            protocol.require(kind == b"blob" and mode in (b"100644", b"100755"), "Source commit contains a symlink/submodule")
            committed[name.decode("utf-8")] = oid.decode()
    for name, raw in snapshot.items():
        oid = hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()
        protocol.require(committed.get(name) == oid, "Source archive differs from exact tagged commit: " + name)
    legal = legal_review(snapshot.get(".github/release-legal.json", b"{}"))
    description = protocol.load_json((build / "project_description.json").read_bytes())
    protocol.require(description.get("project_name") == "Folio" and description.get("target") == "esp32s3" and description.get("project_version") == version, "Build metadata identity/version mismatch")
    protocol.require(Path(description["project_path"]).resolve() == root, "Mixed build/source checkout")
    config = protocol.load_json((build / "config/sdkconfig.json").read_bytes())
    built_version = protocol.load_json((build / "folio-release-version.json").read_bytes())
    validate_build_config(config, built_version, version, sequence)
    idf = Path(description["idf_path"]).resolve()
    actual = subprocess.check_output(["git", "-C", str(idf), "rev-parse", "HEAD"], text=True).strip()
    protocol.require(actual == protocol.IDF_COMMIT, "ESP-IDF commit differs from pinned v6.1")
    protocol.require(subprocess.run(["git", "-C", str(idf), "diff", "--quiet", "HEAD", "--ignore-submodules=none"]).returncode == 0, "Dirty SDK cannot be published as pinned upstream")
    protocol.require(b"version: 6.1.0" in snapshot["dependencies.lock"], "Component lock differs from pinned IDF")
    flash = protocol.load_json((build / "flasher_args.json").read_bytes())
    images = {int(offset, 0): development_package.build_file(build, name) for offset, name in flash["flash_files"].items()}
    protocol.require(set(images) == set(protocol.PARTS) and len(images) == len(flash["flash_files"]), "Unexpected default flash parts")
    protocol.require(flash["extra_esptool_args"].get("chip") == "esp32s3" and flash["flash_settings"].get("flash_size") == "16MB", "Flash metadata mismatch")
    files = {protocol.PARTS[offset]: path.read_bytes() for offset, path in images.items()}
    files["legal-review.json"] = protocol.canonical(legal)
    protocol.require(protocol.app_descriptor(files["folio.bin"])["version"] == version, "App descriptor version mismatch")
    validate_esp_image(files["folio.bin"])
    validate_esp_image(files["bootloader.bin"])
    protocol.require(files["bootloader.bin"][2:4] == bytes([2, 0x4F]), "Bootloader must retain signed DIO/16MB/80MHz ROM header")
    for kind, offset in (("bootloader", 0), ("partition-table", 0x8000), ("app", 0x10000)):
        metadata = flash[kind]
        protocol.require(int(metadata["offset"], 0) == offset and development_package.build_file(build, metadata["file"]) == images[offset] and metadata.get("encrypted", False) in (False, "false"), "Mixed/encrypted flash metadata")
    development_package.validate_partition_table(images[0x8000])
    protocol.require(files["ota_data_initial.bin"] == b"\xff" * 8192, "Initial OTA data must be erased")
    protocol.payload_pair(version, sequence, "preview", files)
    # Compile inputs under checkout must all be in the reviewed source snapshot.
    for row in protocol.load_json((build / "compile_commands.json").read_bytes()):
        source = Path(row["file"]).resolve()
        if source.is_relative_to(idf):
            continue  # CI checks out the pinned SDK underneath the workspace.
        if source.is_relative_to(root):
            protocol.require(source.relative_to(root).as_posix() in snapshot, "Linked source absent from source archive")
        else:
            protocol.require(source.is_relative_to(build), "Unreviewed external compilation source")
    for name in snapshot:
        if name.startswith(("main/", "components/")) or name in ("CMakeLists.txt", "dependencies.lock", "sdkconfig.defaults", "partitions_16M.csv"):
            protocol.require((root / name).stat().st_mtime_ns <= images[0x10000].stat().st_mtime_ns, "Build older than source snapshot")
    resources = development_package.font_resources(root)
    protocol.require(resources is not None, "Font license materials absent")
    # No font image is put in default/offline flash parts. Readable notices remain complete.
    for name, raw in resources["documents"].items():
        files["font-notices/" + name] = raw
    for name, raw in snapshot.items():
        if name in ("LICENSE", "LICENSES.md", "keys/firmware-release-public.pem") or name.startswith("licenses/"):
            files[name] = raw
    import release_licenses
    files.update(release_licenses.collect(root, description))
    files["source.tar.gz"] = archive_sources(snapshot)
    # Auxiliary evidence only; the supported replacement path rebuilds source with the pinned SDK/toolchain.
    relink = {}
    compiler = Path(description["c_compiler"])
    objcopy = compiler.with_name(compiler.name.removesuffix("gcc") + "objcopy")
    protocol.require(objcopy.is_file(), "Matching toolchain objcopy missing")
    import tempfile
    with tempfile.TemporaryDirectory(prefix="folio-release-strip-") as temporary:
        for path in list((build / "esp-idf").rglob("*.a")) + [build / "Folio.elf"]:
            protocol.require(path.is_file() and not path.is_symlink(), "LGPL relink artifacts missing")
            target = Path(temporary) / "stripped"
            subprocess.run([str(objcopy), "--strip-debug", str(path), str(target)], check=True, capture_output=True)
            relink[path.relative_to(build).as_posix()] = target.read_bytes()
        for path in (build / "esp-idf").rglob("*.ld"):
            relink[path.relative_to(build).as_posix()] = path.read_bytes()
    protocol.require(any(name.endswith(".a") for name in relink), "LGPL static relink archives missing")
    relink["sdkconfig.release.json"] = protocol.canonical(config)
    relink["RELINK.txt"] = RELINK_INSTRUCTIONS
    for raw in relink.values():
        protocol.require(b"/Users/" not in raw and not PRIVATE.search(raw), "Private host path/key found in relink materials; use compiler prefix maps")
    files["relink.tar.gz"] = archive_sources(relink)
    provenance = {"schema": 1, "publisher": "qmqsu", "repository": "https://github.com/qmqsu/folio", "version": version,
                  "sequence": sequence, "source_commit": commit, "source_sha256": protocol.digest(files["source.tar.gz"]),
                  "app_sha256": protocol.digest(files["folio.bin"]), "idf_commit": protocol.IDF_COMMIT, "usb_debug": False,
                  "sdk_source": "https://github.com/espressif/esp-idf/tree/" + protocol.IDF_COMMIT,
                  "source_files": {name: protocol.digest(raw) for name, raw in snapshot.items()}, "device_verified": False,
                  "board_profile": {"hardware": "RDP-G01-W", "flash_jedec_id": "0x5e4018", "flash_vendor": "Zbit ZB25VQ128", "flash_mhz": 120, "psram_mhz": 120, "experimental": True, "thermal_stability_verified": False}}
    files["provenance.json"] = protocol.canonical(provenance)
    files["INSTALL.txt"] = ("Folio / RDP-G01-W / ESP32-S3 / 16 MiB / Zbit ZB25VQ128 JEDEC 0x5E4018 only. Back up the complete device before USB installation.\n"
        "EXPERIMENTAL 120 MHz Flash + PSRAM profile: not a generic ESP32-S3/16MB firmware. Other BOMs are unsupported. Temperature/physical-display validation is NOT implied by CI.\n"
        "Only four independent images are provided; NEVER erase-all or merge these images.\n"
        "After independent signature/hash/partition verification and explicit user confirmation:\n"
        "python -m esptool --chip esp32s3 --port YOUR_PORT write-flash --flash-mode keep --flash-freq keep --flash-size keep "
        "0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 folio.bin 0x910000 ota_data_initial.bin\n"
        "No default font resource write. Preserve NVS and storage 0x410000..0x90FFFF. OTA uses folio.bin only.\n"
        "Font installation is a separate licensed-resource operation requiring a complete storage backup and explicit permission.\n"
        "Source and licenses are included; ESP-IDF source/submodules must be fetched at the exact provenance commit. "
        "See the source archive's .github/RELEASING.md for the native source-rebuild/replacement procedure. "
        "relink.tar.gz: " + RELINK_SCOPE +
        "A successful CI build is NOT hardware verification. Preview is not a stable acceptance record.\n").encode()
    write_new(output, files)


def sign_materials(files, key):
    inventory = protocol.canonical({name: protocol.digest(raw) for name, raw in sorted(files.items())})
    r, s = utils.decode_dss_signature(key.sign(inventory, ec.ECDSA(hashes.SHA256())))
    files["materials.json"] = inventory
    files["materials.sig"] = r.to_bytes(32, "big") + s.to_bytes(32, "big")


def verify_materials(files, key):
    signature = files["materials.sig"]
    protocol.require(len(signature) == 64, "Invalid materials signature length")
    key.verify(utils.encode_dss_signature(int.from_bytes(signature[:32], "big"), int.from_bytes(signature[32:], "big")), files["materials.json"], ec.ECDSA(hashes.SHA256()))
    inventory = protocol.load_json(files["materials.json"])
    protocol.require(set(inventory) == set(files) - {"materials.json", "materials.sig"}, "Release materials inventory mismatch")
    for name, expected in inventory.items():
        safe_relative(name)
        protocol.require(protocol.digest(files[name]) == expected, "Release materials hash mismatch")
    return inventory


def sign_candidate(candidate, output, key_path, public_path):
    key, trusted = protocol.private_key(key_path), protocol.public_key(public_path)
    files = read_files(candidate)
    protocol.require("materials.sig" not in files, "Candidate is already signed")
    legal = legal_review(files.get("legal-review.json", b"{}"))
    protocol.require(legal["pending"] == [], "Release pending items must be resolved before signing")
    provenance = protocol.load_json(files["provenance.json"])
    protocol.require(protocol.digest(files["folio.bin"]) == provenance["app_sha256"], "Candidate application changed")
    protocol.require(protocol.digest(files["source.tar.gz"]) == provenance["source_sha256"], "Candidate source changed")
    protocol.require(protocol.app_descriptor(files["folio.bin"])["version"] == provenance["version"], "Candidate mixed version")
    for kind, payload in zip(("ota", "usb"), protocol.payload_pair(provenance["version"], provenance["sequence"], "preview", files)):
        files[kind + ".json"] = protocol.sign(payload, key, trusted)
    sign_materials(files, key)
    write_new(output, files)


def offline_zip(directory, output):
    protocol.require(not output.exists() and not output.resolve().is_relative_to(directory.resolve()), "Offline archive must be a new file outside release directory")
    with zipfile.ZipFile(output, "x", zipfile.ZIP_DEFLATED) as archive:
        for name, raw in sorted(read_files(directory).items()):
            archive.writestr(name, raw)


def promote(candidate, output, acceptance, key_path, public_path):
    key, trusted = protocol.private_key(key_path), protocol.public_key(public_path)
    files = read_files(candidate)
    verify_materials(files, trusted)
    provenance = protocol.load_json(files["provenance.json"])
    protocol.require("-" not in provenance["version"], "Prerelease cannot be promoted to stable")
    protocol.require(protocol.digest(files["folio.bin"]) == provenance["app_sha256"], "Promotion application/provenance mismatch")
    record = validate_acceptance(acceptance.read_bytes(), provenance)
    result = {"acceptance.json": protocol.canonical(record)}
    for kind in ("ota", "usb"):
        payload = protocol.verify(files[kind + ".json"], trusted)
        protocol.require(payload["version"] == provenance["version"] and payload["sequence"] == provenance["sequence"], "Mixed promotion version/sequence")
        payload["channel"] = "stable"
        result[kind + ".json"] = protocol.sign(payload, key, trusted)
    write_new(output, result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare")
    for name in ("root", "build-dir", "output-dir"):
        p.add_argument("--" + name, required=True, type=Path)
    p.add_argument("--version", required=True)
    p.add_argument("--sequence", required=True, type=int)
    p.add_argument("--commit", required=True)
    for command in ("sign", "promote"):
        p = sub.add_parser(command)
        p.add_argument("--candidate", required=True, type=Path)
        p.add_argument("--output-dir", required=True, type=Path)
        p.add_argument("--key", type=Path)
        p.add_argument("--public-key", required=True, type=Path)
        if command == "promote":
            p.add_argument("--acceptance", required=True, type=Path)
    p = sub.add_parser("zip")
    p.add_argument("--directory", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.root, args.build_dir, args.output_dir, args.version, args.sequence, args.commit)
    elif args.command == "sign":
        sign_candidate(args.candidate, args.output_dir, args.key, args.public_key)
    elif args.command == "promote":
        promote(args.candidate, args.output_dir, args.acceptance, args.key, args.public_key)
    else:
        offline_zip(args.directory, args.output)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from None
