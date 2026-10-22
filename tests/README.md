# rshell 测试

两层测试，互不依赖：

| 测试 | 命令 | 依赖 | 覆盖范围 |
| --- | --- | --- | --- |
| 单元测试 | `rshell_tests.exe`（或 `ctest -C Release`） | 无 | 路径/通配符/配置解析/进度与格式化、终端模拟器（VT 解析）等纯逻辑 |
| 端到端测试 | `python tests/e2e_test.py` | Python 3.9+、`paramiko` | 真实 SSH/SFTP 会话上的全部子命令 |
| GUI shell 冒烟测试 | `rshell_shell_probe.exe <host> <port> <user> <password>` | 测试服务器 | PTY 申请、shell 通道读写、resize、通道关闭 |

## 端到端测试

```powershell
python -m pip install paramiko
# 需要先构建好 rshell.exe（默认找 build_cmake/Release/rshell.exe）
python tests/e2e_test.py
# 指定其它构建目录
python tests/e2e_test.py --exe build\Release\rshell.exe
```

CMake 配置阶段找到 Python 后会生成 `rshell_e2e` 目标：

```powershell
cmake --build build_cmake --config Release --target rshell_e2e
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
- 密钥认证：PEM 格式 RSA、OpenSSH 新格式 RSA（不支持时应提示）、Ed25519
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
build_cmake\Release\rshell_shell_probe.exe 127.0.0.1 2299 tester testpass
```

探针会断言：收到提示符、`echo` 回显、`uname` 输出、`exit` 输出、通道关闭。

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
.\build_cmake\Release\rshell-gui.exe     # 复现问题后关闭

# 用法：rshell_term_replay <dump> [cols] [rows]
.\build_cmake\Release\rshell_term_replay.exe "$env:TEMP\rshell-dump.bin" 92 33
```

每条屏幕行会以 `行号|文本|` 的形式打印，可直接与预期画面逐列比对。
最后一行 `flags: underline=… bold=… wide=…` 汇总带属性的单元格数——
“空白处多出一条条横线”这类问题（例如把 `CSI > 4 ; 1 m` 当成
`SGR 4;1` 下划线）只看文本是看不出来的，靠这个计数可以一眼确认。

需要看真实像素时，用 GUI 自带的离屏渲染（与窗口共用同一份 GDI 绘制代码）：

```powershell
# 用法：rshell-gui.exe --render <dump> <out.bmp> <cols> <rows>
.\build_cmake\Release\rshell-gui.exe --render "$env:TEMP\rshell-dump.bin" .\dump.bmp 92 34
```

例如 `ESC[>4;1m` + 若干行空格出图后，整幅图应与主题背景色完全一致
（没有多余横线）；`ESC[4m` 则会在每行底部画出满宽横线。

字形缺失类问题（例如 Cline 输入框前的 `❯` 显示成方框/问号）可以这样定位：
加上 `RSHELL_FONT_DEBUG=1` 再 `--render`，会额外写出 `<out.bmp>.fonts.txt`，
逐行给出“码点 -> 实际使用的字体”：

```powershell
$env:RSHELL_FONT_DEBUG = "1"
.\build_cmake\Release\rshell-gui.exe --render "$env:TEMP\rshell-dump.bin" .\dump.bmp 92 34
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

### 已知限制

- 测试服务器为 `paramiko` 实现，仅用于本机验证：**接受任意公钥**，不可用于生产。
- 测试服务器把远端根目录映射到本地目录，并固定只提供
  `diffie-hellman-group14-sha256` 一种 KEX（`paramiko` 服务端与 libssh2
  客户端的选择顺序不一致，混用会握手失败）。
- 文件权限在 Windows 上只能表达只读位，`chmod` 相关断言只检查可写/只读。
- Windows 默认的 WinCNG 加密后端不支持 **Ed25519** 私钥；OpenSSH 新格式
  （`-----BEGIN OPENSSH PRIVATE KEY-----`）私钥也无法解析。两种情况
  rshell 都会给出提示；需要完整支持时用
  `-DRSHELL_CRYPTO_BACKEND=OpenSSL` 重新构建。
