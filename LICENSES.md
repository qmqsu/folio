# 许可证清单

本清单随代码一并维护。2026-11-14，项目所有者确认自研代码采用[MIT许可证](<LICENSE>)；第三方组件、派生字体及其单独声明不被该许可证替代。`licenses/` 保存第三方许可证全文，原始版权与源码许可段继续保留。公开发行仍须提供对应源码、可重建/重新链接材料及字体声明，不以此清单代替每个发布版本的核查。

| 内容 | 来源 | 许可证 | 备注 |
| --- | --- | --- | --- |
| 页间 Folio 自研应用及网站代码 | 本项目 | MIT | 项目所有者于2026-11-14确认；见根目录LICENSE。下列第三方组件和字体继续单独适用各自许可，不重新许可为MIT。 |
| 网站浏览器依赖 | esptool-js 0.7.1、@noble/hashes、pako、atob-lite | Apache-2.0 / MIT / zlib，分别适用 | 完整版权、许可与固定来源见[网站第三方声明](<site/public/third-party-licenses.txt>)；锁文件固定构建依赖，构建时核对运行库版本。 |
| USB RAM flasher stub | esp-flasher-stub v1.2.2 与固定 esp-stub-lib 子模块 | Apache-2.0 OR MIT，本分发选择 MIT | ESP32-S3 运行字段已与 v1.2.2 上游 JSON 逐项核对；原版权、完整 MIT、精确提交与哈希见[网站声明](<site/public/third-party-licenses.txt>)，不是按 esptool-js 外层许可推断。 |
| Read Pico 板级支持与大多芯片驱动 | [MindReset/read_pico_firmware](https://github.com/MindReset/read_pico_firmware) | Apache-2.0 | 仅纳入独立组件；保留各组件原有 LICENSE 与版权信息。 |
| EPDiy 显示驱动 | 官方固件所用 `components/epdiy` | LGPL-3.0-or-later | 该组件单独适用其目录中的 LICENSE；发布固件前需按 LGPL 履行分发要求。 |
| TrueType 栅格化库 | Sean Barrett, `stb_truetype.h` v1.26 | MIT 或 Public Domain（Unlicense），二选一 | 原始双许可全文保留在 `main/book/vendor/stb_truetype.h` 文件末尾，并原样提取到 `licenses/stb_truetype.txt`。 |
| JPEG 封面解码 | [nothings/stb](https://github.com/nothings/stb)，`stb_image.h` v2.30 | MIT 或 Public Domain（Unlicense），二选一 | 固定提交 `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`；只启用 JPEG；保留原始双许可，防御修改记录于 `main/book/vendor/COVER-SOURCES.md`。 |
| JPEG 大图壁纸解码 | [Espressif esp_jpeg 1.3.1](https://components.espressif.com/components/espressif/esp_jpeg/versions/1.3.1/readme) 中的 ChaN TJpgDec R0.03 | ChaN宽松许可（保留版权、无担保） | 固定提交和归档哈希见[来源记录](<main/book/vendor/tjpgd/README.md>)；核心版权条款保留在源码及[独立许可副本](<licenses/TJpgDec.txt>)。未采用Espressif整图包装器；其Apache-2.0声明仍保留。主机与设备使用同一软件C核心。 |
| PNG 流式封面／壁纸解码 | [kikuchan/pngle](https://github.com/kikuchan/pngle) | MIT | 固定提交 `b1c68193f1d3f8642b3e0e095d457a828038e6fb`；源码、修改说明及上游校验值在 `main/book/vendor/`。 |
| PNG 私有解压器 | pngle 上游随附 miniz v1.15 | Public Domain | 保留原始声明；私有符号加前缀，不替换 ESP32 ROM 的 ZIP 解压器。 |
| 二维码编码 | [Project Nayuki QR Code generator v1.8.0](https://github.com/nayuki/QR-Code-generator/tree/v1.8.0/c) | MIT | 完整 C 源码与固定版本记录位于 `main/vendor/qrcodegen/`，许可位于 `licenses/qrcodegen-MIT.txt`。 |
| 微信读书 Web 协议算法及请求结构 | [danleegle/wxBook](https://github.com/danleegle/wxBook) | MIT | 部分改编 ID 编码、签名、分片算法和请求结构；未复制上游 UI 或阅读器。2026-10-28 获取的两个上游源文件按 SHA-256 固定，见下述 NOTICE；原始版权及许可保留于 `main/net/weread/LICENSE.wxBook`，根目录另附完整许可副本。 |
| 微信读书独立查询与仅位置写回参考 | [0x1abin/crossmux](https://github.com/0x1abin/crossmux/tree/6b5566c844667857acebfe565d373423cdd16e06) | MIT | 固定提交 `6b5566c844667857acebfe565d373423cdd16e06`；研究 `getProgress` 与 `enter/report` 行为，未复制 UI、阅读器或存储实现。原始许可保留于 `main/net/weread/LICENSE.CrossMux`。 |
| 微信读书 JSON 解析 | [DaveGamble/cJSON v1.7.19](https://github.com/DaveGamble/cJSON/tree/c859b25da02955fef659d658b8f324b5cde87be3) | MIT | 固定提交 `c859b25da02955fef659d658b8f324b5cde87be3`；源文件未修改，保留 `main/net/weread/vendor/cjson/LICENSE` 与 `SOURCE.txt` 中的版本/校验值。 |
| 拼音索引 | Unicode 17.0 Unihan `kMandarin` | Unicode License v3 | 默认单字普通话读音；生成脚本、源校验值与映射表随源码，完整许可 `licenses/Unicode-3.0.txt`。 |
| GBK 映射表 | 本项目生成 | 由 Python 标准库 GBK codec 生成 | 生成脚本为 `tools/gen_gbk_table.py`；不打包第三方字库或编码器。 |
| ZIP 解压 | ESP32-S3 ROM miniz，经 ESP-IDF `esp_rom` 声明调用 | 随芯片 ROM / SDK 提供 | 本项目实现 ZIP 与 EPUB 解析，未复制第三方固件阅读器；桌面回归以系统 zlib 替代 ROM 解压调用。 |
| ESP-IDF | [Espressif ESP-IDF](https://github.com/espressif/esp-idf) | Apache-2.0（含单独许可组件） | 以实际链接组件及其文件声明为准。发行工具按实际 build_components 收集 SDK 许可、版权、子模块提交并生成 license-inventory；这不是完整 SPDX/SBOM 或法律合规保证。 |
| 编译器运行时与 C 标准库 | ESP-IDF 固定工具清单指定的 Espressif GCC 15.2.0；本次配置实际使用 picolibc | GCC Runtime Library Exception 3.1 及各库原许可 | 实际配置为 `LIBC_PICOLIBC=true`、`LIBC_NEWLIB=false`。收集器按每次构建配置强制保留 GCC COPYING.RUNTIME / COPYING3，以及 picolibc 目录完整 COPYING.picolibc / COPYING.NEWLIB / COPYING.GPL2；若实际选择 newlib，则核对其 COPYING.NEWLIB。安装工具链的其它原始声明也保留，但不据此断言全部工具或库均链接进固件，不把编译器 GPL 误声明为整个自研应用的许可证。 |
| 内置 Folio 宋体 | Google Fonts / Noto Serif SC | SIL Open Font License 1.1 | 字重 500（Medium）静态子集，修改后的字体更名为 Folio Songti；版权与许可保留，完整许可见 `licenses/NotoSerifSC-OFL.txt`。上游与输出 SHA-256 见 `main/assets/folio_builtin.json`，构建脚本 `tools/build_builtin_font.py`。 |
| 内置 Folio Hei Regular 派生字体 | [Sarasa Gothic SC Regular v1.0.43](https://github.com/be5invis/Sarasa-Gothic/releases/tag/v1.0.43) | SIL Open Font License 1.1 | Renzhi Li、Inter、Adobe、Google原版权保留；裁剪／去hint及布局表／全语言主名改为Folio Hei，避开RFN `Source`；[完整许可](<main/assets/fonts/licenses/Sarasa-LICENSE.txt>)。 |
| 内置 Folio Kai Regular 派生字体 | [LXGW WenKai Regular v1.530](https://github.com/lxgw/LxgwWenKai/releases/tag/v1.530) | SIL Open Font License 1.1 | LXGW及Klee作者原版权保留；修改版主名Folio Kai，避开霞鹜／霞鶩／落霞孤鹜／落霞孤鶩／LXGW，不套用仅网页字体沿用名的例外；[完整许可及额外许可](<main/assets/fonts/licenses/LXGW-WenKai-OFL.txt>)。 |
| 内置 Folio Yuan Regular 派生字体 | [ChillRoundF Regular v3.200](https://github.com/Warren2060/ChillRound/releases/tag/v3.200) | SIL Open Font License 1.1 | 保留ChillType版权；修改版主名Folio Yuan，避开RFN ChillRoundF／ChillRoundM；[完整许可](<main/assets/fonts/licenses/ChillRound-LICENSE.txt>)。 |
| Folio Symbols Medium 微型回退 | 未改变的现有Folio Songti Medium，源自Google Noto Serif SC | SIL Open Font License 1.1 | 仅为圆体缺少的38码点及5个视觉校准字制作43码点子集；保留源轮廓／度量与Google版权，主名改为Folio Symbols；不是额外用户字体，不替换原宋体。[完整许可](<main/assets/fonts/licenses/NotoSerifSC-OFL.txt>)。 |
| TF 卡外部字体 | 用户提供 | 由字体作者指定 | 不随固件分发；用户应保留字体授权文件。 |
| 封面测试图片 | 本项目原创几何插图 | 测试素材 | 仅用于虚构测试书的预览与 PNG/JPEG 回归，不是用户书籍或下载的出版物封面，不嵌入固件。来源见 `tools/tests/fixtures/SOURCES.txt`。 |

SDK 的补充声明由[固定路径／许可段映射](<tools/release_sdk_notices.json>)记录原始行段与 SHA-256；[收集器](<tools/release_licenses.py>)保留 wpa_supplicant README、Cadence/Tensilica、SD/MMC、UBSan、FatFs 源文件声明及 TLSF 祖先完整 BSD-3-Clause，不用单个根 Apache 文本替代。TLSF 原文来自固定子模块可追溯的 [deff9ab 祖先头文件](<https://github.com/espressif/tlsf/blob/deff9ab509341f264addbd3c8ada533678591905/tlsf.h>)，当前额外的 2024 Matthew Conte 声明也保留。WiFi 与 PHY 的固定 README/Object-form 授权随各自 LICENSE 收集；SDK 提到的 FreeBSD net80211 / Sam Leffler / Errno Consulting 完整声明与 WiFi blob 的精确对应仍待核实，不能推定 PHY 同样包含它。补充收集不代表固件法律核查已经通过。

官方组件基线：`MindReset/read_pico_firmware`，提交 `6a247cc94420b85c650b32a6b2c063a360070eee`（2026-10-11）。每个拷入组件的 LICENSE 文件随源码保留；若后续增加组件，应在此表追加精确路径、版本和许可证。上游 EPDiy 子组件采用 LGPL-3.0-or-later，发布计划必须包含对应源码或可获取源码的方式。

微信读书来源及改编范围记录于 [`main/net/weread/NOTICE.sources.txt`](main/net/weread/NOTICE.sources.txt)，其中包含 wxBook 上游文件 SHA-256、CrossMux 固定提交及文件校验值；cJSON 的版本记录见 [`vendor/cjson/SOURCE.txt`](main/net/weread/vendor/cjson/SOURCE.txt)。上述 MIT 版权和许可全文随源目录及根 `licenses/` 保留。Folio 的章节缓存、UTF-16/UTF-8 精确映射、业务生命周期与时长账本为独立实现，仍属于本项目自研代码的许可范围；不因参考协议而将整个应用声明为上游项目或上游许可证下的成品。公开源码和许可保留也不表示当前真实服务互通、账号授权或对外发布核查已经完成。

0.8.30新增三款派生字体及微型回退的官方tag／固定commit／原TTF SHA-256／许可SHA-256见[固定来源清单](<main/assets/fonts/source-manifest.json>)；修改内容、全部原版权与完整OFL见[字体NOTICE](<main/assets/fonts/NOTICE.txt>)，最终字节哈希、范围及回退关系见[字体manifest](<main/assets/fonts/manifest.json>)。[字体生成器](<tools/build_font_assets.py>)保留所有非主名记录与保留字形的轮廓／advance，所有派生字体仍独立适用OFL-1.1，不改变应用代码许可证，不表示上游作者背书。原宋体保持不变；没有引入Harmony或MiSans。

源码中保留完整[字体资源镜像](<main/assets/folio_fonts.bin>)。若另行提供独立资源附件，也不得加入默认USB或OTA写入集合；必须同时提供人可读的NOTICE、4份OFL及来源清单，不能只把许可藏在Flash中。重新分发这些派生字体时保留全部声明，遵守OFL不得单独售卖字体、不得沿用RFN作为修改版主名及不得暗示原作者背书等条件。

全文文件与取用来源：

| 文件 | 来源 |
| --- | --- |
| [Apache-2.0.txt](licenses/Apache-2.0.txt) | 本地 ESP-IDF v6.1.0 的 `LICENSE`，按字节复制。 |
| [LGPL-3.0.txt](licenses/LGPL-3.0.txt) | Espressif `xtensa-esp-elf` 工具链 `esp-15.2.0_20251204` 随附的 `share/licenses/gcc/COPYING3.LIB`，按字节复制。 |
| [GPL-3.0.txt](licenses/GPL-3.0.txt) | 同一工具链的 `share/licenses/gcc/COPYING3`，按字节复制；与引用 GPL v3 条文的 LGPL v3 全文一并保留。 |
| [stb_truetype.txt](licenses/stb_truetype.txt) | 仓库内 `stb_truetype.h` v1.26 末尾的完整双许可段，保留文字与版权信息。 |
| [stb_image.txt](licenses/stb_image.txt) | 固定提交中 `stb_image.h` 末尾完整双许可，原样复制。 |
| [pngle.txt](licenses/pngle.txt) | 固定提交的 pngle `LICENSE` 全文，原样复制。 |
| [miniz-pngle.txt](licenses/miniz-pngle.txt) | pngle 随附 miniz.c 的完整原始声明。 |
| [wxBook-MIT.txt](licenses/wxBook-MIT.txt) | 按字节复制本项目保留的 `main/net/weread/LICENSE.wxBook`；上游来源和源文件快照见上述 NOTICE。 |
| [CrossMux-MIT.txt](licenses/CrossMux-MIT.txt) | 按字节复制 `main/net/weread/LICENSE.CrossMux`，保留该固定提交中的全部版权行。 |
| [cJSON-MIT.txt](licenses/cJSON-MIT.txt) | 按字节复制 cJSON v1.7.19 的 `main/net/weread/vendor/cjson/LICENSE`。 |

GNU 标准原文地址：[LGPL v3](https://www.gnu.org/licenses/lgpl-3.0.txt)、[GPL v3](https://www.gnu.org/licenses/gpl-3.0.txt)。本次使用上述已安装工具链随附的完整文本，不修改许可证内容。
