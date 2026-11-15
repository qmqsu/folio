#!/usr/bin/env python3
"""Collect bounded, real linked-SDK/toolchain notices; not a complete SPDX SBOM."""
import argparse
from pathlib import Path
import re
import subprocess

import release_manifest as protocol

NOTICE = re.compile(r"^(?:LICEN[CS]E|COPYING|COPYRIGHT|NOTICE)(?:$|[0-9._-])", re.IGNORECASE)
RULES = Path(__file__).with_name("release_sdk_notices.json")
RUNTIME_NOTICES = {
    "picolibc": ("picolibc/COPYING.picolibc", "picolibc/COPYING.NEWLIB", "picolibc/COPYING.GPL2"),
    "newlib": ("newlib/COPYING.NEWLIB",),
}


def load_rules():
    rules = protocol.load_json(RULES.read_bytes())
    protocol.require(rules.get("schema") == 1 and rules.get("idf_commit") == protocol.IDF_COMMIT,
                     "Notice mapping does not match the pinned SDK")
    return rules


def notice_bytes(path, base, maximum=2 * 1024 * 1024):
    protocol.require(path.resolve().is_relative_to(base.resolve()) and path.is_file(),
                     "SDK notice missing or escapes approved root: " + path.name)
    protocol.require(path.stat().st_size <= maximum, "SDK notice unexpectedly large")
    raw = path.read_bytes()
    protocol.require(b"\0" not in raw and b"/Users/" not in raw, "SDK notice contains binary/private path")
    raw.decode("utf-8")
    return raw


def selected_libc(description):
    build = Path(description["build_dir"]).resolve()
    path = build / "config/sdkconfig.json"
    protocol.require(path.resolve().is_relative_to(build) and path.is_file(), "Actual build libc configuration missing")
    raw = path.read_bytes()
    config = protocol.load_json(raw)
    flags = [config.get("LIBC_PICOLIBC"), config.get("LIBC_NEWLIB")]
    protocol.require(all(type(flag) is bool for flag in flags) and sum(flags) == 1,
                     "Select exactly one actual LIBC_PICOLIBC/LIBC_NEWLIB")
    return ("picolibc" if flags[0] else "newlib"), protocol.digest(raw)


def fixed_notices(idf, description, rules):
    """Read only explicitly mapped paths/spans; preserve every original byte."""
    linked = set(description["build_components"])
    entries = [row for row in rules["sdk"] if row["component"] in linked]
    known = {row["path"] for row in entries}
    for name in set(rules["source_scope_components"]) & linked:
        for source in description["build_component_info"][name]["sources"]:
            path = Path(source).resolve()
            protocol.require(path.is_relative_to(idf) and path.relative_to(idf).as_posix() in known,
                             "Compiled SDK source needs notice review: " + path.name)
    files, evidence = {}, []
    for row in entries:
        relative = Path(row["path"])
        protocol.require(not relative.is_absolute() and ".." not in relative.parts and relative.parts[:1] == ("components",),
                         "Unsafe fixed SDK notice path")
        raw = notice_bytes(idf / relative, idf, maximum=16 * 1024 * 1024)
        lines = raw.splitlines(keepends=True)
        count = row["lines"]
        protocol.require(type(count) is int and 0 < count <= len(lines), "Fixed SDK notice span is incomplete: " + row["path"])
        notice = b"".join(lines[:count])
        protocol.require(protocol.digest(notice) == row["sha256"], "Fixed SDK notice changed/truncated: " + row["path"])
        name = "sdk-notices/esp-idf/" + relative.as_posix() + ".notice.txt"
        files[name] = notice
        evidence.append({"file": name, "source": "esp-idf/" + row["path"], "lines": [1, count],
                         "source_sha256": protocol.digest(raw), "notice_sha256": row["sha256"], "component": row["component"]})
    for row in rules["external"]:
        if row["component"] not in linked:
            continue
        protocol.require(re.fullmatch(r"[A-Za-z0-9._-]+", row["name"]), "Unsafe upstream notice name")
        raw = row["text"].encode("utf-8")
        protocol.require(protocol.digest(raw) == row["sha256"], "Fixed upstream notice changed/truncated")
        name = "sdk-notices/upstream/" + row["name"]
        files[name] = raw
        evidence.append({key: value for key, value in row.items() if key != "text"} | {"file": name})
    return files, evidence


def verify_runtime_notices(files, libc, rules):
    required = ("gcc/COPYING.RUNTIME", "gcc/COPYING3") + RUNTIME_NOTICES[libc]
    for relative in required:
        name = "sdk-notices/toolchain/" + relative
        protocol.require(name in files, "Required " + libc + "/GCC runtime notice missing: " + relative)
        protocol.require(protocol.digest(files[name]) == rules["toolchain"].get(relative),
                         "Required runtime notice changed/truncated: " + relative)


def collect(root, description):
    root = root.resolve()
    idf = Path(description["idf_path"]).resolve()
    protocol.require(description.get("target") == "esp32s3", "Fixed notice mapping supports ESP32-S3 only")
    rules = load_rules()
    libc, config_digest = selected_libc(description)
    actual_commit = subprocess.check_output(["git", "-C", str(idf), "rev-parse", "HEAD"], text=True).strip()
    protocol.require(actual_commit == protocol.IDF_COMMIT, "SDK HEAD differs from fixed notice mapping")
    dirty = bool(subprocess.check_output(["git", "-C", str(idf), "status", "--porcelain", "--untracked-files=no"], text=True).strip())
    compiler = Path(description["c_compiler"]).resolve()
    protocol.require(compiler.is_file() and compiler.name.endswith("gcc"), "Actual compiler path unavailable")
    toolchain = compiler.parent.parent
    license_root = toolchain / "share/licenses"
    protocol.require(license_root.is_dir(), "Toolchain license directory missing")
    files, component_rows = {}, []
    def add(path, base, prefix):
        files[prefix + path.relative_to(base).as_posix()] = notice_bytes(path, base)
    protocol.require((idf / "LICENSE").is_file(), "ESP-IDF root license missing")
    for path in idf.iterdir():
        if path.is_file() and NOTICE.match(path.name):
            add(path, idf, "sdk-notices/esp-idf/")
    for relative in ("docs/en/COPYRIGHT.rst", "docs/zh_CN/COPYRIGHT.rst"):
        if (idf / relative).is_file():
            add(idf / relative, idf, "sdk-notices/esp-idf/")
    info = description.get("build_component_info")
    protocol.require(isinstance(info, dict) and info, "Linked component inventory missing")
    for name in description["build_components"]:
        if not name:
            continue
        row = info[name]
        directory = Path(row["dir"]).resolve()
        if directory.is_relative_to(idf):
            before = set(files)
            for path in directory.rglob("*"):
                if path.is_file() and NOTICE.match(path.name):
                    add(path, idf, "sdk-notices/esp-idf/")
            component_rows.append({"name": name, "source": "esp-idf/" + directory.relative_to(idf).as_posix(),
                                   "type": row["type"], "notices": sorted(set(files) - before),
                                   "fallback_root_notice": "sdk-notices/esp-idf/LICENSE"})
        else:
            protocol.require(directory.is_relative_to(root), "Unreviewed external linked component")
            component_rows.append({"name": name, "source": directory.relative_to(root).as_posix(), "type": row["type"], "notices": "included in corresponding source archive"})
    for path in license_root.rglob("*"):
        if path.is_file() and NOTICE.match(path.name):
            add(path, license_root, "sdk-notices/toolchain/")
    verify_runtime_notices(files, libc, rules)
    supplemental, source_evidence = fixed_notices(idf, description, rules)
    files.update(supplemental)
    for row in component_rows:
        if isinstance(row["notices"], list):
            row["notices"] = sorted(set(row["notices"]) | {item["file"] for item in source_evidence if item["component"] == row["name"]})
    status = subprocess.check_output(["git", "-C", str(idf), "submodule", "status", "--recursive"], text=True)
    submodules = []
    for line in status.splitlines():
        protocol.require(line.startswith(" "), "Uninitialized/mismatched SDK submodule")
        commit, path, *_ = line.strip().split()
        protocol.require(re.fullmatch(r"[0-9a-f]{40}", commit) and not path.startswith("/") and ".." not in Path(path).parts, "Invalid SDK submodule identity")
        submodules.append({"path": path, "commit": commit})
    identities = {row["path"]: row["commit"] for row in submodules}
    for row in source_evidence:
        if "sdk_submodule_path" in row:
            protocol.require(identities.get(row["sdk_submodule_path"]) == row["sdk_submodule_commit"],
                             "Upstream notice is not bound to the actual SDK submodule")
    version = subprocess.check_output([str(compiler), "--version"], text=True).splitlines()[0]
    protocol.require("esp-15.2.0_20251204" in version, "Toolchain compiler differs from pinned IDF tool manifest")
    inventory = {"schema": 1, "scope": "Linked build-component notices, fixed source/README spans, and installed compiler share/licenses; NOT a complete SPDX/SBOM or legal approval. Installed compiler notices do not imply every tool/runtime is linked.",
                 "idf_commit": actual_commit, "idf_dirty": dirty, "compiler": version, "submodules": submodules,
                 "selected_libc": libc, "build_config_sha256": config_digest,
                 "notice_mapping_sha256": protocol.digest(RULES.read_bytes()), "fixed_notice_sources": source_evidence,
                 "unresolved_reviews": [],
                 "review_notes": [
                     "WiFi/PHY fixed submodules provide Espressif copyright and Object-form Apache-2.0 authorization; exact LICENSE/README texts are retained.",
                     "Six lwIP files have no file-level copyright line; their existing BSD license text and author/modification/relicensing statements are retained without inventing holders from Git authorship.",
                     "Upstream replies are not a release prerequisite; candidate review and hardware acceptance remain separate.",
                 ],
                 "components": component_rows, "files": {name: protocol.digest(raw) for name, raw in sorted(files.items())}}
    files["license-inventory.json"] = protocol.canonical(inventory)
    return files


def main():
    import release_bundle
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--description", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    files = collect(args.root, protocol.load_json(args.description.read_bytes()))
    release_bundle.write_new(args.output, files)
    print(f"Collected {len(files) - 1} real notice files; manual legal review still required")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from None
