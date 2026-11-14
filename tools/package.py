#!/usr/bin/env python3
"""Package a completed ESP-IDF build. This command never connects to a device."""

import argparse
import hashlib
import importlib.util
import json
import shutil
import struct
import subprocess
import sys
import tarfile
from pathlib import Path

FLASH_BYTES = 16 * 1024 * 1024
STORAGE_START = 0x410000
OTA_APP_LIMIT = 0x300000
LAYOUT_VERSION = "folio-16m-factory-storage-ota-v2"
EXPECTED_PARTITIONS = {
    "nvs": (1, 2, 0x9000, 0x5000),
    "phy_init": (1, 1, 0xE000, 0x1000),
    "factory": (0, 0, 0x10000, 0x400000),
    "storage": (1, 0x81, STORAGE_START, 0x500000),
    "otadata": (1, 0, 0x910000, 0x2000),
    "ota_0": (0, 0x10, 0x920000, OTA_APP_LIMIT),
    "ota_1": (0, 0x11, 0xC20000, OTA_APP_LIMIT),
}
FLASH_NAMES = {0: "bootloader.bin", 0x8000: "partition-table.bin", 0x10000: "app.bin", 0x910000: "ota_data_initial.bin"}


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def build_file(build: Path, relative: str) -> Path:
    """Flasher metadata must refer to existing regular files within the build."""
    if not isinstance(relative, str) or Path(relative).is_absolute():
        raise ValueError("Flash image path must be relative to the build directory")
    path = (build / relative).resolve(strict=True)
    if not path.is_relative_to(build) or not path.is_file():
        raise ValueError("Flash image escapes the build directory or is not a regular file")
    return path


def source_file(root: Path, relative: str) -> Path:
    """Resolve a resource/provenance input without following it outside checkout."""
    if not isinstance(relative, str) or not relative or Path(relative).is_absolute() or ".." in Path(relative).parts:
        raise ValueError("Font resource source path must be relative and cannot escape the checkout")
    path = (root / relative).resolve(strict=True)
    if not path.is_relative_to(root) or not path.is_file():
        raise ValueError("Font resource source escapes the checkout or is not a regular file")
    return path


def independent_font_asset(path: Path, root: Path) -> bool:
    """Only these two independent resources are excluded from app freshness."""
    relative = path.relative_to(root)
    return relative == Path("main/assets/folio_fonts.bin") or relative.is_relative_to(Path("main/assets/fonts"))


def font_pack_module():
    # Load our sibling validator, never an executable path supplied in metadata.
    path = Path(__file__).resolve().with_name("build_font_pack.py")
    spec = importlib.util.spec_from_file_location("folio_package_font_pack", path)
    if spec is None or spec.loader is None:
        raise ValueError("Font resource validator is unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def font_resources(root: Path):
    """Validate a new-loader checkout's independent pack and readable notices.

    Legacy/minimal test checkouts without the loader do not require new assets.
    Returned bytes are snapshots: distribution cannot race a subsequent copy.
    """
    loader = root / "main/book/folio_font_pack.c"
    if not loader.exists() and not loader.is_symlink():
        return None
    source_file(root, "main/book/folio_font_pack.c")
    image = source_file(root, "main/assets/folio_fonts.bin").read_bytes()
    validator = font_pack_module()
    verified = validator.verify_image(image)
    start, size = verified["metadata_offset"], verified["metadata_bytes"]
    embedded = validator.load_json(image[start:start + size])
    base = "main/assets/fonts/"
    manifest_raw = source_file(root, base + "manifest.json").read_bytes()
    sources_raw = source_file(root, base + "source-manifest.json").read_bytes()
    notice_raw = source_file(root, base + "NOTICE.txt").read_bytes()
    manifest = validator.load_json(manifest_raw)
    sources = validator.load_json(sources_raw)
    if manifest != embedded["manifest"] or sources != embedded["sources"] or notice_raw.decode("utf-8") != embedded["notice"]:
        raise ValueError("Readable font metadata/NOTICE differs from verified resource image")
    if hashlib.sha256(sources_raw).hexdigest() != manifest["source_manifest_sha256"]:
        raise ValueError("Font source-manifest SHA256 mismatch")
    documents = {"manifest.json": manifest_raw, "source-manifest.json": sources_raw, "NOTICE.txt": notice_raw}
    for row, spec in zip(manifest["fonts"], sources["fonts"]):
        relative = row["license_path"]
        if (not isinstance(relative, str) or Path(relative).is_absolute() or ".." in Path(relative).parts or
                len(Path(relative).parts) != 2 or Path(relative).parts[0] != "licenses" or
                relative != spec["license_path"] or relative in documents):
            raise ValueError("Font license source path escapes the allowed license directory or is duplicated")
        raw = source_file(root, base + relative).read_bytes()
        if hashlib.sha256(raw).hexdigest() != spec["license_sha256"]:
            raise ValueError("Font license SHA256 mismatch: " + relative)
        documents[relative] = raw
    summary = {"image": "font-resources.bin", "format": "FOLFONT1", "version": 1,
               "offset": "0x410000", "size": "0x500000", "bytes": len(image), "sha256": verified["sha256"],
               "total_used": verified["total_used"], "erased_tail_bytes": verified["erased_tail_bytes"],
               "not_default_flash": True, "included_in_merged": False, "included_in_ota": False,
               "requires_complete_storage_backup": True, "requires_explicit_overwrite_permission": True,
               "overwrites_confirmed_storage_only": {"start": "0x410000", "end_inclusive": "0x90ffff"},
               "instructions": "font-resources-install.txt", "notices": "font-resources/NOTICE.txt",
               "source_manifest": "font-resources/source-manifest.json", "licenses": "font-resources/licenses/",
               "device_verified": False}
    return {"image": image, "documents": documents, "summary": summary}


def font_resource_instructions(summary: dict) -> str:
    """Human checklist only: deliberately no automatic flasher or command line."""
    return (
        "INDEPENDENT FONT RESOURCE INSTALLATION — NOT PART OF DEFAULT USB/OTA\n\n"
        "The default usb-install.txt commands, merged.bin and update/ only install application/layout images; "
        "they preserve storage and NEVER install this font resource. If resources are absent or invalid, "
        "the firmware retains the original Songti fallback. Do not add this file to those commands.\n\n"
        "This is a separate destructive replacement of the CONFIRMED storage partition only. "
        "Old storage contents are not migrated or preserved by font-resources.bin.\n"
        "1. Confirm the exact reader/flash identity and read its actual partition table. Proceed only when "
        "storage is exactly 0x410000..0x90FFFF inclusive (0x500000 bytes). Do not infer this from a label alone.\n"
        "2. Read and save the ENTIRE existing 0x500000-byte storage partition first. Verify its byte length "
        "and SHA-256; retain an independent, readable copy and recovery notes. A partial backup is insufficient.\n"
        "3. Obtain explicit user permission to overwrite that entire backed-up storage range. Default firmware "
        "installation permission is NOT permission to replace storage. Stop if existing data must be retained.\n"
        f"4. Check font-resources.bin is {summary['bytes']} bytes and SHA-256 {summary['sha256']}. "
        "Use the included source validator with --verify-only --output /absolute/path/to/this/release/font-resources.bin "
        "against this exact file, not its default source-tree image.\n"
        "5. Only after the above checks and explicit permission, have a qualified operator perform a separate "
        "single-range write: this resource file at 0x410000, ending at 0x90FFFF. Do not change partitions, "
        "bootloader, NVS, otadata, factory/OTA application slots or TF-card data. Never erase-flash or format storage.\n"
        "6. Read back the full 0x500000 bytes and compare SHA-256 before reporting success. Retain the original "
        "backup; restore it only in a separately confirmed recovery operation to the same verified range.\n\n"
        "The packager does not contact a device, read partitions, back up data, obtain overwrite permission, "
        "or mark this installation device-verified. No automatic installer is included.\n"
        "Keep font-resources/NOTICE.txt, all four OFL files, the source manifest and LICENSES.md readable "
        "with every redistribution, not only embedded in inaccessible flash metadata.\n")


def validate_partition_table(path: Path) -> dict[str, dict[str, str | int]]:
    """Validate the actual binary which will be flashed, including its MD5."""
    data = path.read_bytes()
    if not data or len(data) > 0x1000 or len(data) % 32:
        raise ValueError("Invalid binary partition-table length")
    partitions = {}
    md5_seen = False
    for position in range(0, len(data), 32):
        entry = data[position:position + 32]
        if entry == b"\xff" * 32:
            if any(value != 0xFF for value in data[position:]):
                raise ValueError("Unexpected data after partition-table terminator")
            break
        if entry[:2] == b"\xeb\xeb":
            if md5_seen or entry[2:16] != b"\xff" * 14 or entry[16:] != hashlib.md5(data[:position]).digest():
                raise ValueError("Partition-table MD5 mismatch")
            md5_seen = True
            continue
        if md5_seen:
            raise ValueError("Partition entry follows the MD5 record")
        magic, kind, subtype, offset, size, raw_name, flags = struct.unpack("<HBBII16sI", entry)
        if magic != 0x50AA or b"\0" not in raw_name or flags:
            raise ValueError("Invalid or encrypted partition entry")
        name = raw_name.split(b"\0", 1)[0].decode("ascii")
        if name in partitions or not size or offset % 0x1000 or (kind == 0 and offset % 0x10000):
            raise ValueError("Duplicate, empty or unaligned partition")
        if offset < 0x9000 or offset + size > FLASH_BYTES:
            raise ValueError("Partition exceeds the 16 MB flash boundary")
        partitions[name] = (kind, subtype, offset, size)
    if not md5_seen or partitions != EXPECTED_PARTITIONS:
        raise ValueError("Unexpected OTA layout: preserve factory/storage and the approved two 3 MiB OTA slots")
    previous_end = 0x9000
    for name, (_, _, offset, size) in sorted(partitions.items(), key=lambda item: item[1][2]):
        if offset < previous_end:
            raise ValueError(f"Overlapping partition: {name}")
        previous_end = offset + size
    return {name: {"type": kind, "subtype": subtype, "offset": hex(offset), "size": hex(size)}
            for name, (kind, subtype, offset, size) in partitions.items()}


def package_build(root: Path, build: Path, output: Path) -> None:
    root = root.resolve(strict=True)
    build = build.resolve(strict=True)
    output = output.resolve()
    flash = json.loads((build / "flasher_args.json").read_text())
    description = json.loads((build / "project_description.json").read_text())
    if description.get("project_name") != "Folio" or description.get("target") != "esp32s3":
        raise ValueError("This build does not belong to Folio / ESP32-S3")
    if Path(description["project_path"]).resolve() != root:
        raise ValueError("Build source directory does not match this checkout")
    images = {int(offset, 0): build_file(build, path) for offset, path in flash["flash_files"].items()}
    if len(images) != len(flash["flash_files"]) or set(images) != set(FLASH_NAMES):
        raise ValueError("Unexpected flash offsets; this release requires separate OTA data at 0x910000")
    for key, offset in (("bootloader", 0), ("partition-table", 0x8000), ("app", 0x10000)):
        if int(flash[key]["offset"], 0) != offset or build_file(build, flash[key]["file"]) != images[offset]:
            raise ValueError(f"Inconsistent {key} flash metadata")
        if flash[key].get("encrypted", False) not in (False, "false"):
            raise ValueError("Encrypted flash images require a separate packaging workflow")
    if flash["extra_esptool_args"].get("chip") != "esp32s3" or flash["flash_settings"].get("flash_size") != "16MB":
        raise ValueError("Expected ESP32-S3 and 16 MB flash")
    if not 0 < images[0].stat().st_size <= 0x8000:
        raise ValueError("Bootloader overlaps the partition table")
    app = images[0x10000]
    if not 0 < app.stat().st_size <= OTA_APP_LIMIT:
        raise ValueError("Application must fit both 3 MiB OTA slots")
    partitions = validate_partition_table(images[0x8000])
    if images[0x910000].read_bytes() != b"\xff" * 0x2000:
        raise ValueError("Initial OTA data must be exactly 8192 erased bytes for factory boot")
    resources = font_resources(root)
    source_paths = [root / "CMakeLists.txt", root / "sdkconfig.defaults", root / "sdkconfig",
                    root / "partitions_16M.csv", root / "dependencies.lock"]
    for directory in (root / "main", root / "components"):
        source_paths.extend(path for path in directory.rglob("*")
                            if path.is_file() and not independent_font_asset(path, root))
    changed = [str(path.relative_to(root)) for path in source_paths if path.stat().st_mtime_ns > app.stat().st_mtime_ns]
    if changed:
        raise ValueError("Build is older than source files; run tools/idf.sh build first: " + ", ".join(changed[:8]))
    source_directories = ("main", "components", "tools", "docs", "preview", "licenses")
    if output == root or output.is_relative_to(build) or any(output.is_relative_to(root / name) for name in source_directories):
        raise ValueError("Release output must be outside the build and archived source directories")
    output.mkdir(parents=True, exist_ok=False)
    try:
        merge_files = []
        for offset, source in sorted(images.items()):
            name = FLASH_NAMES[offset]
            shutil.copy2(source, output / name)
            if offset < STORAGE_START:
                merge_files.extend([hex(offset), str(output / name)])
        settings = flash["flash_settings"]
        # A high-address OTA-data image must NEVER enter merge-bin: its erased
        # padding would otherwise span and destroy the existing FAT storage.
        subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge-bin", "-o", str(output / "merged.bin"),
                        "--flash-mode", settings["flash_mode"], "--flash-size", settings["flash_size"],
                        "--flash-freq", settings["flash_freq"], *merge_files], check=True)
        merged_size = (output / "merged.bin").stat().st_size
        if merged_size < 0x10000 + app.stat().st_size or merged_size > STORAGE_START:
            raise ValueError("Merged image has an invalid length or crosses the preserved storage boundary 0x410000")
        update = output / "update"; update.mkdir()
        shutil.copy2(app, update / "folio.bin")
        (update / "folio.bin.sha256").write_text(f"{digest(update / 'folio.bin')}  folio.bin\n")
        (output / "usb-install.txt").write_text(
            "First installation of the OTA partition layout requires USB. Back up the device first.\n"
            "Run from this release directory, replacing YOUR_PORT with the chosen serial port:\n\n"
            "Default USB installation: write the four independent application/layout images; this preserves existing NVS and storage.\n"
            f"python -m esptool --chip esp32s3 --port YOUR_PORT write-flash --flash-mode {settings['flash_mode']} "
            f"--flash-size 16MB --flash-freq {settings['flash_freq']} "
            "0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 app.bin 0x910000 ota_data_initial.bin\n\n"
            "Optional merged-image installation: this overwrites NVS, including stored device settings and Wi-Fi credentials.\n"
            f"python -m esptool --chip esp32s3 --port YOUR_PORT write-flash --flash-mode {settings['flash_mode']} "
            f"--flash-size 16MB --flash-freq {settings['flash_freq']} 0x0 merged.bin 0x910000 ota_data_initial.bin\n\n"
            "merged.bin contains only bootloader, partition table and the factory application.\n"
            "The separate OTA-data write selects the factory image; it is required for this USB installation.\n"
            "Do not merge OTA data into merged.bin, pad to 16 MB, or erase-flash. Default USB/OTA must never write storage 0x410000..0x90FFFF.\n"
            "For later in-device OTA updates, use only update/folio.bin and update/folio.bin.sha256.\n" +
            ("Independent fonts are NOT installed by these commands or OTA. See font-resources-install.txt for the separate "
             "full-backup and explicit storage-overwrite permission workflow; never add resources to the default commands.\n"
             if resources else ""))
        if resources:
            (output / "font-resources.bin").write_bytes(resources["image"])
            readable = output / "font-resources"
            readable.mkdir()
            for relative, raw in resources["documents"].items():
                target = readable / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(raw)
            (output / "font-resources-install.txt").write_text(font_resource_instructions(resources["summary"]))
        for source, name in ((root / "LICENSES.md", "LICENSES.md"), (root / "docs/刷机说明.md", "restore.md"),
                             (root / "README.md", "README.md"), (root / "sdkconfig", "sdkconfig")):
            shutil.copy2(source, output / name)
        shutil.copytree(root / "licenses", output / "licenses")
        shutil.copytree(root / "docs", output / "docs")
        with tarfile.open(output / "source.tar.gz", "w:gz") as archive:
            for name in (*source_directories, "CMakeLists.txt", "sdkconfig.defaults", "partitions_16M.csv",
                         "dependencies.lock", "LICENSES.md", "README.md", ".gitignore"):
                archive.add(root / name, arcname="folio/" + name, filter=lambda member: None if "__pycache__" in member.name else member)
            archive.add(root / "sdkconfig", arcname="folio/sdkconfig")
        manifest = {"project": "Folio", "version": description["project_version"],
                    "target": "esp32s3", "hardware": "RDP-G01-W", "idf_version": description.get("git_revision") or description.get("idf_ver"),
                    "device_verified": False, "layout_version": LAYOUT_VERSION, "partitions": partitions,
                    "app_offset": "0x10000", "merged_offset": "0x0", "merged_end_exclusive": hex(merged_size),
                    "merged_components": ["bootloader.bin", "partition-table.bin", "app.bin"],
                    "flash_files": {hex(offset): name for offset, name in FLASH_NAMES.items()},
                    "usb_install_files": {hex(offset): name for offset, name in FLASH_NAMES.items()},
                    "optional_merged_install_files": {"0x0": "merged.bin", "0x910000": "ota_data_initial.bin"},
                    "optional_merged_install_overwrites_nvs": True,
                    "requires_separate_otadata_write": True, "preserved_storage": {"start": "0x410000", "end_exclusive": "0x910000"},
                    "ota_update": {"image": "update/folio.bin", "checksum": "update/folio.bin.sha256", "maximum_app_bytes": OTA_APP_LIMIT,
                                   "requires_installed_layout": LAYOUT_VERSION},
                    "flash_settings": settings, "sha256": {}}
        if resources:
            manifest["font_resources"] = resources["summary"]
            manifest["preserved_storage"]["scope"] = "Default USB, merged and OTA only; independent font installation requires backup and explicit overwrite permission"
        for path in sorted(output.rglob("*")):
            if path.is_file(): manifest["sha256"][path.relative_to(output).as_posix()] = digest(path)
        (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        (output / "sha256.txt").write_text("".join(f"{digest(path)}  {path.relative_to(output).as_posix()}\n" for path in sorted(output.rglob("*")) if path.is_file()))
    except BaseException:
        shutil.rmtree(output)
        raise
    print(f"Packaged development build: {output}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    try:
        package_build(root, args.build_dir, args.output_dir)
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    main()
