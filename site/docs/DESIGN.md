# 视觉与交互设计

已调用 `ui-ux-pro-max`。先查询 `reading editorial monochrome minimal --design-system`，得到 Swiss Modernism / 栅格、留白和清晰层级；自动配色偏棕琥珀、推荐评价轮播不适合本项目，因此未照搬。进一步用 `minimal editorial monochrome --domain style` 命中 **Minimalism & Swiss Style**（黑白、高对比、功能优先）。用 `static client script accessibility --stack astro` 核对静态默认、无JS回退。未导入远程字体或GSAP。

## 本项目选择

- 白纸/墨色：paper `#f8f7f3`、ink `#22221f`、muted `#62625c`、浅灰分区。无彩色渐变、玻璃效果和装饰性动效。
- 书刊结构：编号栏目、细分隔线、左文右图Hero、原生details折叠长风险说明。
- 标题使用本机宋体/serif栈，英文Folio为Georgia；正文系统sans。完全没有运行时字体下载。
- 原创HTML/CSS阅读器示意与原创短句；明确“非实机照片或硬件验收”。未使用任何真实书籍、设备截图、头像或个人姓名。
- 首页按 关于 → 功能 → 安装 → 更新 → 帮助恢复 → 许可。Hero主按钮先到安装说明，不直接触发USB。
- 功能边界：微信读书标“实验”，不宣传PDF/固定版式阅读，不把独立三字体当作默认安装。
- 安装流程渐进披露：能力/签名 → 实物确认 → 备份 → 保存证据 → 授权 → 写入/回读。危险按钮原生disabled，不仅改变颜色。
- 原生按钮、select、checkbox、summary；label明确，44px级操作区，3px键盘焦点和跳过导航链接；状态为单一`role=status / aria-live=polite / aria-atomic=true`。
- 进度来自实际字节，250ms节流播报；无虚假计时百分比。下载100%不会触发成功。
- 窄屏重排，不隐藏关键风险文本；支持 reduced-motion，允许缩放，没有自定义手势或轮播。
- 只有浅色主题，明确 `color-scheme:light`，不声称测试了另一套深色视觉。

## 已做检查

真实Edge headless完成1440、1024、768、375、320与844×390视口，无横向溢出；键盘skip-link与焦点、无JS回退通过。人工查看桌面/手机Hero与未发布安装器截图，留白、排版和禁用态符合上述方向。浏览器网络状态是测试夹具，USB被明确禁用；不是硬件安装演示。

文件：[页面](<../src/pages/index.astro>) · [样式](<../src/styles/global.css>) · [浏览器测试](<../tests/browser.mjs>)。
