# rshell：高级功能、构建与安全说明

面向开发者与需要了解实现细节的用户。日常安装和使用请先看 [README](../README.md)。

---

一个用 C++17 + [libssh2](https://www.libssh2.org/) 实现的跨平台 SSH / SFTP 工具，包含：

- **`rshell`（命令行）**：`exec` 远程命令、`put/get` 传输、`ls/mkdir/rm/rename/chmod`，
  以及交互式 `sftp` 子命令。
- **`rshell-gui`（Windows 图形界面）**：左侧是远端文件浏览器，右侧内置终端，
  会话在独立的「会话管理」窗口里维护（等同 SSH 客户端 + SFTP 管理器）。

两者共用同一套连接、认证与主机密钥校验代码，无第三方 UI 依赖。

## 构建

默认构建 Windows 10/11 版，Win7 兼容版用 `-Win7`：

```powershell
.\build.ps1                 # Windows 10/11 版 -> build\modern\rshell-gui.exe
.\build.ps1 -Win7           # Win7 兼容版 -> build\win7\rshell-gui.exe
.\build.ps1 -Test           # 构建后跑单元测试
.\build.ps1 -Clean -Test    # 删除构建目录后干净重建并测试
```

（`-Modern` 仍可显式使用，等同于默认。）

脚本统一用 NMake 单配置构建：默认模式用 Visual Studio 当前默认 MSVC；
`-Win7` 要求 MSVC 14.44 或更早（优先仓库内 `build_tmp\msvc143`，否则用 VS 已安装的
v143 14.44），并在链接后自动跑导入表检查。脚本结束时打印产物目录。
`-BuildDir` 可改目录；换过工具链或生成器导致缓存不匹配时加 `-Clean`。

需要 Visual Studio 生成器时可以手动 CMake，请用单独的目录，不要和脚本共用：

```powershell
# Windows 10/11（默认推荐）
cmake -S . -B build/modern-vs -DRSHELL_WINDOWS7_COMPAT=OFF
cmake --build build/modern-vs --config Release

# Win7 兼容：VS 2026 中需先添加可选的「MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.44)」
cmake -S . -B build/win7-vs -G "Visual Studio 18 2026" -T "v143,version=14.44" -DRSHELL_WINDOWS7_COMPAT=ON -DRSHELL_CRYPTO_BACKEND=WinCNG
cmake --build build/win7-vs --config Release
```

使用 VS 2022 时，把生成器换成 `Visual Studio 17 2022`、工具集用 `-T v143`。
Win7 兼容构建会设置 Windows 7 API 目标并检查 GUI/CLI 导入表中的常见新版 API；
MSVC 14.50+ 的标准库已取消 Win7 支持，兼容模式会拒绝该工具集。

WinCNG 的 ECDH/ECDSA 依赖 Windows 10+ 的 CNG 接口：默认（modern）构建已开启
（`RSHELL_WINCNG_ECDSA=ON`）。另外 Windows 构建内嵌了 Monocypher/Argon2，WinCNG 后端自带
curve25519-sha256 KEX（`-Win7` 兼容构建也有），所以只开放 curve25519 的服务器两个构建都能连；
只有服务器仅保留 ECDH（不含 curve25519/DH）时，才需要 modern 构建或让服务器放开算法。

脚本产物在构建目录根下；手动 VS 构建在各自的 `Release\` 下：

| 文件 | 说明 |
| --- | --- |
| `build\modern\rshell.exe` | 命令行工具（Windows 10/11） |
| `build\modern\rshell-gui.exe` | 图形界面（Windows 10/11） |
| `build\modern\rshell_tests.exe` | 单元测试 |
| `build\modern\rshell_shell_probe.exe` | 无界面的 shell 通道冒烟测试 |
| `build\win7\rshell*.exe` | 同上，`-Win7` 兼容构建的输出 |

下文示例统一用 `build\modern`；Win7 兼容版把路径换成 `build\win7` 即可。

libssh2 默认静态编译（Windows 使用 WinCNG，无需 OpenSSL）。Windows 构建内嵌 Monocypher
（BSD-2-Clause/CC0）和 Argon2（CC0/Apache-2.0），WinCNG 后端现在支持：

- Ed25519 / X25519：Ed25519 私钥登录、Ed25519 主机密钥、curve25519 KEX；
- OpenSSH 新格式私钥：RSA / ECDSA / Ed25519（含 bcrypt 口令）；
- SEC1 `BEGIN EC PRIVATE KEY` 的 ECDSA（modern 构建）；
- PuTTY PPK v2 / v3：RSA / ECDSA / Ed25519，含 Argon2id/Argon2i/Argon2d 口令保护。

单文件合计只增加约 100 KB。仍不支持 FIDO `sk-*`、PKCS#8 私钥和 PuTTY PPK v1；
需要完整覆盖时用 `-DRSHELL_CRYPTO_BACKEND=OpenSSL` 并在系统中准备 OpenSSL。

Windows 下默认 `/MT` 静态链接 MSVC 运行库：单文件分发，目标机不需要安装 VC++ 运行库
（需要 `/MD` 时加 `-DRSHELL_STATIC_CRT=OFF`）。注意 `CRYPTO_BACKEND` 为空时 libssh2 会
**优先尝试 OpenSSL**，找不到才回退 WinCNG——换构建环境后请用
`dumpbin /dependents build\modern\rshell-gui.exe` 复核，确认没有多出
`libcrypto-*.dll` 依赖。

## 运行环境要求

`rshell-gui.exe` 与 `rshell.exe` 开箱即用：不需要安装、不需要管理员权限。

| 项 | 要求 |
| --- | --- |
| 操作系统 | 默认构建：Windows 10/11 **64 位**（启用 CNG ECDH，要求 Win10+）；`-Win7` 兼容构建：Windows 7 SP1 / Server 2008 R2 SP1 及以上 |
| C++ 运行库 | **无**（默认 `/MT` 静态链接 CRT，单个 exe 即可运行） |
| 网络 | 能出站访问 SSH 端口（默认 22）；程序不监听任何端口 |
| 权限 | 普通用户。读写 `%USERPROFILE%\.rshell\`、`%USERPROFILE%\.ssh\`、`%TEMP%\rshell-drag\` |

新版 DPI 与进程加固 API 改为运行时探测，不再导致 Win7 因缺少入口点而在启动前失败。
Win10/11 保留按监视器 DPI 渲染；旧系统回退到系统 DPI，不能随显示器自动切换缩放。
旧系统不支持的暗色窗口属性、圆角和进程加固策略会跳过，主窗口和功能仍使用 GDI 自绘。
Win7 控制台不支持 ANSI 转义，CLI 会自动改为纯文本输出（不再显示颜色或原始转义序列）。
主密码保险库的 AES-GCM / PBKDF2 与 `CryptProtectMemory` 在 Win7 可用，TPM 解锁仍为可选项；
缺少相应字体时，Win7 上的部分 emoji 可能显示为单色或缺字。API 回退与导入检查不能替代
Win7 实机回归，发布前应在 Win7 SP1 上验证启动、SSH/SFTP、中文输入、缩放、拖放和保险库。

Win7 兼容构建的 KEX 有 `diffie-hellman-*` 和 `curve25519-sha256`（内嵌 X25519），但没有 ECDH。
服务器如果只保留 ECDH（不含 curve25519/DH），仍会报
`SSH 握手失败：Unable to exchange encryption keys`——这是双方算法没有交集，
不是私钥或密码错误。默认（modern）构建支持 ECDH，或改服务器配置即可。

除下列系统 DLL 外没有其它依赖：`USER32` `GDI32` `gdiplus` `dwmapi` `ole32` `comdlg32`
`shell32` `bcrypt` `ws2_32` `crypt32` `KERNEL32`。libssh2 静态链接，加密走系统 WinCNG
（`bcrypt.dll`），不带 OpenSSL。

分发包体积（Release 实测）：

| 方式 | 单文件 | 未压缩总量 | zip | 目标机额外要求 |
| --- | --- | --- | --- | --- |
| `/MT`（默认） | 1,061 KB | 1,061 KB | 525 KB | — |
| `/MD` + 已装运行库 | 747 KB | 747 KB | 369 KB | 需先装 VC++ 运行库 |
| `/MD` + 并列 3 个 DLL | 747 KB | 1,619 KB | 685 KB | 无 |

用 `-DRSHELL_STATIC_CRT=OFF` 可切回 `/MD`，此时目标机需要安装
[VC++ 2015-2022 x64 运行库](https://aka.ms/vc14/vc_redist.x64.exe)，或把
`msvcp140.dll`、`vcruntime140.dll`、`vcruntime140_1.dll` 拷到 exe 同目录。

服务端侧：需要开启 SFTP 子系统；左侧文件面板使用**第二条** SSH 连接，
`MaxSessions < 2` 的服务器上文件面板连不上（终端仍可用）。

运行时文件位置：会话列表 `~/.rshell/sessions.ini`（`RSHELL_SESSIONS` 可覆盖）、
日志 `~/.rshell/gui.log`、拖出下载的临时文件 `%TEMP%\rshell-drag\`、
`~/.ssh/config` 与 `~/.ssh/known_hosts`（首次连接会写入）。
勾选过「记住密码」的会话，密码不在这些文件里：只存在密码保险库
`%LOCALAPPDATA%\rshell\vault.dat`（主密码加密，可选 TPM 解锁）。
不使用 Windows 凭据管理器。

## 图形界面

启动 `rshell-gui.exe` 后：

1. 会话放在独立的「会话管理」窗口里（点左下角「打开会话列表…」、`Ctrl+O`，
   或标题栏 `☰ → 会话管理…`）：左栏是现有会话，右栏是同一份表单，选中即可编辑，
   「保存 / 删除 / 连接」都在这个窗口里完成。首次启动会自动从 `~/.ssh/config`
   与 `~/.rshell/config` 的 alias 导入会话。
2. 新建会话字段为名称、主机、端口、用户、私钥、备注；快捷键 `Ctrl+N`。
   会话保存在 `~/.rshell/sessions.ini`（可用环境变量 `RSHELL_SESSIONS` 覆盖）。
3. 在管理窗口里点「连接」（或双击会话）开始连接：
   - 配置了带口令私钥时弹出「私钥口令」框：勾选「记住私钥口令」后会存进密码保险库，
     下次自动使用；不勾选则只在内存里用到断开为止。会话管理里可随时清除已保存的口令；
   - 未配置私钥时弹出密码框：勾选「记住密码」后，如果保险库还没创建/解锁，
     会先弹出新建/解锁对话框（取消则本次不保存）；**连接成功**后才把口令写进
     密码保险库，下次连接自动使用（服务器拒绝的口令不会被保存）；
   - 密码框里不勾选就只在内存里用到断开为止，不落盘；
   - 首次连接新主机会显示指纹并询问是否写入 `~/.ssh/known_hosts`；
   - 连接失败时弹出错误详情，并写入 `~/.rshell/gui.log`。
4. 连接成功后即可在右侧终端里操作：
   - 支持 256 色 / 真彩、粗体下划线、中文宽字符、备用屏（vim/top）、
     滚动历史（默认 5000 行）；备用屏（TUI）里的光标同样按应用的
     `CSI ? 25 h/l` 显示或隐藏，不会因为进了备用屏就消失；
     退出备用屏（`?1049l`）时光标回到进入前的位置，提示符接着原来那一行，
     不会停在 TUI 最后待过的屏幕中间；
   - 中文输入法的组字串由终端自绘：拼音带下划线画在光标格上，方块光标跟着
     组字位置一起闪；候选框贴着组字串摆（自绘窗口没有系统光标，不处理的话
     组字串和候选框都会跑到屏幕左上角）；滚屏、缩放字号后重新定位，
     组字字体与终端同高；
   - 兼容常见 TUI（Claude Code / Cline / btop / htop 等）：制表符框线、
     DEC 图形字符集（`ESC ( 0`）、`REP` 重复、emoji 呈现（`U+FE0F`）、
     焦点上报与 SGR 鼠标上报；查询类序列（`CSI ? Ps $ p` 模式查询、
     `CSI ? u` kitty 键盘查询）都会应答，程序不必等超时；
   - 暂不渲染 Kitty 图形；其 APC 探测/图像控制字符串会完整忽略，保留 DA1 能力查询响应，
     避免退出 OpenCode 等 TUI 后把 `Gi=31337...` 图形探测负载显示成文本；
     带前缀的 `CSI`（例如 xterm 的 `CSI > 4 ; n m` modifyOtherKeys）
     不会被误当成 SGR 属性；
   - 拖动选择文本会自动复制，`Ctrl+V`（或 `Ctrl+Shift+V`、`Shift+Insert`）粘贴
     （支持 bracketed paste）；有选区时 `Ctrl+C` / `Ctrl+Insert` 也走复制，
     没选区时 `Ctrl+C` 才是发给远端的 `^C`；
     TUI 开启鼠标上报后拖动交给远端，按住 `Shift` 拖动可强制本地选择；
   - `Ctrl++` / `Ctrl+-` / `Ctrl+0` 缩放字体；
   - `Ctrl+Shift+C` 复制、`Ctrl+Shift+A` 全选、滚轮或 `PageUp/PageDown` 翻历史；
   - `Ctrl+Shift+F` 打开历史搜索条，精确查找当前屏幕与最多 5000 行回滚输出（区分大小写，
     按屏幕行匹配，不跨折行）。支持中文、emoji 和重叠匹配，全部命中高亮，当前命中用亮色；
     `Enter` / `Shift+Enter` 或 `F3` / `Shift+F3` 前后循环定位，`Esc` 关闭，`Tab` 回到终端；
   - 断线后在终端与状态栏显示原因（远端退出附带退出码、读写错误或发送超时），
     `Ctrl+Shift+R`、右键菜单或「重新连接」按钮可连接当前会话。重连复用原有密钥与
     密码保险库流程，未保存的密码需要重新输入；不会自动重发旧命令；
   - 按键遵循 xterm 约定：`Backspace` 发 `0x7F`、`Shift+Tab` 发 `ESC[Z`、
     `Alt+字母` 发 `ESC` 前缀、带修饰键的箭头/Home/End/F1-F12 发 `CSI 1;<mod>X`；
   - 拖动窗口边缘会通过 SSH window-change 通知远端调整行列数。
     输入发送与尺寸更新都交给 SSH 工作线程，界面线程只入队；待发送数据上限为 8 MiB，
     超出时提示本次输入未发送，持续 30 秒无发送进展时断开并报告原因。

界面说明：

- 顶部标签栏支持同时保持多个 SSH 会话。点击 `＋` 或按 `Ctrl+T` 新建标签并选择会话，
  `Ctrl+Tab` / `Ctrl+Shift+Tab` 前后切换，`Ctrl+Shift+W`、标签上的 `×` 或鼠标中键关闭。
  标签过多时可以用左右箭头查看；每个标签显示连接状态，后台输出带未读标记。
- 各标签分别保存终端缓冲与滚动位置、搜索查询、SFTP 目录和选中项、传输任务、
  远端指标与临时口令。切换标签不关闭连接；断线重连仍在原标签中完成。
  后台连接和文件事件按连接标识分发，旧连接或已关闭标签的迟到事件会被丢弃。
  关闭仍有传输的标签时会先确认，其他标签的连接和任务继续运行；关闭最后一个标签后保留空白页。

- 使用统一的深蓝灰工作区与蓝紫强调色，SFTP 文件面板和 SSH 终端各有独立标题，
  文件与会话选中项带强调标记；空终端显示 `Ctrl+O` 打开会话、`Ctrl+N` 新建会话引导。
  所有装饰继续复用系统 GDI/GDI+，不新增 UI 库、字体包或图片资源。
- 主界面分两栏：左侧**始终是 SFTP 文件浏览器**（未连接时显示提示与「打开会话列表…」），
  右侧是终端。底栏左侧常驻连接状态与当前会话，右侧固定为传输入口，断线后可直接点
  「重新连接」。中间默认显示 CPU、内存/磁盘占用百分比与采样耗时；宽度不足时先隐藏
  磁盘，再逐步隐藏次要指标，保留完整数字。采样耗时包含远端命令执行，不是纯网络 RTT。
- 复制、保存等普通提示在底栏中间显示约 4 秒后自动消退，不覆盖左侧连接状态；错误用
  醒目文字保留，直到下一条操作提示或点击旁边的 `×`。终端尺寸/字号只在调整后短暂显示。
  鼠标停留底栏可查看系统、核心数、负载、内存用量、终端尺寸/历史/字号等完整说明；
  保险库解锁时仍有常驻标记。
- 标题栏是自绘的（无系统边框）：拖动空白处移动窗口、双击最大化/还原，
  右侧依次是 `☰` 菜单、最小化、最大化/还原、关闭，关闭按钮悬停会变红。
  `☰` 弹出的是自绘菜单（hover 高亮、右对齐快捷键、`↑`/`↓`/`Enter`/`Esc` 可用），
  不再调用系统 `TrackPopupMenu`——后者在暗色窗口上会被画成一条几乎空白的细线。
- 终端右键菜单复用相同的自绘菜单，包含复制、粘贴、全选、历史搜索和重新连接；
  不可用的动作置灰。TUI 开启鼠标上报时仍优先把右键交给远端程序。
- 深色主题与终端配色同源，标题栏右侧是连接状态药丸（未连接 / 连接中 / 已连接）。
- 终端是一张圆角卡片：四周留白，右侧是自绘滚动条（有历史记录时出现，可拖动）。
- 文件面板的所有操作都在**右键菜单**里（上传文件…、新建文件夹…、下载到本地…、
  删除、复制路径、转到目录…（`Ctrl+L`）、刷新、断开连接），底部只留一个动作按钮
  （未选择会话 = 打开会话列表，有当前会话但已断线 = 重新连接，已连接 = 断开连接）。双击目录进入、双击文件下载；
  键盘上 `↑` `↓` 选择、`Enter` 打开、`Backspace` 上级、`F5` 刷新、`Delete` 删除，
  `↑` 小按钮回上级。
- 面板顶部的**路径条**支持原位编辑：点击路径条，或在文件面板按 `Ctrl+L`，即可输入
  路径并按回车跳转，无需弹窗；`Esc`、`Tab` 或失去焦点时取消编辑。`/` 开头是绝对路径、
  `~` 是 home，其余按当前目录解析，`..` / 多重斜杠会被规范化。未编辑时，长路径中间
  省略、保留尾部目录名；编辑时可水平滚动查看完整路径。
- **终端目录跟随（单向）**：在终端里 `cd` 到别处，文件面板会跟着切到同一个目录；
  反过来在面板里翻目录不会影响终端。实现方式是连接后往远端 shell 注入一个 `OSC 7`
  （`file://host/path`）钩子——bash 走 `PROMPT_COMMAND`、zsh 走 `precmd`，每次画提示符
  时上报 `$PWD`，终端解析后交给面板。注入分三步（关回显 → 写钩子 → 恢复回显）完成，
  每一步均等待带连接标识的远端确认帧，不再凭固定延时判断完成。初始化期间绘制原提示符
  快照，结束后移除初始化命令的临时画面（包括滚动历史），同时保留最新目录、标题与窗口尺寸。
  此期间的用户输入按顺序暂存，确认回显恢复后再发送；断开时会清理尚未发送的输入。
  Bash 下每条初始化输入带唯一标记，确认最后一条历史项属于本次输入后，按该输出的实际编号删除；
  兼容 Bash 4.4 执行期间 `HISTCMD` 与显示编号不一致的情况。
  不改 `HISTCONTROL` / `HISTIGNORE`，也不清空或重写历史文件。旧版本已经留下的条目需单独清理。
  这项历史清理仅适配 Bash；Zsh 仍受远端历史设置影响，本版本尚未保证初始化命令不进入 Zsh 历史。
  只认 bash / zsh 这类 POSIX 语法；`dash` 等没有提示符钩子
  的 shell 不会上报（功能静默失效），fish 下钩子会报一条语法错误但不影响回显恢复。
  设置环境变量 `RSHELL_NO_CWD_SYNC=1` 可完全关闭注入与跟随。
- 文件面板按扩展名给了各自的类型图标（文件夹 / 代码 / 可执行 / 图片 / 视频 / 音频 /
  压缩包 / 文档 / PDF / 数据 / 符号链接 / 其它），不依赖本地文件关联——远端路径在本地
  本来也没有 shell 图标可拿。图标是纯几何自绘的（`paintFileIcon`），16px 与拖动浮层里的
  24px 共用同一份代码。
- 文件支持拖放：把本地文件从资源管理器**拖进来**即上传到当前目录；把远端文件
  **拖出去**会先下载到 `%TEMP%\rshell-drag`，再以普通文件交给目标程序（浏览器、
  聊天窗口等）。拖动的全过程中光标旁边有一张浮层卡片：下载阶段显示
  「正在下载到临时目录…」和一条跑马灯进度条，下载完成后变成「拖到目标位置松开」；
  中途松手或按 `Esc` 即取消本次拖出。拖动阈值与系统一致，单击不会误触发。
- 上传和下载（右键菜单、拖入拖出）都会登记到状态栏右侧的**传输列表**：收起时是一枚
  胶囊（`↓` / `↑` / `⇅` + 进行中的项数与总百分比），点开是逐项列表，每行显示文件名、
  方向箭头、进度条和「已传 / 总量」，并可以单独取消；上传取消时远端半截文件会被清掉
  （目标原本就存在时不删，避免误删原文件）。
- 传输入口在空闲时也保持原位置，点击可查看空列表；存在失败任务时优先显示失败标记，
  取消的任务不会被标成「已完成」。
- 文件通道是**独立的 SSH 连接**，所以列大目录、传大文件都不会拖慢终端输入输出；
  打开 SFTP 失败时终端仍然可用，左侧会显示原因。
- 所有弹窗（会话管理、密码、主机指纹确认、错误与确认框、新建文件夹、关于）
  都是自绘的：圆角卡片、深色输入框（单行文字垂直居中）、owner-draw 按钮，
  `Enter` 确认、`Esc` 取消，没有系统 MessageBox 的浅色外壳。
- 未引入任何第三方 UI 库：界面全部由 GDI 自绘，圆角/圆点走系统自带的
  `gdiplus.dll`，暗色边框与窗口圆角走 `dwmapi.dll`。
  没有拖拽动画或贴边分屏之外的系统特效，Win10 上窗口是直角（Win11 自动圆角）。
- 应用图标（任务栏 / 标题栏 / exe 文件本身）编译进资源：`src/rshell.rc` 引用
  `src/res/rshell.ico`（16~256 共 9 档），图标由 `tools/make_icon.py` 用 Pillow
  生成（纯几何，不依赖任何字体或素材），改了配色重跑一次脚本即可：

  ```powershell
  python tools\make_icon.py            # 默认输出 src\res\rshell.ico
  ```

### 口令存储：只用密码保险库

rshell **不使用 Windows 凭据管理器**（`CredWriteW` / `CredReadW` /
`CredDeleteW` 那一套）。凭据管理器按「当前登录用户」解密，同一账户下运行的
任何程序调一次 `CredReadW` 就能读到口令，安全边界太弱。这里只保留密码保险库
（`src/credential_store.cpp` 负责接入，`src/vault.{h,cpp}` 负责加密）：

- `sessions.ini` 只写 `savepassword=1` / `savekeypassphrase=1` 标记，口令本身不写进会话文件；
- 登录密码条目名 `rshell/<会话名>`，私钥口令条目名 `rshell/<会话名>/key-passphrase`，
  都保存在 `%LOCALAPPDATA%\rshell\vault.dat`；
- 保险库未解锁时 `credential_store` 的所有读写都失败，GUI 会引导用户新建/解锁；
- 勾选「记住密码」或「记住私钥口令」后要**连接成功**才写入，避免把被服务器拒绝的口令存下来；
  取消勾选、删除会话、改主机/端口/用户/私钥都会同步处理条目，只改会话名则把
  条目搬到新名字下；
- 不再调用任何 `Cred*` API，也不会读取或删除旧版本留在凭据管理器里的条目
  （它们会原样留在系统里，需要时可用 `cmdkey /delete:rshell/<会话名>` 手工清理）。

安全边界：抗暴力破解能力取决于主密码强度；保险库解锁期间 DEK 在进程内存里，
同一账户的 `ReadProcessMemory` 仍可能拿到它。不想留下任何持久化口令时不要勾选
「记住密码」/「记住私钥口令」（默认都不勾），口令只留在内存里用到断开连接为止。

### 密码保险库（主密码 + 可选 TPM）

凭据管理器的模型是「系统按当前登录用户解密」，所以同一账户下运行的其它程序
调一次 `CredReadW` 也能读到密码。这里改用一层只有用户知道的密钥：
`src/vault.{h,cpp}` 实现了这种容器（`Vault` 类）。GUI 里新建/解锁保险库之后，
口令存储自动接入保险库（见下面的「后端接入与条目清理」）。

文件布局（小端、字段紧凑排列）：

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 8 | magic `RSVAULT1` |
| 8 | 2 | 格式版本（当前 1，更高版本拒绝打开） |
| 10 | 2 | flags（bit0 = 含 TPM slot） |
| 12 | 4 | PBKDF2 迭代次数 |
| 16 | 16 | 盐（每次改主密码重新生成） |
| 32 | 1 | slot 数量 |
| 33 | … | slot[]：type(1) + flags(1) + 保留(2) + 长度(4) + nonce(12) + 数据 |
| 尾部 | … | 条目区：nonce(12) + 长度(4) + AES-256-GCM 密文 + tag(16) |

密钥链：随机 32 字节 **DEK** 用 AES-256-GCM 加密条目表；DEK 再被每个 slot
各包一份：

- **主密码 slot**：`KEK = PBKDF2-HMAC-SHA256(主密码, 盐, 600000)`，
  用 `KEK` 以 AES-256-GCM 包裹 DEK（nonce 存在 slot 里）；
- **TPM slot**（可选）：用 TPM 里 2048 位 RSA 持久密钥做 OAEP 包裹。

这样设计的好处：

- 改主密码只重新生成盐、重新包裹 DEK（48 字节），不需要把条目解密再加密一遍；
- 加/删 TPM 解锁只是增删一个 slot，条目密文原样不动；
- slot 的 GCM AAD 绑定 magic、版本、类型、迭代次数和盐，改其中任何一项都会
  验证失败；条目区 AAD 绑定 magic 和版本，尾部 tag 同时充当完整性校验；
- 未知类型的 slot 会被跳过，后续加新解锁方式时旧版本仍能读文件。

实现细节（`src/vault.cpp`）：

- 派生与加密走 CNG：`BCryptDeriveKeyPBKDF2`、`BCryptEncrypt`（GCM 模式）、
  `BCryptGenRandom`，都是系统 DLL（`bcrypt.lib` / `ncrypt.lib`），不增加分发文件；
- DEK 用 `VirtualLock` 锁进工作集，销毁/锁定/移动时 `SecureZeroMemory` 清零；
- 内存里的条目值用 `ProtectedSecret`（`CryptProtectMemory`）保存，`lock()` 直接清空；
- 落盘是原子的：先写 `<文件>.tmp`，再 `MoveFileExW(MOVEFILE_REPLACE_EXISTING |
  MOVEFILE_WRITE_THROUGH)` 覆盖，断电不会留下半个文件；
- 读取时对 magic / 版本 / 迭代次数 / slot 数量 / 各段长度都做边界检查，
  截断、尾部多余数据、超大长度都直接拒绝。

TPM 解锁（可选）：

- 用 `MS_PLATFORM_KEY_STORAGE_PROVIDER`（Platform Crypto Provider）在 TPM 里创建
  持久 RSA 密钥，名字是 `rshell-vault-<路径哈希>`，一个保险库一把密钥；
- 加解密走 `NCRYPT_PAD_OAEP_FLAG`，并且**必须显式传 `BCRYPT_OAEP_PADDING_INFO`**
  （这里用 SHA-256）：PCP 不接受默认 OAEP 参数，传 `nullptr` 会直接返回
  `NTE_INVALID_PARAMETER (0x80090027)`；同理 PCP 对 `NCryptDecrypt` 的「先问长度」
  调用只返回成功而不填长度，所以解包直接用 32 字节缓冲一次解出来；
- 创建时设置 `NCRYPT_UI_POLICY` + `NCRYPT_UI_PROTECT_KEY_FLAG`（意图是「用这把密钥
  需要用户在场」），但这只是**尽力而为**：在 AMD fTPM 2.0 上实测不弹确认框，解锁是
  静默的。别把它当成第二因子——TPM 提供的是「文件离开这台机器就解不开」；
- 实测（AMD fTPM 2.0，2048 位 RSA）：首次 `enableTpm()` 建密钥约 3 秒（一次性），
  之后每次 TPM 解锁 0.4~0.8 秒（RSA-OAEP 在 TPM 内算，比主密码的 PBKDF2 0.1 秒慢，
  但省掉了输入口令）；
- `disableTpm()` 除了删掉 slot，还会删掉 TPM 里的持久密钥，不留垃圾；
- 密钥名来自文件路径，所以**把 vault.dat 搬到别的路径后 TPM slot 会失效**（主密码
  仍然可用，重新 `enableTpm()` 就恢复）——这条有单元测试覆盖；
- 没有 TPM、密钥被清掉（重装系统、换机器）时 `enableTpm` / `openWithTpm` 只是
  返回失败和原因，**主密码永远可用**——TPM 只是快捷方式，不是唯一入口；
- `Vault::tpmAvailable()` 检查 provider 是否真的实现为硬件，测试默认跳过 TPM
  路径（`RSHELL_TEST_TPM=1` 才跑），避免在 CI 里建真 TPM 密钥。

用法（集成时）：

```cpp
Vault vault;
if (!Vault::create(path, L"主密码", vault, kVaultDefaultIterations, &error)) { /* 失败 */ }
vault.set("user@host:22", password);
vault.save(&error);
vault.lock();                                    // 断开连接 / 空闲 / 锁屏时调用

Vault reopened;
Vault::open(path, L"主密码", reopened, &error);  // 口令错、文件被改都返回 false
```

界面入口（rshell-gui）：

- 主菜单 → 「新建密码保险库…」/「解锁密码保险库…」/「锁定密码保险库」，
  标签由状态决定：`vault.dat` 不存在时是新建，存在但锁定是解锁，已解锁则是
  一键锁定（`lock()` 立刻抹掉内存里的 DEK）；
- 新建时要求两次输入一致且**至少 8 个字符**（界面上的下限，`Vault::create`
  只拒绝空口令），窗口里还有一个「同时启用 TPM 解锁」勾选框——只在本机
  TPM 可用时出现，勾上会在创建后顺手 `enableTpm()`（首次约 3 秒，进度写在
  框里）；TPM 失败不影响保险库本身，关窗后单独提示；
- 解锁时默认**先用 TPM**：文件里带 TPM slot 且本机 TPM 可用时，打开对话框就
  自动在后台尝试 TPM 解锁（实测 0.4~0.8 秒），成功后直接关窗；这期间点「取消」
  就改用主密码，TPM 失败（例如换了机器、密钥被清掉）也会自动落回主密码输入，
  失败原因显示在提示行里。左下角保留「用 TPM 解锁」按钮，失败后可以重试；
- 主密码口令错了不关窗：错误原因就地显示、输入框内容全选，直接重输即可；
- 口令只在内存里活到 `Vault::open()`/`create()` 返回，控件销毁前会先清空；
  解锁成功后状态栏右侧常驻一个「保险库 已解锁」标记，提醒此时 DEK 还在进程里；
- 对话框关闭、慢操作（PBKDF2 / TPM）进行中都不响应关闭：那时候口令已经交出去，
  关掉窗口只会让用户以为操作被取消了。

### 后端接入与条目清理

口令存储只有一套 API（`credential_store.h`），并且只有一个后端：密码保险库。
`setCredentialVault()` 的语义是「接入 / 断开」，不是切换两套存储：

| 时机 | 状态 | 行为 |
| --- | --- | --- |
| 启动、保险库锁定 | 未接入 | 所有读写失败，界面提示先新建/解锁 |
| 保险库解锁 / 新建成功 | 已接入 | `save` 写进 vault 并立刻原子落盘；`load/delete` 只查保险库 |
| 手动锁定 / 退出程序 | 断开 | 先 `setCredentialVault(nullptr)`，再 `Vault::lock()` 抹掉 DEK |

- 勾选「记住密码」时如果保险库还没创建/解锁，会自动弹出新建/解锁对话框；
  用户取消则本次连接照常进行，只是不保存口令；
- 连接一个已保存密码的会话时，如果 `vault.dat` 存在且锁着，GUI 会就地弹
  解锁对话框（取消则退回手输密码），不必先去菜单解锁；
- 解锁时清理保险库里的孤儿条目（会话已删、或已取消「记住密码」）——锁定期间
  删不掉保险库里的条目，只能等下次解锁；
- 解锁状态下删除会话 / 清除密码 / 改主机，都直接改保险库并落盘；锁定状态下
  会先复位 `savepassword` 标记，残留条目在下次解锁时清理；
- 界面文案固定写「密码保险库」，不再出现凭据管理器；保险库锁着时提示
  「已锁定，连接前需要先解锁」；
- **CLI（`rshell.exe`）不接保险库**：它没有主密码入口，不会读写 `vault.dat`，
  只能用 `--password` 或交互式输入，或改用私钥认证。

安全边界：

- 抗暴力破解能力等于主密码强度；600000 次 PBKDF2-SHA256 在现代桌面 CPU 上约
  0.1~0.4 秒，能显著抬高离线字典攻击成本，但挡不住弱口令；
- 保险库解锁期间 DEK 在进程内存里，同一账户的 `ReadProcessMemory` 仍可拿到它
  （条目本身是 `ProtectedSecret` 密文）；要彻底避免，用私钥认证或 `ssh-agent`；
- 文件本身不做 ACL 保护（建议放 `%LOCALAPPDATA%\rshell\`），机密性由密码学保证，
  但文件名/大小/修改时间仍会暴露「有密码库」这件事；
- TPM slot 绑定「这台机器 + 这个 Windows 用户 + 这个文件路径」，备份/迁移/改名后
  只能靠主密码（重新启用 TPM 即可恢复快捷解锁）；TPM 解锁本身不加确认框，等于
  「持有该 Windows 会话的人可以直接解锁」。

内存里的口令：不勾「记住密码」时密码同样要在整个连接期间留在内存里（文件通道
断了要重连）。这部分明文用 `ProtectedSecret`（`src/protected_secret.{h,cpp}`）
处理：密码以 UTF-16 密文形式存放，靠 `CryptProtectMemory`
（`CRYPTPROTECTMEMORY_SAME_PROCESS`，同样在 `crypt32.lib`）原地加密，只有本进程
能解开；密文缓冲和解密工作区都用 `VirtualLock` 锁进工作集，正常换页不会把它们
写进 pagefile（工作集配额被占满时静默退化，不影响功能）。需要用时才 `reveal()`
出一份明文副本，用完立刻 `wipeString` / `wipeWideString` 清零。会话期间的
`g_currentPassword` 和文件通道用于重连的口令（`RemoteFs::password_`）都只存密文，
`ConnectionOptions` 析构时清零，`SshSession` 认证成功后也不再保留。

这套机制覆盖不到的部分：`reveal()` 交给调用方的那份明文副本是普通堆内存，仍可能
被换出，所以“用完立刻清零”比锁页更关键；此外还有密码框 `EDIT` 控件内部缓冲、
libssh2 会话里的认证上下文、进程转储，以及 `--password` 参数在进程列表里可见。
想让口令不以任何形式过内存，请用私钥认证或 `ssh-agent`。

进程加固：GUI 启动时（`hardenProcess()`，在 `runGui` 最前面、加载 GDI+/OLE 之前）
会启用下面这些 Windows 缓解策略，抬高同用户其它程序注入或劫持的成本：

| 策略 | 效果 |
| --- | --- |
| `ProcessDynamicCodePolicy` | 禁止本进程生成/切换可执行内存，注入的 shellcode 无法直接执行 |
| `ProcessImageLoadPolicy` | 不从 UNC 网络路径、低完整性目录加载 DLL |
| `ProcessSignaturePolicy` | 只允许微软签名二进制，**默认关闭**；`set RSHELL_ENFORCE_MS_SIGNED=1` 开启（会挡掉第三方 shell 扩展，文件对话框可能失败） |

GUI 不启用 `ProcessExtensionPointDisablePolicy`：这项策略会影响自绘终端的
IMM/TSF 兼容输入路径，造成微软拼音无法组字，而原生编辑框仍可输入中文。

需要说清楚边界：这些策略挡不住同一用户下的 `OpenProcess` + `ReadProcessMemory`，
只是提高门槛。它们也不能替代“不落盘”的存储方案。

密码框的清理：确认、取消、关闭三种退出路径都会先 `scrubPasswordEdit()` 把 `EDIT`
内容置空再销毁窗口；`textOf()` 读控件时用的 4096 字符临时缓冲在拷出结果后立即
`SecureZeroMemory` 清零。`EDIT` 控件内部的堆缓冲由系统管理，`DestroyWindow` 后
内容何时被复用不受应用控制——这是 Win32 控件的固有行为，也是“密码框窗口存在期间
同会话进程可以用 `WM_GETTEXT` 取值”这条风险无法彻底消除的原因。

远端状态采样每 5 秒一次，只读取 `/proc/loadavg`、`/proc/stat`、`/proc/meminfo`
和 `df -kP /` 这些不需要 root 的接口；非 Linux 主机（没有 `/proc`）只会显示
耗时和磁盘，其余指标留空。

想确认配色、字体回退与框线对齐是否正常，又不想连服务器，可以灌一段离线样例
（终端输出 + 一份假的文件列表和远端指标，用来检查左右分栏与状态栏）：

```powershell
$env:RSHELL_GUI_DEMO = "1"
.\build\modern\rshell-gui.exe
```

TUI 显示异常时，可以设置 `RSHELL_TERM_DUMP=<文件路径>` 启动 GUI，程序会把
远端的原始输出（含转义序列）原样写入该文件，便于定位问题：

```powershell
$env:RSHELL_TERM_DUMP = "$env:TEMP\rshell-dump.bin"
.\build\modern\rshell-gui.exe
```

抓到的 dump 可以离屏复核，不必再连一次服务器：

```powershell
# 文本网格（列对齐、光标位置、属性统计）
.\build\modern\rshell_term_replay.exe "$env:TEMP\rshell-dump.bin" 92 34

# 像素级出图（与 GUI 共用同一份 GDI 绘制代码）
.\build\modern\rshell-gui.exe --render "$env:TEMP\rshell-dump.bin" .\dump.bmp 92 34

# 想知道某个字符用了哪个字体（缺字形会退回 Consolas 画 .notdef 方框）
$env:RSHELL_FONT_DEBUG = "1"    # 输出 .\dump.bmp.fonts.txt：U+276F -> Segoe UI Symbol

# 文件类型图标总览（16/24/32/48 四档 × 12 种类型），调图标时用
.\build\modern\rshell-gui.exe --icons .\icons.bmp
```

字形回退：正文字体是 `Consolas`，它没有 `❯`（U+276F）、`▶`、`⏵`、`✓` 和中文，
所以渲染时按码点逐个挑字体（`Segoe UI Symbol` → `Segoe UI Emoji` → `Microsoft YaHei`
→ `MS Gothic` → `SimSun`，字体没装则跳过），同一行里有不同字体的字符会自动拆段绘制，
每段仍按字符网格给出固定步进，保证列对齐。若某个字符在所有候选字体里都没有字形，
才会显示成方框（此时可用 `RSHELL_FONT_DEBUG` 确认是哪个码点缺字体）。

受限说明：

- 每个已连接标签分别维持终端与 SFTP 后端；同时打开更多标签会增加连接数和终端历史的内存占用。
- 文件通道是**第二条 SSH 连接**：服务器 `MaxSessions < 2`（或禁止多路）时文件面板
  连不上，终端仍可正常使用，左侧会显示原因。
- 上传 / 下载的进度显示在状态栏的**传输列表**里（拖出下载另有浮层进度条）；
  拖出下载需要先落到临时目录，大文件要等下载完才能松手放置（期间浮层会显示进度）。
- 目录操作是单层的：拖入和「上传文件…」只处理文件，删除目录要求目录为空
  （不做递归上传 / 递归删除）；暂不支持右键重命名与就地编辑。
- 目录跟随在首批登录输出稳定后尽快初始化；如果用户已开始输入，则本次跳过自动注入，
  避免把初始化命令插入半行输入。慢连接按远端确认推进，总超时 10 秒，失败时会尝试恢复回显并提示。
- 终端目录跟随依赖 shell 在每次画提示符时上报 `$PWD`（`OSC 7`）。bash / zsh 可以，
  `dash` 之类的纯 POSIX shell 没有提示符钩子；跟随期间目录如果进不去（权限、已删除），
  面板只在列表区域和状态栏提示原因，不弹模态框打断输入。

## AI 助手

桌面版独有的功能（`rshell.exe` 不包含，CLI 体积保持 0 增量）。设计目标是"能真正动手，但每一步都可控"。

### 分层

| 模块 | 职责 |
| --- | --- |
| `ai_json` | 极简 JSON 解析 / 序列化，不引入第三方库 |
| `ai_client` | WinHTTP + SSE 流式解析，OpenAI 兼容协议 |
| `ai_tools` | 工具定义、读写分级、路径黑名单、凭据脱敏、输出截断 |
| `ai_agent` | 对话循环、工具调用、历史裁剪、审批状态机 |
| `ai_ssh_backend` | 把 `RemoteFs` 适配成工具后端 |
| `md` / `md_render` | 基础 Markdown 解析（含表格）与 GDI 混排；解析层不依赖 GDI，可单测 |
| `ai_panel` | GDI 自绘面板（消息、审批卡、输入框、滚动） |
| `ai_tab` | 会话内的停靠/全屏窗口、工作线程、审批桥接、设置视图 |
| `ai_settings` | `~/.rshell/ai.ini` + 保险库条目 `ai.apiKey` |

### 入口

三个入口共用 `toggleAiPanel()`：

- 标题栏菜单按钮左侧的星芒按钮（`g_app.titleAi`）：四角星是纯矢量（`drawSparkle`，8 点多边形），
  不依赖字体，任何系统上都不会缺字；面板打开时整块高亮成主色，等于状态指示。
  它在 `WM_NCHITTEST` 里和窗口按钮一样单独排除，否则会落进 `HTCAPTION` 被当成拖动窗口。
- `Ctrl+J`
- 菜单 →「AI 助手」（`IDM_AI_PANEL`）

### 输入法与组字串

输入框是 GDI 自绘的（没有 EDIT 控件帮忙），所以拼音的组字串也得自己画：

- `WM_IME_COMPOSITION` 里取 `GCS_COMPSTR` + `GCS_CURSORPOS` 交给 `ChatPanel::setPreedit()`；
  提交时取 `GCS_RESULTSTR` 逐字 `onChar()` 上屏，并清空组字串。
- `WM_IME_SETCONTEXT` 去掉 `ISC_SHOWUICOMPOSITIONWINDOW`：否则输入法会另开一个组字窗口，
  既压住输入框，又和自绘的拼音重叠。
- 绘制时按「已输入前半 + 拼音（带下划线）+ 已输入后半」三段拼接；光标落在拼音内部
  （回改拼音时在中间，`GCS_CURSORPOS` 给了位置），组字期间常亮不闪。
  组字串参与水平滚动计算，长拼音不会被输入框右边缘截断。
- 定位：`caretRect()` 同时喂给 `ImmSetCompositionWindow` 和 `ImmSetCandidateWindow`（`CFS_EXCLUDE`），
  候选框贴着光标而不是屏幕左上角；组字串变化后先 `UpdateWindow` 再定位，否则拿到的是上一帧的位置。

### 线程模型

界面线程只碰面板；AI 工作线程负责请求与工具调用；`RemoteFs` 的文件线程负责 libssh2。
审批时工作线程阻塞在条件变量上，界面点「允许/拒绝」后唤醒——不会阻塞终端，也不会并发压两条 SSH 会话。

### 网络

- 只用系统 `winhttp.dll`，TLS 由 SChannel 提供，不打包 OpenSSL/curl
- Win7 回退：`AUTOMATIC_PROXY` 失败退 `DEFAULT_PROXY`；TLS 1.3 标志被拒时退 TLS 1.2
- 请求头固定 `Accept-Encoding: identity`（`WINHTTP_OPTION_DECOMPRESSION` 需要 Win8.1+）
- OpenCode Go（`opencode.ai`）要求每个对话带稳定的 `x-opencode-session`，用于路由与提示缓存；
  仅当 baseURL 含 `opencode.ai` 时发送，会话 ID 每个 AI 标签生成一次并保持
- 明文 HTTP 只允许本机地址（Ollama / vLLM）
- Win7 仍需系统已启用 TLS 1.2 并更新根证书，否则会在握手阶段失败

### 工具与上限

| 工具 | 分级 | 上限 |
| --- | --- | --- |
| `run_command` | 写，必审 | 超时 5~300 秒；缓冲 256 KB，回灌 32 KB |
| `write_file` | 写，必审 | 单次 256 KB，整文件覆盖，权限 0644 |
| `read_file` | 读，免审 | 96 KB，超限标记截断 |
| `list_dir` | 读，免审 | 200 项 |

命令走独立 exec 通道：没有 PTY，`cd` 不跨调用保留；超时只关闭通道，不给远端进程发信号。

### 多会话（跨标签）

AI 面板全局面板只有一份，工具的目标主机跟着当前标签走。四个工具都带可选的 `host` 参数：

- 留空 = 当前标签（或本轮发问时锁定的那台，避免中途切标签串台）。
- 填会话名（例如 `us-vps`）= 直接路由到那个标签的 `RemoteFs`，不需要先 `ssh` 跳过去。
- 名字对不上会返回「没有名为 X 的会话」并列出可用名字，模型据此纠正。

系统提示里会列出当前打开的所有会话（并标出哪个是当前目标），审批卡片也会写成「在 us-vps 上执行命令：…」。

### 安全

- 读路径黑名单：`~/.ssh/id_*`、`*.pem`、`~/.aws/credentials`、`~/.kube/config`、`~/.netrc`、`~/.git-credentials`、`/etc/shadow`、`~/.rshell/*` 等
- 回灌模型前做凭据脱敏：PEM 私钥块、`AKIA…`、`sk-…`、`ghp_…`、JWT、`password=`、`Authorization: Bearer …`
- 请求体固定 `parallel_tool_calls: false`，一次只可能有一张审批卡
- 没有注册审批回调时，写操作默认拒绝（安全默认，不是放行）
- 被拒绝的调用会作为工具结果回灌给模型，让它换方案；「终止任务」会中断整轮

### 配置

```ini
# ~/.rshell/ai.ini（可用 RSHELL_AI_CONFIG 覆盖路径）
base_url=https://api.openai.com/v1
model=gpt-4o-mini
redact_secrets=1
max_steps=16
```

API Key 只存在保险库条目 `ai.apiKey`，配置文件里不出现明文；每次启动需解锁保险库。

### 体积

`rshell.exe` 不变（AI 代码只编进 GUI 目标）；`rshell-gui.exe` 增加约 300 KB。

### 回归

```powershell
.\build.ps1 -Test                                  # 单元测试 + GUI 状态回归
python tests/sftp_test_server.py --root build_tmp/airoot --port 2299 --user tester --password testpass
build\modern\rshell_ai_probe.exe 127.0.0.1 2299 tester testpass   # 真实 SSH/SFTP 工具链
rshell-gui.exe --ai-preview out.bmp    # 离屏渲染演示对话
rshell-gui.exe --ai-paths out.bmp      # 长路径压力测试
rshell-gui.exe --ai-welcome out.bmp    # 空状态
```

### 保活

会话可以单独开「保持活动状态」：勾选后按间隔（默认 30 秒，钳在 5~3600）发送 SSH 层的
`keepalive@openssh.com`（`libssh2_keepalive_config` + `libssh2_keepalive_send`，want_reply=1）。

- 只作用于交互终端会话（`SshShell` 的读线程空闲分支里发送）；文件/AI 通道本来就有 5 秒一次的状态采样。
- 用途：穿过 NAT/防火墙的空闲超时，并尽早发现半开连接（want_reply 拿不到回应会暴露断链）。
- CLI 对应 `--keepalive <秒>`；会话配置存在 `sessions.ini` 的 `keepalive=`。
## 代理连接

SSH 可以经 SOCKS5 或 HTTP CONNECT 代理连出，终端、SFTP 面板和 AI 工具都生效（它们共用 `SshSession`）。

- **接入点只有一处**：`SshSession::openTcp()` 在交给 libssh2 之前把 socket 换成「经代理连出去」的那个；握手、KEX、认证逻辑完全不变。
- **协议**：SOCKS5（含用户名/密码认证，目标主机名交给代理解析，便于远端 DNS）、HTTP CONNECT（含 Basic 认证）。
- **配置**：GUI 在「会话管理 → 代理」填 `socks5://主机:端口` 或 `http://主机:端口`；CLI 用 `--proxy`。
- **凭据**：URL 里的 `用户:口令@` 会在保存前剥离；用户名明文存 `sessions.ini`，密码进密码保险库（条目 `rshell/<会话>/proxy-password`）。
- **失败不静默**：代理解析失败或连不上时直接报错，不会退回直连。
- **Win7**：纯 Winsock 2.2，无新增依赖。

连通性测试（本地 SSH + 两个要求认证的代理）：

```powershell
python tests/proxy_test.py --exe build\modern\rshell.exe
```

## 命令行速览

```powershell
rshell exec user@host "uname -a"
rshell put .\local.txt user@host:/tmp/
rshell get user@host:/var/log/syslog .\logs\
rshell ls -l user@host:/tmp
rshell sftp user@host          # 交互式 sftp
```

完整参数见 `rshell --help`。

## 测试

```powershell
build\modern\rshell_tests.exe         # 单元测试
python tests/e2e_test.py                      # 端到端（需要 paramiko）
cmake --build build\modern --target rshell_e2e
```

详见 `tests/README.md`。


