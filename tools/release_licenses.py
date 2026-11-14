#!/usr/bin/env python3
"""Collect bounded, real linked-SDK/toolchain notices; not a complete SPDX SBOM."""
import argparse
from pathlib import Path
import re
import subprocess

import release_manifest as protocol

NOTICE = re.compile(r"^(?:LICEN[CS]E|COPYING|COPYRIGHT|NOTICE)(?:$|[0-9._-])", re.IGNORECASE)


def collect(root, description):
    root = root.resolve()
    idf = Path(description["idf_path"]).resolve()
    compiler = Path(description["c_compiler"]).resolve()
    protocol.require(compiler.is_file() and compiler.name.endswith("gcc"), "Actual compiler path unavailable")
    toolchain = compiler.parent.parent
    license_root = toolchain / "share/licenses"
    protocol.require(license_root.is_dir(), "Toolchain license directory missing")
    files, component_rows = {}, []
    def add(path, base, prefix):
        protocol.require(path.resolve().is_relative_to(base.resolve()) and path.is_file(), "SDK notice escapes approved root: " + path.name)
        protocol.require(path.stat().st_size <= 2 * 1024 * 1024, "SDK notice unexpectedly large")
        raw = path.read_bytes()
        protocol.require(b"\0" not in raw and b"/Users/" not in raw, "SDK notice contains binary/private path")
        raw.decode("utf-8")
        files[prefix + path.relative_to(base).as_posix()] = raw
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
    required = {"sdk-notices/toolchain/gcc/COPYING.RUNTIME", "sdk-notices/toolchain/gcc/COPYING3", "sdk-notices/toolchain/newlib/COPYING.NEWLIB"}
    protocol.require(required <= files.keys(), "GCC Runtime Library Exception/newlib legal text missing")
    status = subprocess.check_output(["git", "-C", str(idf), "submodule", "status", "--recursive"], text=True)
    submodules = []
    for line in status.splitlines():
        protocol.require(line.startswith(" "), "Uninitialized/mismatched SDK submodule")
        commit, path, *_ = line.strip().split()
        protocol.require(re.fullmatch(r"[0-9a-f]{40}", commit) and not path.startswith("/") and ".." not in Path(path).parts, "Invalid SDK submodule identity")
        submodules.append({"path": path, "commit": commit})
    version = subprocess.check_output([str(compiler), "--version"], text=True).splitlines()[0]
    protocol.require("esp-15.2.0_20251204" in version, "Toolchain compiler differs from pinned IDF tool manifest")
    inventory = {"schema": 1, "scope": "Linked build components, root ESP-IDF notices, and installed compiler share/licenses; NOT a complete SPDX/SBOM or legal approval.",
                 "idf_commit": protocol.IDF_COMMIT, "compiler": version, "submodules": submodules,
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
