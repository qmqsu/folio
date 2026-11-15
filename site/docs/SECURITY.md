# USB 安装安全契约与威胁边界

## 签名信封

```json
{
  "key_id": "folio-release-2026-3",
  "payload": "base64 of original UTF-8 JSON bytes",
  "signature": "base64 of raw 64-byte P-256 r||s signature"
}
```

WebCrypto ECDSA-P256 / SHA-256 对 **原始 payload 字节**验签，不重新序列化。签名通过后才解析 JSON；只接受固定 key id 与代码内固定公钥。清单不能提供或替换公钥。不接受 DER 签名、任意下载地址、公钥轮换字段或额外写入段。生产从不传入测试专用 key 参数；测试临时生成独立密钥，不落盘。

原始网络信封（包括空白）最多4096字节，超过即终止下载；解码payload也受4096字节保守上限限制。payload 必须包含以下字段，只额外允许可选的 `notes` 纯文本（最多512个UTF-8字节，不作为HTML显示）：

- `schema:1`, `kind:'usb'`, `project:'Folio'`, `chip:'esp32s3'`
- `hardware:'RDP-G01-W'`, `layout:'folio-16m-factory-storage-ota-v2'`
- `channel:'preview'|'stable'`，与所选通道一致
- `version`（最多31字符、语义化数字版本/可选预发布后缀）、`sequence`（整数 uint32）
- `flash_size:16777216`
- 四个 `parts:[{url,offset,size,sha256}]`，不接受扩展字段

四个URL必须逐字等于固定HTTPS源 `https://qmqsu.github.io/folio/firmware/<签名version>/` 加对应地址的固定文件名：`bootloader.bin`、`partition-table.bin`、`folio.bin`、`ota_data_initial.bin`。不能跨版本、增加子目录、换文件名或混用地址；无 query/fragment/userinfo/路径转义/`..`，下载禁止重定向，禁带 credentials/referrer。下载流有硬长度限制，实际长度和 SHA-256 必须都匹配。序列高水位只保存在浏览器 localStorage 的非私密数值，不能宣称芯片防回滚：清浏览器数据、换电脑会重置它。

**备用传输源**：为解决家庭宽带无法直接访问 GitHub 的情况，主源仅在发生网络 TypeError 时回退到 `https://gcore.jsdelivr.net/gh/qmqsu/folio@folio-release-assets/` 下的同路径文件。只允许映射主源 `/folio/` 下的路径；404、用户取消、地址变化、超长、签名或哈希错误均不重试。备用源只改变传输位置，不改变签名 payload、版本、镜像地址或校验值，收到字节仍需通过原有全部检查。镜像发布在 Pages 成功核验后执行，并逐文件回读比对。

## 四段白名单

| 地址 | 最大字节数 | 内容检查 |
| --- | ---: | --- |
| 0 | 32768 | ESP 镜像头，chip_id=9，DIO/16MB/80MHz |
| 0x8000 | 4096 | 32字节对齐、精确 Folio v2 分区、MD5、无加密flags、擦除尾部 |
| 0x10000 | 3145728 | ESP32-S3 头，app_desc魔数、Folio项目名、版本与签名一致 |
| 0x910000 | 8192 | 恰好8192字节全FF，明确说明切factory |

检查实际镜像长度、擦除扇区范围与非重叠；签名不取消这些本地限制。串口适配层再次检查地址、长度、哈希和 Flash 厂牌，`eraseAll:false`、压缩写入，三个 flash 参数均为 `keep`，不改变签名镜像头。内部资源 `0x410000..0x90ffff`、NVS、phy_init 与备用应用槽不进入默认写入集合。无任意文件选择/地址输入/merged/整片擦除接口。

## 硬件与原始布局

物理型号必须由用户独立确认，串口探测不能证明整机是 RDP-G01-W。读取 S3、16MiB 与 JEDEC；当前实验性 Flash/PSRAM120MHz profile 仅允许 Zbit ZB25VQ128（RDID little-endian integer `0x18405e`）。未知厂牌/布局只能备份，不能写入。加密、Secure Boot、Secure Download 拒绝支持，不解锁、不擦除 eFuse。

实际读取分区表整个4096字节，验证条目/MD5/尾FF（包括3072字节镜像之外的1024字节，不忽略已有内容）。允许：

1. Folio v2 全部七项精确匹配。
2. [官方固件固定提交](<https://github.com/MindReset/read_pico_firmware/blob/6a247cc94420b85c650b32a6b2c063a360070eee/partitions_16M.csv>) 的**恰好四项**：nvs `(1,2,0x9000,0x5000)`、phy_init `(1,1,0xe000,0x1000)`、factory `(0,0,0x10000,0x400000)`、storage `(1,0x81,0x410000,0x500000)`。无其他项/重复项/加密标志。迁移增加高位 OTA 分区，单独用户确认；不迁移旧软件内部数据。

只接受布局等价关系，绝不根据“官方”“S3”等标签猜测 ThingsOS 或其他出厂系统兼容。上述公开源已经独立读取核对。未来加入原始布局必须附可追溯源、精确布局匹配、NVS/资源保护分析和独立迁移授权，不能做通用“忽略布局”开关。

## 备份、写入与取消

- 获得签名、下载四个合法镜像后才打开串口。串口选择器本身在用户点击时请求，以保留浏览器 transient activation；不调用 `getPorts()` 自动重连。
- 完整读取16MiB，使用带ACK的 READ_FLASH 数据帧并消费/校验结尾MD5。esptool-js0.7.1自带 readFlash 没有消费该帧，所以本站独立实现有界流式协议，避免残留帧污染下一条命令。MD5只做传输完整性；发布认证与最终证据为ECDSA/SHA-256。
- 全备份分区扇区须与先前读取一致；长度不完整或传输校验失败不产生可用备份证据。
- 备份二进制和元数据JSON分别由用户下载；网站明确无法确认浏览器真的落盘。两个下载动作、用户确认实际找到文件、正确SHA证据三者齐备后才可写。
- 迁移授权与factory切换授权均不默认勾选。Font storage独立资源首版不写，不把普通安装授权扩成覆盖授权。
- 写前重新检查所有镜像和实际分区表。四段顺序写入，每段完整回读SHA；最后读取NVS/PHY保留区与备份逐字节比较。
- 任何写入/回读/保留区/关闭串口失败不能成功。进度是实收/实发字节，压缩写入显示传输进度，不暗示写入校验已完成。
- 取消传递AbortSignal并关闭串口；已写字节无法撤销。错误/取消提示恢复指南，不自动重放或恢复。关闭幂等，未写入也在失败路径关闭已打开端口。
- 会话占用串口或操作中注册beforeunload、离站确认；pagehide尽力关闭。浏览器强制退出、系统掉电无法保证提示/清理；不宣称自动回滚。
- 写后不自动复位，串口关闭后由用户按硬件说明重启；成功仅指四段SHA及NVS检查成功，不等于显示、触摸、温升、功耗或云端功能验收。

## 隐私与未验证边界

不扫描用户目录、不访问私人证据，不把原始串口输出、MAC、Wi-Fi凭据、阅读内容写入日志。完整Flash备份仅在浏览器内存和用户主动下载处；Blob URLs不上传。没有服务端、分析像素、Cookie收集或浏览器持久化备份。页面CSP限制脚本/样式/字体到本站，公共下载到固定源。

威胁假设：网站代码与固定公钥通过可信HTTPS交付，签名私钥未泄露。浏览器扩展、被控制的同源网页、操作系统恶意软件可读取备份；网页不能保护被攻陷的宿主。用户误认实体型号不是密码学能排除的风险。

本轮只完成主机状态机/密码学/协议替身测试与真实浏览器渲染；**没有真实串口、USB写入、原生USB重枚举、掉电恢复或长时温升验收**。稳定发布前必须针对实际硬件补足这些证据。
