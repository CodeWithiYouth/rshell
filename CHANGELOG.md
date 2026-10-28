# 更新日志

本文件记录 rshell 每个版本的变化。发布流程会自动把对应版本的段落写进 GitHub Release 说明。

## 未发布

### 新增

- **标题栏 AI 按钮**：菜单按钮左侧新增星芒图标按钮，点一下打开/关闭右侧 AI 助手面板；面板打开时按钮高亮成主色，一眼看得出状态。图标是纯矢量绘制，不依赖字体。

### 改进

- 二进制体积：MSVC 默认改用 `/Os`（终端解析、SFTP 传输缓冲、字符串工具仍用 `/O2`）。
  `rshell-gui.exe` 1777 → 1457 KB，`rshell.exe` 899 → 765 KB；终端回放吞吐实测无变化。

## v1.2.2 — 2026-10-28

### 新增

- **AI 回答支持 Markdown 渲染**：标题、粗体/斜体、行内代码、列表、引用、代码块、表格（含列对齐）。
- **SSH 经代理连接**：支持 SOCKS5（含用户名/密码认证）和 HTTP CONNECT（含 Basic 认证）；终端、SFTP 面板和 AI 工具一起生效。
  - GUI 在「会话管理 → 代理」填写，或 CLI 用 `--proxy socks5://[用户:口令@]主机:端口`。
  - 代理解析失败或连不上时直接报错，不会悄悄退回直连。
- **保持活动状态**：会话可选定时发送 SSH 层的 `keepalive@openssh.com`，默认开启、间隔 30 秒（5~3600 秒可调）；CLI 对应 `--keepalive`。

### 改进

- 会话管理对话框：代理、保活都加了占位提示；`用户/密码` 标签改为在各自输入框前面，不再有歧义。
- 对话框按屏幕工作区自动收缩（高 DPI + 小屏时行高等比压缩），不会再被屏幕边缘裁掉。
- 设置页的代理地址支持 `socks5h://` 和裸 `host:port`（默认按 SOCKS5）。

## v1.2.1 — 2026-10-28

### 新增

- **服务商预设**：OpenCode Go / Command Code / Cline，点一下自动填好 API 地址。
- **获取模型**：从服务商 `/models` 拉实时列表，下拉选择；自动过滤不支持 `/chat/completions` 的模型（例如 Command Code 的 Claude 系列）。

### 修复

- OpenCode Go 需要 `x-opencode-session` 请求头，之前会返回 HTTP 400。
- 设置页第二次打开时输入框不显示。
- 保存设置后空状态仍显示「还没有配置 API Key」；保险库锁定时现在会提示「保险库已锁定」并提供解锁按钮。
- AI 面板头部拥挤：改为两行（标题/操作 + 主机/状态），并新增关闭按钮。

## v1.2.0 — 2026-10-28

### 新增

- **AI 助手（桌面版）**：停在会话右侧的面板，Ctrl+J 或菜单打开，可切换全屏；支持 OpenAI 兼容接口。
- 工具能力：列目录、读文件（免审批）、执行命令、写文件（必须内联确认）。
- 安全策略：读路径黑名单（私钥、云凭据、`/etc/shadow` 等）、回灌模型前脱敏（PEM、AKIA、`sk-`、JWT、`Bearer` 等）。
- 流式输出、审批卡片、停止任务；每个会话独立对话。
- 命令行版不包含 AI 功能，体积保持不变。

### 改进

- 终端上方重复的会话名标题栏移除（标签栏已显示会话名）。
- 配色常量抽到 `ui_theme.h`，供 AI 面板复用。

## v1.1.2

- 修复 Bash 集成把初始化命令写进用户 shell 历史的问题。

## v1.1.1

- 修复图形探针在 TUI 退出后文本错乱。

## v1.1.0

- 会话标签页；更可靠的 shell 启动流程。

## v1.0.2

- 修复中文输入法相关问题；界面文案与状态提示更清晰。

## v1.0.1

- 首个公开版本。

[Unreleased]: https://github.com/CodeWithiYouth/rshell/compare/v1.2.2...HEAD
[v1.2.2]: https://github.com/CodeWithiYouth/rshell/compare/v1.2.1...v1.2.2
[v1.2.1]: https://github.com/CodeWithiYouth/rshell/compare/v1.2.0...v1.2.1
[v1.2.0]: https://github.com/CodeWithiYouth/rshell/compare/v1.1.2...v1.2.0
