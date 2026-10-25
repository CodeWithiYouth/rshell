# rshell 测试

两层测试，互不依赖：

| 测试 | 命令 | 依赖 | 覆盖范围 |
| --- | --- | --- | --- |
| 单元测试 | `rshell_tests.exe`（或 `.\build.ps1 -Test`） | 无 | 路径/通配符/配置解析/进度与格式化、终端模拟器（VT 解析）等纯逻辑 |
| GUI 多标签回归 | `rshell_gui_tabs_probe.exe`（包含在 `.\build.ps1 -Test` 中） | Windows，无需 SSH | 隐藏窗口中的标签切换、后台事件分发、SFTP/传输状态隔离、关闭清理、DPI 与溢出布局 |
| GUI 多连接端到端 | `python tests/gui_tabs_e2e.py --exe build\modern\rshell_gui_tabs_probe.exe` | Python 3.9+、`paramiko` | 真实双 SSH/SFTP、中文输入、后台上传下载、重连与连接建立期间关闭标签 |
| 慢连接目录跟随 | `python tests/gui_tabs_e2e.py --exe build\modern\rshell_gui_tabs_probe.exe --cwd-hooks --cwd-delay 1.8` | Python 3.9+、`paramiko` | 延迟回显、逐字节确认帧、初始化输入暂存及 `stty` 回显清理 |
| 端到端测试 | `python tests/e2e_test.py` | Python 3.9+、`paramiko` | 真实 SSH/SFTP 会话上的全部子命令 |
| GUI shell 冒烟测试 | `rshell_shell_probe.exe <host> <port> <user> <password>` | 测试服务器 | PTY 申请、shell 通道读写、resize、通道关闭 |

## 端到端测试

```powershell
python -m pip install paramiko
# 需要先构建好 rshell.exe（默认依次找 build/modern/rshell.exe、build/win7/rshell.exe）
python tests/e2e_test.py
# 指定其它构建目录
python tests/e2e_test.py --exe build\modern\rshell.exe
```

CMake 配置阶段找到 Python 后会生成 `rshell_e2e` 目标：

```powershell
cmake --build build\modern --target rshell_e2e
```

脚本会在 `build_tmp/e2e/` 下创建临时目录，用 `tests/sftp_test_server.py`
在 `127.0.0.1` 的随机端口拉起一个可写的 SFTP 服务器（用户名 `tester`，
口令 `testpass`），跑完自动清理；加 `--keep` 可保留现场。

覆盖的场景（共 34 项）：

- `ls`：空目录、空子目录、`-l`、`-d`、通配符、缺失路径的退出码
- `put`：单文件、目标为已存在目录、覆盖保护与 `--force`、`-r` 目录树、
  `--resume` 断点续传、`--dry-run`
- `get`：单文件、通配符、`-r` 递归、覆盖保护与 `--force`、`--resume`
- `mkdir --parents`、`rename`、`chmod`（含 `--mode`）、`rm`/`rm -r`/通配符
- `exec` 输出与远端退出码、`rshell <主机> <命令>` 位置参数形式
- 密钥认证：RSA PEM、OpenSSH 新格式 RSA/ECDSA/Ed25519（含无 .pub 时从私钥推导）、SEC1 ECDSA（modern）、Ed25519 主机密钥
- 交互式 `sftp`：`pwd`/`mkdir`/`cd`/`put`/`ls`/`get`/`rm`/`rmdir`/`exit` 脚本化验证

### GUI 终端 / shell 通道

`tests/sftp_test_server.py` 同时提供了一个极简的交互式 shell（`echo`、`pwd`、
`uname`、`size`、`cat <文件>`、`boxes`、`exit`，支持逐字符回显与 Ctrl+C），
用来验证 GUI 的终端链路。无需打开图形界面即可跑通：

```powershell
# 1) 启动测试服务器（随机端口或指定端口）
python tests/sftp_test_server.py --root build_tmp/shellroot --port 2299 `
    --user tester --password testpass

# 2) 另一个窗口运行探针
build\modern\rshell_shell_probe.exe 127.0.0.1 2299 tester testpass
```

探针会断言：收到提示符、`echo` 回显、`uname` 输出、`exit` 输出、通道关闭。

添加 `--queue` 可验证后台发送队列（需使用本项目测试服务器）：

```powershell
build\modern\rshell_shell_probe.exe 127.0.0.1 2299 tester testpass --queue
```

覆盖 4 MiB 数据在远端背压期间快速入队、字节完整性、FIFO 顺序、8 MiB 容量限制、
后台 resize、关闭原因、背压中取消与重连后队列隔离。单元测试另覆盖历史搜索的
宽字符/emoji 列位置、重叠匹配、大小写、ANSI 颜色、历史淘汰及备用屏切换。

在 GUI 里连接该服务器后，可用以下命令检查渲染：

- `size`：回显服务端收到的 PTY 行列数（应与窗口一致）；
- `boxes`：输出一整屏“类 Ink/TUI”画面（满宽圆角边框、中文、emoji、
  DEC 图形字符集），用于肉眼/截图核对边框是否对齐；
- `tui`：进入备用屏并按“绝对定位 + 逐行擦除 + 写满整行”的方式整屏重绘，
  模拟 Bubble Tea / Ink 一类 TUI 的刷新路径；
- `cat <文件>`：把本地文件原样回灌到终端，便于回放抓到的原始输出
  （配合 `RSHELL_TERM_DUMP` 使用）。

抓到原始输出后可以离屏检查终端网格（不启动 GUI）：

```powershell
$env:RSHELL_TERM_DUMP = "$env:TEMP\rshell-dump.bin"
.\build\modern\rshell-gui.exe     # 复现问题后关闭

# 用法：rshell_term_replay <dump> [cols] [rows]
.\build\modern\rshell_term_replay.exe "$env:TEMP\rshell-dump.bin" 92 33
```

每条屏幕行会以 `行号|文本|` 的形式打印，可直接与预期画面逐列比对。
最后一行 `flags: underline=… bold=… wide=…` 汇总带属性的单元格数——
“空白处多出一条条横线”这类问题（例如把 `CSI > 4 ; 1 m` 当成
`SGR 4;1` 下划线）只看文本是看不出来的，靠这个计数可以一眼确认。

需要看真实像素时，用 GUI 自带的离屏渲染（与窗口共用同一份 GDI 绘制代码）：

```powershell
# 用法：rshell-gui.exe --render <dump> <out.bmp> <cols> <rows>
.\build\modern\rshell-gui.exe --render "$env:TEMP\rshell-dump.bin" .\dump.bmp 92 34
```

例如 `ESC[>4;1m` + 若干行空格出图后，整幅图应与主题背景色完全一致
（没有多余横线）；`ESC[4m` 则会在每行底部画出满宽横线。

字形缺失类问题（例如 Cline 输入框前的 `❯` 显示成方框/问号）可以这样定位：
加上 `RSHELL_FONT_DEBUG=1` 再 `--render`，会额外写出 `<out.bmp>.fonts.txt`，
逐行给出“码点 -> 实际使用的字体”：

```powershell
$env:RSHELL_FONT_DEBUG = "1"
.\build\modern\rshell-gui.exe --render "$env:TEMP\rshell-dump.bin" .\dump.bmp 92 34
Get-Content .\dump.bmp.fonts.txt     # U+276F -> Segoe UI Symbol
```

判定规则：非 ASCII 字符先查主字体 `Consolas` 的 cmap（`GetFontUnicodeRanges`，
与代码页无关），没有就按 `Segoe UI Symbol` → `Segoe UI Emoji` → `Microsoft YaHei`
→ `MS Gothic` → `SimSun` 的顺序找第一个有字形的字体；都没命中才会退回主字体画
`.notdef` 方框，此时该行会显示 `-> Consolas`，即为缺字体的证据。

图形界面本身的验证步骤（需人工观察）：

1. 运行 `rshell-gui.exe`，双击左侧会话（或点击「连接」）。
2. 首次连接新主机会弹出主机指纹确认，确认后写入 `~/.ssh/known_hosts`。
3. 终端里执行 `ls --color=auto`、`vim`、`top` 等，检查颜色、备用屏与按键。
4. 拖动选择文本（自动复制到剪贴板）、`Ctrl+V` 粘贴、滚轮翻看历史。

### Windows 7 兼容验证

默认兼容构建需要 MSVC v143（14.44）或更早工具集，命令见根目录 README。
构建 GUI/CLI 后会自动运行 `tools/check_win7_imports.cmake`，拒绝常见的 Win8+ 静态导入。
单元测试通过空 API 函数表模拟 Win7，覆盖系统 DPI 回退、窗口 DPI 回退、边框尺寸缩放，
并检查新版 API 存在/返回无效值时的分支。该导入检查是回归保护，不是完整的系统兼容证明。
兼容构建产物可以照常跑 `ctest`、`tests/e2e_test.py` 和 `rshell_shell_probe --queue`，
本仓库已用 MSVC 14.44 构建并验证通过；这些测试跑在新系统上，仍不能替代下面的实机回归。

交付前还需在 Windows 7 SP1 x64 实机或虚拟机运行：

1. 启动 GUI/CLI，确认没有「无法定位程序输入点」或缺失 DLL。
2. SSH 连接、中文输入、复制粘贴、最大化/还原、100%/150% 系统缩放。
3. SFTP 列目录、路径输入、上传下载、拖放和取消。
4. 新建/解锁主密码保险库；TPM 不可用时主密码路径正常。

### 已知限制

- 测试服务器为 `paramiko` 实现，仅用于本机验证：**接受任意公钥**，不可用于生产。
- 测试服务器把远端根目录映射到本地目录，并固定只提供
  `diffie-hellman-group14-sha256` 一种 KEX（`paramiko` 服务端与 libssh2
  客户端的选择顺序不一致，混用会握手失败）。
- 文件权限在 Windows 上只能表达只读位，`chmod` 相关断言只检查可写/只读。
- Windows 构建内嵌 Monocypher + Argon2，WinCNG 后端已支持 Ed25519/X25519、
  OpenSSH 新格式 RSA/ECDSA/Ed25519、SEC1 ECDSA、PuTTY PPK v2/v3（含 Argon2id/Argon2i/Argon2d 口令）；
  仍不支持 FIDO `sk-*`、PKCS#8 私钥和 PPK v1，需要完整覆盖时用
  `-DRSHELL_CRYPTO_BACKEND=OpenSSL`。
- WinCNG 的 Win7 兼容构建支持 `diffie-hellman-*` 和 `curve25519-sha256`，但没有 ECDH。
  服务器若只开放 ECDH（不含 curve25519/DH），仍会报 `Unable to exchange encryption keys`；
  新版 Windows 构建默认启用 CNG ECDH。
- GUI 连接带口令私钥时会弹出“私钥口令”输入框，可勾选记住并保存到密码保险库；
  会话管理里选中私钥会话后，按钮会变成“清除私钥口令”。






