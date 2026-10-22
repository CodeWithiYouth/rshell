# rshell

一个用 C++17 + [libssh2](https://www.libssh2.org/) 实现的跨平台 SSH / SFTP 工具，包含：

- **`rshell`（命令行）**：`exec` 远程命令、`put/get` 传输、`ls/mkdir/rm/rename/chmod`，
  以及交互式 `sftp` 子命令。
- **`rshell-gui`（Windows 图形界面）**：左侧是远端文件浏览器，右侧内置终端，
  会话在独立的「会话管理」窗口里维护（等同 SSH 客户端 + SFTP 管理器）。

两者共用同一套连接、认证与主机密钥校验代码，无第三方 UI 依赖。

## 构建

```powershell
cmake -S . -B build_cmake            # 需要 CMake 3.20+ 与支持 C++17 的编译器
cmake --build build_cmake --config Release
```

产物：

| 文件 | 说明 |
| --- | --- |
| `build_cmake/Release/rshell.exe` | 命令行工具 |
| `build_cmake/Release/rshell-gui.exe` | 图形界面（Windows） |
| `build_cmake/Release/rshell_tests.exe` | 单元测试 |
| `build_cmake/Release/rshell_shell_probe.exe` | 无界面的 shell 通道冒烟测试 |

libssh2 默认静态编译（Windows 使用 WinCNG，无需 OpenSSL）。如需 Ed25519 /
OpenSSH 新格式私钥，使用 `-DRSHELL_CRYPTO_BACKEND=OpenSSL` 并在系统中准备 OpenSSL。

Windows 下默认 `/MT` 静态链接 MSVC 运行库：单文件分发，目标机不需要安装 VC++ 运行库
（需要 `/MD` 时加 `-DRSHELL_STATIC_CRT=OFF`）。注意 `CRYPTO_BACKEND` 为空时 libssh2 会
**优先尝试 OpenSSL**，找不到才回退 WinCNG——换构建环境后请用
`dumpbin /dependents build_cmake\Release\rshell-gui.exe` 复核，确认没有多出
`libcrypto-*.dll` 依赖。

## 运行环境要求

`rshell-gui.exe` 与 `rshell.exe` 开箱即用：不需要安装、不需要管理员权限。

| 项 | 要求 |
| --- | --- |
| 操作系统 | Windows 10 1703（build 15063）/ Windows Server 2016 及以上，**64 位** |
| C++ 运行库 | **无**（默认 `/MT` 静态链接 CRT，单个 exe 即可运行） |
| 网络 | 能出站访问 SSH 端口（默认 22）；程序不监听任何端口 |
| 权限 | 普通用户。读写 `%USERPROFILE%\.rshell\`、`%USERPROFILE%\.ssh\`、`%TEMP%\rshell-drag\` |

系统下限由 `SetProcessDpiAwarenessContext`（Windows 10 1703 起提供）决定，它是**静态导入**，
因此在 Win8.1 / Win7 或 Win10 1607 上会直接启动失败（提示"无法定位程序输入点"）。

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
勾选过「记住密码」的会话，密码不在这些文件里：默认交给 Windows 凭据管理器
（条目名 `rshell/<会话名>`），解锁「密码保险库」之后改存到
`%LOCALAPPDATA%\rshell\vault.dat`（主密码加密）。

## 图形界面

启动 `rshell-gui.exe` 后：

1. 会话放在独立的「会话管理」窗口里（点左下角「打开会话列表…」、`Ctrl+O`，
   或标题栏 `☰ → 会话管理…`）：左栏是现有会话，右栏是同一份表单，选中即可编辑，
   「保存 / 删除 / 连接」都在这个窗口里完成。首次启动会自动从 `~/.ssh/config`
   与 `~/.rshell/config` 的 alias 导入会话。
2. 新建会话字段为名称、主机、端口、用户、私钥、备注；快捷键 `Ctrl+N`。
   会话保存在 `~/.rshell/sessions.ini`（可用环境变量 `RSHELL_SESSIONS` 覆盖）。
3. 在管理窗口里点「连接」（或双击会话）开始连接：
   - 未配置私钥时弹出密码框：勾选「记住密码」后，**连接成功**才会把口令写进
     当前口令后端——默认是 Windows 凭据管理器，解锁密码保险库后则是保险库，
     下次连接自动使用（服务器拒绝的口令不会被保存）；
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
     带前缀的 `CSI`（例如 xterm 的 `CSI > 4 ; n m` modifyOtherKeys）
     不会被误当成 SGR 属性；
   - 拖动选择文本会自动复制，`Ctrl+V`（或 `Ctrl+Shift+V`、`Shift+Insert`）粘贴
     （支持 bracketed paste）；有选区时 `Ctrl+C` / `Ctrl+Insert` 也走复制，
     没选区时 `Ctrl+C` 才是发给远端的 `^C`；
     TUI 开启鼠标上报后拖动交给远端，按住 `Shift` 拖动可强制本地选择；
   - `Ctrl++` / `Ctrl+-` / `Ctrl+0` 缩放字体；
   - `Ctrl+Shift+C` 复制、`Ctrl+Shift+A` 全选、滚轮或 `PageUp/PageDown` 翻历史；
   - 按键遵循 xterm 约定：`Backspace` 发 `0x7F`、`Shift+Tab` 发 `ESC[Z`、
     `Alt+字母` 发 `ESC` 前缀、带修饰键的箭头/Home/End/F1-F12 发 `CSI 1;<mod>X`；
   - 拖动窗口边缘会通过 SSH window-change 通知远端调整行列数。

界面说明：

- 主界面分两栏：左侧**始终是 SFTP 文件浏览器**（未连接时显示提示与「打开会话列表…」），
  右侧是终端。底部状态栏在连接后显示远端机器的负载 / CPU / 内存 / 磁盘 / 采样延迟。
- 标题栏是自绘的（无系统边框）：拖动空白处移动窗口、双击最大化/还原，
  右侧依次是 `☰` 菜单、最小化、最大化/还原、关闭，关闭按钮悬停会变红。
  `☰` 弹出的是自绘菜单（hover 高亮、右对齐快捷键、`↑`/`↓`/`Enter`/`Esc` 可用），
  不再调用系统 `TrackPopupMenu`——后者在暗色窗口上会被画成一条几乎空白的细线。
- 深色主题与终端配色同源，标题栏右侧是连接状态药丸（未连接 / 连接中 / 已连接）。
- 终端是一张圆角卡片：四周留白，右侧是自绘滚动条（有历史记录时出现，可拖动）。
- 文件面板的所有操作都在**右键菜单**里（上传文件…、新建文件夹…、下载到本地…、
  删除、复制路径、转到目录…（`Ctrl+L`）、刷新、断开连接），底部只留一个动作按钮
  （未连接 = 打开会话列表，已连接 = 断开连接）。双击目录进入、双击文件下载；
  键盘上 `↑` `↓` 选择、`Enter` 打开、`Backspace` 上级、`F5` 刷新、`Delete` 删除，
  `↑` 小按钮回上级。
- 面板顶部的**路径条**既显示当前目录，也接受输入：点一下弹出输入框，`/` 开头是绝对
  路径、`~` 是 home、其余按当前目录解析，`..` / 多重斜杠会被规范化。路径太长时中间
  省略、保留尾部的目录名。
- **终端目录跟随（单向）**：在终端里 `cd` 到别处，文件面板会跟着切到同一个目录；
  反过来在面板里翻目录不会影响终端。实现方式是连接后往远端 shell 注入一个 `OSC 7`
  （`file://host/path`）钩子——bash 走 `PROMPT_COMMAND`、zsh 走 `precmd`，每次画提示符
  时上报 `$PWD`，终端解析后交给面板。注入分三步（关回显 → 写钩子 → 恢复回显）完成，
  第一步的命令回显由第二步按「实际多出几行」擦掉（`ESC[<n>A` 回到注入起点，再
  `ESC[J` 清到屏尾），所以即使远端在注入期间多打了几行也留不下痕迹。
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

### 保存密码用的 Windows API

Windows 10/11 没有“让应用明文保管密码”的接口，能公开调用的只有这两套：

| 方案 | API | 特点 |
| --- | --- | --- |
| 凭据管理器 | `CredWriteW` / `CredReadW` / `CredDeleteW`（`wincred.h`，`advapi32.lib`） | 系统按当前用户用 DPAPI 密钥加密后存进凭据库；用户在「控制面板 → 凭据管理器 → Windows 凭据」里能看到、改、删 |
| DPAPI | `CryptProtectData` / `CryptUnprotectData`（`dpapi.h`，`crypt32.lib`） | 只提供加密原语，密文放哪、怎么清理和迁移都得应用自己管 |

rshell 默认用前者（`src/credential_store.cpp`）：不用自己维护密文文件，用户也能
自己审计和删除。解锁密码保险库之后，同一组函数会把口令改存到保险库（见下面的
「后端切换与迁移」）。实现细节：

- 条目名 `rshell/<会话名>`，「用户名」列显示 `user@host:port`，方便在凭据管理器里
  一眼认出是哪台机器；
- 密码以 UTF-16LE 写进 Generic Credential 的 `CredentialBlob`，凭据管理器界面
  能正常显示和编辑；`Persist = CRED_PERSIST_LOCAL_MACHINE`，不随域账号漫游；
- `sessions.ini` 只写 `savepassword=1` 这个标记，口令本身不写进会话文件；
- 勾选「记住密码」后要**连接成功**才写入，避免把被服务器拒绝的口令存下来；
  取消勾选、删除会话、改主机/端口/用户/私钥都会同步删除条目，只改会话名则把
  条目搬到新名字下；
- 也可以手工管理：

  ```powershell
  cmdkey /list | Select-String rshell    # 查看
  cmdkey /delete:rshell/<会话名>         # 删除
  ```

安全边界：凭据按「当前 Windows 用户 + 本机」解密，换用户或换机器都读不出来；
但同一个登录用户下运行的其它程序同样能调用 `CredReadW` 读到它——这是凭据管理器
本身的模型。不想留下任何持久化口令时不要勾选即可（默认不勾），密码只留在内存里
用到断开连接为止。

### 密码保险库（主密码 + 可选 TPM）

凭据管理器的模型是「系统按当前登录用户解密」，所以同一账户下运行的其它程序
调一次 `CredReadW` 也能读到密码。若要「文件被别人拷走也解不开」，就得再加一层
只有用户知道的密钥：`src/vault.{h,cpp}` 实现了这种容器（`Vault` 类）。
GUI 里新建/解锁保险库之后，口令存储会自动切到保险库，并把凭据管理器里的旧条目
一次性迁进来（见下面的「后端切换与迁移」）。

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
- 解锁时口令错了不关窗：错误原因就地显示、输入框内容全选，直接重输即可。
  文件里带 TPM slot 且本机 TPM 可用时，左下角多一个「用 TPM 解锁」按钮，
  失败原因（例如换了机器、密钥被清掉）也显示在同一行提示里；
- 口令只在内存里活到 `Vault::open()`/`create()` 返回，控件销毁前会先清空；
  解锁成功后状态栏右侧常驻一个「保险库 已解锁」标记，提醒此时 DEK 还在进程里；
- 对话框关闭、慢操作（PBKDF2 / TPM）进行中都不响应关闭：那时候口令已经交出去，
  关掉窗口只会让用户以为操作被取消了。

### 后端切换与迁移

口令存储只有一套 API（`credential_store.h`），后端由 `setCredentialVault()`
切换，所有调用点（连接、会话编辑、删除会话、清除密码）都不用改：

| 时机 | 后端 | 行为 |
| --- | --- | --- |
| 启动、保险库锁定 | Windows 凭据管理器 | 与旧版本完全一致 |
| 保险库解锁 / 新建成功 | 密码保险库 | `save` 写进 vault 并立刻原子落盘；`load` 先查保险库，查不到再回退凭据管理器 |
| 手动锁定 / 退出程序 | Windows 凭据管理器 | 先 `setCredentialVault(nullptr)`，再 `Vault::lock()` 抹掉 DEK |

- 解锁时做一次迁移：遍历 `sessions.ini` 里 `savepassword=1` 的会话，把
  `rshell/<会话名>` 从凭据管理器读出来写进保险库；**全部写盘成功后**才删掉
  凭据管理器里的旧副本，中途失败只是少搬几条，下次解锁再试；
- 迁移同时清理保险库里的孤儿条目（会话已删、或已取消「记住密码」）——锁定期间
  删不掉保险库里的条目，只能等下次解锁；
- 解锁状态下删除会话 / 清除密码 / 改主机，都直接改保险库并落盘；锁定状态下这些
  操作只作用于凭据管理器，保险库里的残留条目在下次解锁时清掉；
- 界面文案跟着后端走：会话管理里的「密码：已保存到 …」、密码框的
  「记住密码（保存到 …）」、删除/清除确认框都写当前后端，不再固定写
  「Windows 凭据管理器」；保险库锁着时提示「当前已锁定，连接前需要先解锁」；
- 连接一个已保存密码的会话时，如果读不到口令、而 `vault.dat` 存在且锁着，
  GUI 会就地弹解锁对话框（取消则退回手输密码），不必先去菜单解锁；
- **CLI（`rshell.exe`）不接保险库**：它没有主密码入口，仍只认凭据管理器。
  口令迁进保险库之后，CLI 的自动登录对这些会话会失败，需要显式传 `--password`
  或改用私钥认证。

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
| `ProcessExtensionPointDisablePolicy` | 禁用 AppInit_DLLs 等传统扩展点注入 |
| `ProcessDynamicCodePolicy` | 禁止本进程生成/切换可执行内存，注入的 shellcode 无法直接执行 |
| `ProcessImageLoadPolicy` | 不从 UNC 网络路径、低完整性目录加载 DLL |
| `ProcessSignaturePolicy` | 只允许微软签名二进制，**默认关闭**；`set RSHELL_ENFORCE_MS_SIGNED=1` 开启（会挡掉第三方 shell 扩展，文件对话框可能失败） |

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
.\build_cmake\Release\rshell-gui.exe
```

TUI 显示异常时，可以设置 `RSHELL_TERM_DUMP=<文件路径>` 启动 GUI，程序会把
远端的原始输出（含转义序列）原样写入该文件，便于定位问题：

```powershell
$env:RSHELL_TERM_DUMP = "$env:TEMP\rshell-dump.bin"
.\build_cmake\Release\rshell-gui.exe
```

抓到的 dump 可以离屏复核，不必再连一次服务器：

```powershell
# 文本网格（列对齐、光标位置、属性统计）
.\build_cmake\Release\rshell_term_replay.exe "$env:TEMP\rshell-dump.bin" 92 34

# 像素级出图（与 GUI 共用同一份 GDI 绘制代码）
.\build_cmake\Release\rshell-gui.exe --render "$env:TEMP\rshell-dump.bin" .\dump.bmp 92 34

# 想知道某个字符用了哪个字体（缺字形会退回 Consolas 画 .notdef 方框）
$env:RSHELL_FONT_DEBUG = "1"    # 输出 .\dump.bmp.fonts.txt：U+276F -> Segoe UI Symbol

# 文件类型图标总览（16/24/32/48 四档 × 12 种类型），调图标时用
.\build_cmake\Release\rshell-gui.exe --icons .\icons.bmp
```

字形回退：正文字体是 `Consolas`，它没有 `❯`（U+276F）、`▶`、`⏵`、`✓` 和中文，
所以渲染时按码点逐个挑字体（`Segoe UI Symbol` → `Segoe UI Emoji` → `Microsoft YaHei`
→ `MS Gothic` → `SimSun`，字体没装则跳过），同一行里有不同字体的字符会自动拆段绘制，
每段仍按字符网格给出固定步进，保证列对齐。若某个字符在所有候选字体里都没有字形，
才会显示成方框（此时可用 `RSHELL_FONT_DEBUG` 确认是哪个码点缺字体）。

受限说明：

- 一次只维持一个连接（在管理窗口里连另一台会断开当前的），未实现多标签页。
- 文件通道是**第二条 SSH 连接**：服务器 `MaxSessions < 2`（或禁止多路）时文件面板
  连不上，终端仍可正常使用，左侧会显示原因。
- 上传 / 下载的进度显示在状态栏的**传输列表**里（拖出下载另有浮层进度条）；
  拖出下载需要先落到临时目录，大文件要等下载完才能松手放置（期间浮层会显示进度）。
- 目录操作是单层的：拖入和「上传文件…」只处理文件，删除目录要求目录为空
  （不做递归上传 / 递归删除）；暂不支持右键重命名与就地编辑。
- 终端目录跟随的注入发生在连上之后的头一两秒：这段时间终端画面会短暂不刷新（避免
  回显闪一下），注入完成后一次性重绘，屏幕上看不到任何命令行痕迹。
- 终端目录跟随依赖 shell 在每次画提示符时上报 `$PWD`（`OSC 7`）。bash / zsh 可以，
  `dash` 之类的纯 POSIX shell 没有提示符钩子；跟随期间目录如果进不去（权限、已删除），
  面板只在列表区域和状态栏提示原因，不弹模态框打断输入。

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
build_cmake\Release\rshell_tests.exe          # 单元测试
python tests/e2e_test.py                      # 端到端（需要 paramiko）
cmake --build build_cmake --config Release --target rshell_e2e
```

详见 `tests/README.md`。
