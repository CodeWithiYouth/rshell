# rshell

rshell 是一款 Windows SSH 客户端，也可以在命令行使用。连接远程主机后，可以在终端执行命令、用 SFTP 浏览和传输文件。

它不需要注册账号，也不依赖浏览器或额外的图形界面运行库。SSH 账号、私钥和本机保存的密码均由你在本机管理。

## 下载

**Windows 10/11，64 位：** [下载 rshell v1.0.2](https://github.com/CodeWithiYouth/rshell/releases/download/v1.0.2/rshell-windows-x64.zip)。

压缩包包含桌面程序 `rshell-gui.exe` 和命令行工具 `rshell.exe`。解压后即可运行，无需安装。校验值见 [SHA256SUMS.txt](https://github.com/CodeWithiYouth/rshell/releases/download/v1.0.2/SHA256SUMS.txt)。

> Windows 7 SP1 有兼容构建方式，但当前下载包面向 Windows 10/11。详见[高级构建说明](docs/TECHNICAL.md)。

## 第一次连接

1. 解压 ZIP，运行 `rshell-gui.exe`。
2. 按 **Ctrl+N** 添加会话，填写会话名称、主机、用户名和 SSH 端口（默认 22）。可选填私钥路径和备注。
3. 保存会话，然后点「连接」。密码登录时输入密码；第一次连接新主机时，先核对屏幕上的主机指纹，再选择是否信任。
4. 连接成功后，右侧是 SSH 终端，左侧可浏览远端文件。

文件操作在 SFTP 面板中右键选择。你也可以将文件拖入面板上传，或把远端文件拖到其他应用中下载。SFTP 使用独立连接；若服务器限制并发 SSH 会话，终端可能可用而文件面板无法连接。

## 常用操作

- **连接管理：** Ctrl+O 打开会话列表；Ctrl+N 新建会话。
- **远端路径：** 点 SFTP 面板顶部的路径框，输入路径并回车；Ctrl+L 也可打开路径输入。
- **搜索终端输出：** Ctrl+Shift+F，然后按 Enter / Shift+Enter 浏览匹配项。
- **断线重连：** Ctrl+Shift+R。
- **终端字号：** Ctrl++ 放大、Ctrl+- 缩小、Ctrl+0 重置。
- **复制与粘贴：** 选中终端文本自动复制；Ctrl+V 粘贴。没有选中文本时，Ctrl+C 会向远端发送中断信号。

## 密码与隐私

rshell 不会把连接密码写进会话列表。若选择记住密码，密码会保存到本机的主密码保险库；你可以不保存，让密码只在当前进程内存中使用。保险库可用主密码解锁，也可选择配置 TPM 快捷解锁。

首次连接时，主机指纹用于确认服务器身份。请通过可信方式核对指纹，避免误信任伪装主机。

## 命令行示例

在 PowerShell 中进入解压目录，运行：

```powershell
.\rshell.exe exec user@host "uname -a"
.\rshell.exe ls -l user@host:/var/log
.\rshell.exe put .\report.txt user@host:/tmp/
.\rshell.exe get user@host:/var/log/syslog .\logs\
.\rshell.exe sftp user@host
```

完整参数运行 `.\rshell.exe --help`。

## 帮助与反馈

- [高级功能、构建与安全说明](docs/TECHNICAL.md)
- [测试指南](tests/README.md)
- [报告问题或提出建议](https://github.com/CodeWithiYouth/rshell/issues)
