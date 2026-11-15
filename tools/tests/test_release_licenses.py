#!/usr/bin/env python3
"""Bounded notice collection; synthetic SDK fixtures, no private keys or network."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_licenses as licenses
import release_manifest as protocol


class LicenseCollectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="folio-notice-fixture-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.idf = self.root / "sdk"
        self.idf.mkdir()
        self.upstream = licenses.load_rules()["external"][0]
        self.notice = self.upstream["text"].encode("utf-8")
        self.relative = "components/wpa_supplicant/README"
        self.source = self.idf / self.relative
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(self.notice + b"\nint synthetic_source_body;\n")
        self.rules = {"sdk": [{"component": "wpa_supplicant", "path": self.relative,
                              "lines": len(self.notice.splitlines()), "sha256": protocol.digest(self.notice)}],
                      "external": [], "source_scope_components": ["wpa_supplicant"]}
        self.description = {"build_components": ["wpa_supplicant"],
                            "build_component_info": {"wpa_supplicant": {"sources": []}}}

    def test_exact_full_notice_and_crlf_are_preserved(self):
        files, evidence = licenses.fixed_notices(self.idf, self.description, self.rules)
        name = "sdk-notices/esp-idf/" + self.relative + ".notice.txt"
        self.assertEqual(files[name], self.notice)
        self.assertIn(b"Redistributions in binary form", files[name])
        self.assertIn(b"Neither the name of the copyright holder", files[name])
        self.assertIn(b"SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.", files[name])
        self.assertTrue(files[name].endswith(b"*/\r\n"))
        self.assertEqual(evidence[0]["source_sha256"], protocol.digest(self.source.read_bytes()))
        self.assertNotIn(b"synthetic_source_body", files[name])

    def test_missing_readme_fails_closed(self):
        self.rules["sdk"][0]["path"] = "components/wpa_supplicant/absent-README"
        with self.assertRaisesRegex(ValueError, "missing or escapes"):
            licenses.fixed_notices(self.idf, self.description, self.rules)

    def test_truncated_or_modified_terms_fail_closed(self):
        for raw in (self.notice[:100], self.notice.replace(b"Redistributions in binary form", b"Removed binary condition"),
                    self.notice.replace(b"CONSEQUENTIAL DAMAGES", b"Removed disclaimer"), self.notice.replace(b"\r\n", b"\n")):
            with self.subTest(raw=protocol.digest(raw)):
                self.source.write_bytes(raw)
                with self.assertRaisesRegex(ValueError, "incomplete|changed/truncated"):
                    licenses.fixed_notices(self.idf, self.description, self.rules)

    def test_shorter_mapping_cannot_pass_original_full_notice_hash(self):
        self.rules["sdk"][0]["lines"] -= 3
        with self.assertRaisesRegex(ValueError, "changed/truncated"):
            licenses.fixed_notices(self.idf, self.description, self.rules)

    def test_external_notice_is_complete_and_tamper_evident(self):
        self.description["build_components"].append("heap")
        self.rules["external"] = [copy.deepcopy(self.upstream)]
        files, evidence = licenses.fixed_notices(self.idf, self.description, self.rules)
        self.assertEqual(files["sdk-notices/upstream/tlsf-BSD-3-Clause.txt"], self.notice)
        self.assertEqual(evidence[-1]["sdk_submodule_commit"], "2867f6883a12920b1969ff9624c0ab0e4185c2ce")
        self.rules["external"][0]["text"] = self.upstream["text"][:100]
        with self.assertRaisesRegex(ValueError, "upstream notice changed/truncated"):
            licenses.fixed_notices(self.idf, self.description, self.rules)

    def test_unreviewed_compiled_source_fails_closed(self):
        self.description["build_component_info"]["wpa_supplicant"]["sources"] = [str(self.idf / "components/wpa_supplicant/new.c")]
        with self.assertRaisesRegex(ValueError, "needs notice review"):
            licenses.fixed_notices(self.idf, self.description, self.rules)

    def test_traversal_and_external_symlink_are_rejected(self):
        outside = self.root / "outside-notice"
        outside.write_bytes(self.notice)
        for relative in ("../outside-notice", str(outside)):
            self.rules["sdk"][0]["path"] = relative
            with self.assertRaisesRegex(ValueError, "Unsafe fixed"):
                licenses.fixed_notices(self.idf, self.description, self.rules)
        link = self.source.parent / "escape"
        link.symlink_to(outside)
        self.rules["sdk"][0]["path"] = link.relative_to(self.idf).as_posix()
        with self.assertRaisesRegex(ValueError, "escapes approved root"):
            licenses.fixed_notices(self.idf, self.description, self.rules)

    def test_actual_build_libc_selection_and_ambiguous_config(self):
        build = self.root / "build"
        config = build / "config/sdkconfig.json"
        config.parent.mkdir(parents=True)
        description = {"build_dir": str(build)}
        for first, second, expected in ((True, False, "picolibc"), (False, True, "newlib")):
            raw = protocol.canonical({"LIBC_PICOLIBC": first, "LIBC_NEWLIB": second})
            config.write_bytes(raw)
            self.assertEqual(licenses.selected_libc(description), (expected, protocol.digest(raw)))
        for value in ({}, {"LIBC_PICOLIBC": True}, {"LIBC_PICOLIBC": True, "LIBC_NEWLIB": True},
                      {"LIBC_PICOLIBC": False, "LIBC_NEWLIB": False}, {"LIBC_PICOLIBC": 1, "LIBC_NEWLIB": False}):
            config.write_bytes(protocol.canonical(value))
            with self.assertRaisesRegex(ValueError, "exactly one"):
                licenses.selected_libc(description)
        with self.assertRaisesRegex(ValueError, "configuration missing"):
            licenses.selected_libc({"build_dir": str(self.root / "no-build")})

    def runtime_fixture(self):
        relatives = ("gcc/COPYING.RUNTIME", "gcc/COPYING3", "newlib/COPYING.NEWLIB") + licenses.RUNTIME_NOTICES["picolibc"]
        files = {"sdk-notices/toolchain/" + path: self.notice + path.encode() for path in relatives}
        rules = {"toolchain": {path: protocol.digest(files["sdk-notices/toolchain/" + path]) for path in relatives}}
        return files, rules

    def test_selected_runtime_notices_are_required_not_interchangeable(self):
        files, rules = self.runtime_fixture()
        licenses.verify_runtime_notices(files, "picolibc", rules)
        licenses.verify_runtime_notices(files, "newlib", rules)
        for selected in ("picolibc", "newlib"):
            for relative in ("gcc/COPYING.RUNTIME", "gcc/COPYING3") + licenses.RUNTIME_NOTICES[selected]:
                key = "sdk-notices/toolchain/" + relative
                missing = {name: data for name, data in files.items() if name != key}
                with self.subTest(selected=selected, path=relative), self.assertRaisesRegex(ValueError, "notice missing"):
                    licenses.verify_runtime_notices(missing, selected, rules)
                changed = dict(files, **{key: files[key][:-1]})
                with self.assertRaisesRegex(ValueError, "changed/truncated"):
                    licenses.verify_runtime_notices(changed, selected, rules)
        newlib_only = {name: data for name, data in files.items() if "/picolibc/" not in name}
        licenses.verify_runtime_notices(newlib_only, "newlib", rules)
        with self.assertRaisesRegex(ValueError, "picolibc"):
            licenses.verify_runtime_notices(newlib_only, "picolibc", rules)

    def test_fixed_mapping_covers_reported_gaps_and_original_terms(self):
        rules = licenses.load_rules()
        rows = {row["path"]: row for row in rules["sdk"]}
        self.assertEqual(len(rows), len(rules["sdk"]))
        for path, lines in {"components/wpa_supplicant/README": 41, "components/esp_system/ubsan.c": 25,
                            "components/sdmmc/sdmmc_common.c": 16, "components/xtensa/xtensa_vectors.S": 29,
                            "components/esp_wifi/lib/README.rst": 10, "components/esp_phy/lib/README.rst": 10}.items():
            # Additional contiguous introductory comments may follow the legal block.
            self.assertGreaterEqual(rows[path]["lines"], lines)
        self.assertIn("components/xtensa/include/xtensa_api.h", rows)
        self.assertIn("components/xtensa/esp32s3/include/xtensa/config/core.h", rows)
        self.assertIn("components/heap/tlsf/tlsf_control_functions.h", rows)
        self.assertIn("components/fatfs/src/ff.c", rows)
        # The MD4 public-domain provenance is below the initial BSD wrapper.
        md4 = rows["components/wpa_supplicant/src/crypto/md4-internal.c"]
        self.assertEqual(md4["lines"], 64)
        self.assertEqual(md4["sha256"], "f624df929a1d054d73ddf4fb173a42856a0f4b8b7580186c09b760eba4b165c6")
        self.assertEqual(protocol.digest(self.notice), "a56c3866a46f99654b0402c177e3baee91d3de58af65f996d77b350f729015a5")
        self.assertEqual(self.upstream["source_sha256"], "f7f73c48810ba60203095667c226e5a600a6ea0f69afba48efff6efbaa628d4f")
        for path in ("gcc/COPYING.RUNTIME", "gcc/COPYING3", "newlib/COPYING.NEWLIB") + licenses.RUNTIME_NOTICES["picolibc"]:
            self.assertIn(path, rules["toolchain"])

    def test_collect_records_actual_sdk_dirty_state_and_libc(self):
        build = self.root / "build"
        config = build / "config/sdkconfig.json"
        config.parent.mkdir(parents=True)
        config.write_bytes(protocol.canonical({"LIBC_PICOLIBC": True, "LIBC_NEWLIB": False}))
        compiler = self.root / "toolchain/bin/fixture-gcc"
        compiler.parent.mkdir(parents=True)
        compiler.write_text("synthetic compiler; mocked subprocess only\n")
        (self.idf / "LICENSE").write_text("Synthetic root notice\n")
        directory = self.idf / "components/unit"
        directory.mkdir(parents=True)
        files, runtime = self.runtime_fixture()
        for path, data in files.items():
            destination = compiler.parent.parent / "share/licenses" / path.removeprefix("sdk-notices/toolchain/")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        description = {"target": "esp32s3", "build_dir": str(build), "idf_path": str(self.idf), "c_compiler": str(compiler),
                       "build_components": ["unit"], "build_component_info": {"unit": {"dir": str(directory), "type": "LIBRARY", "sources": []}}}
        rules = dict(runtime, sdk=[], external=[], source_scope_components=[])
        def output(command, **kwargs):
            if "rev-parse" in command:
                return protocol.IDF_COMMIT + "\n"
            if "submodule" in command:
                return ""
            if "--porcelain" in command:
                return " M tools/idf_py_actions/tools.py\n"
            if "--version" in command:
                return "fixture-gcc (esp-15.2.0_20251204) 15.2.0\n"
            self.fail("Unexpected subprocess: " + repr(command))
        with patch.object(licenses, "load_rules", return_value=rules), patch.object(licenses.subprocess, "check_output", side_effect=output):
            collected = licenses.collect(self.root, description)
        inventory = json.loads(collected["license-inventory.json"])
        self.assertTrue(inventory["idf_dirty"])
        self.assertEqual(inventory["idf_commit"], protocol.IDF_COMMIT)
        self.assertEqual(inventory["selected_libc"], "picolibc")
        self.assertEqual(inventory["build_config_sha256"], protocol.digest(config.read_bytes()))
        self.assertEqual(inventory["unresolved_reviews"], [])
        self.assertIn("not a release prerequisite", inventory["review_notes"][2])
        with patch.object(licenses, "load_rules", return_value=rules), patch.object(licenses.subprocess, "check_output", return_value="0" * 40):
            with self.assertRaisesRegex(ValueError, "SDK HEAD differs"):
                licenses.collect(self.root, description)


if __name__ == "__main__":
    unittest.main()
