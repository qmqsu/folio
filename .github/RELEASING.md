# 从公开源码重建、替换 EPDiy 与发行

本指南依据[标签构建](<workflows/release.yml>)、[稳定提升](<workflows/promote.yml>)、[共用发布器](<workflows/publish.yml>)和[环境门禁](<../tools/release_ci.py>)编写。它描述配置和操作方法，**不表示远端已经配置、法律核查已经通过或硬件已经验收**。

## 1. 材料边界

支持的替换路径是：**完整项目对应源码 + 固定 ESP-IDF/递归子模块/工具链 + 公开 native 配置 → 源码重建 → 修改 EPDiy → 重编译、重链接**。

发行中的 `relink.tar.gz` 只是辅助 archive、linker 片段、ELF 和参考 JSON 配置，**not a standalone complete relink kit**。它不独立包含全部外部 SDK 库、工具链 runtime、链接脚本及顶层链接输入；保留符号和重定位不等于材料自足。JSON 配置是核对用的生成结果，不能当 native `sdkconfig` 输入。以下步骤不需要这些辅助对象，也不需要项目源码包带 `.git`；版本与序号由公开构建脚本派生。

工程上的重建/替换成功，**不会自动满足全部 LGPL 或其他许可证义务**。SDK notice、二进制库授权及完整第三方归属必须另外审阅；Wi-Fi net80211 的精确完整第三方归属仍未核实。[法律门禁](<release-legal.json>)不能因 CI 或重链接成功而自动改为通过。私有设备证据、旧交付包、本机配置及密钥不进入公开源包。

## 2. 从公开源包构建 debugOFF

在 Linux/macOS 的新 **Bash** 终端执行；准备 Python 3.13、Git 及 ESP-IDF 所需的系统编译依赖和网络访问。CI 使用 Python **3.13.3**；其他主机环境不承诺产物逐字节相同。SDK 固定提交为 `fff9895c82d744c7237be8847347bdd1b07c6643`（v6.1），Xtensa 工具链由该提交的工具清单固定为 `esp-15.2.0_20251204`。

先取得并验证可信发行的源包、provenance 和签名材料；不要把未验证下载当作可信输入。将 `FOLIO_SOURCE_ARCHIVE` 设为源包的绝对路径。下面创建全新 ASCII 临时工作区，不读取本机项目 `sdkconfig`，不刷写、不签名、不发布。安装下载和 Python 依赖限制在新工作区的 IDF 工具环境；不要套用他人机器的 Python 虚拟环境路径。

```bash
set -eo pipefail
: "${FOLIO_SOURCE_ARCHIVE:?Set the absolute path to the verified source.tar.gz}"
FOLIO_WORK="$(mktemp -d /tmp/folio-source.XXXXXX)"
FOLIO_WORK="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$FOLIO_WORK")"
export FOLIO_WORK
tar -xzf "$FOLIO_SOURCE_ARCHIVE" -C "$FOLIO_WORK"
export FOLIO_SOURCE="$FOLIO_WORK/folio"
test -f "$FOLIO_SOURCE/CMakeLists.txt"

unset IDF_PYTHON_ENV_PATH
export IDF_PATH="$FOLIO_WORK/esp-idf"
export IDF_TOOLS_PATH="$FOLIO_WORK/idf-tools"
IDF_COMMIT=fff9895c82d744c7237be8847347bdd1b07c6643
git init "$IDF_PATH"
git -C "$IDF_PATH" remote add origin https://github.com/espressif/esp-idf.git
git -C "$IDF_PATH" fetch --depth=1 origin "$IDF_COMMIT" refs/tags/v6.1:refs/tags/v6.1
git -C "$IDF_PATH" checkout --detach "$IDF_COMMIT"
git -C "$IDF_PATH" submodule update --init --recursive
test "$(git -C "$IDF_PATH" rev-parse HEAD)" = "$IDF_COMMIT"
test "$(git -C "$IDF_PATH" rev-parse 'v6.1^{commit}')" = "$IDF_COMMIT"
test -z "$(git -C "$IDF_PATH" status --porcelain)"
"$IDF_PATH/install.sh" esp32s3
. "$IDF_PATH/export.sh"
python -m pip install -r "$FOLIO_SOURCE/.github/release-requirements.txt"

export IDF_TARGET=esp32s3
export PYTHONDONTWRITEBYTECODE=1
export FOLIO_BUILD_DIR="$FOLIO_WORK/build"
# Keep the same value for the baseline and replacement builds; no project Git metadata is needed.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(python -c 'import time; print(int(time.time()))')}"
mkdir -p "$FOLIO_BUILD_DIR/log"
prefix_map="-ffile-prefix-map=$FOLIO_WORK=/tmp/folio-build -fdebug-prefix-map=$FOLIO_WORK=/tmp/folio-build -ffile-prefix-map=$FOLIO_SOURCE=/usr/src/folio -fdebug-prefix-map=$FOLIO_SOURCE=/usr/src/folio"
build_folio() {
    (
        cd "$FOLIO_SOURCE"
        python "$IDF_PATH/tools/idf.py" --no-ccache -B "$FOLIO_BUILD_DIR" \
            -D IDF_TARGET=esp32s3 \
            -D "SDKCONFIG=$FOLIO_BUILD_DIR/sdkconfig.release" \
            -D "SDKCONFIG_DEFAULTS=$FOLIO_SOURCE/sdkconfig.defaults;$FOLIO_SOURCE/.github/sdkconfig.release" \
            -D "CMAKE_C_FLAGS=$prefix_map" -D "CMAKE_CXX_FLAGS=$prefix_map" build
    )
}
build_folio
python - "$FOLIO_BUILD_DIR" "$IDF_PATH" <<'PY'
import hashlib, json, pathlib, sys
build, sdk = map(pathlib.Path, sys.argv[1:])
config = json.loads((build / "config/sdkconfig.json").read_text())
description = json.loads((build / "project_description.json").read_text())
assert config["FOLIO_USB_DEBUG"] is False
assert description["idf_path"] == str(sdk) and description["git_revision"] == "v6.1"
raw = (build / "Folio.bin").read_bytes()
assert len(raw) <= 3 * 1024 * 1024
print("debugOFF", len(raw), hashlib.sha256(raw).hexdigest())
PY
FOLIO_PYTHON=python sh "$FOLIO_SOURCE/tools/tests/run_release_tests.sh"
```

这里的 native 配置输入确实是[公开默认值](<../sdkconfig.defaults>)加[release 覆盖](<sdkconfig.release>)，没有复制本机配置。`build/log` 必须提前创建，不能为绕过干净 SDK 的目录创建问题而修改 SDK。SDK 需要其精确 Git 提交、子模块和 v6.1 tag；**项目源包无需 Git metadata**。需要比较特定发行时间信息时，应事先把 `SOURCE_DATE_EPOCH` 设为经核对的发行提交 Unix 时间；上面的默认值仅用于本次本地两次构建保持一致，不宣称与官方镜像逐字节一致。

## 3. 实际修改和重链接检查

在同一终端、同一 build 目录继续。先保留原始源码和完整基线，再修改副本。下面示范的标记位于真实 `epd_init` 调用路径，不是一个随后被 GC 的 unused 函数。它只是本地工程证明，不应作为正式功能变更发行。

```bash
test ! -e "$FOLIO_WORK/source-original"
test ! -e "$FOLIO_WORK/baseline-build"
cp -R "$FOLIO_SOURCE" "$FOLIO_WORK/source-original"
cp -R "$FOLIO_BUILD_DIR" "$FOLIO_WORK/baseline-build"
python - "$FOLIO_SOURCE/components/epdiy/src/epdiy.c" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
source = path.read_text()
anchor = "void epd_init(const EpdBoardDefinition* board, const EpdDisplay_t* disp, enum EpdInitOptions options) {\n"
assert source.count(anchor) == 1, "Review the changed EPDiy entrypoint before editing"
replacement = anchor + (
    "    static const char* volatile folio_relink_proof_marker;\n"
    "    folio_relink_proof_marker = \"FOLIO_EPDiy_SOURCE_REBUILD_PROOF\";\n"
)
path.write_text(source.replace(anchor, replacement, 1))
PY
build_folio
python - "$FOLIO_WORK" <<'PY'
import hashlib, pathlib, shutil, sys
work = pathlib.Path(sys.argv[1])
before, after = work / "baseline-build", work / "build"
marker = b"FOLIO_EPDiy_SOURCE_REBUILD_PROOF"
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def archives(root):
    return {p.relative_to(root).as_posix(): sha(p) for p in root.rglob("*.a")}
old, new = archives(before), archives(after)
assert old.keys() == new.keys()
changed = sorted(name for name in old if old[name] != new[name])
assert changed == ["esp-idf/epdiy/libepdiy.a"], changed
for name in ("Folio.elf", "Folio.bin"):
    assert marker not in (before / name).read_bytes()
    assert marker in (after / name).read_bytes()
    assert sha(before / name) != sha(after / name)
    print(name, "before", sha(before / name), "after", sha(after / name))
shutil.copytree(after, work / "modified-build")
print("Only EPDiy archive changed; both source/build states preserved locally.")
PY
```

只写的 volatile 指针可能产生 set-but-unused 警告，但保留的 store 和 ELF/bin 标记应仍通过检查。还可检查实际 link map 的 `read_pico_init → epd_init`、保留的 literal/字符串地址与实际对象重定位；不要只凭字符串存在就宣称完成硬件运行验证。正式替换可编辑 EPDiy 实现本身，仍应保留修改来源、许可证及对应源码。

本地构建不需要生产私钥。设备上的发布者签名 OTA 与所有者自行构建安装是不同通路；后者参见[构建与刷机](<../docs/刷机说明.md>)，必须另行确认硬件、备份和写入范围。本指南不会自动连接设备，也不准以“证明 LGPL”绕过安全校验。

## 4. GitHub 一次性配置

由仓库所有者在 Settings → Environments 配置**两个签名环境**：

| 环境 | Deployment branches and tags | 人工审阅 | Environment secret |
|---|---|---|---|
| `release-signing` | Selected branches and tags：仅添加 **Tag** 类型 `v*` | Required reviewers 添加实际维护者；**Prevent self-review 不勾选**，允许发起者亲自审阅 | `FOLIO_SIGNING_KEY_PEM` |
| `release-stable` | Selected branches and tags：仅添加 **Branch** 类型 `main` | 同上；不能以自动批准代替人工审阅 | `FOLIO_SIGNING_KEY_PEM` |

两个环境分别放置同一已受控生产 P-256 私钥的 PEM，必须对应[仓库发布公钥](<../keys/firmware-release-public.pem>)。通过环境 secret 的受控界面配置，不把它变成 repository/org 级共享 secret、不写入源码或日志、不生成临时生产密钥。允许 self-review 仅允许维护者手动批准自己发起的运行，不取消 required reviewers，也不自动提升 stable。

当前 [API 门禁](<../tools/release_ci.py#L88-L94>)要求真实存在 required reviewers 和 branch/tag restriction；它只检查限制存在，**不会替管理员验证具体模式是否正好是上述值**，也不检查 self-review 开关。因此还须人工核对具体类型和模式；不要误将 `v*` 配成 Branch。

另外，[发布器](<workflows/publish.yml>)使用独立 `github-pages` 部署环境，不放生产签名 secret。若该环境设置 ref 限制，应允许 Branch `main` 和 Tag `v*`：标签工作流即使 checkout 当前 main 网站，部署来源仍可能是 tag。

Settings → Pages → Build and deployment 的 **Source 选择 GitHub Actions**，不是 Deploy from a branch。普通 main 推送通过检查后即可部署网站介绍页；不需要签名私钥，也不应制造假固件填补尚未发布的通道。工作流已经为发布 job 声明 `contents: write`、`pages: write`、`id-token: write`；保持其它 job 最小权限，并确认仓库/组织策略允许这些 workflow 权限。这里不给所有 job 开全局写权限。

## 5. 后续发布：tag → preview → 人工 stable

1. 在 main 完成代码审阅、主机/站点检查、公开源码逐文件白名单与法律材料审阅。[源码白名单](<release-source-files.txt>)必须包含本指南；不要把私有工程证据、密钥、备份加入白名单。缺材料时保持 fail-closed；别为让工作流变绿而改法律 gate。
2. 为将要发布的实际源码设置正确版本。标签必须与[项目版本](<../CMakeLists.txt>)及构建序号一致，可先运行 `python tools/release_ci.py version v<version>` 核对格式。仅在明确决定发布后创建并推送该源码的 `v<version>` 标签，不能重用旧标签或旧序号。`-dev`/`-rc.N` 和正式版本号都先进入 preview。
3. 标签触发 build，使用固定 SDK、独立 release 配置、debugOFF 和完整材料门禁。只有 build 成功后才进入 `release-signing` 等待人工批准；签名绑定镜像和源码/许可材料。拒绝或缺密钥时应失败，不补造“成功包”。
4. 共用发布器先发布不可变 Release 材料，再原子更新完整 Pages；发布用 `folio-pages-publication` 串行锁并校验状态，不覆盖旧版本。正式版本号最初也标记为 GitHub prerelease，preview 不等于实机验收。
5. 真正验收**该已签名包中的同一镜像**。在 main 新增 `.github/release-acceptance/<version>.json`，绑定准确 source commit、app SHA-256、审核者、UTC 测试时间及全部硬件检查。检查项和拒绝条件见[验收校验器](<../tools/release_bundle.py>)与[协议说明](<../docs/发布与在线更新.md>)；单独 HTTPS 404、帧缓存截图、源码重建或主机测试均不代替完整实机验收。
6. 只可提升无 `-dev`/`-rc` 后缀的既有 preview。从 **main** 手工运行 `Promote hardware-accepted stable release`，输入原标签，在 `release-stable` 审阅同一包和真实验收记录。流程下载原离线包、**绝不重建镜像**；只签 stable 通道，原子部署后才把 GitHub Release 改为 stable。序号不能伪增以规避设备反回滚。
7. 若不可变 Release 已成功而 Pages 后续失败，从 main 运行[恢复工作流](<workflows/recover-release.yml>)并输入原标签，恢复原签名字节；不重新签名、不重建、不覆盖资产。若权威状态与线上快照不一致，按明确的失败信息人工核对，不能把错误当成首次空站点。

普通 main push 和 pull request 的实际入口是[Checks](<workflows/check.yml>)：PR 不部署、不拿生产签名 secret；main 检查通过后更新当前网站并保留旧固件与通道。详细签名协议、单调序号、四段写入限制和完整验收项见[发布与在线更新](<../docs/发布与在线更新.md>)。
