# 页间 Folio

给小纸 Pico 的原生墨水屏阅读器固件。读书、整理书架、换一张关机壁纸；联网只在需要时发生。

- **官网与安装向导**：<https://qmqsu.github.io/folio/>（首次 Pages 部署完成后可访问）
- **源码与发行记录**：<https://github.com/qmqsu/folio>
- **当前源码**：`0.8.37`。官网、USB 安装向导与签名在线更新处于联调阶段，尚不能视为通过实机验收的稳定版。没有已发布签名清单时，网站会禁用安装；不会用示例固件占位。

## 可以做什么

- TXT / EPUB 原生阅读，中文排版、目录、书签、续读和阅读统计。
- 书架搜索、排序、真实封面；字体、字号、行距、页边距可调整。
- USB 供电之外可离线阅读；Wi-Fi 传书支持批量上传书籍、图片与字体。
- 时间锁屏与自定义关机壁纸。壁纸最高 32 MiB / 3200 万像素 / 最长边 8192；书籍封面的限制单独计算。
- 微信读书实验功能：登录、书架、下载后离线阅读与手动同步。使用非公开协议，可能随上游变化失效；只处理当前账号有权访问的内容，不是腾讯官方客户端。
- 新版设置页提供手动检查、下载和确认安装在线更新；保留 TF 卡文件升级，不要求以后每次都接电脑刷机。

EPUB 主要处理文字、目录和封面，暂不支持正文图片布局、CSS、固定排版、DRM。TF 卡书架最多显示 128 本；支持 FAT32。不要把实验功能或模拟测试理解为所有网络、硬件与故障场景均已验证。

## 只适用于明确匹配的硬件

当前目标是 **RDP-G01-W（小纸 Pico）、ESP32-S3、16 MiB Flash、8 MiB PSRAM、4.7 英寸 1216 × 684 / 16 灰阶屏**。

当前运行配置使用实验性 **120 MHz Flash / PSRAM**，已知适配 Zbit ZB25VQ128（JEDEC `0x5E4018`）。镜像启动头的 80 MHz 与应用运行时的 120 MHz 不是同一参数。温升和长时稳定性没有因此得到保证；不支持不同板型、未知 Flash 型号或改装存储芯片。

浏览器安装只接受经过完整校验的 Folio 布局，或[明确核对过的官方四分区布局](<https://github.com/MindReset/read_pico_firmware/blob/6a247cc94420b85c650b32a6b2c063a360070eee/partitions_16M.csv>)。这**不等于支持所有出厂固件**。ThingsOS、其他第三方布局或未知分区只能备份，不会自动迁移。

## 首次安装

桌面版 Chrome / Edge 等支持 Web Serial 的浏览器，通过 HTTPS 打开官网。Safari、Firefox 和多数移动浏览器不支持此刷机通路。

1. 手动核对设备标识，用 USB 数据线连接；关闭占用同一串口的监视器。
2. 选择已发布的通道，核对版本与风险；向导先验证签名并获取四段镜像。
3. 选择串口，读取芯片与分区信息，完成 **16 MiB 全 Flash 备份**、SHA-256 校验及下载保存确认。
4. 确认启动槽切换后才允许写入，官方布局迁移另需确认；逐段读回和保留区检查通过后才显示成功。

默认只写 bootloader、分区表、factory 应用和 OTA 初始化记录。**不执行 erase-all，不默认写 NVS 或 storage 字体区，不使用 merged 镜像。** USB 重装会重置 OTA 启动选择；备份不能省略。

备份可能包含 Wi-Fi、账号信息和其他隐私，请私密保存在自己电脑中，不要上传到 Issues 或公共网盘。网站不把设备备份上传给服务器。TF 卡与内部 Flash 分离，重要书籍和阅读进度仍应单独备份。

详情见[构建与刷机](<docs/刷机说明.md>)。此向导的主机模拟通过不代表真实浏览器 USB 安装已经验收。

## 以后怎样更新

首次安装带在线更新功能的 Folio 后，在设备的固件更新页手动检查。默认稳定通道；主动选择测试通道才会看到预发布版本。

- 通过已保存的 Wi-Fi 连接 GitHub Pages；需要正确的系统时间和可用 TF 卡。
- 验证内置发布公钥的 P-256 / SHA-256 签名、硬件/布局/通道、版本序号和镜像 SHA-256。
- 下载成功后再次确认安装；安装前要求有效电量至少 20%，并持续检查取消和供电状态。
- 写入未运行的 OTA 槽，完整验证成功后才切换启动目标；应用更新不写 NVS、factory 或 storage。
- 不后台自动安装。GitHub 不可达时保留 TF 卡升级路径，不通过关闭 HTTPS 校验来绕过故障。

手工 TF 更新使用 `update/folio.bin` 和 `update/folio.bin.sha256`。该离线通路的 SHA-256 是完整性校验，不是发布者身份签名；只使用你信任并自行核验的镜像。它也允许所有者自行修改、构建和安装固件，不要求生产签名私钥。

在线下载使用独立临时目录，不覆盖手工更新文件。取消可能需等待 DNS 解析和网络收尾；请等任务退出，不要立即拔卡或断电。当前不会跨重启恢复候选，也不会自动删除旧的失败暂存；不要为清理暂存而删除整个 `.folio`，其中还含阅读记录。

## 书籍、图片和字体

在 TF 卡根目录放置 `books`、`fonts`、`picture`。不需要再建立 `sdcard` 文件夹；隐藏的 `.folio` 保存进度、书签、缓存及统计。

应用自带原宋体。另有三款 OFL 派生资源字体：Folio 黑体（源自更纱黑体）、Folio 楷体（源自霞鹜文楷）、Folio 圆体（源自寒蝉全圆体，另附缺字补字）。它们不属于默认 USB 或 OTA 写入内容；资源不可用时临时回退原宋体，不清除记住的选择。

独立字体资源占用既有 5 MiB storage 区。**该区可能已有数据，任何覆盖都必须重新备份并单独明确确认。** 不会因为刷应用就顺便安装字体。自备 TF 字体支持 TrueType glyf / Unicode cmap4，文件最高 6 MiB，实际可用受内存限制；不支持 TTC / CFF OTF。字体来源与许可见[许可清单](<LICENSES.md>)。

## 构建与开发

使用 ESP-IDF v6.1 / ESP32-S3 工具链。发行 CI 固定 SDK 提交和工具清单，不依赖浮动 `latest`。

```sh
. "$IDF_PATH/export.sh"
sh tools/idf.sh build
```

[构建脚本](<tools/idf.sh>)把输出放在系统临时目录，避免含中文的工程路径触发工具链问题；也可设置 `FOLIO_BUILD_DIR`。发布配置显式关闭 USB 调试，不把本地 `sdkconfig` 打包公开。

```sh
# Host tests; online/release tests require Python cryptography.
sh tools/tests/run_update_tests.sh
sh tools/tests/run_online_update_tests.sh
sh tools/tests/run_release_tests.sh
sh tools/tests/run_settings_tests.sh
sh tools/tests/run_power_tests.sh
FOLIO_PYTHON=python3 sh tools/tests/run_ui_preview.sh main/assets/folio_builtin.ttf /tmp/folio-ui-preview

# Static site; use the pinned package manager from site/package.json.
cd site
pnpm install --frozen-lockfile
pnpm test
pnpm run check
pnpm run build
```

主机测试使用真实解析/签名算法及模拟硬件；UI 截图是帧缓存，不证明墨水屏物理灰阶、残影或实际功耗。真实网络、USB、TF、断电回滚与温升必须另行验收。

## 发布流程与许可

普通 `main` 推送检查并部署网站；版本标签先产生测试版。稳定版必须使用已发布的同一份镜像，绑定源码提交与镜像哈希的实机验收记录，再经受保护环境人工审批；不会重建一个不同二进制冒充已验收版本。

第一次部署需要仓库所有者在 GitHub Pages 设置中选择 **GitHub Actions**。固件签名环境和密钥另行配置；网站介绍页不依赖签名私钥。[发布与在线更新](<docs/发布与在线更新.md>)说明配置和升级规则；源码包重建、EPDiy 替换及 GitHub 环境设置见[发行维护指南](<.github/RELEASING.md>)。

自研代码采用 [MIT](<LICENSE>)。EPDiy、官方板级组件、协议参考、字体及其他第三方代码分别保留原许可；根目录 MIT 不重新许可它们。发行材料包含对应源码、许可、构建信息及 LGPL 重链接材料。详细范围见[LICENSES](<LICENSES.md>)。没有完整材料或实机证据时，不把构建标为稳定固件。
