# Pinned wallpaper JPEG decoder

- Origin: Espressif `idf-extra-components`, `esp_jpeg` **1.3.1**, commit `746e83ddbea0db9c3d24993a87c4c737a60337ae`.
- Source: https://github.com/espressif/idf-extra-components/tree/746e83ddbea0db9c3d24993a87c4c737a60337ae/esp_jpeg/tjpgd
- Registry: https://components.espressif.com/components/espressif/esp_jpeg/versions/1.3.1/readme
- Exact archive: https://components.espressif.com/api/downloads/?object_type=component&object_id=64a7c4fb-8c0e-4fdb-89b7-37759ff6ca27
- Archive SHA-256: `636a9b2ecfd3da462fbe00f16d7f37087640a0f06f1fcdca0929ab639d8418a8`.
- Original `tjpgd.c` SHA-256: `1286f5e32e7a4750ee36fd57d2c442c99a74ffed0e81cb5d2fbf92fccf64a84d`.
- Original `tjpgd.h` SHA-256: `53d1156fd9fcc97d346e5ee8c16ae39c34d71c691dcad3c524424bb91519cd24`.

The core is **TJpgDec R0.03, ChaN (2021)**. Its permissive license is retained verbatim at the beginning of `tjpgd.c`: use, modification and redistribution (including commercial products) are unrestricted, at the user's responsibility, without warranty; source redistributions must retain the copyright notice. The surrounding Espressif component uses Apache-2.0; its license is retained in `esp_jpeg.LICENSE`. Neither the Espressif whole-image wrapper nor its build/dependency files is used.

Folio supplies only `tjpgdcnf.h`: 512-byte input, grayscale output, scaling enabled, arithmetic clipping, basic Huffman decoding, no default Huffman tables. The embedding wallpaper translation unit prefixes `jd_prepare`/`jd_decomp` as `folio_wall_jd_prepare`/`folio_wall_jd_decomp`, preventing interposition on the ESP32-S3 ROM symbols. Host and firmware use the same C core, not a ROM/host split. All decoder input is additionally bounded and prevalidated by the wallpaper wrapper.
