# rshell

rshell 是一款 Windows SSH 客户端，也可以在命令行使用。连接远程主机后，可以在终端执行命令、用 SFTP 浏览和传输文件；桌面版还内置了可以执行运维操作的 AI 助手。

它不需要注册账号，也不依赖浏览器或额外的图形界面运行库。SSH 账号、私钥和本机保存的密码均由你在本机管理。

## 下载

**Windows 10/11，64 位，按需下载：**

- **[桌面版 rshell-gui.exe](https://github.com/CodeWithiYouth/rshell/releases/download/v1.2.6/rshell-gui.exe)**：包含 SSH 终端、会话管理和 SFTP 文件面板，大多数用户选这个。
- **[命令行版 rshell.exe](https://github.com/CodeWithiYouth/rshell/releases/download/v1.2.6/rshell.exe)**：在 PowerShell / CMD 或脚本中使用。

两个程序可独立运行，下载需要的一个即可，无需解压或安装。SHA-256 校验值见 [v1.2.6 发布说明](https://github.com/CodeWithiYouth/rshell/releases/tag/v1.2.6)。

> Windows 7 SP1 有兼容构建方式，但当前下载包面向 Windows 10/11。详见[高级构建说明](docs/TECHNICAL.md)。

## 第一次连接

1. 下载并运行 `rshell-gui.exe`。
2. 按 **Ctrl+N** 添加会话，填写会话名称、主机、用户名和 SSH 端口（默认 22）。可选填私钥路径和备注。
3. 保存会话，然后点「连接」。密码登录时输入密码；第一次连接新主机时，先核对屏幕上的主机指纹，再选择是否信任。
4. 连接成功后，右侧是 SSH 终端，左侧可浏览远端文件。

文件操作在 SFTP 面板中右键选择。你也可以将文件拖入面板上传，或把远端文件拖到其他应用中下载。SFTP 使用独立连接；若服务器限制并发 SSH 会话，终端可能可用而文件面板无法连接。

## 常用操作

- **连接管理：** Ctrl+O 打开会话列表；Ctrl+N 新建会话。
- **多标签会话：** 点击顶部 `＋` 或按 Ctrl+T 打开新标签；连接另一台主机会保留已有连接。
- **切换与关闭：** Ctrl+Tab / Ctrl+Shift+Tab 切换标签；点击标签上的 `×`、鼠标中键或 Ctrl+Shift+W 关闭当前标签。
- **远端路径：** 点 SFTP 面板顶部的路径框，输入路径并回车；Ctrl+L 也可打开路径输入。
- **搜索终端输出：** Ctrl+Shift+F，然后按 Enter / Shift+Enter 浏览匹配项。
- **断线重连：** Ctrl+Shift+R。
- **保持活动状态：** 会话管理里勾选「保活」并填间隔（默认 30 秒），空闲时定时发送 SSH 保活消息，避免 NAT/防火墙把连接断开。
- **AI 助手：** 标题栏右上角的星芒按钮、Ctrl+J，或菜单 →「AI 助手」，在当前会话右侧展开（面板打开时按钮高亮）；面板内可切换到全屏。
- **终端字号：** Ctrl++ 放大、Ctrl+- 缩小、Ctrl+0 重置。
- **复制与粘贴：** 选中终端文本自动复制；Ctrl+V 粘贴。没有选中文本时，Ctrl+C 会向远端发送中断信号。

## AI 助手

桌面版内置了 AI 助手（命令行版不包含），可以让你用自然语言排查服务器问题。它连接的是**当前会话的主机**，走左侧文件面板那条独立 SSH 连接，不会打断终端。

1. 先连上一台主机（文件面板就绪）。
2. 按 **Ctrl+J**、点标题栏的星芒按钮，或菜单 →「AI 助手」打开面板。
3. 点「设置」，选一个服务商预设（OpenCode Go / Command Code / Cline），地址会自动填好；填 API Key 后点「获取模型」，从列表里挑一个，保存即可。自定义服务商可以手填地址，或用底下这些常见地址：
   - OpenAI：`https://api.openai.com/v1`
   - DeepSeek：`https://api.deepseek.com/v1`
   - 本地 Ollama：`http://127.0.0.1:11434/v1`（仅本机允许明文 HTTP）
   - OpenCode Go：`https://opencode.ai/inference/go/openai/v1`，模型填 `kimi-k3` / `deepseek-v4.1-flash` 这类不带前缀的 ID
4. 直接提问，例如「磁盘怎么满了」「看看 nginx 为什么返回 502」。

回答支持基础的 Markdown 渲染：标题、列表、引用、代码块和表格都会按格式显示。

**权限模型：**

- **读操作**（列目录、读文件）直接执行，结果会做凭据脱敏；私钥、云凭据、密码库等路径会被直接拒绝。
- **写操作**（执行命令、写文件）必须先在面板里确认，卡片会显示完整命令或文件内容，点「允许」后才执行。
- 一次只会有一个待确认操作；生成过程中点「停止」会终止整轮任务。

API Key 保存在本机主密码保险库，不会写进配置文件；每次启动需要解锁保险库后才能使用。

## 密码与隐私

rshell 不会把连接密码写进会话列表。若选择记住密码，密码会保存到本机的主密码保险库；你可以不保存，让密码只在当前进程内存中使用。保险库可用主密码解锁，也可选择配置 TPM 快捷解锁。

首次连接时，主机指纹用于确认服务器身份。请通过可信方式核对指纹，避免误信任伪装主机。

## 命令行示例

在 PowerShell 中进入 `rshell.exe` 所在目录，运行：

```powershell
.\rshell.exe exec user@host "uname -a"
.\rshell.exe ls -l user@host:/var/log
.\rshell.exe put .\report.txt user@host:/tmp/
.\rshell.exe get user@host:/var/log/syslog .\logs\
.\rshell.exe --proxy socks5://127.0.0.1:1080 exec user@host "uname -a"
.\rshell.exe sftp user@host
```

完整参数运行 `.\rshell.exe --help`。--keepalive <秒> 可开启保活；`--proxy` 支持 `socks5://[用户:口令@]主机:端口` 和 `http://...`（HTTP CONNECT）。

## 帮助与反馈

- [更新日志](CHANGELOG.md)
- [高级功能、构建与安全说明](docs/TECHNICAL.md)
- [测试指南](tests/README.md)
- [报告问题或提出建议](https://github.com/CodeWithiYouth/rshell/issues)
