# 真实固件界面图

这些 WebP 由 `tools/tests/run_ui_preview.sh` 使用当前固件的真实 UI 代码渲染，不是手写 HTML 示意，也不是实机照片。

- `home.webp`：首页、继续阅读、阅读统计；
- `library.webp`：书架；
- `reading.webp`：正文阅读；
- `settings.webp`：传书与系统设置。

书籍、封面、正文和统计内容均为合成测试夹具，不包含用户真实书籍、阅读记录、照片、账号或凭据。生成源为 `/tmp` 中的主机预览输出，生成命令见 `tools/tests/run_ui_preview.sh`。更新固件 UI 后应重新生成，不能把旧图当作新版本证据。
