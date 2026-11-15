# 页间 Folio 静态介绍页

介绍页只讲 Folio 是什么、长什么样怎么装，并提供一键刷机。自研站点 MIT；第三方依赖保留各自许可。Astro 5.18.2 + TypeScript 7.0.2，静态输出到 [dist](<dist/>)，发布基路径 `/folio/`。

页面只有一份自托管脚本（刷机 UI）和一份 CSS，没有 CDN、远程字体、后端、账号或分析服务；CSP 只放行本域脚本，`connect-src` 仅本站清单域与备用镜像。构建产出的 HTML 约 7 KB。

**一键刷机**（[quick.ts](<src/installer/quick.ts>)）的顺序：校验签名清单 → 下载并校验四段镜像 → 核对芯片/料号/分区布局 →（可选）读整片 16 MiB 备份 → 写入四段并逐段回读。「顺手备份 16 MiB」默认勾选，勾上会在写入前自动下载备份并保留一个手动保存链接；取消勾选就是纯一键。它不改分区表、不写 NVS 与 storage；因此只接受已兼容布局，原厂四分区和未知布局一律拒绝。页面加载时静默检查清单，没有发布或校验失败时按钮与备份开关都保持禁用。

旧的逐步安装向导（备份、证据下载、迁移授权、四步状态机）仍保留在 [src/installer](<src/installer/>) 里并有单测，但页面不再引用它。若以后恢复那条更重的路径，约束见下一节。

## 本机构建

必须用 DSH bundled Node 启动 pnpm，不能依赖机器上的旧 Node。以下从 `site` 工作目录执行：

```sh
export NODE="$HOME/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/node/bin/node"
export PNPM="$HOME/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/pnpm/bin/pnpm.mjs"
export PATH="$(dirname "$NODE"):$PATH"
export ASTRO_TELEMETRY_DISABLED=1
"$NODE" "$PNPM" install --frozen-lockfile --store-dir "$PWD/.pnpm-store"
"$NODE" "$PNPM" test
"$NODE" "$PNPM" run check
"$NODE" "$PNPM" run build
"$NODE" "$PNPM" run test:browser
```

通用 CI：Node ≥22.23.4、pnpm 11.28.5；执行 `pnpm install --frozen-lockfile && pnpm test && pnpm run check && pnpm run build`。构建任务同步生成浏览器依赖的完整许可文本。

[配置](<pnpm-workspace.yaml>) 将 store/cache 放在站点内，并固定 Rollup 的 WASM 实现：本机 bundled Node 受 macOS Library Validation 限制，不能加载其他 Team ID 的原生 binding。没有重签 Node、关闭系统安全功能或安装全局软件。未使用的 sharp 安装脚本明确禁用；本站没有需要 sharp 的图片处理。pnpm 11 不读取旧 `.npmrc` 的这些配置，不能只依赖它来限制缓存位置。

## 发布接口

- 用户只使用稳定 USB 指针：`https://qmqsu.github.io/folio/channels/stable-usb.json`；preview 仅用于内部签名候选，不在网页或设备设置中提供选择。
- 版本资源：`https://qmqsu.github.io/folio/firmware/<version>/*.bin`
- **不放示例生产清单、不捏造版本、不内置测试密钥。** 用户点击检查后才请求公共清单。404 为“未发布”，网络或签名错误为“不可用”，连接/写入均禁用。
- 国内主源不可达时，仅在网络传输失败（TypeError）后尝试备用 CDN `https://gcore.jsdelivr.net/gh/qmqsu/folio@folio-release-assets/`。只映射相同路径，禁止重定向；404、地址变化、超长、签名或 SHA 错误不切换。发布器在 Pages 验证成功后，将相同固件/清单写入该分支并逐文件校验。
- 发布流水线可将渠道指针和固件合并至 [静态产物](<dist/>) 后部署。本站不生成固件、生产密钥或签名。
- 固定公钥 SEC1 SHA-256：`46d906373e6ad72238432ccacab0a0d56de0c41a99a651dfb50257a718c6f928`；key id：`folio-release-2026-3`。这是 lead 提供的生产**公钥**。

详见 [安全契约](<docs/SECURITY.md>)。OTA `kind:'ota'` 清单绝不能用于 USB。

浏览器完整依赖许可在 [公开 notice](<public/third-party-licenses.txt>)。RAM stub 不是笼统继承站点 MIT：[确切来源与哈希](<public/licenses/stub-provenance.txt>) 核实为 esp-flasher-stub v1.2.2 / commit `23959b780454adf885916d42f2274ec648e96a94`，子模块 `esp-stub-lib@d61983fa4d1f66b9c088608c1f702ad877121279`，选择其双许可中的 [MIT 选项全文](<public/licenses/esp-flasher-stub-MIT.txt>)；esptool-js 本身仍是 Apache-2.0。实际S3 stub运行字段已与上游发布文件逐一比较相同，构建脚本检查固定包哈希，升级依赖需重审。

## 逐步安装器源码（页面不再引用）

以下是保留的安装器源码所实现的约束，供以后再上线时对照；当前页面不提供刷写入口。

1. 桌面 Chrome/Edge + HTTPS + Web Serial + WebCrypto。无 JS、手机、Safari/Firefox 均不允许连接/烧录；普通页面仍可读。
2. 用户根据实物或购买资料确认 **RDP-G01-W**，不能仅凭 S3 或 USB VID/PID。随后读取芯片、Flash RDID、容量和安全信息。
3. 目前运行时 Flash/PSRAM 120MHz profile 为实验配置：只允许 **Zbit ZB25VQ128**，JEDEC `5e 40 18`，esptool 寄存器整数 `0x18405e`。其他 16MiB 厂牌可备份但拒写。Secure Boot/加密/安全下载模式不支持。
4. 实际分区表（含 MD5 和完整 4KiB 扇区尾部）须精确匹配 Folio v2，或 [固定官方提交的四分区表](<https://github.com/MindReset/read_pico_firmware/blob/6a247cc94420b85c650b32a6b2c063a360070eee/partitions_16M.csv>)。后者须单独授权新增高位 OTA 分区；不是对所有原厂、ThingsOS 或第三方固件的兼容承诺。未知布局只允许完整备份。
5. 下载校验四段镜像 → 完整读取 16MiB → 生成 SHA-256 证据 → 用户分别保存备份/证据并确认落盘 → 明确同意切 factory（迁移还需另行同意）→ 写入/逐段回读 SHA → 验证 NVS/PHY 保留区 → 关闭串口 → 才报告完成。
6. 无 erase-all、merged、字体资源写入。原生 USB 复位不自动重试其他策略。成功后不自动重启设备，提示用户按硬件说明重启。

本轮已在私有调试设备上完成完整 Flash 备份和 app-only 安装／回读，**但网页四段安装流程尚未完成硬件验收**。页面没有把静态构建、模拟测试或浏览器截图当作实机刷机成功。备份下载事件不能证明落盘，必须由用户在电脑中核对。温度、120MHz 稳定性、原生 USB 复位/重枚举与真实掉电恢复仍需后续独立硬件验证。

## 浏览器验证

[浏览器脚本](<tests/browser.mjs>) 启动一个只服务构建产物的临时 loopback 服务器，用现有 Microsoft Edge headless 与**独立站点测试 profile**截图；不会使用个人浏览器 profile。可通过 `FOLIO_TEST_BROWSER` 指定现有 Chromium 类浏览器可执行文件。不安装浏览器，不保留测试服务器。

- 真实布局/渲染：1440、1024、768、375、320px 与 844×390 横屏，无横向溢出；全页只有一个 `h1`，五个区块都在。
- 图片真的解码：逐张检查 `naturalWidth`，不留空白框。
- 真实键盘：跳到正文、可见焦点；首屏与导航的「一键刷机」都落到 `#flash`。
- 刷机入口受控：清单 404 时按钮与备份开关都禁用、不弹串口；伪造签名信封被拒后仍禁用；备份开关默认勾选。
- 旧逐步向导的节点（`#connect-backup`、`#install-button` 等）必须查不到。
- 只发本域与清单域的请求；页面恰好一份自托管模块脚本。
- 无 JavaScript：标题、说明、六张功能卡照常可读，刷机按钮保持禁用。
- 证据写入 [浏览器报告](<../artifacts/firmware-0832/site/browser-report.json>) 与同目录 PNG。

## 代码边界

- [页面](<src/pages/index.astro>)、[样式](<src/styles/global.css>)：纸色 + 墨色 + 单一朱红强调色；首屏、三屏界面、六张功能卡、三条安装路线、关于与页脚；截图来自固件 UI 代码渲染的合成示例，不使用私人书籍、照片或阅读记录。
- [签名与镜像](<src/installer/manifest.ts>)：固定信任锚、严格 payload/四段地址与长度、URL、SHA、应用身份。
- [分区](<src/installer/partition.ts>)：Folio/固定官方布局识别、MD5 与擦除尾部。
- [状态机](<src/installer/controller.ts>)：下载、备份、确认、写入、回读、取消和关闭。
- [串口适配](<src/installer/serial.ts>)：固定 esptool-js，READ_FLASH 帧/ACK/尾 MD5 校验，不打印 MAC/原始 Flash。
- [一键刷机](<src/installer/quick.ts>)、[页面接线](<src/installer/quick-ui.ts>)：签名校验、四段写入与回读、芯片/料号/布局门禁；不含备份。**当前页面只用这一层**。
- [交互层](<src/installer/ui.ts>)：旧向导的能力检测、串口手势、进度与中断提示。页面不再导入。
- [单元测试](<tests/>)：仅临时测试密钥、人工构造镜像/内存 Flash 和协议替身，不接触生产私钥或硬件。

首次 pnpm 依赖安装因 pnpm 11 忽略旧 `.npmrc`，曾意外使用机器默认 package store；已报告 lead，并改用站点配置与显式 `--store-dir`。未读取/清理外部缓存，不影响源码与发布产物；后续 frozen install 已在站点 store 验证通过。
