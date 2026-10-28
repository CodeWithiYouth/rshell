// gui_main.cpp - rshell 图形界面：会话列表 + 内置终端
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <windowsx.h>

// GDI+ 只用来画抗锯齿圆角/圆点：系统自带 gdiplus.dll，编译期多出十几 KB，
// 换来卡片、按钮的平滑边缘（纯 GDI 的 RoundRect 台阶很明显）。
// gdiplus.h 依赖 COM 接口声明，windows.h 在 WIN32_LEAN_AND_MEAN 下不包含，需显式引入。
#include <objidl.h>
#include <ole2.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "credential_store.h"
#include "ai_panel.h"
#include "ai_tab.h"
#include "dnd_virtual_file.h"
#include "protected_secret.h"
#include "remote_fs.h"
#include "resource.h"
#include "session_store.h"
#include "ssh_session.h"
#include "ssh_shell.h"
#include "terminal.h"
#include "ui_theme.h"
#include "util.h"
#include "vault.h"
#include "win_compat.h"

using namespace rshell;

namespace {

constexpr wchar_t kMainClass[] = L"RshellMainWindow";
constexpr wchar_t kTermClass[] = L"RshellTerminalWindow";
constexpr wchar_t kListClass[] = L"RshellFileList";

enum : UINT {
    WM_SHELL_DATA = WM_APP + 1,   // lParam = std::string*（UI 线程负责释放）
    WM_SHELL_CLOSED = WM_APP + 2,
    WM_CONNECT_OK = WM_APP + 3,
    WM_CONNECT_FAIL = WM_APP + 4,
    WM_FS_EVENT = WM_APP + 5,     // lParam = FsEvent*（UI 线程负责释放）
    WM_FS_STATS = WM_APP + 6,     // lParam = RemoteStats*（UI 线程负责释放）
    WM_ASK_HOSTKEY = WM_APP + 7,  // lParam = HostKeyRequest*（工作线程同步等待）
    WM_VAULT_TPM_DONE = WM_APP + 8,  // lParam = std::shared_ptr<TpmAttempt>*（UI 线程释放）
};

// 目录跟随分三步执行，每步由远端私有 OSC 确认；定时器只负责静默等待和超时。
// 1) stty -echo  2) 写 OSC 7 钩子  3) stty echo
// 初始化期间绘制原提示符快照，用户输入暂存到回显恢复后发送。
constexpr UINT_PTR kCwdHookTimer1 = 2;
constexpr UINT_PTR kCwdHookTimer2 = 3;
constexpr UINT_PTR kCwdHookTimer3 = 4;
constexpr UINT_PTR kCwdHookTimer4 = 5;

enum : int {
    ID_LIST = 1001,
    ID_BTN_ACTION,      // 侧栏主按钮：未连接 -> 打开会话列表，已连接 -> 断开
    ID_BTN_FS_UP,       // 文件面板：上级目录
    ID_STATUS,
    ID_TERM,
    ID_SEARCH_EDIT,
    IDM_CONNECT = 2001,
    IDM_DISCONNECT,
    IDM_OPEN,
    IDM_NEW,
    IDM_EDIT,
    IDM_DELETE,
    IDM_COPY,
    IDM_PASTE,
    IDM_SELECT_ALL,
    IDM_ZOOM_IN,
    IDM_ZOOM_OUT,
    IDM_ZOOM_RESET,
    IDM_SEARCH,
    IDM_RECONNECT,
    IDM_ABOUT,
    IDM_EXIT,
    IDM_NEW_TAB,
    IDM_CLOSE_TAB,
    IDM_NEXT_TAB,
    IDM_PREV_TAB,
    IDM_AI_PANEL,
    // 文件面板右键菜单（都在 popup::Item 里，走同一个 WM_COMMAND 分发）
    IDM_FS_UPLOAD = 2101,
    IDM_FS_MKDIR,
    IDM_FS_DOWNLOAD,
    IDM_FS_DELETE,
    IDM_FS_REFRESH,
    IDM_FS_COPY_PATH,
    IDM_FS_GOTO,  // 转到目录…（路径条也能点）
    // 密码保险库：新建 / 解锁 / 锁定同一个入口，标签随状态变
    IDM_VAULT = 2201,
};

struct SessionState {
    UINT_PTR tabId = 0;
    bool aiOpen = false;   // 右侧 AI 面板是否展开
    bool aiFull = false;   // 展开时是否全屏
    ai::AiTab* aiTab = nullptr;  // 每个会话各有一份对话
    bool unread = false;
    enum class DownloadState { Active, Done, Failed, Cancelled };
    struct DownloadTask {
        int id = 0;
        std::string remote;  // 远端路径：进度事件靠它对应到任务
        std::wstring name;   // 文件名
        std::uint64_t done = 0;
        std::uint64_t total = 0;
        DownloadState state = DownloadState::Active;
        bool drag = false;   // 拖到资源管理器：具体落到哪个目录由系统决定
        bool upload = false;  // true = 上传（本地 -> 远端），false = 下载
        std::shared_ptr<std::atomic<bool>> cancel;  // 取消开关，传输线程轮询
    };
    Terminal termData;
    std::shared_ptr<SshShell> shell = std::make_shared<SshShell>();
    std::string currentSession;
    std::wstring statusText = L"就绪";
    bool statusError = false;
    ULONGLONG statusUntil = 0;
    ULONGLONG terminalInfoUntil = 0;
    int statusFontSize = 16;
    bool connecting = false;
    UINT_PTR connectionId = 0;
    bool searchOpen = false;
    bool searchDirty = false;
    std::vector<TerminalMatch> searchMatches;
    int searchIndex = -1;
    // 每个连接独立维护 OSC 7 注入状态、静默等待和重绘冻结。
    bool shellGotOutput = false;
    DWORD lastShellOutputTick = 0;
    DWORD lastKeyTick = 0;
    int cwdHookTries = 0;
    long long cwdHookStartLine = 0;
    int cwdHookPhase = 0;  // 1=关回显，2=安装钩子，3=恢复回显；各阶段由远端确认。
    bool cwdHookAcked = false;
    ULONGLONG cwdHookStarted = 0;
    ULONGLONG cwdHookDeadline = 0;
    std::unique_ptr<Terminal> cwdHookSnapshot;
    std::string cwdHookPendingInput;
    std::string cwdHookHistoryTag;
    bool holdTermPaint = false;
    std::shared_ptr<RemoteFs> fs = std::make_shared<RemoteFs>();                 // 独立 SSH 连接：SFTP + 机器状态采样
    bool fsReady = false;        // 独立连接已建立
    bool fsFailed = false;       // 独立连接失败（左侧显示原因）
    std::string fsPath;          // 当前远端目录
    std::string fsHome;          // 登录后的 home
    std::string fsError;         // 列目录失败原因
    std::string fsQuietPath;     // 终端跟随触发的导航：列目录失败只在状态栏提示
    std::vector<RemoteEntry> fsEntries;
    int fsSel = -1;
    int fsHover = -1;
    int fsScroll = 0;
    int fsPendingUploads = 0;    // 排队中的上传数：全部完成时只刷新一次目录
    std::vector<DownloadTask> downloads;
    int downloadSeq = 0;
    bool downloadsOpen = false;    // 下载列表是否展开
    RemoteStats stats;
    bool hasSelection = false;
    bool selecting = false;
    std::size_t selStartLine = 0;
    std::size_t selEndLine = 0;
    int selStartCol = 0;
    int selEndCol = 0;
    std::wstring searchQuery;
    ProtectedSecret currentPassword;
    ProtectedSecret currentKeyPassphrase;
    std::string pendingCredentialTarget, pendingCredentialSession;
    std::string pendingKeyCredentialTarget, pendingKeyCredentialSession;
};

struct App : SessionState {
    HINSTANCE inst = nullptr;
    HWND main = nullptr;
    HWND list = nullptr;
    HWND fsPathEdit = nullptr;
    HWND term = nullptr;
    HWND status = nullptr;
    HWND btnNew = nullptr;
    HWND btnEdit = nullptr;
    HWND btnDel = nullptr;
    HWND btnConnect = nullptr;

    SessionStore store;
    std::shared_ptr<Libssh2Runtime> runtime;
    RECT tabsRect{};
    int firstVisibleTab = 0;

    // 主密码保险库：解锁后一直留在内存里，退出/手动锁定/（后续）空闲超时再 lock()
    Vault vault;

    std::wstring windowTitle = L"rshell";

    HWND statusHint = nullptr;
    std::wstring statusHintText;
    int hoverStatusAction = 0;

    HFONT font = nullptr;
    HFONT fontBold = nullptr;
    HFONT uiFont = nullptr;
    HFONT uiFontBold = nullptr;
    HFONT titleFont = nullptr;
    HFONT tinyFont = nullptr;
    HICON appIcon = nullptr;       // 应用图标：标题栏 / Alt+Tab
    HICON appIconSmall = nullptr;  // 小尺寸：任务栏角标、标题栏左上角
    int dpi = 96;
    int fontSize = 16;
    int cellW = 8;
    int cellH = 18;
    int ascent = 14;
    int uiTextH = 16;  // 界面字体一行的高度：单行输入框按它做垂直居中

    HDC memDC = nullptr;
    HBITMAP memBmp = nullptr;
    int bmpW = 0;
    int bmpH = 0;

    // 字形回退：主字体缺字时按 Unicode 码点挑选回退字体（避免 TUI 边框/CJK 错位）
    std::vector<HFONT> fallbackFonts;
    std::vector<HFONT> fallbackFontsBold;
    std::vector<std::wstring> fallbackNames;  // 与上面两条链一一对应（用于 emoji 特判）
    std::unordered_map<std::uint32_t, HFONT> glyphFontCache;
    // 各字体的 cmap 覆盖区间（含端点），用于判定“这个字体有没有该字的字形”
    std::unordered_map<HFONT, std::vector<std::pair<char32_t, char32_t>>> glyphRanges;

    // ---- 自绘布局状态 ----
    int titleH = 0;
    int statusH = 0;
    int termHeaderH = 0;  // 终端上方的辅助条（搜索框 / 重连按钮），平时为 0
    int listTop = 0;
    int sidebarW = 0;
    int contentTop = 0;
    int contentBottom = 0;
    // 命令按钮（新建/编辑/删除/连接）与标题栏按钮的 hover/press 状态
    struct UiButton {
        int id = 0;
        RECT rc{};
        bool primary = false;
        bool secondary = false;
    };
    std::vector<UiButton> buttons;
    int hoverButton = -1;
    int pressButton = -1;
    int hoverTitleBtn = -1;  // 0=菜单 1=最小化 2=最大化 3=关闭
    int pressTitleBtn = -1;
    RECT titleButtons[4]{};  // 供 WM_NCHITTEST 排除

    // ---- 终端卡片 ----
    int termPadX = 0;
    int termPadY = 0;
    int scrollbarW = 0;
    bool sbHover = false;
    bool sbDrag = false;
    int sbDragStartY = 0;
    int sbDragStartOffset = 0;

    HWND searchEdit = nullptr;

    RECT searchBox{}, searchPrev{}, searchNext{}, searchClose{};
    bool cursorOn = true;
    // TranslateMessage 对 Ctrl+字母照样会生成 WM_CHAR（哪怕 WM_KEYDOWN 已经处理过），
    // 不拦的话 Ctrl+V 会粘两次、有选区时 Ctrl+C 会"复制 + 发 ^C"。
    // 存的是要吞掉的字符码，只有真正匹配才吞，避免误伤下一次正常输入。
    int swallowChar = -1;

    // ---- 输入法组字串 ----
    // 组字串由终端自己画（见 drawImeComposition）：输入法自带的组字窗口正好
    // 贴在光标格上，会把光标整个盖住，打字时看起来像“光标不闪了”。
    // imeCursorPos 是 IME 给的 GCS_CURSORPOS，单位是 UTF-16 码元。
    std::wstring imeComposition;
    int imeCursorPos = 0;

    // ---- 左侧文件面板：SFTP 文件浏览器（独立 SSH 连接） ----

    bool fsDragArmed = false;    // 左键按下、还没越过拖动阈值
    POINT fsDragOrigin{};
    // 文件面板标题行的路径条：显示当前目录，点一下可以输入新路径
    RECT fsPathRect{};
    bool hoverPathRect = false;

    // 传输列表的公共交互状态
    int hoverDownloadCancel = -1;  // 悬停的「取消」按钮对应的任务下标
    bool hoverDownloadChip = false;
    int panelTop = 0;              // 展开时面板顶边（内容区给它让位）


};

App g_app;

// 活动标签的状态放在 App 的基类中，以复用现有终端/SFTP 绘制代码；
// 其余标签保存在各自的堆对象中。后端本体不随状态移动，工作线程始终持有原连接。
std::vector<std::unique_ptr<SessionState>> g_tabs;
std::size_t g_activeTab = 0;
UINT_PTR g_selectedTabId = 0;
UINT_PTR g_nextTabId = 1;
UINT_PTR g_nextConnectionId = 1;
std::atomic<bool> g_applicationClosing{false};

bool foregroundSession() { return g_app.tabId == g_selectedTabId; }

void initializeSessionTabs() {
    if (!g_tabs.empty()) return;
    g_app.tabId = g_nextTabId++;
    g_app.connectionId = g_nextConnectionId++;
    g_selectedTabId = g_app.tabId;
    g_activeTab = 0;
    g_tabs.emplace_back(nullptr);
}

SessionState* findSessionState(UINT_PTR id, bool connection) {
    if ((connection ? g_app.connectionId : g_app.tabId) == id) return &g_app;
    for (auto& state : g_tabs)
        if (state && (connection ? state->connectionId : state->tabId) == id) return state.get();
    return nullptr;
}

void renameSessionTabs(const std::string& from, const std::string& to) {
    auto rename = [&](SessionState& state) {
        if (state.currentSession == from) state.currentSession = to;
        if (state.pendingCredentialSession == from) {
            state.pendingCredentialSession = to;
            state.pendingCredentialTarget = credentialTarget(to);
        }
        if (state.pendingKeyCredentialSession == from) {
            state.pendingKeyCredentialSession = to;
            state.pendingKeyCredentialTarget = keyPassphraseTarget(to);
        }
    };
    rename(g_app);
    for (auto& state : g_tabs) if (state) rename(*state);
}

// 只在 UI 线程中临时切换事件上下文，退出作用域时恢复当前可见标签。
// 嵌套窗口消息也按自己的所属连接查找状态，不依赖当前标签的下标。
class SessionStateScope {
public:
    explicit SessionStateScope(SessionState* state) : saved_(state == &g_app ? nullptr : state) {
        if (saved_) std::swap(static_cast<SessionState&>(g_app), *saved_);
    }
    ~SessionStateScope() {
        if (saved_) std::swap(static_cast<SessionState&>(g_app), *saved_);
    }
    SessionStateScope(const SessionStateScope&) = delete;
    SessionStateScope& operator=(const SessionStateScope&) = delete;
private:
    SessionState* saved_;
};

void setCwdTimer(UINT_PTR timer, UINT delay) {
    SetTimer(g_app.main, g_app.tabId * 8 + timer, delay, nullptr);
}
void killCwdTimer(UINT_PTR timer) {
    KillTimer(g_app.main, g_app.tabId * 8 + timer);
}

void newSessionTab();
void switchSessionTab(std::size_t index);
void closeSessionTab(std::size_t index);
void cycleSessionTab(int direction);
void paintSessionTabs(HDC dc);
int hitSessionTab(POINT pt, bool* close = nullptr);
bool handleSessionTabClick(POINT pt);
void ensureActiveTabVisible(int width);

// 把会话里的代理配置套到连接参数上；返回 false 表示配置无法解析。
// 代理解析失败时调用方必须中止连接，绝不能悄悄退回直连。
bool applySessionProxy(ConnectionOptions& options, const Session& session, std::string& error);

void connectToSession(const Session& session, const std::wstring& password,
                      const std::wstring& keyPassphrase);
void messageWarn(const std::wstring& title, const std::wstring& text);
void stopRemoteFs();
void startRemoteFs(const Session& session, const std::wstring& password,
                   const std::wstring& keyPassphrase);

// 密码/私钥口令只在内存里留到断开连接：文件通道要用同一个凭据再连一次

// 用户在密码框里勾了「记住密码/记住私钥口令」时，先记下要写的条目，等连接成功再落库：
// 口令刚被服务器拒绝的话不该被保存下来。
void disconnectSession();
void updateButtons();
void doCopy();
void doPaste();
void doSelectAll();
void onConnectCommand();
void connectStoredSession(const std::string& name);
void openTerminalMenu(HWND hwnd, POINT screenPt);
void showTerminalSearch(bool show);
void refreshTerminalSearch(bool reveal = true);
void stepTerminalSearch(int direction);
void hideStatusHint();
void openMainMenu(HWND hwnd);
void layoutMain(HWND hwnd);
void toggleAiPanel();
void setAiPanelFull(bool full);
bool askHostKey(const std::string& host, const std::string& keyType,
                const std::string& fingerprint, UINT_PTR connectionId = 0);
void openSessionManager(bool startBlank = false);
void openFileContextMenu(HWND hwnd, POINT screenPt);
// 传输任务（状态栏列表）：新增、取消、清理已完成、展开高度
void addDownloadTask(const std::string& remote, const std::wstring& name, std::uint64_t total,
                     bool drag, std::shared_ptr<std::atomic<bool>> cancel,
                     bool upload = false);
void cancelDownloadTask(int id);
void clearFinishedDownloads();
int downloadsPanelHeight();
void syncDownloadLayout();

// ---------------------------------------------------------------- 辅助

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                  nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
}

std::string toUtf8(const wchar_t* text) { return text ? toUtf8(std::wstring(text)) : std::string(); }

// 宽字符串清零后再释放：密码用完别留在堆里等下次分配捡到
void wipeWide(std::wstring& text) { wipeWideString(text); }

// ---------------------------------------------------------------- 口令存放位置
//
// 口令只有一处持久化位置：密码保险库（vault.dat）。未解锁时读不到条目，
// 界面上的提示文字都由下面几个函数生成，避免写成“已保存”却其实还没解锁。

bool vaultFileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// 保险库路径在进程生命周期内不会变，缓存一次：会话管理窗口每次重绘都会问
// “口令现在存在哪儿”，没必要每次重建路径。
const std::wstring& vaultPath() {
    static const std::wstring path = Vault::defaultPath();
    return path;
}

bool vaultFilePresent() { return vaultFileExists(vaultPath()); }

// 已保存口令的实际位置（信息性文案）：锁定时 entry 读不出来，只能靠文件是否存在
// 判断，所以要顺带提醒「需要先解锁」，否则用户会以为密码丢了。
std::wstring savedPasswordLocation() {
    if (g_app.vault.unlocked()) return L"密码保险库";
    if (vaultFilePresent()) return L"密码保险库（已锁定，连接前需要先解锁）";
    return L"密码保险库（尚未创建）";
}

// 这一次操作实际会动到的后端：保险库没解锁时不会写任何地方。
std::wstring activePasswordBackend() { return L"密码保险库"; }

std::wstring rememberPasswordCheckboxText() {
    if (g_app.vault.unlocked()) return L"记住密码（保存到密码保险库）";
    if (vaultFilePresent()) return L"记住密码（连接时解锁密码保险库）";
    return L"记住密码（连接时创建密码保险库）";
}

std::wstring rememberKeyPassphraseCheckboxText() {
    if (g_app.vault.unlocked()) return L"记住私钥口令（保存到密码保险库）";
    if (vaultFilePresent()) return L"记住私钥口令（连接时解锁密码保险库）";
    return L"记住私钥口令（连接时创建密码保险库）";
}

// 后面几个函数会用到：有 vault.dat 时弹出解锁对话框，成功返回 true。
bool ensureVaultUnlocked();

// 主菜单的保险库入口：没有文件就新建，有文件且锁定就解锁，已解锁则锁定。
void vaultCommand();

void setStatus(const std::wstring& text, bool error = false) {
    if (foregroundSession()) hideStatusHint();
    g_app.statusText = text;
    g_app.statusError = error && !text.empty();
    g_app.statusUntil = !error && !text.empty() ? GetTickCount64() + 4000 : 0;
    if (foregroundSession() && g_app.status) SetWindowTextW(g_app.status, text.c_str());
    if (g_app.main) InvalidateRect(g_app.main, nullptr, FALSE);
}

bool statusNoticeActive(ULONGLONG now) {
    return !g_app.statusText.empty() && (g_app.statusError || now < g_app.statusUntil);
}

void expireStatusNotices(ULONGLONG now) {
    bool changed = false;
    if (g_app.statusUntil && now >= g_app.statusUntil) {
        g_app.statusText.clear();
        g_app.statusUntil = 0;
        changed = true;
    }
    if (g_app.terminalInfoUntil && now >= g_app.terminalInfoUntil) {
        g_app.terminalInfoUntil = 0;
        changed = true;
    }
    if (changed && g_app.main) InvalidateRect(g_app.main, nullptr, FALSE);
}

// 调试用：设置 RSHELL_TERM_DUMP=<文件> 可把远端原始输出保存下来，便于排查渲染问题
std::ofstream* terminalDump() {
    static std::ofstream out;
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        const char* path = std::getenv("RSHELL_TERM_DUMP");
        if (path && *path) {
            out.open(utf8ToPath(path), std::ios::binary | std::ios::trunc);
        }
    }
    return out.is_open() ? &out : nullptr;
}

// 轻量日志：~/.rshell/gui.log（便于排查连接问题，不含密码）
void guiLog(const std::string& text) {
    std::ofstream out(utf8ToPath(homeDirectory() + "/.rshell/gui.log"),
                      std::ios::binary | std::ios::app);
    if (out) out << isoNow() << "  " << text << "\n";
}

// ---------------------------------------------------------------- 字体与绘制

// 按当前 DPI 缩放逻辑像素
int dp(int value) { return MulDiv(value, g_app.dpi > 0 ? g_app.dpi : 96, 96); }

// 枚举回调：只要命中一次就够
int CALLBACK foundFontProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lParam) {
    *reinterpret_cast<int*>(lParam) = 1;
    return 0;
}

// 字体是否真的装在系统里。
// 不能用 GetTextFaceW 比较名字：中文系统上会返回本地化名（Microsoft YaHei -> 微软雅黑），
// 直接比较会把明明存在的字体误判成缺失。EnumFontFamiliesEx 走字体映射，能吃别名。
bool fontFaceAvailable(HDC dc, const wchar_t* face) {
    LOGFONTW filter{};
    filter.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(filter.lfFaceName, face);
    int found = 0;
    EnumFontFamiliesExW(dc, &filter, foundFontProc, reinterpret_cast<LPARAM>(&found), 0);
    return found != 0;
}

// 创建回退字体：字体没装就返回 nullptr（CreateFontIndirect 会静默换成别的字体）
HFONT createFontChecked(const LOGFONTW& lf, const wchar_t* face) {
    HDC dc = GetDC(g_app.term ? g_app.term : g_app.main);
    const bool available = fontFaceAvailable(dc, face);
    ReleaseDC(g_app.term ? g_app.term : g_app.main, dc);
    if (!available) return nullptr;
    return CreateFontIndirectW(&lf);
}

void createFonts() {
    if (g_app.font) DeleteObject(g_app.font);
    if (g_app.fontBold) DeleteObject(g_app.fontBold);
    if (g_app.uiFont) DeleteObject(g_app.uiFont);
    if (g_app.uiFontBold) DeleteObject(g_app.uiFontBold);
    if (g_app.titleFont) DeleteObject(g_app.titleFont);
    if (g_app.tinyFont) DeleteObject(g_app.tinyFont);
    for (HFONT f : g_app.fallbackFonts) DeleteObject(f);
    for (HFONT f : g_app.fallbackFontsBold) DeleteObject(f);
    g_app.fallbackFonts.clear();
    g_app.fallbackFontsBold.clear();
    g_app.fallbackNames.clear();
    g_app.glyphFontCache.clear();
    g_app.glyphRanges.clear();

    LOGFONTW lf{};
    lf.lfHeight = -dp(g_app.fontSize);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcscpy_s(lf.lfFaceName, L"Consolas");
    g_app.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    g_app.fontBold = CreateFontIndirectW(&lf);

    // 回退字体链：符号 → emoji → 中日韩。Consolas 缺 ❯▶⏵ 之类的 dingbat，
    // 缺字形时若仍用主字体绘制，GDI 只会画 .notdef 方框（看起来像“？”）。
    static const wchar_t* kFallbacks[] = {
        L"Segoe UI Symbol", L"Segoe UI Emoji", L"Microsoft YaHei", L"MS Gothic", L"SimSun"};
    for (const wchar_t* face : kFallbacks) {
        LOGFONTW fb = lf;
        fb.lfWeight = FW_NORMAL;
        fb.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
        wcscpy_s(fb.lfFaceName, face);
        HFONT normal = createFontChecked(fb, face);
        fb.lfWeight = FW_BOLD;
        HFONT boldFace = createFontChecked(fb, face);
        g_app.fallbackFonts.push_back(normal);
        g_app.fallbackFontsBold.push_back(boldFace);
        g_app.fallbackNames.emplace_back(face);
    }

    // 界面字体：Segoe UI（中文自动落到系统字体链接的雅黑），
    // 另外两份用于标题与次级说明，避免所有文字一个粗细没有层次。
    auto makeUiFont = [](int size, int weight, LPCWSTR face) {
        LOGFONTW lf{};
        lf.lfHeight = -dp(size);
        lf.lfWeight = weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, face);
        return CreateFontIndirectW(&lf);
    };
    g_app.uiFont = makeUiFont(13, FW_NORMAL, L"Segoe UI");
    g_app.uiFontBold = makeUiFont(13, FW_SEMIBOLD, L"Segoe UI");
    g_app.titleFont = makeUiFont(14, FW_SEMIBOLD, L"Segoe UI");
    g_app.tinyFont = makeUiFont(11, FW_NORMAL, L"Segoe UI");

    HDC dc = GetDC(g_app.term ? g_app.term : g_app.main);
    HGDIOBJ old = SelectObject(dc, g_app.font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    g_app.cellH = tm.tmHeight > 0 ? tm.tmHeight : g_app.fontSize + 4;
    g_app.ascent = tm.tmAscent;
    SIZE sz{};
    GetTextExtentPoint32W(dc, L"M", 1, &sz);
    g_app.cellW = sz.cx > 0 ? sz.cx : g_app.fontSize / 2;

    // 界面字体的行高：对话框里的单行输入框用它算垂直居中位置
    SelectObject(dc, g_app.uiFont);
    TEXTMETRICW uiTm{};
    GetTextMetricsW(dc, &uiTm);
    g_app.uiTextH = uiTm.tmHeight > 0 ? static_cast<int>(uiTm.tmHeight) : dp(16);

    SelectObject(dc, old);
    ReleaseDC(g_app.term ? g_app.term : g_app.main, dc);
}

// 字体是否包含该码点的字形。
// 注意：不能用 GetGlyphIndicesW —— 它会按 DC 当前的代码页做字符集转换，
// 在内存 DC 上会把中文之类的字符误判成“缺字形”（例如 YaHei 被判成没有“中”）。
// 这里直接读字体的 cmap（GetFontUnicodeRanges），与代码页无关。
bool fontHasGlyph(HDC dc, HFONT font, char32_t ch) {
    if (!font || ch > 0xFFFF) return false;  // 补充平面字符走 emoji 特判
    auto cached = g_app.glyphRanges.find(font);
    if (cached == g_app.glyphRanges.end()) {
        std::vector<std::pair<char32_t, char32_t>> ranges;
        HGDIOBJ old = SelectObject(dc, font);
        const UINT size = GetFontUnicodeRanges(dc, nullptr);
        if (size > 0) {
            std::vector<std::uint8_t> buf(size);
            auto* set = reinterpret_cast<GLYPHSET*>(buf.data());
            if (GetFontUnicodeRanges(dc, set) > 0) {
                for (DWORD i = 0; i < set->cRanges; ++i) {
                    const char32_t lo = set->ranges[i].wcLow;
                    ranges.emplace_back(lo, lo + set->ranges[i].cGlyphs - 1);
                }
            }
        }
        SelectObject(dc, old);
        cached = g_app.glyphRanges.emplace(font, std::move(ranges)).first;
    }
    for (const auto& range : cached->second) {
        if (ch >= range.first && ch <= range.second) return true;
    }
    return false;
}

// 为某个码点选择字体（结果缓存），保证 TUI 边框/符号/中文都能正确显示
HFONT fontForChar(HDC dc, char32_t ch, bool bold) {
    if (ch < 0x80) return bold ? g_app.fontBold : g_app.font;
    const std::uint32_t key = static_cast<std::uint32_t>(ch) | (bold ? 0x80000000u : 0u);
    auto it = g_app.glyphFontCache.find(key);
    if (it != g_app.glyphFontCache.end()) return it->second;

    HFONT primary = bold ? g_app.fontBold : g_app.font;
    const std::vector<HFONT>& chain = bold ? g_app.fallbackFontsBold : g_app.fallbackFonts;
    HFONT chosen = nullptr;

    if (ch <= 0xFFFF) {
        if (fontHasGlyph(dc, primary, ch)) chosen = primary;
        for (std::size_t i = 0; !chosen && i < chain.size(); ++i) {
            if (chain[i] && fontHasGlyph(dc, chain[i], ch)) chosen = chain[i];
        }
    } else {
        // 补充平面（emoji）：GetGlyphIndicesW 测不到，按字体职责挑选
        for (std::size_t i = 0; i < chain.size() && i < g_app.fallbackNames.size() && !chosen; ++i) {
            if (chain[i] && g_app.fallbackNames[i].find(L"Emoji") != std::wstring::npos) {
                chosen = chain[i];
            }
        }
    }
    if (!chosen) chosen = primary;  // 都没字形时保留主字体（画出 .notdef，便于暴露缺字）

    g_app.glyphFontCache[key] = chosen;
    return chosen;
}

void destroyBackBuffer() {
    if (g_app.memDC) {
        DeleteDC(g_app.memDC);
        g_app.memDC = nullptr;
    }
    if (g_app.memBmp) {
        DeleteObject(g_app.memBmp);
        g_app.memBmp = nullptr;
    }
    g_app.bmpW = 0;
    g_app.bmpH = 0;
}

bool ensureBackBuffer(HDC ref, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (g_app.memDC && g_app.bmpW == width && g_app.bmpH == height) return true;
    destroyBackBuffer();
    g_app.memDC = CreateCompatibleDC(ref);
    g_app.memBmp = CreateCompatibleBitmap(ref, width, height);
    if (!g_app.memDC || !g_app.memBmp) {
        destroyBackBuffer();
        return false;
    }
    SelectObject(g_app.memDC, g_app.memBmp);
    g_app.bmpW = width;
    g_app.bmpH = height;
    return true;
}

// ---------------------------------------------------------------- 主题与绘制原语
//
// 配色常量已抽到 ui_theme.h，GUI 与 AI 面板共用同一份。

ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusReady = false;

void startGdiplus() {
    if (g_gdiplusReady) return;
    Gdiplus::GdiplusStartupInput input;
    g_gdiplusReady = Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) == Gdiplus::Ok;
}

void stopGdiplus() {
    if (!g_gdiplusReady) return;
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    g_gdiplusReady = false;
}

inline Gdiplus::Color gpColor(std::uint32_t c, int alpha = 255) {
    return Gdiplus::Color(static_cast<BYTE>(alpha), static_cast<BYTE>((c >> 16) & 0xFF),
                          static_cast<BYTE>((c >> 8) & 0xFF), static_cast<BYTE>(c & 0xFF));
}

// 抗锯齿圆角矩形。纯 GDI 的 RoundRect 边缘是硬台阶，DPI 一高就很脏，
// 所以卡片、按钮、胶囊标签统一走 GDI+ 路径。
void fillRoundRect(HDC dc, const RECT& rc, int radius, std::uint32_t color,
                   std::uint32_t borderColor = 0) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    const float r = static_cast<float>(std::min(radius, std::min(w, h) / 2));
    if (!g_gdiplusReady) {
        HBRUSH brush = CreateSolidBrush(toColorRef(color));
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, static_cast<int>(r * 2),
                  static_cast<int>(r * 2));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
        if (borderColor) {
            HPEN pen = CreatePen(PS_SOLID, 1, toColorRef(borderColor));
            HGDIOBJ oldPen2 = SelectObject(dc, pen);
            HGDIOBJ oldBrush2 = SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, static_cast<int>(r * 2),
                      static_cast<int>(r * 2));
            SelectObject(dc, oldBrush2);
            SelectObject(dc, oldPen2);
            DeleteObject(pen);
        }
        return;
    }
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    const float d = r * 2.0f;
    const float x = static_cast<float>(rc.left);
    const float y = static_cast<float>(rc.top);
    if (r <= 0.0f) {
        path.AddRectangle(Gdiplus::RectF(x, y, fw, fh));
    } else {
        path.AddArc(x, y, d, d, 180.0f, 90.0f);
        path.AddArc(x + fw - d, y, d, d, 270.0f, 90.0f);
        path.AddArc(x + fw - d, y + fh - d, d, d, 0.0f, 90.0f);
        path.AddArc(x, y + fh - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }
    Gdiplus::SolidBrush brush(gpColor(color));
    g.FillPath(&brush, &path);
    if (borderColor) {
        Gdiplus::Pen pen(gpColor(borderColor), 1.0f);
        g.DrawPath(&pen, &path);
    }
}

void fillCircle(HDC dc, int centerX, int centerY, int radius, std::uint32_t color) {
    if (radius <= 0) return;
    if (!g_gdiplusReady) {
        HBRUSH brush = CreateSolidBrush(toColorRef(color));
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, centerX - radius, centerY - radius, centerX + radius + 1, centerY + radius + 1);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
        return;
    }
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(gpColor(color));
    g.FillEllipse(&brush, static_cast<float>(centerX - radius), static_cast<float>(centerY - radius),
                  static_cast<float>(radius * 2), static_cast<float>(radius * 2));
}

void fillRect(HDC dc, const RECT& rc, std::uint32_t color) {
    HBRUSH brush = CreateSolidBrush(toColorRef(color));
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

// 1px 分隔线（用填充而非 LineTo，避免画笔对齐抖动）
void drawHLine(HDC dc, int x1, int x2, int y, std::uint32_t color) {
    RECT r{x1, y, x2, y + 1};
    fillRect(dc, r, color);
}

int textWidth(HDC dc, const std::wstring& text, HFONT font) {
    if (text.empty()) return 0;
    HGDIOBJ old = SelectObject(dc, font ? font : g_app.uiFont);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

void drawText(HDC dc, const std::wstring& text, RECT rc, std::uint32_t color, HFONT font,
              UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    if (text.empty()) return;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, toColorRef(color));
    HGDIOBJ old = SelectObject(dc, font ? font : g_app.uiFont);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rc, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

// 省略号截断：长会话名不能撑破卡片
std::wstring ellipsize(HDC dc, const std::wstring& text, HFONT font, int maxWidth) {
    if (text.empty() || maxWidth <= 0) return std::wstring();
    if (textWidth(dc, text, font) <= maxWidth) return text;
    const std::wstring dots = L"…";
    int lo = 0;
    int hi = static_cast<int>(text.size());
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (textWidth(dc, text.substr(0, static_cast<std::size_t>(mid)) + dots, font) <= maxWidth)
            lo = mid;
        else
            hi = mid - 1;
    }
    return text.substr(0, static_cast<std::size_t>(lo)) + dots;
}

// 路径的中间省略：尾部（目录名本身）最有信息量，所以从右往左保留，
// 前面补一个省略号。DT_PATH_ELLIPSIS 只按反斜杠切，Linux 路径用不上。
std::wstring pathEllipsize(HDC dc, const std::wstring& text, HFONT font, int maxWidth) {
    if (text.empty() || maxWidth <= 0) return std::wstring();
    if (textWidth(dc, text, font) <= maxWidth) return text;
    const std::wstring dots = L"…";
    const std::wstring tail = L"/";
    int lo = 0;
    int hi = static_cast<int>(text.size());
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        std::wstring candidate = dots + tail +
                                 text.substr(text.size() - static_cast<std::size_t>(mid));
        if (textWidth(dc, candidate, font) <= maxWidth)
            lo = mid;
        else
            hi = mid - 1;
    }
    if (lo <= 0) return dots;
    std::size_t from = text.size() - static_cast<std::size_t>(lo);
    // 尽量从一个目录分隔符之后开始，免得半截名字；整段都没有分隔符时保持现状
    std::size_t cut = from;
    while (cut > 0 && cut < text.size() && text[cut - 1] != L'/') ++cut;
    if (cut < text.size()) from = cut;
    return dots + tail + text.substr(from);
}

inline std::uint32_t dimColor(std::uint32_t fg, std::uint32_t bg) {
    auto mix = [](std::uint32_t a, std::uint32_t b) { return std::uint32_t((a + b) / 2) & 0xFF; };
    return (mix((fg >> 16) & 0xFF, (bg >> 16) & 0xFF) << 16) |
           (mix((fg >> 8) & 0xFF, (bg >> 8) & 0xFF) << 8) | mix(fg & 0xFF, bg & 0xFF);
}

bool isSelectedLine(std::size_t line, int col) {
    if (!g_app.hasSelection) return false;
    std::size_t from = std::min(g_app.selStartLine, g_app.selEndLine);
    std::size_t to = std::max(g_app.selStartLine, g_app.selEndLine);
    if (line < from || line > to) return false;
    if (from == to) {
        int lo = std::min(g_app.selStartCol, g_app.selEndCol);
        int hi = std::max(g_app.selStartCol, g_app.selEndCol);
        return col >= lo && col < hi;
    }
    if (line == from) {
        int lo = (g_app.selStartLine <= g_app.selEndLine) ? g_app.selStartCol : g_app.selEndCol;
        return col >= lo;
    }
    if (line == to) {
        int hi = (g_app.selStartLine <= g_app.selEndLine) ? g_app.selEndCol : g_app.selStartCol;
        return col < hi;
    }
    return true;
}

void appendUtf16(std::wstring& out, char32_t ch) {
    if (ch < 0x10000) {
        out.push_back(static_cast<wchar_t>(ch));
    } else {
        char32_t v = ch - 0x10000;
        out.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
    }
}

// 组字串里的一个字符：码点、在 UTF-16 串里的下标（GCS_CURSORPOS 的单位）、列宽
struct ImeGlyph {
    char32_t ch = U' ';
    std::size_t unit = 0;
    int width = 1;
};

std::vector<ImeGlyph> decodeImeComposition() {
    std::vector<ImeGlyph> glyphs;
    const std::wstring& text = g_app.imeComposition;
    for (std::size_t i = 0; i < text.size();) {
        char32_t ch = text[i];
        std::size_t next = i + 1;
        if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < text.size()) {
            const char32_t low = text[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                ch = 0x10000 + ((ch - 0xD800) << 10) + (low - 0xDC00);
                next = i + 2;
            }
        }
        ImeGlyph g;
        g.ch = ch;
        g.unit = i;
        // 组字串里的拼音/汉字按终端列宽算，宽字符占两列
        g.width = std::max(1, Terminal::charWidth(ch));
        glyphs.push_back(g);
        i = next;
    }
    return glyphs;
}

// 输入法组字串（正在敲的拼音）由终端自己画在光标格上，带下划线表示“尚未提交”；
// 光标用终端自己的方块光标，跟着 IME 报告的组字位置一起闪。
// 这样光标不会被输入法的组字窗口盖住，字高行高也与网格完全对齐。
void drawImeComposition(HDC mem, int padX, int padY, int cw, int ch) {
    Terminal& term = g_app.termData;
    const int cols = term.cols();
    const int x0 = padX + term.cursorCol() * cw;
    const int y0 = padY + term.cursorRow() * ch;

    const std::vector<ImeGlyph> glyphs = decodeImeComposition();
    std::vector<int> glyphCol;  // 每个字符相对组字串起点的列
    glyphCol.reserve(glyphs.size());
    int total = 0;
    for (const ImeGlyph& g : glyphs) {
        glyphCol.push_back(total);
        total += g.width;
    }

    // 组字光标停在哪个字符前面（GCS_CURSORPOS 是 UTF-16 下标）
    int caretCol = total;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        if (static_cast<int>(glyphs[i].unit) >= g_app.imeCursorPos) {
            caretCol = glyphCol[i];
            break;
        }
    }

    SetBkMode(mem, TRANSPARENT);
    SetTextAlign(mem, TA_LEFT | TA_BASELINE);
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const ImeGlyph& g = glyphs[i];
        const int col = glyphCol[i];
        if (col >= cols) break;
        const int x = x0 + col * cw;
        const int w = std::min(g.width, cols - col) * cw;
        fillRect(mem, RECT{x, y0, x + w, y0 + ch}, term.theme().bg);

        std::wstring one;
        appendUtf16(one, g.ch);
        std::vector<INT> dx(one.size(), static_cast<INT>(w));
        HFONT f = fontForChar(mem, g.ch, false);
        HGDIOBJ oldFont = SelectObject(mem, f ? f : g_app.font);
        SetTextColor(mem, toColorRef(term.theme().fg));
        ExtTextOutW(mem, x, y0 + g_app.ascent, 0, nullptr, one.c_str(),
                    static_cast<UINT>(one.size()), dx.data());
        // 下划线：和真实终端一致，表示这段还没提交
        HPEN pen = CreatePen(PS_SOLID, 1, toColorRef(term.theme().fg));
        HGDIOBJ oldPen = SelectObject(mem, pen);
        MoveToEx(mem, x, y0 + ch - 2, nullptr);
        LineTo(mem, x + w, y0 + ch - 2);
        SelectObject(mem, oldPen);
        DeleteObject(pen);
        SelectObject(mem, oldFont);
    }

    // 闪烁熄灭的那半个周期不画方块，光标看起来才是在闪
    if (!g_app.cursorOn) return;
    const int caret = std::clamp(caretCol, 0, std::max(0, cols - 1));
    const int cx = x0 + caret * cw;
    fillRect(mem, RECT{cx, y0, cx + cw, y0 + ch}, term.theme().cursor);
    // 方块里的字符用底色再描一遍，否则光标格是空的
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const ImeGlyph& g = glyphs[i];
        const int col = glyphCol[i];
        if (caret < col || caret >= col + g.width) continue;
        std::wstring one;
        appendUtf16(one, g.ch);
        std::vector<INT> dx(one.size(), static_cast<INT>(g.width * cw));
        HFONT f = fontForChar(mem, g.ch, false);
        HGDIOBJ oldFont = SelectObject(mem, f ? f : g_app.font);
        SetTextColor(mem, toColorRef(term.theme().bg));
        ExtTextOutW(mem, x0 + col * cw, y0 + g_app.ascent, 0, nullptr, one.c_str(),
                    static_cast<UINT>(one.size()), dx.data());
        SelectObject(mem, oldFont);
        break;
    }
}

// 把终端网格绘制到指定 DC（WM_PAINT 与离屏渲染工具 --render 共用同一份逻辑）
void drawTerminalGrid(HDC mem, int width, int height, bool focused, bool chrome = false) {
    Terminal& term = g_app.termData;
    const int cols = term.cols();
    const int cw = g_app.cellW;
    const int ch = g_app.cellH;
    // chrome=true：窗口内绘制，终端是一张圆角卡片，四周留白、右侧留滚动条；
    // chrome=false：--render 离屏出图，保持 1:1 纯网格，便于回归比对。
    const int padX = chrome ? g_app.termPadX : 0;
    const int padY = chrome ? g_app.termPadY : 0;
    const int sbW = chrome ? g_app.scrollbarW : 0;
    const int rows = std::max(1, (height - padY * 2) / (ch > 0 ? ch : 1));

    if (chrome) {
        fillRect(mem, RECT{0, 0, width, height}, ui::bgApp);
        fillRoundRect(mem, RECT{0, 0, width - 1, height - 1}, dp(10), term.theme().bg, ui::border);
    } else {
        fillRect(mem, RECT{0, 0, width, height}, term.theme().bg);
    }

    const std::size_t history = term.scrollbackCount();
    const int offset = std::min<int>(term.scrollOffset(), static_cast<int>(history));
    const std::size_t firstLine = history - static_cast<std::size_t>(offset);
    std::vector<std::uint32_t> searchColors;
    if (chrome && g_app.searchOpen && !g_app.searchMatches.empty()) {
        searchColors.resize(static_cast<std::size_t>(rows * cols));
        auto it = std::lower_bound(g_app.searchMatches.begin(), g_app.searchMatches.end(), firstLine,
                                  [](const TerminalMatch& m, std::size_t line) { return m.line < line; });
        for (; it != g_app.searchMatches.end() && it->line < firstLine + rows; ++it) {
            const bool current = it - g_app.searchMatches.begin() == g_app.searchIndex;
            for (int col = it->column; col < std::min(cols, it->column + it->width); ++col)
                searchColors[(it->line - firstLine) * cols + col] = current ? ui::warn : 0x514326;
        }
    }
    auto searchColor = [&](int row, int col) -> std::uint32_t {
        return searchColors.empty() ? 0 : searchColors[static_cast<std::size_t>(row * cols + col)];
    };
    SetBkMode(mem, TRANSPARENT);
    // 基线对齐：不同回退字体的 ascent 不同，按基线绘制才能保证同一行文字上下对齐
    SetTextAlign(mem, TA_LEFT | TA_BASELINE);

    for (int row = 0; row < rows; ++row) {
        const std::size_t lineIndex = firstLine + static_cast<std::size_t>(row);
        const std::vector<Cell>& cells = term.lineAt(lineIndex);
        if (cells.empty()) continue;
        const int y = row * ch;
        const int ty = y + g_app.ascent;

        int col = 0;
        while (col < cols) {
            std::uint32_t bg = term.theme().bg;
            if (col < static_cast<int>(cells.size())) {
                const Cell& c = cells[static_cast<std::size_t>(col)];
                bg = (c.flags & kFlagReverse) ? c.fg : c.bg;
            }
            if (searchColor(row, col)) bg = searchColor(row, col);
            if (isSelectedLine(lineIndex, col)) bg = term.theme().selection;
            int end = col + 1;
            while (end < cols) {
                std::uint32_t other = term.theme().bg;
                if (end < static_cast<int>(cells.size())) {
                    const Cell& c = cells[static_cast<std::size_t>(end)];
                    other = (c.flags & kFlagReverse) ? c.fg : c.bg;
                }
                if (searchColor(row, end)) other = searchColor(row, end);
                if (isSelectedLine(lineIndex, end)) other = term.theme().selection;
                if (other != bg) break;
                ++end;
            }
            SetBkColor(mem, toColorRef(bg));
            RECT r{padX + col * cw, padY + y, padX + end * cw, padY + y + ch};
            ExtTextOutW(mem, padX + col * cw, padY + y, ETO_OPAQUE, &r, L"", 0, nullptr);
            col = end;
        }

        col = 0;
        while (col < cols && col < static_cast<int>(cells.size())) {
            const Cell& first = cells[static_cast<std::size_t>(col)];
            std::uint32_t fg = (first.flags & kFlagReverse) ? first.bg : first.fg;
            if (searchColor(row, col)) fg = searchColor(row, col) == ui::warn ? 0x10141D : ui::text;
            if (isSelectedLine(lineIndex, col)) fg = term.theme().fg;
            const std::uint8_t flags = first.flags;
            const bool bold = (flags & kFlagBold) != 0;
            std::wstring text;
            std::vector<INT> dx;
            const int startCol = col;
            int end = col;
            HFONT runFont = nullptr;  // 本段实际使用的字体（缺字形时会切到回退字体）
            while (end < cols && end < static_cast<int>(cells.size())) {
                const Cell& c = cells[static_cast<std::size_t>(end)];
                std::uint32_t cfg = (c.flags & kFlagReverse) ? c.bg : c.fg;
                if (searchColor(row, end)) cfg = searchColor(row, end) == ui::warn ? 0x10141D : ui::text;
                if (isSelectedLine(lineIndex, end)) cfg = term.theme().fg;
                const bool sameStyle = (cfg == fg) &&
                                       ((c.flags & ~kFlagWideTail) == (flags & ~kFlagWideTail));
                if (end > startCol && !sameStyle) break;
                if (!(c.flags & kFlagWideTail)) {
                    const char32_t ch32 = c.ch == 0 ? U' ' : c.ch;
                    // 字体也要参与分段：Consolas 没有 ❯/▶/⏵ 和中文，必须换成有字形的字体
                    HFONT want = fontForChar(mem, ch32, bold);
                    if (end > startCol && want != runFont) break;
                    runFont = want;
                    const std::size_t before = text.size();
                    appendUtf16(text, ch32);
                    const int cellSpan = (c.flags & kFlagWide) ? 2 : 1;
                    dx.push_back(static_cast<INT>(cellSpan * cw));
                    for (std::size_t k = before + 1; k < text.size(); ++k) dx.push_back(0);
                }
                ++end;
            }
            if (!text.empty()) {
                SetTextColor(mem, toColorRef(fg));
                HGDIOBJ oldFont =
                    SelectObject(mem, runFont ? runFont : (bold ? g_app.fontBold : g_app.font));
                // 显式给出每个字符的步进，保证与字符网格严格对齐：
                // 让 GDI 自己累加字宽会在长行上产生亚像素漂移，TUI 的边框会错位。
                ExtTextOutW(mem, padX + startCol * cw, padY + ty, 0, nullptr, text.c_str(),
                            static_cast<UINT>(text.size()), dx.data());
                if (flags & kFlagUnderline) {
                    HPEN pen = CreatePen(PS_SOLID, 1, toColorRef(fg));
                    HGDIOBJ oldPen = SelectObject(mem, pen);
                    MoveToEx(mem, padX + startCol * cw, padY + y + ch - 2, nullptr);
                    LineTo(mem, padX + end * cw, padY + y + ch - 2);
                    SelectObject(mem, oldPen);
                    DeleteObject(pen);
                }
                SelectObject(mem, oldFont);
            }
            col = end;
        }
    }

    // 光标 / 输入法组字串：正在组字时先画组字覆盖层，光标跟着 IME 的组字位置闪
    if (focused && offset == 0 && !g_app.imeComposition.empty()) {
        drawImeComposition(mem, padX, padY, cw, ch);
    } else if (focused && g_app.cursorOn && term.cursorVisible() && offset == 0) {
        const int cx = padX + term.cursorCol() * cw;
        const int cy = padY + term.cursorRow() * ch;
        RECT cr{cx, cy, cx + cw, cy + ch};
        HBRUSH brush = CreateSolidBrush(toColorRef(term.theme().cursor));
        FillRect(mem, &cr, brush);
        DeleteObject(brush);
        wchar_t glyph[2] = {L' ', 0};
        const std::vector<Cell>& cells =
            term.lineAt(history + static_cast<std::size_t>(term.cursorRow()));
        if (term.cursorCol() < static_cast<int>(cells.size())) {
            char32_t c = cells[static_cast<std::size_t>(term.cursorCol())].ch;
            if (c > 0 && c < 0x10000) glyph[0] = static_cast<wchar_t>(c);
        }
        SetTextColor(mem, toColorRef(term.theme().bg));
        HFONT cf = fontForChar(mem, static_cast<char32_t>(glyph[0]), false);
        HGDIOBJ oldFont = SelectObject(mem, cf ? cf : g_app.font);
        ExtTextOutW(mem, cx, cy + g_app.ascent, 0, nullptr, glyph, 1, nullptr);
        SelectObject(mem, oldFont);
    }

    // 自绘滚动条：只在有历史输出时出现，滑块位置从底部算（offset=0 贴底）
    if (chrome && sbW > 0) {
        const int visible = term.rows();
        const int maxOffset = static_cast<int>(history);
        const int total = visible + maxOffset;
        if (maxOffset > 0 && total > visible) {
            const int trackTop = padY;
            const int trackBottom = height - padY;
            const int trackH = std::max(dp(20), trackBottom - trackTop);
            const int thumbH = std::clamp(trackH * visible / total, dp(26), trackH);
            const int pos = maxOffset - offset;
            const int thumbY = trackTop + (trackH - thumbH) * pos / maxOffset;
            RECT thumb{width - sbW + dp(3), thumbY, width - dp(3), thumbY + thumbH};
            const bool active = g_app.sbHover || g_app.sbDrag;
            fillRoundRect(mem, thumb, sbW / 2,
                          ui::mix(ui::textFaint, term.theme().bg, active ? 0.62 : 0.34));
        }
    }
}

class TerminalDisplayScope {
public:
    TerminalDisplayScope() {
        if (g_app.holdTermPaint && g_app.cwdHookSnapshot) {
            snapshot_ = g_app.cwdHookSnapshot.get();
            snapshot_->resize(g_app.termData.cols(), g_app.termData.rows());
            std::swap(g_app.termData, *snapshot_);
        }
    }
    ~TerminalDisplayScope() { if (snapshot_) std::swap(g_app.termData, *snapshot_); }
    TerminalDisplayScope(const TerminalDisplayScope&) = delete;
    TerminalDisplayScope& operator=(const TerminalDisplayScope&) = delete;
private:
    Terminal* snapshot_ = nullptr;
};

void paintTerminal(HWND hwnd) {
    if (g_app.holdTermPaint && !g_app.cwdHookSnapshot) {
        ValidateRect(hwnd, nullptr);
        return;
    }
    TerminalDisplayScope display;
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0 || !ensureBackBuffer(dc, width, height)) {
        EndPaint(hwnd, &ps);
        return;
    }

    drawTerminalGrid(g_app.memDC, width, height, GetFocus() == hwnd, true);
    // 仅在首次打开的空终端显示引导，保留断开后的全部输出。
    if (g_app.currentSession.empty() && !g_app.connecting && !g_app.shell->running() &&
        g_app.termData.scrollbackCount() == 0 && width >= dp(320) && height >= dp(220)) {
        bool blank = true;
        for (int row = 0; row < g_app.termData.rows() && blank; ++row)
            blank = g_app.termData.plainLine(static_cast<std::size_t>(row)).empty();
        if (blank) {
            HDC mem = g_app.memDC;
            const int saved = SaveDC(mem);
            SetTextAlign(mem, TA_LEFT | TA_TOP);
            fillRoundRect(mem, RECT{0, 0, width - 1, height - 1}, dp(10),
                          g_app.termData.theme().bg, ui::border);
            const int center = width / 2;
            const int top = std::max(dp(20), height / 2 - dp(100));
            const RECT icon{center - dp(26), top, center + dp(26), top + dp(52)};
            fillRoundRect(mem, icon, dp(14), ui::bgActive, ui::border);
            drawText(mem, L">_", icon, ui::accent, g_app.titleFont,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            drawText(mem, L"连接你的下一台主机", RECT{dp(16), top + dp(72), width - dp(16), top + dp(104)},
                     ui::text, g_app.titleFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            drawText(mem, L"选择一个会话，开始 SSH 终端与文件管理。",
                     RECT{dp(16), top + dp(110), width - dp(16), top + dp(136)},
                     ui::textDim, g_app.tinyFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            const RECT shortcut{center - dp(124), top + dp(156), center + dp(124), top + dp(188)};
            fillRoundRect(mem, shortcut, dp(7), ui::bgPanel, ui::borderSoft);
            drawText(mem, L"Ctrl+O  打开会话     Ctrl+N  新建", shortcut,
                     ui::textDim, g_app.tinyFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            RestoreDC(mem, saved);
        }
    }
    BitBlt(dc, 0, 0, width, height, g_app.memDC, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

// 取某个字体实际生效的字面名（用于诊断）
std::wstring faceOfFont(HDC dc, HFONT font) {
    if (!font) return L"<null>";
    HGDIOBJ old = SelectObject(dc, font);
    wchar_t name[LF_FACESIZE] = {0};
    GetTextFaceW(dc, LF_FACESIZE, name);
    SelectObject(dc, old);
    return name;
}

// 离屏渲染：把抓到的原始输出画成 BMP，用于核对渲染结果（含下划线等属性）
bool renderDumpToBmp(const std::wstring& dumpPath, const std::wstring& outPath, int cols,
                     int rows) {
    std::ifstream in(dumpPath, std::ios::binary);
    if (!in) return false;
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty()) return false;

    if (g_app.dpi <= 0) g_app.dpi = wincompat::systemDpi();
    createFonts();
    g_app.termData.reset();
    g_app.termData.resize(cols, rows);
    g_app.termData.feed(data);

    const int width = cols * g_app.cellW;
    const int height = rows * g_app.cellH;
    if (width <= 0 || height <= 0) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;  // 负值 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    drawTerminalGrid(mem, width, height, false);
    GdiFlush();

    // 诊断：RSHELL_FONT_DEBUG=1 时把「码点 -> 实际字体」写到 <out>.fonts.txt，
    // 便于确认 ❯/中文/emoji 究竟用了哪个字体（缺字形时会退回 Consolas 画 .notdef）。
    if (GetEnvironmentVariableW(L"RSHELL_FONT_DEBUG", nullptr, 0) > 0) {
        std::map<std::uint32_t, std::wstring> used;
        const std::size_t history = g_app.termData.scrollbackCount();
        for (int row = 0; row < rows; ++row) {
            const std::vector<Cell>& cells = g_app.termData.lineAt(history + static_cast<std::size_t>(row));
            for (const Cell& cell : cells) {
                if ((cell.flags & kFlagWideTail) || cell.ch < 0x80) continue;
                if (!used.count(cell.ch)) {
                    used[cell.ch] = faceOfFont(mem, fontForChar(mem, cell.ch, false));
                }
            }
        }
        std::ofstream report(std::wstring(outPath) + L".fonts.txt", std::ios::binary | std::ios::trunc);
        for (const auto& entry : used) {
            char buf[64] = {0};
            std::snprintf(buf, sizeof(buf), "U+%04X -> ", entry.first);
            std::string face;
            const int need = WideCharToMultiByte(CP_UTF8, 0, entry.second.c_str(),
                                                 static_cast<int>(entry.second.size()), nullptr, 0,
                                                 nullptr, nullptr);
            if (need > 0) {
                face.resize(static_cast<std::size_t>(need));
                WideCharToMultiByte(CP_UTF8, 0, entry.second.c_str(),
                                    static_cast<int>(entry.second.size()), face.data(), need,
                                    nullptr, nullptr);
            }
            report << buf << face << "\n";
        }
    }


    bool ok = false;
    {
        BITMAPFILEHEADER fh{};
        fh.bfType = 0x4D42;  // 'BM'
        fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fh.bfSize = fh.bfOffBits + static_cast<DWORD>(width) * 4u * static_cast<DWORD>(height);
        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
            out.write(reinterpret_cast<const char*>(&bi.bmiHeader), sizeof(BITMAPINFOHEADER));
            out.write(static_cast<const char*>(bits),
                      static_cast<std::streamsize>(width) * 4 * height);
            ok = static_cast<bool>(out);
        }
    }

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

// ---------------------------------------------------------------- 终端交互

void flushTerminalResponse() {
    std::string resp = g_app.termData.takeResponse();
    if (!resp.empty() && g_app.shell->running()) g_app.shell->write(resp);
}

void sendInput(const std::string& text) {
    if (text.empty()) return;
    if (g_app.cwdHookPhase) {
        constexpr std::size_t limit = 8 * 1024 * 1024;
        if (text.size() > limit - g_app.cwdHookPendingInput.size())
            setStatus(L"初始化期间的待发送队列已满，本次输入未发送", true);
        else g_app.cwdHookPendingInput += text;
        return;
    }
    if (text != "\x1b[I" && text != "\x1b[O") g_app.lastKeyTick = GetTickCount();
    if (g_app.shell->running()) {
        if (!g_app.shell->write(text)) setStatus(L"发送队列已满或连接已断开，本次输入未发送", true);
    } else {
        setStatus(L"尚未连接：请先选择一个会话并连接");
    }
}

void writeWideInput(const std::wstring& text) {
    if (text.empty()) return;
    sendInput(toUtf8(text));
}

// ---------------------------------------------------------------- 鼠标上报

bool mouseReportingActive() { return g_app.termData.mouseMode() != MouseMode::None; }

// button: 0 左键、1 中键、2 右键、64/65 滚轮上/下；press=false 表示松开
void sendMouse(int button, bool press, int x, int y) {
    const int cw = g_app.cellW > 0 ? g_app.cellW : 8;
    const int ch = g_app.cellH > 0 ? g_app.cellH : 18;
    const int cols = std::max(1, g_app.termData.cols());
    const int rows = std::max(1, g_app.termData.rows());
    const int col = std::clamp(x / cw, 0, cols - 1) + 1;
    const int row = std::clamp(y / ch, 0, rows - 1) + 1;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int cb = button | (shift ? 4 : 0) | (alt ? 8 : 0) | (ctrl ? 16 : 0);

    std::string seq;
    if (g_app.termData.mouseSgr()) {
        seq = "\x1b[<" + std::to_string(cb) + ";" + std::to_string(col) + ";" +
              std::to_string(row) + (press ? "M" : "m");
    } else if (g_app.termData.mouseUrxvt()) {
        seq = "\x1b[" + std::to_string(cb + 32) + ";" + std::to_string(col) + ";" +
              std::to_string(row) + "M";
    } else {
        // X10 编码（坐标上限 223）
        seq = "\x1b[M";
        seq.push_back(static_cast<char>(32 + (press ? cb : cb + 3)));
        seq.push_back(static_cast<char>(32 + std::min(col, 223)));
        seq.push_back(static_cast<char>(32 + std::min(row, 223)));
    }
    sendInput(seq);
}

void layoutTerminal(HWND hwnd) {
    if (g_app.searchOpen) g_app.searchDirty = true;
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_app.termPadX = dp(10);
    g_app.termPadY = dp(8);
    g_app.scrollbarW = dp(10);
    // 窗口刚创建/最小化时客户区只有 10x10，此时 resize 会把顶部空白行挤进回滚历史，
    // 表现为一启动就有几十行“历史”并冒出滚动条。低于阈值直接跳过。
    if (rc.right < dp(160) || rc.bottom < dp(120)) return;
    const int cw = g_app.cellW > 0 ? g_app.cellW : 8;
    const int ch = g_app.cellH > 0 ? g_app.cellH : 18;
    const int availW = std::max(dp(40), static_cast<int>(rc.right) - g_app.termPadX * 2 -
                                            g_app.scrollbarW);
    const int availH = std::max(dp(20), static_cast<int>(rc.bottom) - g_app.termPadY * 2);
    int cols = std::max(20, availW / cw);
    int rows = std::max(5, availH / ch);
    if (cols != g_app.termData.cols() || rows != g_app.termData.rows() ||
        g_app.statusFontSize != g_app.fontSize) {
        g_app.statusFontSize = g_app.fontSize;
        g_app.terminalInfoUntil = GetTickCount64() + 2000;
        if (g_app.main) InvalidateRect(g_app.main, nullptr, FALSE);
        g_app.termData.resize(cols, rows);
        if (g_app.shell->running()) g_app.shell->resize(cols, rows);
        if (foregroundSession()) {
            for (auto& state : g_tabs) if (state) {
                state->termData.resize(cols, rows);
                state->searchDirty = state->searchOpen;
                if (state->shell->running()) state->shell->resize(cols, rows);
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

std::size_t lineIndexFromY(int y) {
    const int ch = g_app.cellH > 0 ? g_app.cellH : 18;
    const int row = std::max(0, (y - g_app.termPadY) / ch);
    const std::size_t history = g_app.termData.scrollbackCount();
    const int offset = std::min<int>(g_app.termData.scrollOffset(), static_cast<int>(history));
    long long index = static_cast<long long>(history) - offset + row;
    if (index < 0) index = 0;
    return static_cast<std::size_t>(index);
}

int colFromX(int x) {
    const int cw = g_app.cellW > 0 ? g_app.cellW : 8;
    return std::clamp((x - g_app.termPadX) / cw, 0, g_app.termData.cols() - 1);
}

// 把滚动条的纵向落点换算成回滚行数（0 = 贴底）
int scrollbarTargetOffset(HWND hwnd, int y) {
    const int history = static_cast<int>(g_app.termData.scrollbackCount());
    if (history <= 0) return 0;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int trackTop = g_app.termPadY;
    const int trackH = std::max(dp(20), static_cast<int>(rc.bottom) - g_app.termPadY * 2);
    const int visible = g_app.termData.rows();
    const int rawThumb = trackH * visible / (visible + history);
    const int thumbH = std::min(trackH, std::max(dp(26), rawThumb));
    const int travel = std::max(1, trackH - thumbH);
    const int pos = std::clamp(y - trackTop - thumbH / 2, 0, travel);
    return std::clamp(pos * history / travel, 0, history);
}

// 剪贴板常被别的程序短暂占用（刚在别处复制过就会这样），OpenClipboard 会直接失败，
// 所以这里重试几次；返回 false 让调用方给用户一个明确提示，而不是静默失败。
bool copyTextToClipboard(const std::wstring& text) {
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (OpenClipboard(g_app.main)) {
            EmptyClipboard();
            bool ok = false;
            if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                if (void* dst = GlobalLock(mem)) {
                    std::memcpy(dst, text.c_str(), bytes);
                    GlobalUnlock(mem);
                    ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
                }
                if (!ok) GlobalFree(mem);  // 成功时所有权已交给系统，不能再释放
            }
            CloseClipboard();
            return ok;
        }
        Sleep(12);
    }
    return false;
}

bool clipboardText(std::wstring& out) {
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (OpenClipboard(g_app.main)) {
            bool ok = false;
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* src = static_cast<const wchar_t*>(GlobalLock(data))) {
                    out = src;
                    GlobalUnlock(data);
                    ok = true;
                }
            }
            CloseClipboard();
            return ok;
        }
        Sleep(12);
    }
    return false;
}

void doCopy() {
    if (!g_app.hasSelection) return;
    std::string text = g_app.termData.textRange(g_app.selStartLine, g_app.selStartCol,
                                                g_app.selEndLine, g_app.selEndCol);
    if (text.empty()) return;
    if (!copyTextToClipboard(toWide(text))) {
        setStatus(L"复制失败：剪贴板正被其它程序占用", true);
        return;
    }
    setStatus(L"已复制选中内容（Ctrl+V 粘贴）");
}

void doPaste() {
    std::wstring text;
    if (!clipboardText(text)) {
        setStatus(L"剪贴板里没有文本内容");
        return;
    }
    if (text.empty()) return;

    // 剪贴板里的换行是 CRLF，远端 shell 只要一个回车：normalizePasteText 会把
    // \r\n / \n / \r 统一成 \r，否则 "\r\n" 会变成两次回车，粘贴多行命令时每行
    // 之间都会多出一个空命令（多一次空提示符）。
    const bool bracketed = g_app.termData.bracketedPaste();
    std::string utf8 = normalizePasteText(toUtf8(text), bracketed);
    if (bracketed) utf8 = "\x1b[200~" + utf8 + "\x1b[201~";
    sendInput(utf8);
    // 粘贴时如果正在翻历史，内容会落在看不见的地方，先回到最底部
    if (g_app.termData.scrollOffset() != 0) {
        g_app.termData.scrollToBottom();
        if (g_app.term) InvalidateRect(g_app.term, nullptr, FALSE);
    }
}

void doSelectAll() {
    const std::size_t history = g_app.termData.scrollbackCount();
    g_app.selStartLine = history;
    g_app.selStartCol = 0;
    g_app.selEndLine = history + static_cast<std::size_t>(g_app.termData.rows()) - 1;
    g_app.selEndCol = g_app.termData.cols();
    g_app.hasSelection = true;
    InvalidateRect(g_app.term, nullptr, FALSE);
}

// 方向键/Home/End：应用光标键模式下用 SS3 序列
std::string cursorSequence(const char* normal, const char* application) {
    return g_app.termData.applicationCursorKeys() ? std::string(application)
                                                  : std::string(normal);
}

// CSI 1;<mod>X 形式的修饰键编码（xterm 约定）
std::string modifierSequence(int mod, char finalChar) {
    return "\x1b[1;" + std::to_string(mod) + finalChar;
}

std::string modifierSequence(int mod, int code) {
    return "\x1b[" + std::to_string(code) + ";" + std::to_string(mod) + "~";
}

int modifierCode(bool shift, bool alt, bool ctrl) {
    int mod = 1;
    if (shift) mod += 1;
    if (alt) mod += 2;
    if (ctrl) mod += 4;
    return mod;
}

// 返回 true 表示按键已被处理（不再交给默认窗口过程）
bool handleKeyDown(HWND hwnd, WPARAM key, bool alt) {
    g_app.lastKeyTick = GetTickCount();  // 用户开始输入后，本次不再自动注入初始化命令。
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const int mod = modifierCode(shift, alt, ctrl);
    const bool modified = mod != 1;

    if (alt && key == VK_F4) return false;  // 保留系统 Alt+F4 关闭窗口
    if (ctrl && shift && !alt && key == 'F') {
        g_app.swallowChar = 0x06;
        showTerminalSearch(true);
        return true;
    }
    if (ctrl && shift && !alt && key == 'R') {
        g_app.swallowChar = 0x12;
        PostMessageW(g_app.main, WM_COMMAND, IDM_RECONNECT, 0);
        return true;
    }
    if (ctrl && !alt && key == 'J') {
        // Ctrl+J：打开/切换到全局 AI 标签
        g_app.swallowChar = 0x0A;
        toggleAiPanel();
        return true;
    }
    if (g_app.searchOpen && !ctrl && !alt && (key == VK_F3 || key == VK_ESCAPE)) {
        if (key == VK_F3) stepTerminalSearch(shift ? -1 : 1);
        else { g_app.swallowChar = 0x1B; showTerminalSearch(false); }
        return true;
    }

    if (ctrl && !alt && (key == VK_OEM_PLUS || key == VK_ADD)) {
        g_app.fontSize = std::min(40, g_app.fontSize + 2);
        createFonts();
        layoutTerminal(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (ctrl && !alt && (key == VK_OEM_MINUS || key == VK_SUBTRACT)) {
        g_app.fontSize = std::max(9, g_app.fontSize - 2);
        createFonts();
        layoutTerminal(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (ctrl && !alt && key == '0') {
        g_app.fontSize = 16;
        createFonts();
        layoutTerminal(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (ctrl && shift && !alt && key == 'C') {
        g_app.swallowChar = 0x03;  // 别让 WM_CHAR 再走一遍
        doCopy();
        return true;
    }
    if (ctrl && shift && !alt && key == 'A') {
        g_app.swallowChar = 0x01;
        doSelectAll();
        return true;
    }
    // Ctrl+V / Ctrl+Shift+V 粘贴：Windows Terminal 的习惯，不往远端发 ^V
    if (ctrl && !alt && key == 'V') {
        g_app.swallowChar = 0x16;
        doPaste();
        return true;
    }
    // xterm 系的习惯：Shift+Insert 粘贴、Ctrl+Insert 复制
    if (!ctrl && !alt && shift && key == VK_INSERT) {
        doPaste();
        return true;
    }
    if (ctrl && !alt && !shift && key == VK_INSERT && g_app.hasSelection) {
        doCopy();
        return true;
    }
    // 有选区时 Ctrl+C 先复制（不然用户多半是想复制而不是 ^C），没选区才发中断
    if (ctrl && !alt && !shift && key == 'C' && g_app.hasSelection) {
        g_app.swallowChar = 0x03;  // 复制完不要再把 ^C 发给远端
        doCopy();
        return true;
    }

    switch (key) {
        case VK_UP:
            sendInput(modified ? modifierSequence(mod, 'A') : cursorSequence("\x1b[A", "\x1bOA"));
            return true;
        case VK_DOWN:
            sendInput(modified ? modifierSequence(mod, 'B') : cursorSequence("\x1b[B", "\x1bOB"));
            return true;
        case VK_RIGHT:
            sendInput(modified ? modifierSequence(mod, 'C') : cursorSequence("\x1b[C", "\x1bOC"));
            return true;
        case VK_LEFT:
            sendInput(modified ? modifierSequence(mod, 'D') : cursorSequence("\x1b[D", "\x1bOD"));
            return true;
        case VK_HOME:
            sendInput(modified ? modifierSequence(mod, 'H') : cursorSequence("\x1b[H", "\x1bOH"));
            return true;
        case VK_END:
            sendInput(modified ? modifierSequence(mod, 'F') : cursorSequence("\x1b[F", "\x1bOF"));
            return true;
        case VK_INSERT:
            sendInput(modified ? modifierSequence(mod, 2) : std::string("\x1b[2~"));
            return true;
        case VK_DELETE:
            sendInput(modified ? modifierSequence(mod, 3) : std::string("\x1b[3~"));
            return true;
        case VK_BACK:
            return false;  // 由 WM_CHAR 处理（区分 Backspace / Ctrl+Backspace）
        case VK_TAB:
            // Shift+Tab 是 TUI 常用的反向切换键（如 Cline 的 auto-approve）
            sendInput(shift ? "\x1b[Z" : "\t");
            return true;
        case VK_RETURN:
            sendInput(alt ? "\x1b\r" : "\r");
            return true;
        case VK_ESCAPE:
            sendInput("\x1b");
            return true;
        case VK_SPACE:
            if (ctrl) {
                sendInput(std::string(1, '\0'));
                return true;
            }
            return false;
        case VK_PRIOR:  // PageUp：带修饰键时发给远端，否则翻看历史
            if (modified) {
                sendInput(modifierSequence(mod, 5));
            } else {
                g_app.termData.scrollView(g_app.termData.rows() - 1);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return true;
        case VK_NEXT:
            if (modified) {
                sendInput(modifierSequence(mod, 6));
            } else {
                g_app.termData.scrollView(-(g_app.termData.rows() - 1));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return true;
        case VK_F1:
            sendInput(modified ? modifierSequence(mod, 'P') : std::string("\x1bOP"));
            return true;
        case VK_F2:
            sendInput(modified ? modifierSequence(mod, 'Q') : std::string("\x1bOQ"));
            return true;
        case VK_F3:
            sendInput(modified ? modifierSequence(mod, 'R') : std::string("\x1bOR"));
            return true;
        case VK_F4:
            sendInput(modified ? modifierSequence(mod, 'S') : std::string("\x1bOS"));
            return true;
        case VK_F5:
            sendInput(modified ? modifierSequence(mod, 15) : std::string("\x1b[15~"));
            return true;
        case VK_F6:
            sendInput(modified ? modifierSequence(mod, 17) : std::string("\x1b[17~"));
            return true;
        case VK_F7:
            sendInput(modified ? modifierSequence(mod, 18) : std::string("\x1b[18~"));
            return true;
        case VK_F8:
            sendInput(modified ? modifierSequence(mod, 19) : std::string("\x1b[19~"));
            return true;
        case VK_F9:
            sendInput(modified ? modifierSequence(mod, 20) : std::string("\x1b[20~"));
            return true;
        case VK_F10:
            sendInput(modified ? modifierSequence(mod, 21) : std::string("\x1b[21~"));
            return true;
        case VK_F11:
            sendInput(modified ? modifierSequence(mod, 23) : std::string("\x1b[23~"));
            return true;
        case VK_F12:
            sendInput(modified ? modifierSequence(mod, 24) : std::string("\x1b[24~"));
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------- 输入法

// 自绘终端没有系统光标（没有 CreateCaret），IMM 无从得知组字窗口该摆在哪，
// 于是组字串和候选框会落到屏幕左上角 (0,0)。这里把终端光标所在的格子告诉
// 输入法：COMPOSITIONFORM / CANDIDATEFORM 用的都是客户区坐标，与
// drawTerminalGrid 里画光标的那套坐标保持一致。
void updateImeWindow(HWND hwnd) {
    HIMC imc = ImmGetContext(hwnd);
    if (!imc) return;

    const int cw = g_app.cellW > 0 ? g_app.cellW : 8;
    const int ch = g_app.cellH > 0 ? g_app.cellH : 18;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    // 回滚看历史时光标已被推出屏幕：至少把位置夹在客户区内，别让候选框飞出窗口
    const int maxX = std::max(g_app.termPadX, static_cast<int>(rc.right) - cw * 2);
    const int maxY = std::max(g_app.termPadY, static_cast<int>(rc.bottom) - ch);
    const int cx = std::clamp(g_app.termPadX + g_app.termData.cursorCol() * cw,
                              std::min(g_app.termPadX, maxX), maxX);
    const int cy = std::clamp(g_app.termPadY + g_app.termData.cursorRow() * ch,
                              std::min(g_app.termPadY, maxY), maxY);

    // 组字串由终端自绘，这里给输入法留个锚点：部分 IME 仍用它定位自家的辅助窗口
    COMPOSITIONFORM comp{};
    comp.dwStyle = CFS_POINT;
    comp.ptCurrentPos = POINT{cx, cy};
    ImmSetCompositionWindow(imc, &comp);

    // 候选列表：把整段组字串标成「别遮住」，输入法会贴着这块摆，
    // 而不是像 CFS_CANDIDATEPOS 那样直接压在拼音上。
    int compCells = 0;
    for (const ImeGlyph& g : decodeImeComposition()) compCells += g.width;
    const int areaLimit = std::max(cw, static_cast<int>(rc.right) - cx);
    const int areaW = std::min(std::max(cw * 2, compCells * cw), areaLimit);
    CANDIDATEFORM cand{};
    cand.dwIndex = 0;
    cand.dwStyle = CFS_EXCLUDE;
    cand.ptCurrentPos = comp.ptCurrentPos;
    cand.rcArea = RECT{cx, cy, cx + areaW, cy + ch};
    ImmSetCandidateWindow(imc, &cand);

    // 组字串字体跟随终端字体：默认是 9pt 宋体，字高和行高都对不齐
    LOGFONTW lf{};
    if (g_app.font && GetObjectW(g_app.font, sizeof(lf), &lf) == static_cast<int>(sizeof(lf))) {
        lf.lfWeight = FW_NORMAL;
        ImmSetCompositionFontW(imc, &lf);
    }

    ImmReleaseContext(hwnd, imc);
}

// ---------------------------------------------------------------- 终端窗口

LRESULT CALLBACK terminalProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SessionStateScope visible(findSessionState(g_selectedTabId, false));
    switch (msg) {
        case WM_PAINT:
            paintTerminal(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            layoutTerminal(hwnd);
            return 0;
        case WM_SETFOCUS:
            g_app.cursorOn = true;
            // 焦点回到终端时先把输入法的组字/候选位置摆好，避免第一次组字前
            // 输入法仍拿着上一次（或默认的屏幕左上角）坐标
            updateImeWindow(hwnd);
            if (g_app.termData.focusReporting()) sendInput("\x1b[I");
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KILLFOCUS:
            g_app.cursorOn = true;
            // 焦点跑了，组字串不会再有 ENDCOMPOSITION，自己收干净
            g_app.imeComposition.clear();
            g_app.imeCursorPos = 0;
            if (g_app.termData.focusReporting()) sendInput("\x1b[O");
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSELEAVE:
            if (g_app.sbHover) {
                g_app.sbHover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (handleKeyDown(hwnd, wp, false)) return 0;
            break;  // 未处理的按键交给默认窗口过程，供 IMM/TSF 输入法接管
        case WM_SYSKEYDOWN:
            // Alt+方向键等：先尝试发送到远端；Alt+F4 等交给系统
            if (handleKeyDown(hwnd, wp, true)) return 0;
            break;
        case WM_SYSCHAR: {
            // Alt+字符：发 ESC 前缀（readline / TUI 常用）
            wchar_t wc = static_cast<wchar_t>(wp);
            if (wc == 0x08) {  // Alt+Backspace
                sendInput("\x1b\x7f");
                return 0;
            }
            if (wc >= 0x20 && wc != 0x7F) {
                std::wstring one(1, wc);
                sendInput(std::string("\x1b") + toUtf8(one));
                return 0;
            }
            break;
        }
        case WM_CHAR: {
            wchar_t wc = static_cast<wchar_t>(wp);
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            // WM_KEYDOWN 里已经处理过的组合键，把它派生出的 WM_CHAR 吞掉
            if (g_app.swallowChar >= 0) {
                const int swallow = g_app.swallowChar;
                g_app.swallowChar = -1;
                if (static_cast<int>(wc) == swallow) return 0;
            }
            // Ctrl 仍按住且字符是可见字符：这是 Ctrl+数字/符号（缩放用的 Ctrl+0 / Ctrl+=）
            // 派生出来的，不该当普通输入发给远端。带 Alt 的是 AltGr（如德语键盘的 @），
            // 必须放行；0x7F 是 Ctrl+Backspace，要留给下面的分支发 ^H。
            if (ctrl && !alt && wc >= 0x20 && wc != 0x7F) return 0;
            // 这些键已在 WM_KEYDOWN 中处理，避免重复发送
            if (wc == 0x09 || wc == 0x0D || wc == 0x1B) return 0;
            if (wc == 0x20 && ctrl) return 0;
            if (wc == 0x08) {  // Backspace → DEL（终端约定）
                sendInput("\x7f");
                return 0;
            }
            if (wc == 0x7F) {  // Ctrl+Backspace → BS
                sendInput("\x08");
                return 0;
            }
            if (wc == 0x03) {  // Ctrl+C
                if (shift) {
                    doCopy();
                } else {
                    sendInput("\x03");
                }
                return 0;
            }
            if (wc == 0x16) {  // Ctrl+V
                doPaste();
                return 0;
            }
            if (wc == 0x01 && shift) {  // Ctrl+Shift+A
                doSelectAll();
                return 0;
            }
            static wchar_t pendingHigh = 0;
            if (wc >= 0xD800 && wc <= 0xDBFF) {
                pendingHigh = wc;
                return 0;
            }
            if (pendingHigh != 0) {
                std::wstring pair;
                pair.push_back(pendingHigh);
                pair.push_back(wc);
                pendingHigh = 0;
                writeWideInput(pair);
                return 0;
            }
            std::wstring one(1, wc);
            writeWideInput(one);
            return 0;
        }
        case WM_IME_STARTCOMPOSITION:
            // 组字开始：清掉上一次的组字串，把候选框挪到终端光标处。
            // 组字串交给终端自己画（见 drawImeComposition），输入法自己的组字窗口
            // 会被 WM_IME_SETCONTEXT 关掉，否则它正好盖住光标。
            g_app.imeComposition.clear();
            g_app.imeCursorPos = 0;
            updateImeWindow(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_IME_COMPOSITION: {
            HIMC imc = ImmGetContext(hwnd);
            if (imc) {
                if (lp & GCS_COMPSTR) {
                    const LONG bytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
                    if (bytes > 0) {
                        std::wstring text(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
                        ImmGetCompositionStringW(imc, GCS_COMPSTR, text.data(), bytes);
                        // IMM 报的字节数可能带上结尾的 '\0'
                        while (!text.empty() && text.back() == L'\0') text.pop_back();
                        g_app.imeComposition = std::move(text);
                    } else {
                        g_app.imeComposition.clear();
                    }
                    // GCS_CURSORPOS：低字是字符下标，高字是字节下标，只取低字
                    const LONG caret = ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0);
                    g_app.imeCursorPos = static_cast<int>(caret & 0xFFFF);
                }
                // 提交后组字串就没了，立刻退回普通光标
                if (lp & GCS_RESULTSTR) {
                    g_app.imeComposition.clear();
                    g_app.imeCursorPos = 0;
                }
                ImmReleaseContext(hwnd, imc);
            }
            updateImeWindow(hwnd);  // 组字串变长，候选框要跟着让位
            InvalidateRect(hwnd, nullptr, FALSE);
            // 剩下的交给 DefWindowProc：提交的字符串由 IMM 转成后续的 WM_CHAR
            break;
        }
        case WM_IME_ENDCOMPOSITION:
            g_app.imeComposition.clear();
            g_app.imeCursorPos = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_IME_SETCONTEXT:
            // 组字串由终端自绘：去掉 ISC_SHOWUICOMPOSITIONWINDOW，
            // 输入法就不会再开一个组字窗口贴在光标上把光标盖住。
            lp &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (mouseReportingActive()) {
                sendMouse(delta > 0 ? 64 : 65, true, pt.x, pt.y);
                return 0;
            }
            g_app.termData.scrollView(delta > 0 ? 3 : -3);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            // 自绘滚动条：点击/拖动都生效
            {
                RECT clientRc;
                GetClientRect(hwnd, &clientRc);
                if (x >= clientRc.right - g_app.scrollbarW &&
                    g_app.termData.scrollbackCount() > 0) {
                    g_app.sbDrag = true;
                    SetCapture(hwnd);
                    const int target = scrollbarTargetOffset(hwnd, y);
                    g_app.termData.scrollView(target - g_app.termData.scrollOffset());
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (mouseReportingActive() && !shift) {
                SetFocus(hwnd);
                sendMouse(0, true, x, y);
                return 0;
            }
            SetFocus(hwnd);
            SetCapture(hwnd);
            g_app.selecting = true;
            g_app.hasSelection = false;
            g_app.selStartLine = lineIndexFromY(y);
            g_app.selStartCol = colFromX(x);
            g_app.selEndLine = g_app.selStartLine;
            g_app.selEndCol = g_app.selStartCol;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            if (mouseReportingActive()) {
                sendMouse(2, true, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return 0;
            }
            break;
        }
        case WM_MBUTTONDOWN: {
            if (mouseReportingActive()) {
                sendMouse(1, true, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return 0;
            }
            break;
        }
        case WM_MBUTTONUP: {
            if (mouseReportingActive()) {
                sendMouse(1, false, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return 0;
            }
            break;
        }
        case WM_LBUTTONDBLCLK: {
            const int y = GET_Y_LPARAM(lp);
            const std::size_t line = lineIndexFromY(y);
            g_app.selStartLine = line;
            g_app.selEndLine = line;
            g_app.selStartCol = 0;
            g_app.selEndCol = g_app.termData.cols();
            g_app.hasSelection = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int pointerX = GET_X_LPARAM(lp);
            const int pointerY = GET_Y_LPARAM(lp);
            if (g_app.sbDrag) {
                const int target = scrollbarTargetOffset(hwnd, pointerY);
                g_app.termData.scrollView(target - g_app.termData.scrollOffset());
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            {
                RECT clientRc;
                GetClientRect(hwnd, &clientRc);
                const bool over = pointerX >= clientRc.right - g_app.scrollbarW &&
                                  g_app.termData.scrollbackCount() > 0;
                if (over != g_app.sbHover) {
                    g_app.sbHover = over;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
            }
            if (mouseReportingActive() && !g_app.selecting) {
                const MouseMode mode = g_app.termData.mouseMode();
                if (mode == MouseMode::Button || mode == MouseMode::Any) {
                    const int x = GET_X_LPARAM(lp);
                    const int y = GET_Y_LPARAM(lp);
                    const bool left = (wp & MK_LBUTTON) != 0;
                    const bool middle = (wp & MK_MBUTTON) != 0;
                    const bool right = (wp & MK_RBUTTON) != 0;
                    if (left) {
                        sendMouse(32 + 0, true, x, y);  // 拖动
                    } else if (middle) {
                        sendMouse(32 + 1, true, x, y);
                    } else if (right) {
                        sendMouse(32 + 2, true, x, y);
                    } else if (mode == MouseMode::Any) {
                        sendMouse(35, true, x, y);  // 无按键移动
                    }
                }
                return 0;
            }
            if (!g_app.selecting) return 0;
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            g_app.selEndLine = lineIndexFromY(y);
            g_app.selEndCol = colFromX(x);
            g_app.hasSelection =
                (g_app.selStartLine != g_app.selEndLine || g_app.selStartCol != g_app.selEndCol);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_app.sbDrag) {
                g_app.sbDrag = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_app.selecting) {
                g_app.selecting = false;
                ReleaseCapture();
                if (g_app.hasSelection) {
                    doCopy();  // 选中即复制（Windows 终端习惯），提示由 doCopy 给
                }
            } else if (mouseReportingActive()) {
                sendMouse(0, false, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            if (mouseReportingActive()) {
                sendMouse(2, false, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return 0;
            }
            POINT pt;
            GetCursorPos(&pt);
            openTerminalMenu(hwnd, pt);
            return 0;
        }
        case WM_CONTEXTMENU:
            if (GET_X_LPARAM(lp) == -1 && GET_Y_LPARAM(lp) == -1) {
                POINT pt{g_app.termPadX, g_app.termPadY};
                ClientToScreen(hwnd, &pt);
                openTerminalMenu(hwnd, pt);
            }
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_COPY:
                    doCopy();
                    return 0;
                case IDM_PASTE:
                    doPaste();
                    return 0;
                case IDM_SELECT_ALL:
                    doSelectAll();
                    return 0;
                case IDM_SEARCH:
                case IDM_RECONNECT:
                    PostMessageW(g_app.main, WM_COMMAND, wp, lp);
                    return 0;
                default:
                    return 0;
            }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 会话与连接

// 当前会话就是标题栏显示的那个（g_app.currentSession 是名字）
const Session* selectedSession() { return g_app.store.find(g_app.currentSession); }

// 自绘按钮的可用状态由按钮自己算，这里只负责触发重绘
void updateButtons() {
    if (!foregroundSession()) return;
    if (!g_app.fsReady && g_app.fsPathEdit) ShowWindow(g_app.fsPathEdit, SW_HIDE);
    if (g_app.main) InvalidateRect(g_app.main, nullptr, FALSE);
    if (g_app.list) InvalidateRect(g_app.list, nullptr, FALSE);
}

void endCwdHookFreeze() {
    if (!g_app.holdTermPaint) return;
    if (g_app.cwdHookSnapshot) {
        g_app.termData.restoreDisplay(std::move(*g_app.cwdHookSnapshot));
        g_app.cwdHookSnapshot.reset();
    }
    g_app.holdTermPaint = false;
    if (g_app.term) InvalidateRect(g_app.term, nullptr, FALSE);
}

void stopCwdHook() {
    if (g_app.main) {
        killCwdTimer(kCwdHookTimer1);
        killCwdTimer(kCwdHookTimer2);
        killCwdTimer(kCwdHookTimer3);
        killCwdTimer(kCwdHookTimer4);
    }
    g_app.cwdHookTries = 0;
    g_app.cwdHookStartLine = 0;
    g_app.cwdHookPhase = 0;
    g_app.cwdHookAcked = false;
    g_app.cwdHookDeadline = 0;
    g_app.cwdHookPendingInput.clear();
    g_app.cwdHookHistoryTag.clear();
    endCwdHookFreeze();  // 断线 / 取消注入时画面不能一直冻着
}

std::string cwdHookAckCommand(int phase) {
    return "printf '\\033]777;rshell-cwd;" + std::to_string(g_app.connectionId) + ":" +
        std::to_string(phase) + "\\007'";
}

std::string cwdHookInput(const std::string& command, int phase) {
    if (g_app.cwdHookHistoryTag.empty()) {
        GUID guid{}; wchar_t text[40]{};
        if (SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, text, 40))
            g_app.cwdHookHistoryTag = toUtf8(std::wstring(text + 1, 36));
        else g_app.cwdHookHistoryTag = std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()) + "-" + std::to_string(g_app.connectionId);
    }
    return buildCwdHookInput(command, g_app.cwdHookHistoryTag + "-" + std::to_string(phase));
}

void finishCwdHook(bool timedOut = false) {
    std::string pending = std::move(g_app.cwdHookPendingInput);
    const bool started = g_app.cwdHookStarted != 0;
    const auto elapsed = GetTickCount64() - g_app.cwdHookStarted;
    stopCwdHook();
    if (!pending.empty()) sendInput(pending);
    if (timedOut) {
        guiLog("目录跟随初始化超时，已恢复终端输入");
        setStatus(L"目录跟随初始化超时，已恢复终端输入", true);
    } else if (started) guiLog("目录跟随初始化完成（" + std::to_string(elapsed) + " ms）");
}

void abortCwdHook() {
    if (g_app.shell->running()) g_app.shell->write(cwdHookInput("stty echo", 4) + "\r");
    finishCwdHook(true);
}

void handleCwdHookAck(const std::string& ack) {
    if (!g_app.cwdHookPhase || ack != std::to_string(g_app.connectionId) + ":" +
        std::to_string(g_app.cwdHookPhase)) return;
    if (g_app.cwdHookPhase == 1) {
        g_app.cwdHookAcked = true;
        setCwdTimer(kCwdHookTimer2, 40);
    } else if (g_app.cwdHookPhase == 2) {
        killCwdTimer(kCwdHookTimer3);
        g_app.cwdHookPhase = 3;
        g_app.cwdHookAcked = false;
        g_app.shell->write(cwdHookInput(buildCwdHookRestoreEcho() + "; " + cwdHookAckCommand(3), 3) + "\r");
        setCwdTimer(kCwdHookTimer4, 40);
    } else {
        g_app.cwdHookAcked = true;
        setCwdTimer(kCwdHookTimer4, 40);
    }
}

// 远端确认关回显后再安装钩子，不再凭固定网络延时猜测命令完成时间。
bool sendCwdHookScript() {
    // 数出"注入起点 → 当前光标"差了几行：第 1 步必然被 tty 回显，
    // 远端可能因此多出 1~N 行（多行提示符、PROMPT_COMMAND 打印、shell 报错都算），
    // 所以擦除不能写死"上移一行"。
    const long long nowLine =
        static_cast<long long>(g_app.termData.scrollbackCount()) + g_app.termData.cursorRow();
    const int erase = std::clamp(static_cast<int>(nowLine - g_app.cwdHookStartLine), 1,
                                 std::max(1, g_app.termData.cursorRow()));
    // 擦除序列和钩子本体都在 util.cpp 的 buildCwdHookScript 里，这里只补一个回车
    return g_app.shell->write(cwdHookInput(buildCwdHookScript(erase) + "; " + cwdHookAckCommand(2), 2) + "\r");
}

void disconnectSession() {
    stopCwdHook();
    g_app.connectionId = g_nextConnectionId++;
    if (g_app.shell->running() || g_app.connecting) {
        if (g_app.connecting) g_app.shell = std::make_shared<SshShell>();
        else g_app.shell->close();
        g_app.connecting = false;
        setStatus(L"已断开连接");
        g_app.termData.feed("\r\n[已主动断开连接]\r\n");
        g_app.searchDirty = g_app.searchOpen;
        InvalidateRect(g_app.term, nullptr, FALSE);
    }
    stopRemoteFs();
    g_app.currentPassword.clear();
    g_app.currentKeyPassphrase.clear();
    g_app.pendingCredentialTarget.clear();
    g_app.pendingCredentialSession.clear();
    g_app.pendingKeyCredentialTarget.clear();
    g_app.pendingKeyCredentialSession.clear();
    layoutMain(g_app.main);
    updateButtons();
}

// 把会话里的代理配置套到连接参数上；返回 false 表示配置无法解析。
// 代理解析失败时调用方必须中止连接，绝不能悄悄退回直连。
bool applySessionProxy(ConnectionOptions& options, const Session& session, std::string& error) {
    if (session.proxyUrl.empty()) return true;
    ProxyConfig proxy;
    if (!parseProxyUrl(session.proxyUrl, proxy, error)) return false;
    proxy.user = session.proxyUser;
    if (session.saveProxyPassword) {
        std::string stored;
        if (loadCredential(proxyPasswordTarget(session.name), stored)) {
            proxy.password = stored;
            wipeString(stored);
        }
    }
    options.proxy = proxy;
    return true;
}

void connectToSession(const Session& session, const std::wstring& password,
                      const std::wstring& keyPassphrase) {
    if (g_app.connecting) return;
    if (g_app.shell->running()) disconnectSession();
    stopCwdHook();
    g_app.shellGotOutput = false;
    g_app.lastShellOutputTick = g_app.lastKeyTick = 0;
    g_app.connectionId = g_nextConnectionId++;
    const UINT_PTR connectionId = g_app.connectionId;
    g_app.shell->close();  // 等旧工作线程结束后才能替换回调。

    ConnectionOptions options;
    options.host = session.host;
    options.port = session.port > 0 ? session.port : 22;
    options.user = session.user;
    options.keyPath = session.keyPath;
    // toUtf8 会产生一份 UTF-8 明文，用完立刻抹掉（options 里那份由
    // ConnectionOptions 的析构函数负责）
    std::string utf8Password = toUtf8(password);
    options.password = utf8Password;
    wipeString(utf8Password);
    std::string utf8KeyPassphrase = toUtf8(keyPassphrase);
    options.keyPassphrase = utf8KeyPassphrase;
    wipeString(utf8KeyPassphrase);
    options.timeoutSec = 20;
    options.allowDefaultKeys = true;

    if (session.keepAliveSec > 0)
        options.keepAliveSec = std::clamp(session.keepAliveSec, 5, 3600);

    std::string proxyError;
    if (!applySessionProxy(options, session, proxyError)) {
        messageWarn(L"代理配置无效",
                    L"会话 “" + toWide(session.name) + L"” 的代理地址无法解析：" + toWide(proxyError));
        setStatus(L"代理配置无效", true);
        return;
    }

    if (options.user.empty()) {
        if (const char* env = std::getenv("USERNAME")) options.user = env;
    }

    HWND mainWindow = g_app.main;
    options.confirmHostKey = [mainWindow, connectionId](const std::string& hostField,
                                          const std::string& fingerprint,
                                          const std::string& keyType) {
        guiLog("未知主机密钥 " + hostField + " " + keyType + " " + fingerprint);
        (void)mainWindow;
        // 连接发生在工作线程：SendMessage 会把询问同步到 UI 线程的自绘对话框
        return askHostKey(hostField, keyType, fingerprint, connectionId);
    };

    auto shell = g_app.shell;
    const auto runtime = g_app.runtime;
    shell->setCallbacks(ShellCallbacks{
        [mainWindow, connectionId](std::string data) {
            auto* payload = new std::string(std::move(data));
            if (!PostMessageW(mainWindow, WM_SHELL_DATA, connectionId, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        },
        [mainWindow, connectionId, source = shell.get()]() {
            auto* reason = new std::string(source->closedReason());
            if (!PostMessageW(mainWindow, WM_SHELL_CLOSED, connectionId, reinterpret_cast<LPARAM>(reason)))
                delete reason;
        }});

    g_app.connecting = true;
    g_app.currentSession = session.name;
    g_app.currentPassword.assign(password);
    g_app.currentKeyPassphrase.assign(keyPassphrase);
    g_app.termData.reset();
    g_app.hasSelection = false;
    g_app.searchMatches.clear();
    g_app.searchIndex = -1;
    g_app.searchDirty = g_app.searchOpen;
    guiLog("开始连接 " + options.user + "@" + options.host + ":" +
           std::to_string(options.port) + " key=" + options.keyPath +
           (options.password.empty()
                ? (keyPassphrase.empty() ? " (无密码)" : " (使用私钥口令)")
                : " (使用密码)"));
    setStatus(L"正在连接 " + toWide(session.host) + L" …");
    layoutMain(g_app.main);
    updateButtons();

    const int cols = g_app.termData.cols();
    const int rows = g_app.termData.rows();
    std::thread([mainWindow, connectionId, options, cols, rows, shell, runtime]() mutable {
        try {
            shell->start(options, "xterm-256color", cols, rows);
            guiLog("连接成功，已打开 shell 通道");
            PostMessageW(mainWindow, WM_CONNECT_OK, connectionId, 0);
        } catch (const std::exception& ex) {
            guiLog(std::string("连接失败：") + ex.what());
            auto* message = new std::string(ex.what());
            if (!PostMessageW(mainWindow, WM_CONNECT_FAIL, connectionId, reinterpret_cast<LPARAM>(message))) {
                delete message;
            }
        }
        shell.reset();  // 在 runtime 的生命周期内释放可能已被关闭标签放弃的连接。
    }).detach();
}

// ---------------------------------------------------------------- 对话框

// 对话框外观：自绘无边框卡片（圆角 + 暗色 + 自绘按钮/输入框），
// 与主窗口同一套配色，不用系统 MessageBox 的浅色外壳。
namespace dlg {

constexpr std::uint32_t bgCard = ui::bgPanel;  // 卡片底色
constexpr std::uint32_t bgInput = 0x0E1723;    // 输入框底色
constexpr std::uint32_t bgBtn = ui::bgCard;   // 次级按钮

HBRUSH editBrush() {
    static HBRUSH brush = nullptr;
    if (!brush) brush = CreateSolidBrush(toColorRef(bgInput));
    return brush;
}

HBRUSH cardBrush() {
    static HBRUSH brush = nullptr;
    if (!brush) brush = CreateSolidBrush(toColorRef(bgCard));
    return brush;
}

// 按钮的 hover 状态（owner-draw 拿不到鼠标是否停留，得自己记）
struct ButtonState {
    bool hover = false;
    bool primary = false;
};
std::unordered_map<HWND, ButtonState> g_buttons;

// 所有 EDIT / BUTTON 共用一个原过程指针（各自是同一套内建窗口类）
WNDPROC g_editProc = nullptr;
WNDPROC g_buttonProc = nullptr;

// 输入框的圆角外框矩形（键 = EDIT 的 HWND）。EDIT 的实际高度只有一行字，
// 不能拿窗口矩形反推外框，所以建控件时就记下来。
std::unordered_map<HWND, RECT> g_editFrames;

LRESULT CALLBACK dialogEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCDESTROY:
            g_editFrames.erase(hwnd);  // 控件销毁后别在表里留死句柄
            break;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                return 0;
            }
            if (wp == VK_RETURN) {
                PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                return 0;
            }
            break;
        case WM_CHAR:
            // 回车/ESC 已经在上面的 WM_KEYDOWN 处理过，别再插到文本里
            if (wp == VK_RETURN || wp == VK_ESCAPE) return 0;
            break;
        default:
            break;
    }
    return CallWindowProcW(g_editProc, hwnd, msg, wp, lp);
}

LRESULT CALLBACK dialogButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCDESTROY:
            g_buttons.erase(hwnd);
            break;
        case WM_MOUSEMOVE: {
            auto it = g_buttons.find(hwnd);
            if (it != g_buttons.end() && !it->second.hover) {
                it->second.hover = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            break;
        }
        case WM_MOUSELEAVE: {
            auto it = g_buttons.find(hwnd);
            if (it != g_buttons.end() && it->second.hover) {
                it->second.hover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        default:
            break;
    }
    return CallWindowProcW(g_buttonProc ? g_buttonProc : DefWindowProcW, hwnd, msg, wp, lp);
}

// 把输入框换成深色、去掉系统边框；回车/ESC 交给父窗口
void styleEdit(HWND edit) {
    if (!edit) return;
    if (!g_editProc) g_editProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(edit, GWLP_WNDPROC));
    SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(dialogEditProc));
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(dp(9), dp(9)));
    // EM_SETBKGNDCOLOR = WM_USER + 67（有的 SDK 头没导出这个宏）
    SendMessageW(edit, WM_USER + 67, 0, static_cast<LPARAM>(toColorRef(bgInput)));
}

void styleButton(HWND button, bool primary) {
    if (!button) return;
    if (!g_buttonProc)
        g_buttonProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(button, GWLP_WNDPROC));
    ButtonState state;
    state.primary = primary;
    g_buttons[button] = state;
    SetWindowLongPtrW(button, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(dialogButtonProc));
}

// 画一个自绘按钮（主按钮实心主色，次级按钮描边）
void paintButton(HDC dc, const RECT& rc, const std::wstring& label, bool primary, bool hover,
                 bool pressed, bool focused, bool enabled) {
    std::uint32_t bg = 0;
    std::uint32_t fg = 0;
    std::uint32_t border = 0;
    if (primary) {
        bg = pressed ? ui::actionPressed : (hover ? ui::actionHover : ui::action);
        fg = 0xFFFFFF;
        border = ui::mix(ui::action, ui::accent, 0.35);
        if (!enabled) {
            bg = ui::mix(ui::action, bgCard, 0.65);
            fg = ui::mix(0xFFFFFF, bgCard, 0.45);
            border = ui::borderSoft;
        }
    } else {
        bg = pressed ? ui::mix(bgBtn, 0x000000, 0.2) : (hover ? ui::bgHover : bgBtn);
        fg = enabled ? ui::text : ui::mix(ui::textFaint, bgCard, 0.5);
        border = enabled ? (hover ? ui::accent : ui::border) : ui::borderSoft;
    }
    fillRoundRect(dc, rc, dp(7), bg, border);
    if (focused && enabled) {
        // 只加一圈内描边，不能再用 fillRoundRect 的填充色（会盖成黑色）
        RECT inner = rc;
        InflateRect(&inner, -dp(2), -dp(2));
        fillRoundRect(dc, inner, dp(5), bg, ui::accent);
    }
    drawText(dc, label, rc, fg, g_app.uiFontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// 输入框背后的圆角底板 + 描边（EDIT 本身没有边框，焦点态由这里体现）
void paintInputFrame(HDC dc, const RECT& rc, bool focused) {
    fillRoundRect(dc, rc, dp(6), bgInput, focused ? ui::accent : ui::border);
}

// 圆角窗口区域：无边框窗口若不裁剪，四角会露出直角背景
void applyRoundRegion(HWND hwnd, int width, int height) {
    const int radius = dp(12);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    SetWindowRgn(hwnd, region, TRUE);  // 成功后 region 归系统所有
}

// 每个对话框的关闭按钮 hover 状态
std::unordered_map<HWND, bool> g_closeHover;

void paintFrame(HDC dc, const RECT& rc, const std::wstring& title, const std::wstring& subtitle,
                bool closeHover) {
    fillRoundRect(dc, rc, dp(12), bgCard);

    RECT titleRc{dp(20), dp(12), rc.right - dp(48), dp(36)};
    drawText(dc, title, titleRc, ui::text, g_app.titleFont,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!subtitle.empty()) {
        RECT subRc{dp(20), dp(32), rc.right - dp(48), dp(48)};
        drawText(dc, subtitle, subRc, ui::textDim, g_app.tinyFont,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // 右上角关闭按钮：一个细的 ×
    const int size = dp(28);
    RECT closeRc{rc.right - dp(10) - size, dp(10), rc.right - dp(10), dp(10) + size};
    if (closeHover) fillRoundRect(dc, closeRc, dp(6), ui::mix(ui::danger, bgCard, 0.82));
    const int cx = (closeRc.left + closeRc.right) / 2;
    const int cy = (closeRc.top + closeRc.bottom) / 2;
    const int arm = dp(5);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, dp(1)),
                         toColorRef(closeHover ? 0xFFFFFF : ui::textDim));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, cx - arm, cy - arm, nullptr);
    LineTo(dc, cx + arm + 1, cy + arm + 1);
    MoveToEx(dc, cx - arm, cy + arm, nullptr);
    LineTo(dc, cx + arm + 1, cy - arm - 1);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

int closeButtonSize() { return dp(28); }
int closeButtonLeft(const RECT& rc) { return rc.right - dp(10) - closeButtonSize(); }
int closeButtonTop() { return dp(10); }

// 统一的对话框绘制入口：双缓冲 + 圆角卡片 + 标题
void paintDialog(HWND hwnd, const std::wstring& title, const std::wstring& subtitle,
                 const std::function<void(HDC)>& extra = nullptr) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    fillRect(mem, rc, bgCard);  // 圆角区外用卡片色，避免边缘露白
    paintFrame(mem, rc, title, subtitle, g_closeHover[hwnd]);
    if (extra) extra(mem);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// 对话框窗口：无边框 + 阴影 + 暗色 DWM 边 + 圆角，居中于主窗口
HWND createDialogWindow(const wchar_t* className, const std::wstring& title, int width,
                        int height) {
    // owner 取当前活动窗口（同一线程内）：在会话管理窗口里再弹提示时，
    // 提示框会挂在管理窗口上，禁用的也是它——z 序和模态行为才不会错位。
    HWND ownerWindow = GetActiveWindow();
    if (!ownerWindow || GetWindowThreadProcessId(ownerWindow, nullptr) != GetCurrentThreadId() ||
        ownerWindow == g_app.main) {
        ownerWindow = g_app.main;
    }
    HWND dlg = CreateWindowExW(0, className, title.c_str(),
                               WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, CW_USEDEFAULT,
                               CW_USEDEFAULT, width, height, ownerWindow, nullptr, g_app.inst,
                               nullptr);
    if (!dlg) return nullptr;

    const BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(dlg, 20, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(dlg, 19, &dark, sizeof(dark));
    }
    const int corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(dlg, 33, &corner, sizeof(corner));
    applyRoundRegion(dlg, width, height);

    RECT owner{};
    RECT self{0, 0, width, height};
    if (g_app.main && GetWindowRect(g_app.main, &owner)) {
        const int x = owner.left + ((owner.right - owner.left) - width) / 2;
        const int y = owner.top + ((owner.bottom - owner.top) - height) / 2;
        SetWindowPos(dlg, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }
    (void)self;
    return dlg;
}

// 无边框窗口的标题区可拖动，其余交给子控件
bool isOnCloseButton(HWND hwnd, POINT pt) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    RECT closeRc{closeButtonLeft(rc), closeButtonTop(), rc.right - dp(10),
                 closeButtonTop() + closeButtonSize()};
    return PtInRect(&closeRc, pt) != 0;
}

// 追踪关闭按钮 hover
void trackCloseHover(HWND hwnd, int x, int y) {
    POINT pt{x, y};
    const bool hover = isOnCloseButton(hwnd, pt);
    if (g_closeHover[hwnd] != hover) {
        g_closeHover[hwnd] = hover;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = hwnd;
    TrackMouseEvent(&track);
}

// 输入框和静态文字统一走深色配色
bool handleDialogColor(UINT msg, HDC dc, HWND control, LRESULT& result) {
    switch (msg) {
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            SetTextColor(dc, toColorRef(ui::text));
            SetBkColor(dc, toColorRef(bgInput));
            result = reinterpret_cast<LRESULT>(editBrush());
            return true;
        }
        case WM_CTLCOLORSTATIC: {
            SetTextColor(dc, toColorRef(ui::textDim));
            SetBkColor(dc, toColorRef(bgCard));
            result = reinterpret_cast<LRESULT>(cardBrush());
            (void)control;
            return true;
        }
        default:
            break;
    }
    return false;
}

// owner-draw 按钮：DRAWITEMSTRUCT 直接交给 paintButton
void handleDrawItem(HWND hwnd, const DRAWITEMSTRUCT& dis) {
    if (dis.CtlType != ODT_BUTTON) return;
    wchar_t label[256] = L"";
    GetWindowTextW(dis.hwndItem, label, 256);
    HBRUSH bg = CreateSolidBrush(toColorRef(bgCard));
    FillRect(dis.hDC, &dis.rcItem, bg);
    DeleteObject(bg);

    const auto it = g_buttons.find(dis.hwndItem);
    const bool hover = it != g_buttons.end() && it->second.hover;
    const bool primary = it != g_buttons.end() && it->second.primary;
    paintButton(dis.hDC, dis.rcItem, label, primary, hover,
                (dis.itemState & ODS_SELECTED) != 0, (dis.itemState & ODS_FOCUS) != 0,
                (dis.itemState & ODS_DISABLED) == 0);
    (void)hwnd;
}

}  // namespace dlg

// ---------------------------------------------------------------- 自绘弹出菜单
//
// 系统 TrackPopupMenu 在暗色窗口上会跟随系统浅色主题：菜单被画成一条几乎没有内容的
// 细线（背景白、文字又按暗色取色），既看不清也没法点。这里用自绘弹出窗口实现同样的
// 交互：hover 高亮、右对齐快捷键、上下键/回车/Esc、点空白或失焦关闭。
// 标题栏 ☰ 主菜单和文件区右键菜单共用这一份实现。
namespace popup {

struct Item {
    int id = 0;
    std::wstring text;
    std::wstring shortcut;
    bool separator = false;
    bool danger = false;
    bool enabled = true;
};

struct State {
    HWND hwnd = nullptr;
    std::vector<Item> items;
    int hover = -1;
    int result = 0;
    bool done = false;
};

State g_popup;

constexpr wchar_t kPopupClass[] = L"RshellPopupMenu";

int itemHeight() { return dp(30); }
int separatorHeight() { return dp(11); }
int padY() { return dp(6); }
int padX() { return dp(6); }

int contentHeight(const std::vector<Item>& items) {
    int height = padY() * 2;
    for (const Item& item : items) height += item.separator ? separatorHeight() : itemHeight();
    return height;
}

// 第 index 项的矩形（相对弹出窗口客户区）
RECT itemRect(const RECT& client, int index) {
    int y = padY();
    for (int i = 0; i < index; ++i) {
        const Item& prev = g_popup.items[static_cast<std::size_t>(i)];
        y += prev.separator ? separatorHeight() : itemHeight();
    }
    const Item& item = g_popup.items[static_cast<std::size_t>(index)];
    const int h = item.separator ? separatorHeight() : itemHeight();
    return RECT{padX(), y, client.right - padX(), y + h};
}

int hitItem(HWND hwnd, POINT pt) {
    RECT client;
    GetClientRect(hwnd, &client);
    for (int i = 0; i < static_cast<int>(g_popup.items.size()); ++i) {
        const Item& item = g_popup.items[static_cast<std::size_t>(i)];
        if (item.separator || !item.enabled) continue;
        const RECT rc = itemRect(client, i);
        if (PtInRect(&rc, pt)) return i;
    }
    return -1;
}

void finish(int result) {
    // 结果一旦落定就不再改写：窗口销毁、owner 重新激活都会再收到 WM_ACTIVATE(WA_INACTIVE)，
    // 那次清场消息不能把用户真正选中的 id 覆盖成 0。
    if (g_popup.done) return;
    g_popup.result = result;
    g_popup.done = true;
}

// 上下移动但不落在分隔线/禁用项上
void moveHover(int step) {
    const int n = static_cast<int>(g_popup.items.size());
    int index = g_popup.hover;
    for (int guard = 0; guard < n; ++guard) {
        index = (index + step + n) % n;
        const Item& item = g_popup.items[static_cast<std::size_t>(index)];
        if (!item.separator && item.enabled) {
            g_popup.hover = index;
            return;
        }
    }
}

}  // namespace popup

namespace popup {

void paintPopup(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    if (client.right <= 0 || client.bottom <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    fillRect(mem, client, ui::bgCard);
    fillRoundRect(mem, client, dp(8), ui::bgCard, ui::border);

    for (int i = 0; i < static_cast<int>(g_popup.items.size()); ++i) {
        const Item& item = g_popup.items[static_cast<std::size_t>(i)];
        const RECT rc = itemRect(client, i);
        if (item.separator) {
            drawHLine(mem, rc.left + dp(6), rc.right - dp(6), (rc.top + rc.bottom) / 2,
                      ui::borderSoft);
            continue;
        }
        if (i == g_popup.hover) {
            RECT hi = rc;
            hi.top += dp(1);
            hi.bottom -= dp(1);
            fillRoundRect(mem, hi, dp(6), ui::bgHover);
        }
        const std::uint32_t fg =
            !item.enabled ? ui::textFaint : (item.danger ? ui::danger : ui::text);
        RECT textRc = rc;
        textRc.left += dp(12);
        textRc.right -= dp(12);
        if (!item.shortcut.empty()) {
            const int shortcutW = textWidth(mem, item.shortcut, g_app.tinyFont);
            drawText(mem, item.shortcut,
                     RECT{textRc.right - shortcutW, rc.top, textRc.right, rc.bottom},
                     ui::textFaint, g_app.tinyFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            textRc.right -= shortcutW + dp(16);
        }
        drawText(mem, ellipsize(mem, item.text, g_app.uiFont, textRc.right - textRc.left), textRc,
                 fg, g_app.uiFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK popupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paintPopup(hwnd);
            return 0;
        case WM_MOUSEMOVE: {
            const int index = hitItem(hwnd, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (index != g_popup.hover) {
                g_popup.hover = index;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            // 按下即选中：菜单项不需要“按住拖动”
            const int index = hitItem(hwnd, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            finish(index >= 0 ? g_popup.items[static_cast<std::size_t>(index)].id : 0);
            return 0;
        }
        case WM_KEYDOWN: {
            if (wp == VK_ESCAPE) {
                finish(0);
                return 0;
            }
            if (wp == VK_RETURN || wp == VK_SPACE) {
                if (g_popup.hover >= 0)
                    finish(g_popup.items[static_cast<std::size_t>(g_popup.hover)].id);
                return 0;
            }
            if (wp == VK_DOWN) {
                moveHover(1);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_UP) {
                moveHover(-1);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_HOME || wp == VK_END) {
                g_popup.hover = wp == VK_HOME ? -1 : 0;
                moveHover(wp == VK_HOME ? 1 : -1);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) finish(0);
            return 0;
        case WM_CLOSE:
            finish(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 在 owner 的 screenPt（屏幕坐标）处弹出菜单，返回被点中的 id（0 表示取消）
int run(HWND owner, POINT screenPt, const std::vector<Item>& items) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = popupProc;
        wc.hInstance = g_app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kPopupClass;
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
        registered = true;
    }
    if (items.empty()) return 0;

    g_popup = State();
    g_popup.items = items;

    HDC dc = GetDC(owner ? owner : nullptr);
    int textW = 0;
    int shortcutW = 0;
    for (const Item& item : items) {
        if (item.separator) continue;
        textW = std::max(textW, textWidth(dc, item.text, g_app.uiFont));
        shortcutW = std::max(shortcutW, textWidth(dc, item.shortcut, g_app.tinyFont));
    }
    ReleaseDC(owner, dc);

    const int width = std::max(dp(200), dp(30) + textW + (shortcutW > 0 ? shortcutW + dp(28) : 0));
    const int height = contentHeight(items);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST), &mi);
    int x = screenPt.x;
    int y = screenPt.y;
    if (x + width > mi.rcWork.right) x = mi.rcWork.right - width;
    if (y + height > mi.rcWork.bottom) y = screenPt.y - height;  // 放不下就向上展开
    x = std::max<int>(x, mi.rcWork.left);
    y = std::max<int>(y, mi.rcWork.top);

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPopupClass, L"", WS_POPUP, x,
                                  y, width, height, owner, nullptr, g_app.inst, nullptr);
    if (!window) return 0;
    g_popup.hwnd = window;
    dlg::applyRoundRegion(window, width, height);

    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(window);

    // 自己跑一个小消息循环：菜单关闭前不返回，调用方的命令分发保持同步
    MSG msg;
    while (!g_popup.done && IsWindow(window) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // 先把结果取出来再销毁窗口：销毁会触发 WM_ACTIVATE(WA_INACTIVE) 走一遍清场逻辑，
    // 提前取走才不会被这次清场覆盖成 0。
    const int result = g_popup.result;
    if (IsWindow(window)) DestroyWindow(window);
    g_popup.hwnd = nullptr;
    if (owner && IsWindow(owner)) SetForegroundWindow(owner);
    return result;
}

}  // namespace popup

// ---------------------------------------------------------------- 通用消息框
//
// 系统 MessageBox 是浅色的，和暗色界面割裂；这里用纯自绘窗口实现同样的语义：
// 图标 + 正文 + 可选等宽详情 + 1~2 个按钮，Enter/Esc/方向键都能用。

using namespace dlg;

struct MessageSpec {
    int kind = 0;  // 0=信息 1=警告 2=错误 3=询问
    std::wstring title;
    std::wstring text;
    std::wstring detail;  // 等宽小字，用于指纹、错误详情
    std::vector<std::wstring> buttons{L"确定"};
    int defaultIndex = 0;
};

struct MessageState {
    HWND dlg = nullptr;
    MessageSpec spec;
    int result = -1;
    int hover = -1;
    int focus = 0;
};

MessageState g_message;

constexpr int kMsgBtnW = 96;
constexpr int kMsgBtnH = 32;
constexpr int kMsgBtnGap = 10;
constexpr int kMsgPad = 22;

RECT msgTextRect(const RECT& rc) {
    return RECT{dp(kMsgPad) + dp(46), dp(54), rc.right - dp(kMsgPad), rc.bottom - dp(80)};
}

RECT msgDetailRect(const RECT& rc) {
    RECT text = msgTextRect(rc);
    const int textH = std::max<int>(dp(24), (text.bottom - text.top) / 2);
    return RECT{text.left, text.top + textH + dp(8), text.right, text.bottom};
}

std::vector<RECT> msgButtonRects(const RECT& rc, int count) {
    std::vector<RECT> out;
    if (count <= 0) return out;
    const int totalW = count * dp(kMsgBtnW) + (count - 1) * dp(kMsgBtnGap);
    int x = rc.right - dp(kMsgPad) - totalW;
    const int y = rc.bottom - dp(kMsgPad) - dp(kMsgBtnH);
    for (int i = 0; i < count; ++i) {
        out.push_back(RECT{x, y, x + dp(kMsgBtnW), y + dp(kMsgBtnH)});
        x += dp(kMsgBtnW) + dp(kMsgBtnGap);
    }
    return out;
}

void paintKindIcon(HDC dc, const RECT& rc, int kind) {
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    const std::uint32_t color = kind == 1 ? ui::warn : (kind == 2 ? ui::danger : ui::accent);
    fillCircle(dc, cx, cy, dp(14), ui::mix(color, bgCard, 0.22));
    const std::wstring glyph = kind == 2 ? L"×" : (kind == 1 ? L"!" : (kind == 3 ? L"?" : L"i"));
    drawText(dc, glyph, rc, color, g_app.uiFontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void paintMessage(HWND hwnd) {
    const MessageSpec& spec = g_message.spec;
    paintDialog(hwnd, spec.title, L"", [&](HDC dc) {
        RECT rc;
        GetClientRect(hwnd, &rc);

        RECT iconRc{dp(kMsgPad), dp(60), dp(kMsgPad) + dp(34), dp(94)};
        paintKindIcon(dc, iconRc, spec.kind);

        RECT textRc = msgTextRect(rc);
        if (spec.detail.empty()) textRc.bottom = rc.bottom - dp(72);
        drawText(dc, spec.text, textRc, ui::text, g_app.uiFont, DT_LEFT | DT_TOP | DT_WORDBREAK);

        if (!spec.detail.empty()) {
            RECT detailRc = msgDetailRect(rc);
            fillRoundRect(dc, detailRc, dp(6), bgInput, ui::borderSoft);
            RECT inner = detailRc;
            InflateRect(&inner, -dp(10), -dp(8));
            drawText(dc, spec.detail, inner, ui::textDim, g_app.font,
                     DT_LEFT | DT_TOP | DT_WORDBREAK);
        }

        const auto buttons = msgButtonRects(rc, static_cast<int>(spec.buttons.size()));
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            const bool primary = static_cast<int>(i) == spec.defaultIndex;
            paintButton(dc, buttons[i], spec.buttons[i], primary,
                        g_message.hover == static_cast<int>(i), false,
                        g_message.focus == static_cast<int>(i), true);
        }
    });
}

void finishMessage(int result) {
    g_message.result = result;
    if (g_message.dlg) DestroyWindow(g_message.dlg);
}

LRESULT CALLBACK messageDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT colorResult = 0;
    if (handleDialogColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), colorResult))
        return colorResult;

    switch (msg) {
        case WM_PAINT:
            paintMessage(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            trackCloseHover(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            RECT rc;
            GetClientRect(hwnd, &rc);
            const auto buttons = msgButtonRects(rc, static_cast<int>(g_message.spec.buttons.size()));
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hover = -1;
            for (std::size_t i = 0; i < buttons.size(); ++i) {
                if (PtInRect(&buttons[i], pt)) hover = static_cast<int>(i);
            }
            if (hover != g_message.hover) {
                g_message.hover = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (isOnCloseButton(hwnd, pt)) {
                finishMessage(-1);
                return 0;
            }
            const auto buttons = msgButtonRects(rc, static_cast<int>(g_message.spec.buttons.size()));
            for (std::size_t i = 0; i < buttons.size(); ++i) {
                if (PtInRect(&buttons[i], pt)) {
                    finishMessage(static_cast<int>(i));
                    return 0;
                }
            }
            return 0;
        }
        case WM_KEYDOWN: {
            const int count = static_cast<int>(g_message.spec.buttons.size());
            if (wp == VK_ESCAPE) {
                finishMessage(count > 1 ? count - 1 : -1);
                return 0;
            }
            if (wp == VK_RETURN) {
                finishMessage(g_message.focus >= 0 ? g_message.focus : g_message.spec.defaultIndex);
                return 0;
            }
            if (wp == VK_LEFT || (wp == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000))) {
                g_message.focus = (g_message.focus + count - 1) % std::max(1, count);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_RIGHT || wp == VK_TAB) {
                g_message.focus = (g_message.focus + 1) % std::max(1, count);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (isOnCloseButton(hwnd, pt)) return HTCLIENT;
            if (pt.y < dp(50)) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_CLOSE:
            finishMessage(-1);
            return 0;
        case WM_DESTROY:
            g_message.dlg = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void runModalLoop(HWND dlg);

// 计算合适的窗口尺寸后弹出自绘消息框，返回按钮下标（关闭/ESC 返回 -1）
int showMessage(const MessageSpec& spec) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = messageDialogProc;
        wc.hInstance = g_app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"RshellMessageDialog";
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
        registered = true;
    }

    HDC dc = GetDC(g_app.main ? g_app.main : nullptr);
    // 宽度按正文长度收缩/展开，最多 520 逻辑像素
    const int rawW = textWidth(dc, spec.text, g_app.uiFont);
    int width = std::clamp(rawW + dp(kMsgPad) * 2 + dp(46), dp(300), dp(560));
    if (!spec.detail.empty()) {
        const int detailW = textWidth(dc, spec.detail, g_app.font);
        width = std::max(width, std::min(dp(620), detailW + dp(kMsgPad) * 2 + dp(66)));
    }

    // 正文高度（DT_CALCRECT 会按给定宽度折行）
    RECT calcText{0, 0, width - dp(kMsgPad) * 2 - dp(46), 0};
    HGDIOBJ oldFont = SelectObject(dc, g_app.uiFont);
    if (!spec.text.empty()) {
        DrawTextW(dc, spec.text.c_str(), static_cast<int>(spec.text.size()), &calcText,
                  DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    }
    const int textHeight = std::max<int>(dp(22), calcText.bottom);

    int height = dp(54) + textHeight + dp(80);
    if (!spec.detail.empty()) {
        RECT calcDetail{0, 0, width - dp(kMsgPad) * 2 - dp(66), 0};
        SelectObject(dc, g_app.font);
        DrawTextW(dc, spec.detail.c_str(), static_cast<int>(spec.detail.size()), &calcDetail,
                  DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        height += std::max<int>(dp(28), calcDetail.bottom) + dp(30);
    }
    SelectObject(dc, oldFont);
    ReleaseDC(g_app.main ? g_app.main : nullptr, dc);

    g_message = MessageState();
    g_message.spec = spec;
    g_message.focus = spec.defaultIndex;

    HWND dlg = createDialogWindow(L"RshellMessageDialog", spec.title, width, height);
    if (!dlg) return -1;
    g_message.dlg = dlg;
    runModalLoop(dlg);
    return g_message.result;
}

void messageInfo(const std::wstring& title, const std::wstring& text) {
    MessageSpec spec;
    spec.kind = 0;
    spec.title = title;
    spec.text = text;
    showMessage(spec);
}

void messageWarn(const std::wstring& title, const std::wstring& text) {
    MessageSpec spec;
    spec.kind = 1;
    spec.title = title;
    spec.text = text;
    showMessage(spec);
}

void messageError(const std::wstring& title, const std::wstring& text,
                  const std::wstring& detail = std::wstring()) {
    MessageSpec spec;
    spec.kind = 2;
    spec.title = title;
    spec.text = text;
    spec.detail = detail;
    showMessage(spec);
}

bool messageConfirm(const std::wstring& title, const std::wstring& text,
                    const std::wstring& okLabel = L"确定",
                    const std::wstring& cancelLabel = L"取消") {
    MessageSpec spec;
    spec.kind = 3;
    spec.title = title;
    spec.text = text;
    spec.buttons = {okLabel, cancelLabel};
    spec.defaultIndex = 0;
    return showMessage(spec) == 0;
}

// ---------------------------------------------------------------- 主机密钥确认
//
// 连接跑在工作线程，这里用 SendMessage 把确认请求同步到 UI 线程，
// 这样弹出的是自绘对话框而不是系统 MessageBox。

struct HostKeyRequest {
    UINT_PTR connectionId = 0;
    std::string host;
    std::string keyType;
    std::string fingerprint;
    bool accepted = false;
};

bool runHostKeyDialog(const std::string& host, const std::string& keyType,
                      const std::string& fingerprint) {
    MessageSpec spec;
    spec.kind = 1;
    spec.title = L"确认主机密钥";
    spec.text = L"该主机尚未记录在 known_hosts 中。请核对指纹，确认这是你要连接的主机：";
    spec.detail = toWide(host) + L"\n" + toWide(keyType) + L"\n" + toWide(fingerprint);
    spec.buttons = {L"取消", L"信任并连接"};
    spec.defaultIndex = 0;  // 默认停在「取消」，避免误信任
    return showMessage(spec) == 1;
}

bool askHostKey(const std::string& host, const std::string& keyType,
                const std::string& fingerprint, UINT_PTR connectionId) {
    if (g_applicationClosing.load()) return false;
    if (!g_app.main || !IsWindow(g_app.main)) return false;
    if (GetCurrentThreadId() == GetWindowThreadProcessId(g_app.main, nullptr)) {
        return runHostKeyDialog(host, keyType, fingerprint);
    }
    HostKeyRequest request;
    request.connectionId = connectionId;
    request.host = host;
    request.keyType = keyType;
    request.fingerprint = fingerprint;
    SendMessageW(g_app.main, WM_ASK_HOSTKEY, 0, reinterpret_cast<LPARAM>(&request));
    return request.accepted;
}

std::wstring textOf(HWND control) {
    if (!control) return std::wstring();
    // 注意：密码框（ES_PASSWORD）的 WM_GETTEXTLENGTH 返回 0（不泄露长度），
    // 所以不能先用它算缓冲区大小，否则读出来永远是空字符串。
    std::vector<wchar_t> buf(4096, L'\0');
    const int len = GetWindowTextW(control, buf.data(), static_cast<int>(buf.size()));
    std::wstring text(buf.data(), len > 0 ? static_cast<std::size_t>(len) : 0);
    // 密码框（ES_PASSWORD）也走这条路径，缓冲里是明文口令：
    // 拷进返回值之后立刻清零，别把副本留在栈/堆上。
    SecureZeroMemory(buf.data(), buf.size() * sizeof(wchar_t));
    return text;
}

void useUiFont(HWND control) {
    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(g_app.uiFont ? g_app.uiFont
                                                       : GetStockObject(DEFAULT_GUI_FONT)),
                 TRUE);
}

HWND addEdit(HWND parent, int id, const std::wstring& text, int x, int y, int w, int h,
             DWORD extraStyle = 0);
void rememberEditFrame(HWND edit, const RECT& frame);

HWND addEdit(HWND parent, int id, const std::wstring& text, int x, int y, int w, int h,
             DWORD extraStyle) {
    // 单行 EDIT 的文字是按控件顶端对齐的：控件撑满外框（30px）时字会贴在下面/上面，
    // 看起来“没居中”。这里让 EDIT 只占一行字的高度，再在外框里垂直居中，
    // 外框（圆角底板 + 描边）由父窗口在 WM_PAINT 里画。
    const int frameH = h > 0 ? h : dp(30);
    const int inset = dp(1);
    const int editH = std::max<int>(dp(12), std::min(frameH - inset * 2, g_app.uiTextH + dp(2)));
    const int editY = y + (frameH - editH) / 2;
    HWND c = CreateWindowExW(0, L"EDIT", text.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extraStyle,
                             x + inset, editY, std::max<int>(dp(10), w - inset * 2), editH, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_app.inst, nullptr);
    useUiFont(c);
    dlg::styleEdit(c);
    rememberEditFrame(c, RECT{x, y, x + w, y + frameH});
    return c;
}

HWND addButton(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h,
               bool primary = false) {
    // owner-draw：WM_DRAWITEM 里用 dlg::paintButton 画，和主界面按钮同一套视觉
    HWND c = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             x, y, w, h, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_app.inst, nullptr);
    useUiFont(c);
    dlg::styleButton(c, primary);
    return c;
}

// 自绘勾选框：系统 BS_CHECKBOX 在深色对话框里会画出浅色底，和这套 UI 不搭
void paintCheckbox(HDC dc, const RECT& rc, const std::wstring& label, bool checked,
                   bool pressed) {
    HBRUSH bg = CreateSolidBrush(toColorRef(dlg::bgCard));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    const int box = dp(16);
    const int top = rc.top + (rc.bottom - rc.top - box) / 2;
    const RECT boxRc{rc.left, top, rc.left + box, top + box};
    if (checked) {
        fillRoundRect(dc, boxRc, dp(4), pressed ? ui::actionPressed : ui::action, ui::accent);
        HPEN pen = CreatePen(PS_SOLID, std::max<int>(1, dp(2)), toColorRef(0xFFFFFF));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        POINT check[3] = {{boxRc.left + dp(4), boxRc.top + dp(8)},
                          {boxRc.left + dp(7), boxRc.top + dp(11)},
                          {boxRc.left + dp(12), boxRc.top + dp(5)}};
        Polyline(dc, check, 3);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    } else {
        fillRoundRect(dc, boxRc, dp(4), dlg::bgInput, pressed ? ui::accent : ui::border);
    }

    drawText(dc, label, RECT{boxRc.right + dp(8), rc.top, rc.right, rc.bottom}, ui::textDim,
             g_app.uiFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

HWND addCheckbox(HWND parent, int id, const std::wstring& text, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, L"BUTTON", text.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x, y, w, h,
                             parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             g_app.inst, nullptr);
    useUiFont(c);
    return c;
}

// 输入框的圆角外框画在父窗口上：EDIT 自身背景同色，视觉上就是一个圆角输入框。
// 外框矩形在建控件时记下来（见 g_editFrames）。
void rememberEditFrame(HWND edit, const RECT& frame) { g_editFrames[edit] = frame; }

const RECT* editFrame(HWND edit) {
    const auto it = g_editFrames.find(edit);
    return it == g_editFrames.end() ? nullptr : &it->second;
}

// 点在外框的上下留白里也要能聚焦到输入框
bool focusEditAt(HWND parent, POINT pt) {
    for (const auto& pair : g_editFrames) {
        HWND edit = pair.first;
        if (!IsWindow(edit) || GetParent(edit) != parent) continue;
        if (!PtInRect(&pair.second, pt)) continue;
        if (GetFocus() != edit) {
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, -1);
        }
        return true;
    }
    return false;
}

void paintEditFrame(HDC dc, HWND parent, HWND edit) {
    if (!edit) return;
    RECT r{};
    if (const RECT* frame = editFrame(edit)) {
        r = *frame;
    } else {
        GetWindowRect(edit, &r);
        MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&r), 2);
        InflateRect(&r, dp(1), dp(1));
    }
    dlg::paintInputFrame(dc, r, GetFocus() == edit);
}

void runModalLoop(HWND dlg) {
    // 还原到自己的 owner 而不是主窗口：管理窗口里还能再弹消息框（嵌套模态）。
    // owner 可能本来就是禁用的（外层模态），关闭时必须恢复原状而不是一律启用。
    HWND owner = GetWindow(dlg, GW_OWNER);
    if (!owner || !IsWindow(owner)) owner = g_app.main;
    const BOOL ownerWasEnabled = owner ? IsWindowEnabled(owner) : FALSE;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner && IsWindow(owner)) {
        EnableWindow(owner, ownerWasEnabled);
        SetForegroundWindow(owner);
    }
}

// ---------------------------------------------------------------- 会话管理窗口
//
// 会话的浏览与编辑放在这个独立窗口里：左边是现有会话，右边是同一份表单。
// 主窗口左侧因此始终显示远端文件，不再需要在“会话列表 / 文件树”之间切换。

// remember 非空时显示「记住密码」勾选框：初值取 *remember，点「连接」后写回。
// savedBefore 表示这条会话之前存过密码（用来解释为什么又弹了框）。
bool promptPassword(const std::wstring& title, std::wstring& password, bool* remember = nullptr,
                    bool savedBefore = false);
// 私钥带口令时用同一个对话框，只换标题/字段名/勾选框文字。
bool promptKeyPassphrase(const std::wstring& title, std::wstring& passphrase,
                         bool* remember = nullptr, bool savedBefore = false);

struct SessionMgrState {
    HWND dlg = nullptr;
    HWND name = nullptr;
    HWND host = nullptr;
    HWND port = nullptr;
    HWND user = nullptr;
    HWND key = nullptr;
    HWND note = nullptr;
    HWND proxyUrl = nullptr;
    HWND proxyUser = nullptr;
    HWND proxyPass = nullptr;
    HWND keepAliveSec = nullptr;
    bool keepAliveOn = false;
    HWND pwdClear = nullptr;
    int selected = -1;
    int hover = -1;
    int scroll = 0;
    bool connectAfterClose = false;
    std::string connectName;
};

constexpr wchar_t kProxyCueUrl[] = L"socks5://主机:端口 或 http://主机:端口";
constexpr wchar_t kProxyCueUser[] = L"代理用户名，可留空";
constexpr wchar_t kProxyCuePass[] = L"留空=用已保存的";

// 空输入框上画提示文字；深色主题下系统 cue banner 不可见，所以自己画
LRESULT CALLBACK proxyCueProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                              DWORD_PTR ref) {
    switch (msg) {
        case WM_PAINT: {
            const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
            if (GetWindowTextLengthW(hwnd) == 0 && GetFocus() != hwnd) {
                HDC dc = GetDC(hwnd);
                RECT rc{};
                GetClientRect(hwnd, &rc);
                rc.left += 6;
                rc.right -= 6;
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, toColorRef(ui::textFaint));
                HGDIOBJ oldFont =
                    SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0)));
                DrawTextW(dc, reinterpret_cast<const wchar_t*>(ref), -1, &rc,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(dc, oldFont);
                ReleaseDC(hwnd, dc);
            }
            return result;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
            InvalidateRect(hwnd, nullptr, TRUE);
            return result;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, proxyCueProc, id);
            break;
        default: break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

SessionMgrState g_mgr;

constexpr int ID_MGR_NAME = 3210;
constexpr int ID_MGR_HOST = 3211;
constexpr int ID_MGR_PORT = 3212;
constexpr int ID_MGR_USER = 3213;
constexpr int ID_MGR_KEY = 3214;
constexpr int ID_MGR_NOTE = 3215;
constexpr int ID_MGR_PROXY_URL = 3216;
constexpr int ID_MGR_PROXY_USER = 3217;
constexpr int ID_MGR_PROXY_PASS = 3218;
constexpr int ID_MGR_KEEPALIVE = 3219;
constexpr int ID_MGR_BROWSE = 3220;
constexpr int ID_MGR_NEW = 3230;
constexpr int ID_MGR_SAVE = 3231;
constexpr int ID_MGR_DELETE = 3232;
constexpr int ID_MGR_CLOSE = 3233;
constexpr int ID_MGR_CONNECT = 3234;
constexpr int ID_MGR_CLEAR_PWD = 3235;

RECT dialogWorkArea() {
    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) || work.right <= work.left ||
        work.bottom <= work.top) {
        work = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    return work;
}

// 对话框不能超出工作区：小屏 + 高缩放下 820x620 换算成物理像素会大于屏幕
int mgrWidth() {
    const RECT work = dialogWorkArea();
    const int limit = (work.right - work.left) - dp(24);
    return std::max(dp(700), std::min(dp(820), limit));
}

int mgrHeight() {
    const RECT work = dialogWorkArea();
    const int limit = (work.bottom - work.top) - dp(24);
    return std::max(dp(480), std::min(dp(620), limit));
}

// 屏幕矮的时候按比例压缩行高，而不是把窗口撑出屏幕
int mgrFieldHeight() {
    const RECT work = dialogWorkArea();
    const int available = (work.bottom - work.top) - dp(24);
    const int natural = dp(620);
    if (available >= natural) return dp(30);
    const double scale = std::clamp(static_cast<double>(available) / natural, 0.65, 1.0);
    return std::max(dp(20), static_cast<int>(dp(30) * scale));
}

int mgrFieldGap() {
    const RECT work = dialogWorkArea();
    const int available = (work.bottom - work.top) - dp(24);
    const int natural = dp(620);
    if (available >= natural) return dp(10);
    const double scale = std::clamp(static_cast<double>(available) / natural, 0.65, 1.0);
    return std::max(dp(4), static_cast<int>(dp(10) * scale));
}
int mgrPad() { return dp(20); }
int mgrListW() { return dp(262); }
int mgrListTop() { return dp(96); }
int mgrItemH() { return dp(54); }
int mgrItemGap() { return dp(6); }

// 左栏列表与右栏表单的位置都从同一份布局算，绘制和建控件不会错位
struct MgrLayout {
    RECT name;
    RECT host;
    RECT port;
    RECT user;
    RECT key;
    RECT note;
    RECT proxyUrl;
    RECT proxyUser;
    RECT proxyPass;
    RECT keepAliveBox;
    RECT keepAliveSec;
    RECT hint;
    int keepAliveLabelX = 0;
    int keepAliveUnitX = 0;
    RECT browse;
    RECT pwdState;
    RECT pwdClear;
    RECT labelSave;
    RECT save;
    RECT remove;
    RECT btnNew;
    RECT btnClose;
    RECT btnConnect;
    int left = 0;
    int labelW = 0;
    int userLabelX = 0;
    int proxyPassLabelX = 0;
    int listBottom = 0;
};

MgrLayout mgrLayout() {
    MgrLayout l;
    const int pad = mgrPad();
    const int fieldH = mgrFieldHeight();
    l.labelW = dp(52);
    l.left = pad + mgrListW() + dp(26);
    const int fieldX = l.left + l.labelW;
    const int fieldRight = mgrWidth() - pad;

    int y = mgrListTop();
    l.name = RECT{fieldX, y, fieldRight, y + fieldH};
    y += fieldH + mgrFieldGap();
    l.host = RECT{fieldX, y, fieldRight, y + fieldH};
    y += fieldH + mgrFieldGap();
    l.port = RECT{fieldX, y, fieldX + dp(72), y + fieldH};
    l.userLabelX = l.port.right + dp(16);
    l.user = RECT{l.userLabelX + dp(38), y, fieldRight, y + fieldH};
    y += fieldH + mgrFieldGap();
    l.key = RECT{fieldX, y, fieldRight - dp(88), y + fieldH};
    l.browse = RECT{fieldRight - dp(80), y, fieldRight, y + fieldH};
    y += fieldH + mgrFieldGap();
    l.note = RECT{fieldX, y, fieldRight, y + fieldH};
    y += fieldH + mgrFieldGap();

    // 代理：一个 URL 字段（socks5://host:port 或 http://host:port），
    // 用户名单独存，密码进保险库
    l.proxyUrl = RECT{fieldX, y, fieldRight, y + fieldH};
    y += fieldH + mgrFieldGap();
    l.proxyUser = RECT{fieldX, y, fieldX + dp(150), y + fieldH};
    l.proxyPassLabelX = l.proxyUser.right + dp(14);
    l.proxyPass = RECT{l.proxyPassLabelX + dp(38), y, fieldRight, y + fieldH};
    y += fieldH + mgrFieldGap();

    // 保活：勾选框 + 间隔（秒）
    const int boxSize = dp(16);
    l.keepAliveBox = RECT{fieldX, y + (fieldH - boxSize) / 2, fieldX + boxSize,
                          y + (fieldH - boxSize) / 2 + boxSize};
    l.keepAliveLabelX = l.keepAliveBox.right + dp(8);
    l.keepAliveSec = RECT{fieldX + dp(210), y, fieldX + dp(270), y + fieldH};
    l.keepAliveUnitX = l.keepAliveSec.right + dp(8);
    y += fieldH + mgrFieldGap();
    l.hint = RECT{fieldX, y, fieldRight, y + dp(36)};

    // 密码状态行：左边两行说明，右边「清除密码」只在真的存过密码时可用
    l.pwdClear = RECT{fieldRight - dp(112), y + dp(42), fieldRight, y + dp(70)};
    l.pwdState = RECT{l.left, y + dp(42), l.pwdClear.left - dp(12), y + dp(74)};

    const int actionY = y + dp(84);
    l.save = RECT{fieldRight - dp(84), actionY, fieldRight, actionY + dp(32)};
    l.remove = RECT{l.save.left - dp(10) - dp(88), actionY, l.save.left - dp(10),
                    actionY + dp(32)};
    l.labelSave = RECT{fieldX, actionY, l.remove.left - dp(12), actionY + dp(32)};

    const int bottomY = mgrHeight() - pad - dp(32);
    l.btnNew = RECT{pad, bottomY, pad + dp(120), bottomY + dp(32)};
    l.btnConnect = RECT{fieldRight - dp(96), bottomY, fieldRight, bottomY + dp(32)};
    l.btnClose = RECT{l.btnConnect.left - dp(10) - dp(84), bottomY, l.btnConnect.left - dp(10),
                      bottomY + dp(32)};
    l.listBottom = bottomY - dp(16);
    return l;
}
int mgrMaxScroll() {
    const int n = static_cast<int>(g_app.store.sessions().size());
    const int content = n * mgrItemH() + (n > 0 ? (n - 1) * mgrItemGap() : 0);
    return std::max(0, content - (mgrLayout().listBottom - mgrListTop()));
}

RECT mgrItemRect(int index) {
    const int y = mgrListTop() + index * (mgrItemH() + mgrItemGap()) - g_mgr.scroll;
    return RECT{mgrPad(), y, mgrPad() + mgrListW(), y + mgrItemH()};
}

int mgrIndexFromPoint(int x, int y) {
    const int pad = mgrPad();
    if (x < pad || x > pad + mgrListW()) return -1;
    if (y < mgrListTop() || y > mgrLayout().listBottom) return -1;
    const int local = y - mgrListTop() + g_mgr.scroll;
    if (local < 0) return -1;
    const int stride = mgrItemH() + mgrItemGap();
    const int index = local / stride;
    if (local - index * stride > mgrItemH()) return -1;
    if (index < 0 || index >= static_cast<int>(g_app.store.sessions().size())) return -1;
    return index;
}

void mgrEnsureVisible(int index) {
    if (index < 0) return;
    const int top = index * (mgrItemH() + mgrItemGap());
    const int bottom = top + mgrItemH();
    const int viewH = mgrLayout().listBottom - mgrListTop();
    if (top - g_mgr.scroll < 0) g_mgr.scroll = top;
    if (bottom - g_mgr.scroll > viewH) g_mgr.scroll = bottom - viewH;
    g_mgr.scroll = std::clamp(g_mgr.scroll, 0, mgrMaxScroll());
}

// 第二行：user@host:port
std::wstring sessionDetail(const Session& s) {
    std::wstring detail;
    if (!s.user.empty()) detail += toWide(s.user) + L"@";
    detail += toWide(s.host);
    if (s.port > 0 && s.port != 22) detail += L":" + std::to_wstring(s.port);
    return detail;
}

void mgrLoadSession(const Session& s) {
    if (g_mgr.name) SetWindowTextW(g_mgr.name, toWide(s.name).c_str());
    if (g_mgr.host) SetWindowTextW(g_mgr.host, toWide(s.host).c_str());
    if (g_mgr.port) SetWindowTextW(g_mgr.port, std::to_wstring(s.port > 0 ? s.port : 22).c_str());
    if (g_mgr.user) SetWindowTextW(g_mgr.user, toWide(s.user).c_str());
    if (g_mgr.key) SetWindowTextW(g_mgr.key, toWide(s.keyPath).c_str());
    if (g_mgr.note) SetWindowTextW(g_mgr.note, toWide(s.note).c_str());
    if (g_mgr.proxyUrl) SetWindowTextW(g_mgr.proxyUrl, toWide(s.proxyUrl).c_str());
    if (g_mgr.proxyUser) SetWindowTextW(g_mgr.proxyUser, toWide(s.proxyUser).c_str());
    // 密码永远不回填：要改就重新输入，留空表示沿用保险库里的
    if (g_mgr.proxyPass) SetWindowTextW(g_mgr.proxyPass, L"");
    g_mgr.keepAliveOn = s.keepAliveSec > 0;
    if (g_mgr.keepAliveSec) {
        const int seconds = s.keepAliveSec > 0 ? s.keepAliveSec : 30;
        SetWindowTextW(g_mgr.keepAliveSec, std::to_wstring(seconds).c_str());
        EnableWindow(g_mgr.keepAliveSec, g_mgr.keepAliveOn ? TRUE : FALSE);
    }
    if (g_mgr.pwdClear) {
        SetWindowTextW(g_mgr.pwdClear,
                       s.keyPath.empty() ? L"清除密码" : L"清除私钥口令");
        const bool saved = s.keyPath.empty() ? s.savePassword : s.saveKeyPassphrase;
        EnableWindow(g_mgr.pwdClear, saved ? TRUE : FALSE);
    }
}

Session mgrFormSession() {
    Session s;
    s.name = trim(toUtf8(textOf(g_mgr.name)));
    s.host = trim(toUtf8(textOf(g_mgr.host)));
    s.user = trim(toUtf8(textOf(g_mgr.user)));
    s.keyPath = trim(toUtf8(textOf(g_mgr.key)));
    s.note = toUtf8(textOf(g_mgr.note));
    s.proxyUrl = trim(toUtf8(textOf(g_mgr.proxyUrl)));
    s.proxyUser = trim(toUtf8(textOf(g_mgr.proxyUser)));
    if (g_mgr.keepAliveOn) {
        const int seconds = _wtoi(textOf(g_mgr.keepAliveSec).c_str());
        s.keepAliveSec = std::clamp(seconds > 0 ? seconds : 30, 5, 3600);
    } else {
        s.keepAliveSec = 0;
    }
    s.port = _wtoi(textOf(g_mgr.port).c_str());
    return s;
}

bool mgrDirty() {
    if (!g_mgr.dlg) return false;
    Session form = mgrFormSession();
    if (form.port <= 0) form.port = 22;
    const auto& sessions = g_app.store.sessions();
    Session stored;
    stored.port = 22;
    if (g_mgr.selected >= 0 && g_mgr.selected < static_cast<int>(sessions.size()))
        stored = sessions[static_cast<std::size_t>(g_mgr.selected)];
    return form.name != stored.name || form.host != stored.host || form.port != stored.port ||
           form.user != stored.user || form.keyPath != stored.keyPath || form.note != stored.note ||
           form.proxyUrl != stored.proxyUrl || form.proxyUser != stored.proxyUser ||
           (g_mgr.proxyPass && GetWindowTextLengthW(g_mgr.proxyPass) > 0) ||
           form.keepAliveSec != stored.keepAliveSec;
}

bool mgrConfirmDiscard() {
    if (!mgrDirty()) return true;
    return messageConfirm(L"放弃修改", L"表单里有还没保存的修改，切换会话会丢失它们。", L"丢弃",
                          L"继续编辑");
}
void paintSessionMgr(HDC dc, HWND hwnd) {
    const MgrLayout l = mgrLayout();
    const auto& sessions = g_app.store.sessions();

    // ---- 左栏 ----
    drawText(dc, L"现有会话", RECT{mgrPad(), dp(70), mgrPad() + mgrListW(), dp(90)},
             ui::textFaint, g_app.tinyFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const std::wstring count = std::to_wstring(sessions.size());
    const int countW = textWidth(dc, count, g_app.tinyFont);
    drawText(dc, count,
             RECT{mgrPad() + mgrListW() - countW, dp(70), mgrPad() + mgrListW(), dp(90)},
             ui::textFaint, g_app.tinyFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    if (sessions.empty()) {
        drawText(dc, L"还没有会话。\n点左下角「新建会话」添加一个，填好地址就能直接连接。",
                 RECT{mgrPad(), mgrListTop(), mgrPad() + mgrListW(), mgrListTop() + dp(60)},
                 ui::textFaint, g_app.tinyFont, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
        const RECT rc = mgrItemRect(i);
        if (rc.bottom < mgrListTop() || rc.top > l.listBottom) continue;
        const Session& s = sessions[static_cast<std::size_t>(i)];
        const bool selected = i == g_mgr.selected;
        const bool hovered = i == g_mgr.hover;
        fillRoundRect(dc, rc, dp(8),
                      selected ? ui::bgActive : (hovered ? ui::bgHover : ui::bgPanel),
                      selected ? ui::mix(ui::accent, ui::bgCard, 0.5) : 0);
        if (selected)
            fillRoundRect(dc, RECT{rc.left + dp(2), rc.top + dp(12), rc.left + dp(5), rc.bottom - dp(12)},
                          dp(2), ui::accent);

        RECT nameRc{rc.left + dp(12), rc.top + dp(7), rc.right - dp(34), rc.top + dp(29)};
        drawText(dc, ellipsize(dc, toWide(s.name), g_app.uiFontBold, nameRc.right - nameRc.left),
                 nameRc, selected ? ui::text : ui::textDim, g_app.uiFontBold,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT detailRc{rc.left + dp(12), rc.top + dp(28), rc.right - dp(12), rc.bottom - dp(6)};
        drawText(dc, ellipsize(dc, sessionDetail(s), g_app.tinyFont, detailRc.right - detailRc.left),
                 detailRc, ui::textFaint, g_app.tinyFont,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // 当前会话右上角的绿点
        if (!g_app.currentSession.empty() && s.name == g_app.currentSession)
            fillCircle(dc, rc.right - dp(14), rc.top + dp(16), dp(3), ui::success);
    }

    const int maxScroll = mgrMaxScroll();
    if (maxScroll > 0) {
        const int trackTop = mgrListTop();
        const int trackH = std::max<int>(dp(20), l.listBottom - trackTop);
        const int thumbH = std::max(dp(24), trackH * trackH / (trackH + maxScroll));
        const int thumbY = trackTop + (trackH - thumbH) * g_mgr.scroll / maxScroll;
        fillRoundRect(dc,
                      RECT{mgrPad() + mgrListW() - dp(3), thumbY, mgrPad() + mgrListW(),
                           thumbY + thumbH},
                      dp(2), ui::mix(ui::textFaint, ui::bgCard, 0.35));
    }

    // ---- 右栏 ----
    auto label = [&](const wchar_t* text, int x, int y) {
        drawText(dc, text, RECT{x, y, x + l.labelW, y + dp(30)}, ui::textDim, g_app.uiFont,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    label(L"名称", l.left, l.name.top);
    label(L"主机", l.left, l.host.top);
    label(L"端口", l.left, l.port.top);
    label(L"用户", l.userLabelX, l.port.top);
    label(L"私钥", l.left, l.key.top);
    label(L"备注", l.left, l.note.top);
    label(L"代理", l.left, l.proxyUrl.top);
    label(L"用户", l.left, l.proxyUser.top);
    label(L"密码", l.proxyPassLabelX, l.proxyPass.top);

    paintEditFrame(dc, hwnd, g_mgr.name);
    paintEditFrame(dc, hwnd, g_mgr.host);
    paintEditFrame(dc, hwnd, g_mgr.port);
    paintEditFrame(dc, hwnd, g_mgr.user);
    paintEditFrame(dc, hwnd, g_mgr.key);
    paintEditFrame(dc, hwnd, g_mgr.note);
    paintEditFrame(dc, hwnd, g_mgr.proxyUrl);
    paintEditFrame(dc, hwnd, g_mgr.proxyUser);
    paintEditFrame(dc, hwnd, g_mgr.proxyPass);

    // 保活行：勾选框 + 间隔（秒）
    label(L"保活", l.left, l.keepAliveSec.top);
    fillRoundRect(dc, l.keepAliveBox, dp(3), g_mgr.keepAliveOn ? ui::action : dlg::bgInput,
                  g_mgr.keepAliveOn ? ui::action : ui::border);
    if (g_mgr.keepAliveOn)
        drawText(dc, L"✓", l.keepAliveBox, 0xFFFFFF, g_app.tinyFont,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    drawText(dc, L"保持活动状态",
             RECT{l.keepAliveLabelX, l.keepAliveSec.top, l.keepAliveSec.left - dp(10),
                  l.keepAliveSec.bottom},
             ui::text, g_app.uiFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    drawText(dc, L"秒",
             RECT{l.keepAliveUnitX, l.keepAliveSec.top, l.keepAliveUnitX + dp(30),
                  l.keepAliveSec.bottom},
             ui::textDim, g_app.uiFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    paintEditFrame(dc, hwnd, g_mgr.keepAliveSec);

    const RECT& hint = l.hint;
    drawText(dc, L"私钥可留空（会尝试 ~/.ssh 默认密钥）；代理留空表示直连。", hint,
             ui::textFaint, g_app.tinyFont, DT_LEFT | DT_TOP | DT_WORDBREAK);

    // 口令本体在密码保险库里，sessions.ini 只有 savepassword/savekeypassphrase 标记
    const Session* selectedSession =
        g_mgr.selected >= 0 && g_mgr.selected < static_cast<int>(g_app.store.sessions().size())
            ? &g_app.store.sessions()[static_cast<std::size_t>(g_mgr.selected)]
            : nullptr;
    bool passwordSaved = selectedSession && selectedSession->savePassword;
    std::wstring passwordText;
    if (selectedSession && !selectedSession->keyPath.empty()) {
        passwordSaved = selectedSession->saveKeyPassphrase;
        passwordText = passwordSaved
                           ? L"私钥口令：已保存到 " + savedPasswordLocation() +
                                 L"，连接时自动使用"
                           : L"私钥口令：未保存（连接时输入口令，可勾选记住）";
    } else if (passwordSaved) {
        passwordText = L"密码：已保存到 " + savedPasswordLocation() + L"，连接时自动使用";
    } else {
        passwordText = L"密码：未保存（连接时勾选「记住密码」即可保存）";
    }
    drawText(dc, passwordText, l.pwdState, passwordSaved ? ui::success : ui::textFaint,
             g_app.tinyFont, DT_LEFT | DT_TOP | DT_WORDBREAK);

    const std::wstring formState =
        g_mgr.selected >= 0 && g_mgr.selected < static_cast<int>(g_app.store.sessions().size())
            ? L"修改后点「保存」写回，点「连接」直接连过去。"
            : L"正在新建会话：填好后点「保存」或「连接」。";
    drawText(dc, formState, l.labelSave, ui::textFaint, g_app.tinyFont,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}
void mgrSelect(HWND hwnd, int index) {
    if (index == g_mgr.selected) return;
    if (!mgrConfirmDiscard()) return;
    g_mgr.selected = index;
    const auto& sessions = g_app.store.sessions();
    if (index >= 0 && index < static_cast<int>(sessions.size()))
        mgrLoadSession(sessions[static_cast<std::size_t>(index)]);
    else
        mgrLoadSession(Session{});
    mgrEnsureVisible(index);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// 会话被编辑时同步密码保险库里的条目：
//   - 只改了名字：把密码搬到新条目名下
//   - 改了主机/端口/用户/私钥：旧口令不再适用，直接删掉
//   - 其它情况：保留原来的标记
// 返回 true 表示"这条会话不再有已保存的密码"（调用方可以据此提示用户）
bool syncSavedPasswordOnEdit(const Session& old, Session& updated) {
    if (!old.savePassword) {
        updated.savePassword = false;
        return false;
    }
    const bool connectionChanged = old.host != updated.host || old.port != updated.port ||
                                   old.user != updated.user || old.keyPath != updated.keyPath;
    if (connectionChanged) {
        deleteCredential(credentialTarget(old.name));
        updated.savePassword = false;
        return true;
    }
    if (old.name != updated.name) {
        const std::string from = credentialTarget(old.name);
        const std::string to = credentialTarget(updated.name);
        std::string password;
        std::string error;
        if (loadCredential(from, password, &error) &&
            saveCredential(to, password, &error)) {
            deleteCredential(from);
            updated.savePassword = true;
            wipeString(password);
            return false;
        }
        guiLog("迁移已保存的密码失败：" + error);
        deleteCredential(from);
        updated.savePassword = false;
        wipeString(password);
        return true;
    }
    updated.savePassword = true;
    return false;
}

// 保存表单；保存失败（校验不过）返回 false
bool mgrSave(HWND hwnd) {
    Session s = mgrFormSession();
    if (s.name.empty()) s.name = s.host;
    if (s.name.empty() || s.host.empty()) {
        messageWarn(L"信息不完整", L"名称和主机不能为空。");
        SetFocus(s.host.empty() ? g_mgr.host : g_mgr.name);
        return false;
    }
    if (s.port <= 0 || s.port > 65535) {
        messageWarn(L"端口无效", L"端口必须在 1-65535 之间。");
        SetFocus(g_mgr.port);
        return false;
    }

    // 代理地址：先校验再规范化，避免把密码写进 sessions.ini
    std::string proxyPasswordFromUrl;
    if (!s.proxyUrl.empty()) {
        ProxyConfig proxy;
        std::string error;
        if (!parseProxyUrl(s.proxyUrl, proxy, error)) {
            messageWarn(L"代理地址无效",
                        toWide(error) + L"\n\n示例：socks5://127.0.0.1:1080 或 http://proxy:8080");
            SetFocus(g_mgr.proxyUrl);
            return false;
        }
        if (!proxy.user.empty()) s.proxyUser = proxy.user;
        proxyPasswordFromUrl = proxy.password;
        s.proxyUrl = proxyToUrl(proxy);  // 去掉 URL 里的凭据，只留 scheme://host:port
    }
    if (!s.keyPath.empty()) s.keyPath = expandPath(s.keyPath);

    if (g_mgr.selected < 0 && g_app.store.find(s.name)) {
        messageWarn(L"名称已存在", L"已经有同名会话了，换个名字，或先在左侧选中它再修改。");
        SetFocus(g_mgr.name);
        return false;
    }
    const auto& sessions = g_app.store.sessions();
    bool passwordDropped = false;
    std::string renamedFrom;
    bool proxyPasswordDropped = false;
    if (g_mgr.selected >= 0 && g_mgr.selected < static_cast<int>(sessions.size())) {
        const Session old = sessions[static_cast<std::size_t>(g_mgr.selected)];
        // 改名要把口令搬到新条目名下；保险库锁着时先请用户解锁，
        // 否则读不到旧口令，只能把保存标记清掉，等于丢密码。
        if ((old.savePassword || old.saveKeyPassphrase) && old.name != s.name &&
            !g_app.vault.unlocked() && vaultFilePresent() && !ensureVaultUnlocked()) {
            messageWarn(L"需要解锁密码保险库",
                        L"改名后要把已保存的口令迁到新条目名下，请先解锁密码保险库；"
                        L"取消解锁则本次修改不会保存。");
            return false;
        }
        passwordDropped = syncSavedPasswordOnEdit(old, s);

        // 私钥口令与密码分开存：主机/端口/用户/私钥任一变化都视为换了一把钥匙
        if (old.saveKeyPassphrase) {
            const bool keyChanged = old.host != s.host || old.port != s.port ||
                                    old.user != s.user || old.keyPath != s.keyPath;
            if (keyChanged) {
                deleteCredential(keyPassphraseTarget(old.name));
                s.saveKeyPassphrase = false;
                passwordDropped = true;
            } else if (old.name != s.name) {
                std::string passphrase;
                std::string error;
                const std::string from = keyPassphraseTarget(old.name);
                const std::string to = keyPassphraseTarget(s.name);
                if (loadCredential(from, passphrase, &error) &&
                    saveCredential(to, passphrase, &error)) {
                    deleteCredential(from);
                    wipeString(passphrase);
                } else {
                    guiLog("迁移已保存的私钥口令失败：" + error);
                    deleteCredential(from);
                    s.saveKeyPassphrase = false;
                    wipeString(passphrase);
                    passwordDropped = true;
                }
            } else {
                s.saveKeyPassphrase = true;
            }
        }
        if (old.name != s.name) { renamedFrom = old.name; g_app.store.remove(old.name); }

        // 代理密码：改了代理地址或用户名就作废旧口令；改名则迁移条目
        if (old.saveProxyPassword) {
            const bool proxyChanged =
                old.proxyUrl != s.proxyUrl || old.proxyUser != s.proxyUser || s.proxyUrl.empty();
            if (proxyChanged) {
                deleteCredential(proxyPasswordTarget(old.name));
                proxyPasswordDropped = true;
                s.saveProxyPassword = false;
            } else if (old.name != s.name) {
                std::string stored;
                std::string error;
                if (loadCredential(proxyPasswordTarget(old.name), stored, &error) &&
                    saveCredential(proxyPasswordTarget(s.name), stored, &error)) {
                    deleteCredential(proxyPasswordTarget(old.name));
                    s.saveProxyPassword = true;
                } else {
                    guiLog("迁移已保存的代理密码失败：" + error);
                    s.saveProxyPassword = false;
                    proxyPasswordDropped = true;
                }
                wipeString(stored);
            } else {
                s.saveProxyPassword = true;
            }
        }
    }

    // 新输入的代理密码（含 URL 里带的）存进保险库；新会话也走这里
    if (!s.proxyUrl.empty()) {
        std::wstring typed = g_mgr.proxyPass ? textOf(g_mgr.proxyPass) : std::wstring();
        std::string proxyPassword = typed.empty() ? proxyPasswordFromUrl : toUtf8(typed);
        if (!proxyPassword.empty()) {
            if (g_app.vault.unlocked()) {
                std::string error;
                if (saveCredential(proxyPasswordTarget(s.name), proxyPassword, &error)) {
                    s.saveProxyPassword = true;
                } else {
                    guiLog("保存代理密码失败：" + error);
                }
            } else {
                guiLog("保险库未解锁，代理密码本次不保存");
            }
        }
        wipeString(proxyPassword);
    }

    g_app.store.upsert(s);
    g_app.store.save();
    if (!renamedFrom.empty()) renameSessionTabs(renamedFrom, s.name);
    if (g_app.currentSession.empty() && !g_app.connecting && !g_app.shell->running())
        g_app.currentSession = s.name;

    g_mgr.selected = -1;
    const auto& after = g_app.store.sessions();
    for (int i = 0; i < static_cast<int>(after.size()); ++i) {
        if (after[i].name == s.name) {
            g_mgr.selected = i;
            break;
        }
    }
    mgrLoadSession(s);
    mgrEnsureVisible(g_mgr.selected);
    setStatus(L"已保存会话 " + toWide(s.name) +
              (passwordDropped ? L"（旧口令已从 " + activePasswordBackend() + L" 删除）" : L""));
    InvalidateRect(hwnd, nullptr, FALSE);
    updateButtons();
    return true;
}

void mgrDelete(HWND hwnd) {
    const auto& sessions = g_app.store.sessions();
    if (g_mgr.selected < 0 || g_mgr.selected >= static_cast<int>(sessions.size())) return;
    const Session removed = sessions[static_cast<std::size_t>(g_mgr.selected)];
    const std::string name = removed.name;
    const bool savedSecret = removed.savePassword || removed.saveKeyPassphrase;
    const std::wstring secretName = removed.keyPath.empty() ? L"密码" : L"私钥口令";
    const std::wstring passwordNote =
        savedSecret
            ? (g_app.vault.unlocked()
                   ? L"\n\n" + activePasswordBackend() + L" 里保存的" + secretName + L"也会一起删除。"
                   : L"\n\n这条会话的" + secretName + L"保存在密码保险库里；保险库锁定期间无法立即删除，"
                     L"下次解锁时会自动清理。")
            : std::wstring();
    if (!messageConfirm(L"删除会话",
                        L"确定删除会话 “" + toWide(name) + L"” 吗？（不会影响远端主机）" +
                            passwordNote,
                        L"删除", L"取消"))
        return;
    // 会话没了，口令也没有归属了：一并清掉，别在保险库里留孤儿
    if (removed.savePassword) deleteCredential(credentialTarget(name));
    if (removed.saveKeyPassphrase) deleteCredential(keyPassphraseTarget(name));
    if (removed.saveProxyPassword) deleteCredential(proxyPasswordTarget(name));
    g_app.store.remove(name);
    g_app.store.save();

    const int count = static_cast<int>(g_app.store.sessions().size());
    g_mgr.selected = count > 0 ? std::min(g_mgr.selected, count - 1) : -1;
    if (g_mgr.selected >= 0)
        mgrLoadSession(g_app.store.sessions()[static_cast<std::size_t>(g_mgr.selected)]);
    else
        mgrLoadSession(Session{});
    mgrEnsureVisible(g_mgr.selected);
    InvalidateRect(hwnd, nullptr, FALSE);
    updateButtons();
}

// 「清除密码」：从当前后端里删掉条目，并把会话上的标记复位
void mgrClearPassword(HWND hwnd) {
    const auto& sessions = g_app.store.sessions();
    if (g_mgr.selected < 0 || g_mgr.selected >= static_cast<int>(sessions.size())) return;
    if (!mgrConfirmDiscard()) return;
    Session session = sessions[static_cast<std::size_t>(g_mgr.selected)];
    const bool keyMode = !session.keyPath.empty();
    const bool saved = keyMode ? session.saveKeyPassphrase : session.savePassword;
    const std::wstring what = keyMode ? L"私钥口令" : L"密码";
    if (!saved) {
        messageInfo(L"没有保存" + what, L"这个会话没有保存过" + what + L"。");
        return;
    }
    if (!messageConfirm(L"清除" + what,
                        L"从 " + activePasswordBackend() + L" 里删除会话 “" + toWide(session.name) +
                            L"” 的" + what + L"？\n\n下次连接需要重新输入。",
                        L"清除", L"取消"))
        return;

    // 条目只在保险库里：锁着就先解锁；没有 vault.dat 时没有可删的条目，
    // 直接复位标记即可。
    if (!g_app.vault.unlocked() && vaultFilePresent() && !ensureVaultUnlocked()) return;

    std::string error;
    const std::string target =
        keyMode ? keyPassphraseTarget(session.name) : credentialTarget(session.name);
    if (g_app.vault.unlocked() && !deleteCredential(target, &error)) {
        messageWarn(L"清除失败", toWide(error));
        return;
    }
    if (keyMode)
        session.saveKeyPassphrase = false;
    else
        session.savePassword = false;
    g_app.store.upsert(session);
    g_app.store.save();
    mgrLoadSession(session);
    setStatus(L"已清除 “" + toWide(session.name) + L"” 的保存" + what);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void mgrNew(HWND hwnd) {
    if (!mgrConfirmDiscard()) return;
    g_mgr.selected = -1;
    mgrLoadSession(Session{});
    SetFocus(g_mgr.name);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// 连接：有改动先存盘，然后关掉管理窗口交给调用方去连
void mgrConnect(HWND hwnd) {
    Session form = mgrFormSession();
    if (form.name.empty() && form.host.empty()) {
        messageWarn(L"还没有会话", L"请先在左侧选择一个会话，或新建一个。");
        return;
    }
    if (mgrDirty() && !mgrSave(hwnd)) return;
    form = mgrFormSession();
    if (form.name.empty()) form.name = form.host;
    if (!g_app.store.find(form.name)) return;
    g_mgr.connectAfterClose = true;
    g_mgr.connectName = form.name;
    DestroyWindow(hwnd);
}
LRESULT CALLBACK sessionMgrProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT colorResult = 0;
    if (dlg::handleDialogColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp),
                               colorResult))
        return colorResult;

    switch (msg) {
        case WM_CREATE: {
            const MgrLayout l = mgrLayout();
            g_mgr.name = addEdit(hwnd, ID_MGR_NAME, L"", l.name.left, l.name.top,
                                 l.name.right - l.name.left, l.name.bottom - l.name.top);
            g_mgr.host = addEdit(hwnd, ID_MGR_HOST, L"", l.host.left, l.host.top,
                                 l.host.right - l.host.left, l.host.bottom - l.host.top);
            g_mgr.port =
                addEdit(hwnd, ID_MGR_PORT, L"", l.port.left, l.port.top, l.port.right - l.port.left,
                        l.port.bottom - l.port.top, ES_NUMBER);
            g_mgr.user = addEdit(hwnd, ID_MGR_USER, L"", l.user.left, l.user.top,
                                 l.user.right - l.user.left, l.user.bottom - l.user.top);
            g_mgr.key = addEdit(hwnd, ID_MGR_KEY, L"", l.key.left, l.key.top,
                                l.key.right - l.key.left, l.key.bottom - l.key.top);
            g_mgr.note = addEdit(hwnd, ID_MGR_NOTE, L"", l.note.left, l.note.top,
                                 l.note.right - l.note.left, l.note.bottom - l.note.top);
            g_mgr.proxyUrl = addEdit(hwnd, ID_MGR_PROXY_URL, L"", l.proxyUrl.left,
                                     l.proxyUrl.top, l.proxyUrl.right - l.proxyUrl.left,
                                     l.proxyUrl.bottom - l.proxyUrl.top);
            g_mgr.proxyUser = addEdit(hwnd, ID_MGR_PROXY_USER, L"", l.proxyUser.left,
                                      l.proxyUser.top, l.proxyUser.right - l.proxyUser.left,
                                      l.proxyUser.bottom - l.proxyUser.top);
            g_mgr.proxyPass = addEdit(hwnd, ID_MGR_PROXY_PASS, L"", l.proxyPass.left,
                                      l.proxyPass.top, l.proxyPass.right - l.proxyPass.left,
                                      l.proxyPass.bottom - l.proxyPass.top, ES_PASSWORD);
            g_mgr.keepAliveSec = addEdit(hwnd, ID_MGR_KEEPALIVE, L"30", l.keepAliveSec.left,
                                         l.keepAliveSec.top,
                                         l.keepAliveSec.right - l.keepAliveSec.left,
                                         l.keepAliveSec.bottom - l.keepAliveSec.top, ES_NUMBER);
            // 深色主题下 EM_SETCUEBANNER 不渲染，自己画占位文字
            if (g_mgr.proxyUrl) SetWindowSubclass(g_mgr.proxyUrl, proxyCueProc, 1, reinterpret_cast<DWORD_PTR>(kProxyCueUrl));
            if (g_mgr.proxyUser) SetWindowSubclass(g_mgr.proxyUser, proxyCueProc, 2, reinterpret_cast<DWORD_PTR>(kProxyCueUser));
            if (g_mgr.proxyPass) SetWindowSubclass(g_mgr.proxyPass, proxyCueProc, 3, reinterpret_cast<DWORD_PTR>(kProxyCuePass));
            auto button = [&](int id, const wchar_t* text, const RECT& rc, bool primary = false) {
                return addButton(hwnd, id, text, rc.left, rc.top, rc.right - rc.left,
                                 rc.bottom - rc.top, primary);
            };
            button(ID_MGR_BROWSE, L"浏览", l.browse);
            button(ID_MGR_SAVE, L"保存", l.save);
            button(ID_MGR_DELETE, L"删除", l.remove);
            g_mgr.pwdClear = button(ID_MGR_CLEAR_PWD, L"清除密码", l.pwdClear);
            button(ID_MGR_NEW, L"＋ 新建会话", l.btnNew, true);
            button(ID_MGR_CLOSE, L"关闭", l.btnClose);
            button(ID_MGR_CONNECT, L"连接", l.btnConnect, true);

            const auto& sessions = g_app.store.sessions();
            mgrLoadSession(g_mgr.selected >= 0 && g_mgr.selected < static_cast<int>(sessions.size())
                               ? sessions[static_cast<std::size_t>(g_mgr.selected)]
                               : Session{});
            return 0;
        }
        case WM_PAINT:
            paintDialog(hwnd, L"会话管理", L"现有会话都在左边；右侧表单选中即可编辑",
                        [&](HDC dc) { paintSessionMgr(dc, hwnd); });
            return 0;
        case WM_DRAWITEM:
            dlg::handleDrawItem(hwnd, *reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            dlg::trackCloseHover(hwnd, pt.x, pt.y);
            const int index = mgrIndexFromPoint(pt.x, pt.y);
            if (index != g_mgr.hover) {
                g_mgr.hover = index;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_mgr.hover != -1) {
                g_mgr.hover = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (dlg::isOnCloseButton(hwnd, pt)) {
                DestroyWindow(hwnd);
                return 0;
            }
            const int index = mgrIndexFromPoint(pt.x, pt.y);
            if (index >= 0) {
                mgrSelect(hwnd, index);
                return 0;
            }
            // 保活勾选框
            {
                const MgrLayout layout = mgrLayout();
                if (PtInRect(&layout.keepAliveBox, pt)) {
                    g_mgr.keepAliveOn = !g_mgr.keepAliveOn;
                    if (g_mgr.keepAliveSec)
                        EnableWindow(g_mgr.keepAliveSec, g_mgr.keepAliveOn ? TRUE : FALSE);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            focusEditAt(hwnd, pt);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int index = mgrIndexFromPoint(pt.x, pt.y);
            if (index < 0) return 0;
            if (index != g_mgr.selected) mgrSelect(hwnd, index);
            mgrConnect(hwnd);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            g_mgr.scroll =
                std::clamp(g_mgr.scroll - delta * dp(40) / WHEEL_DELTA, 0, mgrMaxScroll());
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            const int count = static_cast<int>(g_app.store.sessions().size());
            if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (wp == VK_RETURN) {
                mgrConnect(hwnd);
                return 0;
            }
            if ((wp == VK_DOWN || wp == VK_UP) && count > 0) {
                const int step = wp == VK_DOWN ? 1 : -1;
                mgrSelect(hwnd, std::clamp(g_mgr.selected + step, 0, count - 1));
                return 0;
            }
            if (wp == VK_DELETE) {
                mgrDelete(hwnd);
                return 0;
            }
            if (wp == VK_INSERT) {
                mgrNew(hwnd);
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
                InvalidateRect(hwnd, nullptr, FALSE);  // 重画输入框焦点描边
                return 0;
            }
            if (id == IDOK) {
                mgrSave(hwnd);
                return 0;
            }
            if (id == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            switch (id) {
                case ID_MGR_BROWSE: {
                    wchar_t file[MAX_PATH] = L"";
                    OPENFILENAMEW ofn{};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"私钥文件\0*.*\0\0";
                    ofn.lpstrFile = file;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_mgr.key, file);
                    return 0;
                }
                case ID_MGR_SAVE:
                    mgrSave(hwnd);
                    return 0;
                case ID_MGR_DELETE:
                    mgrDelete(hwnd);
                    return 0;
                case ID_MGR_CLEAR_PWD:
                    mgrClearPassword(hwnd);
                    return 0;
                case ID_MGR_NEW:
                    mgrNew(hwnd);
                    return 0;
                case ID_MGR_CONNECT:
                    mgrConnect(hwnd);
                    return 0;
                case ID_MGR_CLOSE:
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    break;
            }
            return 0;
        }
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (dlg::isOnCloseButton(hwnd, pt)) return HTCLIENT;
            return pt.y < dp(50) ? HTCAPTION : HTCLIENT;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_mgr.dlg = nullptr;
            g_mgr.name = nullptr;
            g_mgr.host = nullptr;
            g_mgr.port = nullptr;
            g_mgr.user = nullptr;
            g_mgr.key = nullptr;
            g_mgr.note = nullptr;
            g_mgr.pwdClear = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void openSessionManager(bool startBlank) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = sessionMgrProc;
        wc.hInstance = g_app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"RshellSessionMgr";
        wc.style = CS_DROPSHADOW | CS_DBLCLKS;
        RegisterClassExW(&wc);
        registered = true;
    }

    g_mgr = SessionMgrState();
    const auto& sessions = g_app.store.sessions();
    if (!startBlank) {
        for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
            if (sessions[i].name == g_app.currentSession) {
                g_mgr.selected = i;
                break;
            }
        }
        if (g_mgr.selected < 0 && !sessions.empty()) g_mgr.selected = 0;
    }

    HWND dialog = dlg::createDialogWindow(L"RshellSessionMgr", L"会话", mgrWidth(), mgrHeight());
    if (!dialog) return;
    g_mgr.dlg = dialog;
    mgrEnsureVisible(g_mgr.selected);
    runModalLoop(dialog);

    if (!g_mgr.connectAfterClose) return;
    const std::string name = g_mgr.connectName;
    g_mgr.connectAfterClose = false;
    g_mgr.connectName.clear();
    connectStoredSession(name);
}

void connectStoredSession(const std::string& name) {
    const Session* session = g_app.store.find(name);
    if (!session) return;
    Session target = *session;
    if (g_app.connecting || g_app.shell->running() ||
        (!g_app.currentSession.empty() && g_app.currentSession != name)) newSessionTab();
    std::wstring password;
    bool remember = target.savePassword;

    if (target.keyPath.empty()) {
        bool reused = false;
        if (target.savePassword) {
            // 口令只可能保存在密码保险库里
            std::string saved;
            std::string error;
            if (loadCredential(credentialTarget(target.name), saved, &error)) {
                password = toWide(saved);
                wipeString(saved);
                reused = true;
                guiLog("使用已保存的密码连接 " + target.name);
            } else if (!g_app.vault.unlocked() && vaultFilePresent()) {
                // 保险库锁着：就地请求解锁（取消也没关系，下面会退回到手输密码），
                // 解锁成功后重试一次。
                guiLog("已保存的密码在锁着的保险库里（" + target.name + "）：" + error);
                setStatus(L"已保存的密码在密码保险库里，请先解锁");
                SendMessageW(g_app.main, WM_COMMAND, MAKEWPARAM(IDM_VAULT, 0), 0);
                if (g_app.vault.unlocked() &&
                    loadCredential(credentialTarget(target.name), saved, &error)) {
                    password = toWide(saved);
                    wipeString(saved);
                    reused = true;
                    guiLog("使用密码保险库里的密码连接 " + target.name);
                } else {
                    guiLog("读取已保存的密码失败（" + target.name + "）：" + error);
                    setStatus(L"已保存的密码不可用，请重新输入");
                }
            } else {
                guiLog("读取已保存的密码失败（" + target.name + "）：" + error);
                setStatus(L"已保存的密码不可用，请重新输入");
            }
        }
        if (!reused) {
            if (!promptPassword(L"连接 " + toWide(target.host), password, &remember,
                                target.savePassword))
                return;
            if (remember) {
                // 「记住密码」只写密码保险库：还没解锁就就地新建/解锁。
                // 用户在保险库对话框里取消时，本次连接照常进行，只是不落盘。
                if (!g_app.vault.unlocked()) {
                    vaultCommand();
                    if (!g_app.vault.unlocked()) {
                        remember = false;
                        setStatus(L"密码保险库未解锁，本次不会保存密码");
                    }
                }
                if (remember) {
                    // 等连接成功再写：口令刚被服务器拒绝的话不该被保存
                    g_app.pendingCredentialTarget = credentialTarget(target.name);
                    g_app.pendingCredentialSession = target.name;
                }
            } else if (target.savePassword) {
                // 用户这次取消了勾选：把旧的条目一起删掉
                deleteCredential(credentialTarget(target.name));
                target.savePassword = false;
                g_app.store.upsert(target);
                g_app.store.save();
            }
        }
    }
    std::wstring keyPassphrase;
    bool rememberKey = target.saveKeyPassphrase;
    if (!target.keyPath.empty() && privateKeyNeedsPassphrase(target.keyPath)) {
        bool reused = false;
        if (target.saveKeyPassphrase) {
            std::string saved;
            std::string error;
            if (loadCredential(keyPassphraseTarget(target.name), saved, &error)) {
                keyPassphrase = toWide(saved);
                wipeString(saved);
                reused = true;
                guiLog("使用已保存的私钥口令连接 " + target.name);
            } else if (!g_app.vault.unlocked() && vaultFilePresent()) {
                guiLog("已保存的私钥口令在锁着的保险库里（" + target.name + "）：" + error);
                setStatus(L"已保存的私钥口令在密码保险库里，请先解锁");
                SendMessageW(g_app.main, WM_COMMAND, MAKEWPARAM(IDM_VAULT, 0), 0);
                if (g_app.vault.unlocked() &&
                    loadCredential(keyPassphraseTarget(target.name), saved, &error)) {
                    keyPassphrase = toWide(saved);
                    wipeString(saved);
                    reused = true;
                    guiLog("使用密码保险库里的私钥口令连接 " + target.name);
                } else {
                    guiLog("读取已保存的私钥口令失败（" + target.name + "）：" + error);
                    setStatus(L"已保存的私钥口令不可用，请重新输入");
                }
            } else {
                guiLog("读取已保存的私钥口令失败（" + target.name + "）：" + error);
                setStatus(L"已保存的私钥口令不可用，请重新输入");
            }
        }
        if (!reused) {
            if (!promptKeyPassphrase(L"私钥口令 - " + toWide(target.host), keyPassphrase,
                                     &rememberKey, target.saveKeyPassphrase)) {
                wipeWide(password);
                wipeWide(keyPassphrase);
                return;
            }
            if (rememberKey) {
                if (!g_app.vault.unlocked()) {
                    vaultCommand();
                    if (!g_app.vault.unlocked()) {
                        rememberKey = false;
                        setStatus(L"密码保险库未解锁，本次不会保存私钥口令");
                    }
                }
                if (rememberKey) {
                    g_app.pendingKeyCredentialTarget = keyPassphraseTarget(target.name);
                    g_app.pendingKeyCredentialSession = target.name;
                }
            } else if (target.saveKeyPassphrase) {
                deleteCredential(keyPassphraseTarget(target.name));
                target.saveKeyPassphrase = false;
                g_app.store.upsert(target);
                g_app.store.save();
            }
        }
    }

    connectToSession(target, password, keyPassphrase);
    wipeWide(password);
    wipeWide(keyPassphrase);
}

// ---------------------------------------------------------------- 密码对话框
//
// 连接不带私钥的会话时问一次密码：只在内存里留到连接完成。
// 勾上「记住密码」的话，口令交给密码保险库（未解锁会先弹出新建/解锁对话框），
// sessions.ini 只写一个 savepassword=1 的标记。

struct PasswordDialogState {
    HWND dlg = nullptr;
    HWND edit = nullptr;
    HWND remember = nullptr;  // nullptr = 不显示勾选框
    bool showRemember = false;
    bool rememberChecked = false;
    bool ok = false;
    std::wstring rememberLabel;  // 勾选框文字（后端不同，措辞也不同）
};

PasswordDialogState g_passwordDialog;
std::wstring g_passwordValue;
std::wstring g_passwordTitle = L"登录";
std::wstring g_passwordLabel = L"密码";
bool g_passwordSavedBefore = false;

constexpr int ID_PWD_REMEMBER = 202;

// 把密码框里的明文副本抹掉。
// EDIT 的内部缓冲由系统管理，我们清不到底；这里能做到的是：在窗口销毁前
// 先把文本置空，缩短明文在控件里的存活时间，也让它不再能被 WM_GETTEXT /
// UI Automation 取到。
void scrubPasswordEdit() {
    if (!g_passwordDialog.edit) return;
    // 三个调用点都会紧接着 DestroyWindow，不需要再请求重绘
    SetWindowTextW(g_passwordDialog.edit, L"");
}

LRESULT CALLBACK passwordDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT colorResult = 0;
    if (dlg::handleDialogColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp),
                               colorResult))
        return colorResult;

    switch (msg) {
        case WM_CREATE: {
            g_passwordDialog.edit =
                addEdit(hwnd, 201, L"", dp(104), dp(84), dp(292), dp(30), ES_PASSWORD);
            if (g_passwordDialog.showRemember) {
                g_passwordDialog.remember = addCheckbox(
                    hwnd, ID_PWD_REMEMBER, g_passwordDialog.rememberLabel.c_str(), dp(72),
                    dp(120), dp(316), dp(24));
                EnableWindow(g_passwordDialog.remember, TRUE);
            }
            // 没有勾选框时按钮上移，窗口也矮一些（见 promptPassword）
            const int buttonY = g_passwordDialog.showRemember ? dp(156) : dp(132);
            addButton(hwnd, IDCANCEL, L"取消", dp(230), buttonY, dp(76), dp(32));
            addButton(hwnd, IDOK, L"连接", dp(314), buttonY, dp(74), dp(32), true);
            return 0;
        }
        case WM_PAINT: {
            std::wstring subtitle;
            if (g_passwordSavedBefore) {
                subtitle = L"上次保存的" + g_passwordLabel + L"已失效，请重新输入";
            } else if (g_passwordDialog.showRemember) {
                subtitle = L"勾选后存到 " + activePasswordBackend() + L"，下次自动使用";
            } else {
                subtitle = g_passwordLabel + L"只用于本次连接，不会写入配置文件";
            }
            paintDialog(hwnd, g_passwordTitle, subtitle, [&](HDC dc) {
                drawText(dc, g_passwordLabel.c_str(),
                         RECT{dp(24), dp(84), dp(100), dp(114)}, ui::textDim,
                         g_app.uiFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                paintEditFrame(dc, hwnd, g_passwordDialog.edit);
            });
            return 0;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT& dis = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis.hwndItem == g_passwordDialog.remember) {
                paintCheckbox(dis.hDC, dis.rcItem, g_passwordDialog.rememberLabel.c_str(),
                              g_passwordDialog.rememberChecked,
                              (dis.itemState & ODS_SELECTED) != 0);
                return TRUE;
            }
            dlg::handleDrawItem(hwnd, dis);
            return TRUE;
        }
        case WM_MOUSEMOVE:
            dlg::trackCloseHover(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONDOWN:
            focusEditAt(hwnd, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_LBUTTONUP:
            if (dlg::isOnCloseButton(hwnd, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}))
                DestroyWindow(hwnd);
            return 0;
        case WM_SETFOCUS:
            if (g_passwordDialog.edit) {
                SetFocus(g_passwordDialog.edit);
                SendMessageW(g_passwordDialog.edit, EM_SETSEL, 0, -1);
            }
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (id == ID_PWD_REMEMBER) {
                g_passwordDialog.rememberChecked = !g_passwordDialog.rememberChecked;
                InvalidateRect(g_passwordDialog.remember, nullptr, FALSE);
                return 0;
            }
            if (id == IDOK) {
                g_passwordValue = textOf(g_passwordDialog.edit);
                // 空密码没有可保存的内容，别把勾选状态带出去
                if (g_passwordValue.empty()) g_passwordDialog.rememberChecked = false;
                g_passwordDialog.ok = true;
                // 值已经取到 g_passwordValue，先把控件里的副本清掉再销毁窗口
                scrubPasswordEdit();
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDCANCEL) {
                scrubPasswordEdit();
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (dlg::isOnCloseButton(hwnd, pt)) return HTCLIENT;
            return pt.y < dp(50) ? HTCAPTION : HTCLIENT;
        }
        case WM_CLOSE:
            scrubPasswordEdit();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_passwordDialog.dlg = nullptr;
            g_passwordDialog.edit = nullptr;
            g_passwordDialog.remember = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool promptSecret(const std::wstring& title, const std::wstring& label,
                  const std::wstring& rememberLabel, std::wstring& value,
                  bool* remember, bool savedBefore) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = passwordDialogProc;
        wc.hInstance = g_app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"RshellPasswordDialog";
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
        registered = true;
    }
    g_passwordDialog = PasswordDialogState();
    g_passwordDialog.showRemember = remember != nullptr;
    g_passwordDialog.rememberChecked = remember != nullptr && *remember;
    g_passwordDialog.rememberLabel = rememberLabel;
    g_passwordLabel = label;
    g_passwordSavedBefore = savedBefore;
    g_passwordValue.clear();
    g_passwordTitle = title;

    const int height = remember ? dp(214) : dp(190);
    HWND dlg = dlg::createDialogWindow(L"RshellPasswordDialog", title, dp(420), height);
    if (!dlg) return false;
    g_passwordDialog.dlg = dlg;
    runModalLoop(dlg);

    const bool ok = g_passwordDialog.ok;
    if (ok) {
        value = g_passwordValue;
        if (remember) *remember = g_passwordDialog.rememberChecked;
    }
    // 对话框自己的那份副本用完就抹掉（调用方拿走的那份由它自己管）
    wipeWideString(g_passwordValue);
    return ok;
}

bool promptPassword(const std::wstring& title, std::wstring& password, bool* remember,
                    bool savedBefore) {
    return promptSecret(title, L"密码", rememberPasswordCheckboxText(), password,
                        remember, savedBefore);
}

bool promptKeyPassphrase(const std::wstring& title, std::wstring& passphrase,
                         bool* remember, bool savedBefore) {
    return promptSecret(title, L"私钥口令", rememberKeyPassphraseCheckboxText(),
                        passphrase, remember, savedBefore);
}

// ---------------------------------------------------------------- 密码保险库
//
// 主密码保险库的「新建 / 解锁」都在这个对话框里完成：
//   - 口令校验就地做：错了只在框里给出原因并选中原来的输入，不关窗口；
//   - 口令只活到 Vault::create()/open() 返回：控件销毁前清空，Vault 自己
//     用 DPAPI 密文 + VirtualLock 保管 DEK；
//   - 解锁模式在「文件里有 TPM slot 且本机 TPM 可用」时默认先自动试 TPM：
//     TPM 在后台线程里解锁，期间点「取消」就改用主密码；失败也直接落回主密码输入；
//     输入框旁边保留一个「用 TPM 解锁」按钮，失败后可以重试；
//   - 新建时可以顺带启用 TPM：enableTpm 要在 TPM 里生成 RSA 密钥（首次约 3 秒），
//     所以先把界面刷成「正在…」，否则用户会以为点了没反应。

enum class VaultPromptMode { Create, Unlock };

struct VaultPromptState {
    VaultPromptMode mode = VaultPromptMode::Unlock;
    HWND dlg = nullptr;
    HWND password = nullptr;
    HWND confirm = nullptr;    // 仅新建
    HWND tpm = nullptr;        // 仅新建：启用 TPM 的勾选框（本机没 TPM 就不建）
    HWND tpmButton = nullptr;  // 仅解锁：用 TPM 解锁
    bool tpmAvailable = false;
    bool hasTpmSlot = false;
    bool tpmChecked = false;
    bool busy = false;
    bool ok = false;
    bool viaTpm = false;
    bool tpmEnableFailed = false;  // 新建成功但 TPM 没启用：关掉窗口后再提示
    std::string tpmEnableError;
    std::wstring note;  // 框内一行提示（进度 / 错误）
    bool noteIsError = false;
};

VaultPromptState g_vaultPrompt;

// TPM 解锁是同步的 BCrypt/NCrypt 调用（实测 0.4~0.8 秒），放到工作线程里跑，
// 这样进度框还能响应「改用主密码」。取消后结果直接丢弃（Vault 析构时清密钥）。
struct TpmAttempt {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> ready{false};  // 结果字段写入完成后置位（release/acquire）
    bool ok = false;
    std::string error;
    Vault vault;
};
std::shared_ptr<TpmAttempt> g_tpmAttempt;

constexpr UINT WM_VAULT_TPM_START = WM_APP + 20;  // 对话框内部：WM_CREATE 之后开始尝试

constexpr int ID_VAULT_TPM = 301;
constexpr int ID_VAULT_TPM_UNLOCK = 302;
constexpr int kVaultDlgW = 460;

// 新建要多一个确认框和勾选框，窗口比解锁高
int vaultDialogHeight(VaultPromptMode mode) {
    return mode == VaultPromptMode::Create ? dp(302) : dp(252);
}

void vaultPromptSetNote(HWND hwnd, const std::wstring& text, bool isError) {
    g_vaultPrompt.note = text;
    g_vaultPrompt.noteIsError = isError;
    if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
}

// 慢操作（PBKDF2 0.1 秒 / TPM 0.4~3 秒）期间禁用控件，并立刻把「正在…」画出来
void vaultPromptSetBusy(HWND hwnd, bool busy, const std::wstring& note) {
    g_vaultPrompt.busy = busy;
    for (HWND control : {g_vaultPrompt.password, g_vaultPrompt.confirm, g_vaultPrompt.tpm,
                         g_vaultPrompt.tpmButton}) {
        if (control && IsWindow(control)) EnableWindow(control, busy ? FALSE : TRUE);
    }
    if (hwnd) {
        g_vaultPrompt.note = note;
        g_vaultPrompt.noteIsError = false;
        // 后面是同步的耗时调用：不强制重绘的话，提示要等操作结束才出现
        InvalidateRect(hwnd, nullptr, TRUE);
        if (busy) UpdateWindow(hwnd);
    }
}

// 取消正在跑的 TPM 尝试：结果之后才回来也没关系，工作线程会看到 cancelled
// 并把 Vault 直接丢弃（析构时清 DEK / 条目）。
void vaultPromptCancelPendingTpm() {
    if (!g_tpmAttempt) return;
    g_tpmAttempt->cancelled.store(true);
    g_tpmAttempt.reset();
}

// 默认入口：有 TPM slot 且本机 TPM 可用时自动尝试。放到工作线程里跑，
// 对话框在这 0.4~0.8 秒内仍能响应「取消（改用主密码）」。
void vaultPromptStartTpm(HWND hwnd) {
    if (g_vaultPrompt.busy || g_tpmAttempt) return;
    if (!g_vaultPrompt.hasTpmSlot || !g_vaultPrompt.tpmAvailable) return;

    auto attempt = std::make_shared<TpmAttempt>();
    g_tpmAttempt = attempt;
    vaultPromptSetBusy(hwnd, true, L"正在用 TPM 解锁…（点「取消」改用主密码）");

    const std::wstring path = Vault::defaultPath();
    std::thread([attempt, path]() {
        Vault opened;
        std::string error;
        const bool ok = Vault::openWithTpm(path, opened, &error);
        attempt->ok = ok;
        attempt->error = error;
        attempt->vault = std::move(opened);
        attempt->ready.store(true, std::memory_order_release);
        auto* payload = new std::shared_ptr<TpmAttempt>(attempt);
        if (!PostMessageW(g_app.main, WM_VAULT_TPM_DONE, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }).detach();
}

// TPM 试过但用户不想等（或失败了）之后回到主密码输入。
void vaultPromptSwitchToPassword(HWND hwnd) {
    vaultPromptCancelPendingTpm();
    vaultPromptSetBusy(hwnd, false, L"");
    vaultPromptSetNote(hwnd, L"请输入主密码解锁", false);
    if (g_vaultPrompt.password) {
        SetFocus(g_vaultPrompt.password);
        SendMessageW(g_vaultPrompt.password, EM_SETSEL, 0, -1);
    }
}

// 关窗之前先把控件里的明文清掉（EDIT 内部缓冲由系统管理，清不彻底，
// 但至少不再能被 WM_GETTEXT / UI Automation 取到）
void vaultPromptClose(HWND hwnd) {
    vaultPromptCancelPendingTpm();
    if (g_vaultPrompt.password) SetWindowTextW(g_vaultPrompt.password, L"");
    if (g_vaultPrompt.confirm) SetWindowTextW(g_vaultPrompt.confirm, L"");
    DestroyWindow(hwnd);
}

void vaultPromptSubmit(HWND hwnd) {
    if (g_vaultPrompt.busy) return;
    const bool creating = g_vaultPrompt.mode == VaultPromptMode::Create;

    std::wstring password = textOf(g_vaultPrompt.password);
    if (password.empty()) {
        vaultPromptSetNote(hwnd, L"主密码不能为空", true);
        SetFocus(g_vaultPrompt.password);
        return;
    }

    if (creating) {
        std::wstring confirm = textOf(g_vaultPrompt.confirm);
        // 第一次回车：主密码填了、确认框还空着，就把焦点挪过去（不算提交）
        if (confirm.empty()) {
            wipeWide(password);
            SetFocus(g_vaultPrompt.confirm);
            return;
        }
        if (confirm != password) {
            wipeWide(confirm);
            wipeWide(password);
            SetWindowTextW(g_vaultPrompt.confirm, L"");
            vaultPromptSetNote(hwnd, L"两次输入的主密码不一致", true);
            SetFocus(g_vaultPrompt.confirm);
            return;
        }
        wipeWide(confirm);
        // 界面上的下限；Vault::create 本身只拒绝空口令
        if (password.size() < 8) {
            wipeWide(password);
            vaultPromptSetNote(hwnd, L"主密码至少 8 个字符（建议 12 个以上）", true);
            SetFocus(g_vaultPrompt.password);
            SendMessageW(g_vaultPrompt.password, EM_SETSEL, 0, -1);
            return;
        }
    }

    vaultPromptSetBusy(hwnd, true, creating ? L"正在创建保险库…" : L"正在用主密码解锁…");
    const std::wstring path = Vault::defaultPath();
    std::string error;
    Vault opened;
    const bool success =
        creating ? Vault::create(path, password, opened, kVaultDefaultIterations, &error)
                 : Vault::open(path, password, opened, &error);
    wipeWide(password);  // 已交给 Vault，本地这份明文不再需要

    if (!success) {
        vaultPromptSetBusy(hwnd, false, L"");
        vaultPromptSetNote(hwnd, toWide(error.empty() ? "无法打开保险库" : error), true);
        SetFocus(g_vaultPrompt.password);
        SendMessageW(g_vaultPrompt.password, EM_SETSEL, 0, -1);
        return;
    }

    // 勾了「同时启用 TPM」：失败不影响保险库本身，记下来关窗后提示
    if (creating && g_vaultPrompt.tpmChecked) {
        vaultPromptSetBusy(hwnd, true, L"正在让 TPM 生成密钥（首次约 3 秒）…");
        std::string tpmError;
        if (!opened.enableTpm(&tpmError)) {
            g_vaultPrompt.tpmEnableFailed = true;
            g_vaultPrompt.tpmEnableError = tpmError;
        }
    }

    g_app.vault = std::move(opened);
    g_vaultPrompt.ok = true;
    vaultPromptClose(hwnd);
}

LRESULT CALLBACK vaultDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT colorResult = 0;
    if (dlg::handleDialogColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp),
                               colorResult))
        return colorResult;

    const bool creating = g_vaultPrompt.mode == VaultPromptMode::Create;
    switch (msg) {
        case WM_CREATE: {
            if (creating) {
                g_vaultPrompt.password =
                    addEdit(hwnd, 201, L"", dp(96), dp(76), dp(340), dp(30), ES_PASSWORD);
                g_vaultPrompt.confirm =
                    addEdit(hwnd, 202, L"", dp(96), dp(120), dp(340), dp(30), ES_PASSWORD);
                if (g_vaultPrompt.tpmAvailable) {
                    g_vaultPrompt.tpm = addCheckbox(
                        hwnd, ID_VAULT_TPM, L"同时启用 TPM 解锁（本机 TPM 保护，免输主密码）",
                        dp(24), dp(158), dp(412), dp(26));
                }
                addButton(hwnd, IDCANCEL, L"取消", dp(276), dp(252), dp(76), dp(32));
                addButton(hwnd, IDOK, L"创建", dp(360), dp(252), dp(76), dp(32), true);
            } else {
                g_vaultPrompt.password =
                    addEdit(hwnd, 201, L"", dp(96), dp(84), dp(340), dp(30), ES_PASSWORD);
                addButton(hwnd, IDCANCEL, L"取消", dp(276), dp(192), dp(76), dp(32));
                addButton(hwnd, IDOK, L"解锁", dp(360), dp(192), dp(76), dp(32), true);
                if (g_vaultPrompt.hasTpmSlot && g_vaultPrompt.tpmAvailable) {
                    g_vaultPrompt.tpmButton = addButton(hwnd, ID_VAULT_TPM_UNLOCK, L"用 TPM 解锁",
                                                        dp(24), dp(192), dp(132), dp(32));
                    // 默认先试 TPM；对话框画出来之后立刻开始，不阻塞创建流程
                    PostMessageW(hwnd, WM_VAULT_TPM_START, 0, 0);
                }
            }
            SetFocus(g_vaultPrompt.password);
            return 0;
        }
        case WM_PAINT: {
            const std::wstring subtitle =
                creating ? L"这条主密码是唯一入口，忘记后无法找回"
                         : L"口令只用于本次解锁；随时可以在主菜单里锁定";
            paintDialog(hwnd, L"密码保险库", subtitle, [&](HDC dc) {
                const int labelTop = creating ? dp(76) : dp(84);
                drawText(dc, L"主密码", RECT{dp(24), labelTop, dp(92), labelTop + dp(30)},
                         ui::textDim, g_app.uiFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                paintEditFrame(dc, hwnd, g_vaultPrompt.password);
                if (creating) {
                    drawText(dc, L"再次输入", RECT{dp(24), dp(120), dp(92), dp(150)}, ui::textDim,
                             g_app.uiFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    paintEditFrame(dc, hwnd, g_vaultPrompt.confirm);
                    if (!g_vaultPrompt.tpmAvailable) {
                        drawText(dc, L"本机没有可用的 TPM，只能用主密码解锁",
                                 RECT{dp(24), dp(158), dp(436), dp(184)}, ui::textFaint,
                                 g_app.tinyFont,
                                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }
                }
                if (!g_vaultPrompt.note.empty()) {
                    const int noteTop = creating ? dp(190) : dp(124);
                    drawText(dc, g_vaultPrompt.note,
                             RECT{dp(24), noteTop, dp(436), noteTop + dp(58)},
                             g_vaultPrompt.noteIsError ? ui::danger : ui::textDim, g_app.tinyFont,
                             DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
                }
            });
            return 0;
        }
        case WM_VAULT_TPM_START:
            vaultPromptStartTpm(hwnd);
            return 0;
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT& dis = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (g_vaultPrompt.tpm && dis.hwndItem == g_vaultPrompt.tpm) {
                paintCheckbox(dis.hDC, dis.rcItem,
                              L"同时启用 TPM 解锁（本机 TPM 保护，免输主密码）",
                              g_vaultPrompt.tpmChecked, (dis.itemState & ODS_SELECTED) != 0);
                return TRUE;
            }
            dlg::handleDrawItem(hwnd, dis);
            return TRUE;
        }
        case WM_MOUSEMOVE:
            dlg::trackCloseHover(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONDOWN:
            // 点在圆角外框的留白里也要能聚焦输入框
            focusEditAt(hwnd, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == ID_VAULT_TPM) {
                g_vaultPrompt.tpmChecked = !g_vaultPrompt.tpmChecked;
                InvalidateRect(g_vaultPrompt.tpm, nullptr, FALSE);
                return 0;
            }
            if (id == ID_VAULT_TPM_UNLOCK) {
                vaultPromptStartTpm(hwnd);
                return 0;
            }
            if (id == IDOK) {
                vaultPromptSubmit(hwnd);
                return 0;
            }
            if (id == IDCANCEL) {
                // TPM 正在后台尝试时，「取消」不是关闭窗口，而是改用主密码
                if (g_tpmAttempt) {
                    vaultPromptSwitchToPassword(hwnd);
                } else {
                    vaultPromptClose(hwnd);
                }
                return 0;
            }
            return 0;
        }
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (dlg::isOnCloseButton(hwnd, pt)) return HTCLIENT;
            return pt.y < dp(50) ? HTCAPTION : HTCLIENT;
        }
        case WM_CLOSE:
            // 正在算 PBKDF2 / 等 TPM 时不让关：这时口令已经交出去，关掉也没意义
            if (!g_vaultPrompt.busy) vaultPromptClose(hwnd);
            return 0;
        case WM_DESTROY:
            g_vaultPrompt.dlg = nullptr;
            g_vaultPrompt.password = nullptr;
            g_vaultPrompt.confirm = nullptr;
            g_vaultPrompt.tpm = nullptr;
            g_vaultPrompt.tpmButton = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct VaultPromptResult {
    bool ok = false;
    bool created = false;
    bool viaTpm = false;
};

// 弹「新建 / 解锁」对话框；成功时 g_app.vault 已解锁。
VaultPromptResult promptVault(VaultPromptMode mode) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = vaultDialogProc;
        wc.hInstance = g_app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"RshellVaultDialog";
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
        registered = true;
    }

    const std::wstring path = Vault::defaultPath();
    vaultPromptCancelPendingTpm();  // 上一次尝试如果还没结束，结果作废
    g_vaultPrompt = VaultPromptState();
    g_vaultPrompt.mode = mode;
    // 两个探测都只读文件头 / 查一次 provider，几十微秒，不会拖慢弹窗
    g_vaultPrompt.tpmAvailable = Vault::tpmAvailable();
    g_vaultPrompt.hasTpmSlot = mode == VaultPromptMode::Unlock && Vault::tpmSlotPresent(path);
    if (g_vaultPrompt.hasTpmSlot && !g_vaultPrompt.tpmAvailable) {
        // 换了机器 / TPM 被清掉：直接说明为什么没走默认的 TPM 路径
        g_vaultPrompt.note = L"本机 TPM 不可用，请输入主密码";
    }

    const std::wstring title =
        mode == VaultPromptMode::Create ? L"新建密码保险库" : L"解锁密码保险库";
    HWND dlg = dlg::createDialogWindow(L"RshellVaultDialog", title, dp(kVaultDlgW),
                                       vaultDialogHeight(mode));
    if (!dlg) return VaultPromptResult{};
    g_vaultPrompt.dlg = dlg;
    runModalLoop(dlg);

    VaultPromptResult result;
    result.ok = g_vaultPrompt.ok;
    result.created = g_vaultPrompt.ok && mode == VaultPromptMode::Create;
    result.viaTpm = g_vaultPrompt.viaTpm;

    // 保险库已经建好，只是 TPM 那一步失败：单独提示，别让用户以为整个创建失败
    if (result.ok && g_vaultPrompt.tpmEnableFailed) {
        messageWarn(L"TPM 解锁没有启用",
                    L"保险库已经创建，可以正常用主密码解锁；但这次没能启用 TPM 解锁：\n\n" +
                        toWide(g_vaultPrompt.tpmEnableError) +
                        L"\n\nTPM 只是免输主密码的快捷方式，主密码始终可用。");
    }
    g_vaultPrompt = VaultPromptState();
    return result;
}

// 连接按钮 / 菜单里的「连接」：已连接时断开，否则打开会话管理窗口
void onConnectCommand() {
    if (g_app.shell->running()) {
        disconnectSession();
        return;
    }
    if (g_app.connecting) return;
    if (g_app.store.find(g_app.currentSession)) connectStoredSession(g_app.currentSession);
    else openSessionManager();
}

void revealTerminalMatch() {
    if (g_app.searchIndex < 0 || g_app.searchIndex >= static_cast<int>(g_app.searchMatches.size())) return;
    const auto& match = g_app.searchMatches[static_cast<std::size_t>(g_app.searchIndex)];
    const int top = std::max(0, static_cast<int>(match.line) - g_app.termData.rows() / 2);
    const int offset = std::clamp(static_cast<int>(g_app.termData.scrollbackCount()) - top,
                                  0, static_cast<int>(g_app.termData.scrollbackCount()));
    g_app.termData.scrollView(offset - g_app.termData.scrollOffset());
}

void refreshTerminalSearch(bool reveal) {
    if (!foregroundSession()) return;
    if (!g_app.searchOpen) return;
    std::u32string query;
    const std::wstring text = textOf(g_app.searchEdit);
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t ch = text[i];
        if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < text.size() &&
            text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
            ch = 0x10000 + ((ch - 0xD800) << 10) + (text[++i] - 0xDC00);
        query.push_back(ch);
    }
    g_app.searchMatches = g_app.termData.findAll(query);
    if (g_app.searchMatches.empty()) g_app.searchIndex = -1;
    else if (reveal || g_app.searchIndex < 0) g_app.searchIndex = 0;
    else g_app.searchIndex = std::min(g_app.searchIndex, static_cast<int>(g_app.searchMatches.size()) - 1);
    g_app.searchDirty = false;
    if (reveal) revealTerminalMatch();
    InvalidateRect(g_app.main, nullptr, FALSE);
    InvalidateRect(g_app.term, nullptr, FALSE);
}

void stepTerminalSearch(int direction) {
    if (g_app.searchDirty) refreshTerminalSearch(false);
    const int count = static_cast<int>(g_app.searchMatches.size());
    if (count == 0) return;
    g_app.searchIndex = (g_app.searchIndex + direction + count) % count;
    revealTerminalMatch();
    InvalidateRect(g_app.main, nullptr, FALSE);
    InvalidateRect(g_app.term, nullptr, FALSE);
}

WNDPROC g_searchEditProc = nullptr;
LRESULT CALLBACK searchEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN || wp == VK_F3) {
            stepTerminalSearch((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            return 0;
        }
        if (wp == VK_ESCAPE) { showTerminalSearch(false); return 0; }
        if (wp == VK_TAB) { SetFocus(g_app.term); return 0; }
        if ((wp == 'A' || wp == 'F') && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB || wp == 1 || wp == 6))
        return 0;
    return CallWindowProcW(g_searchEditProc, hwnd, msg, wp, lp);
}

void showTerminalSearch(bool show) {
    if (show && !g_app.searchEdit) {
        g_app.searchEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                                          0, 0, 0, 0, g_app.main,
                                          reinterpret_cast<HMENU>(ID_SEARCH_EDIT), g_app.inst, nullptr);
        if (!g_app.searchEdit) return;
        g_searchEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_app.searchEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(searchEditProc)));
        SendMessageW(g_app.searchEdit, EM_SETCUEBANNER, FALSE,
                     reinterpret_cast<LPARAM>(L"搜索输出（区分大小写）"));
    }
    g_app.searchOpen = show;
    if (g_app.searchEdit) ShowWindow(g_app.searchEdit, show ? SW_SHOW : SW_HIDE);
    layoutMain(g_app.main);
    if (show) {
        SetFocus(g_app.searchEdit);
        SendMessageW(g_app.searchEdit, EM_SETSEL, 0, -1);
        refreshTerminalSearch();
    } else SetFocus(g_app.term);
    InvalidateRect(g_app.main, nullptr, FALSE);
    InvalidateRect(g_app.term, nullptr, FALSE);
}

// ---------------------------------------------------------------- SFTP 文件面板
//
// 连上之后左侧从会话列表切换成远端文件树。所有远端操作都在 RemoteFs 的
// 独立线程里跑，UI 只负责画和派发，不会因为列大目录而卡住终端。

int fileItemH() { return dp(32); }
int filePad() { return dp(4); }

int fileMaxScroll() {
    if (!g_app.list) return 0;
    RECT rc;
    GetClientRect(g_app.list, &rc);
    const int n = static_cast<int>(g_app.fsEntries.size());
    const int content = filePad() * 2 + n * fileItemH();
    return std::max(0, content - static_cast<int>(rc.bottom));
}

int fileIndexFromPoint(int x, int y) {
    if (!g_app.list) return -1;
    RECT rc;
    GetClientRect(g_app.list, &rc);
    if (x < 0 || x >= rc.right || y < 0 || y >= rc.bottom) return -1;
    const int local = y + g_app.fsScroll - filePad();
    if (local < 0) return -1;
    const int index = local / fileItemH();
    if (index < 0 || index >= static_cast<int>(g_app.fsEntries.size())) return -1;
    return index;
}

void fileEnsureVisible(int index) {
    if (!g_app.list || index < 0) return;
    RECT rc;
    GetClientRect(g_app.list, &rc);
    const int top = filePad() + index * fileItemH();
    const int bottom = top + fileItemH();
    if (top - g_app.fsScroll < 0) g_app.fsScroll = top;
    if (bottom - g_app.fsScroll > rc.bottom) g_app.fsScroll = bottom - rc.bottom + filePad();
    g_app.fsScroll = std::clamp(g_app.fsScroll, 0, fileMaxScroll());
}

const RemoteEntry* selectedFileEntry() {
    if (g_app.fsSel < 0 || g_app.fsSel >= static_cast<int>(g_app.fsEntries.size())) return nullptr;
    return &g_app.fsEntries[static_cast<std::size_t>(g_app.fsSel)];
}

void fsRefresh() {
    if (!g_app.fsPath.empty()) g_app.fs->listDir(g_app.fsPath);
}

// quiet：由终端目录跟随触发的导航。远端目录可能进不去（权限 / 已被删除），
// 这种失败只写状态栏和面板里的提示，不弹模态框打断用户敲命令。
void fsNavigate(const std::string& path, bool quiet = false) {
    g_app.fsPath = normalizeRemotePath(path);
    g_app.fsQuietPath = quiet ? g_app.fsPath : std::string();
    g_app.fsSel = -1;
    g_app.fsScroll = 0;
    g_app.fsError.clear();
    g_app.fsEntries.clear();
    g_app.fs->listDir(g_app.fsPath);
    if (g_app.list) InvalidateRect(g_app.list, nullptr, FALSE);
    if (g_app.main) InvalidateRect(g_app.main, nullptr, FALSE);
}

// 终端里 cd 之后让文件面板跟过去。单向：面板里的浏览不会回写终端。
// 只有 OSC 7 上报的目录真的变了才会触发（见 Terminal::takeCwdChange），
// 所以用户在面板里手动翻目录后，下一条命令的提示符不会把他拽回去。
void syncTerminalCwd() {
    if (!g_app.fsReady) return;
    const std::string& cwd = g_app.termData.cwd();
    if (cwd.empty() || cwd == g_app.fsPath) return;
    fsNavigate(cwd, true);
}

bool promptTextDialog(const std::wstring& title, const std::wstring& label,
                      std::wstring& value);

// 路径框输入：/ 开头是绝对路径，~ 是 home，其余按当前目录解析
std::string resolveFsInput(const std::string& raw) {
    const std::string text = trim(raw);
    if (text.empty()) return std::string();
    if (text == "~") return g_app.fsHome;
    if (text.rfind("~/", 0) == 0) {
        if (!isAbsoluteRemotePath(g_app.fsHome)) return std::string();
        return remoteJoin(g_app.fsHome, text.substr(2));
    }
    if (isAbsoluteRemotePath(text)) return normalizeRemotePath(text);
    return remoteJoin(g_app.fsPath, text);
}

WNDPROC g_pathEditProc = nullptr;

LRESULT CALLBACK pathEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_KEYDOWN:
            if ((wp == 'A' || wp == 'L') && (GetKeyState(VK_CONTROL) & 0x8000)) {
                SendMessageW(hwnd, EM_SETSEL, 0, -1);
                return 0;
            }
            if (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB) {
                std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(hwnd)) + 1, L'\0');
                GetWindowTextW(hwnd, value.data(), static_cast<int>(value.size()));
                value.resize(std::wcslen(value.c_str()));
                // 先退出编辑，再提交，避免异步刷新或错误弹窗覆盖输入状态。
                SetFocus(g_app.list);
                ShowWindow(hwnd, SW_HIDE);
                if (wp == VK_RETURN && g_app.fsReady) {
                    const std::string target = resolveFsInput(toUtf8(value));
                    if (!target.empty() && target != g_app.fsPath) fsNavigate(target);
                }
                return 0;
            }
            break;
        case WM_CHAR:
            if (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB || wp == 1 || wp == 12)
                return 0;
            break;
        case WM_KILLFOCUS:
            ShowWindow(hwnd, SW_HIDE);
            InvalidateRect(g_app.main, &g_app.fsPathRect, FALSE);
            break;
        default:
            break;
    }
    return CallWindowProcW(g_pathEditProc, hwnd, msg, wp, lp);
}

void focusFsPath() {
    if (!g_app.fsReady) {
        messageWarn(L"SFTP 未就绪", L"文件通道还没连上，请稍后再试。");
        return;
    }
    if (!g_app.fsPathEdit) {
        g_app.fsPathEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                                          0, 0, 0, 0, g_app.main, nullptr, g_app.inst, nullptr);
        if (!g_app.fsPathEdit) return;
        g_pathEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_app.fsPathEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pathEditProc)));
    }
    layoutMain(g_app.main);
    SetWindowTextW(g_app.fsPathEdit, toWide(g_app.fsPath).c_str());
    ShowWindow(g_app.fsPathEdit, SW_SHOW);
    SetFocus(g_app.fsPathEdit);
    SendMessageW(g_app.fsPathEdit, EM_SETSEL, 0, -1);
    InvalidateRect(g_app.main, &g_app.fsPathRect, FALSE);
}

void fsGoUp() {
    if (g_app.fsPath.empty() || g_app.fsPath == "/") return;
    const std::string parent = remoteDirName(g_app.fsPath);
    if (parent.empty() || parent == g_app.fsPath) return;
    fsNavigate(parent);
}

// 双击：目录进入，文件弹出另存为并下载
void fsOpenEntry(int index) {
    if (index < 0 || index >= static_cast<int>(g_app.fsEntries.size())) return;
    const RemoteEntry entry = g_app.fsEntries[static_cast<std::size_t>(index)];
    if (entry.isDir()) {
        fsNavigate(entry.path);
        return;
    }

    wchar_t file[MAX_PATH] = L"";
    wcsncpy_s(file, toWide(entry.name).c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_app.main;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"所有文件\0*.*\0\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    // 登记下载任务：状态栏会展开列表显示进度，可随时取消
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    addDownloadTask(entry.path, toWide(entry.name), entry.size, false, cancel);
    g_app.fs->download(entry.path, toUtf8(file), cancel);
    setStatus(L"正在下载 " + toWide(entry.name) + L" …");
}

// 上传：支持多选，逐个排队（RemoteFs 内部串行执行）
void fsUploadFiles() {
    if (!g_app.fsReady) {
        messageWarn(L"SFTP 未就绪", L"文件通道还没连上，请稍后再试。");
        return;
    }
    std::vector<wchar_t> buffer(64 * 1024, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_app.main;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = L"所有文件\0*.*\0\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    std::vector<std::wstring> files;
    const std::wstring first = buffer.data();
    const wchar_t* next = buffer.data() + first.size() + 1;
    if (*next == L'\0') {
        files.push_back(first);
    } else {
        while (*next) {
            files.push_back(first + L"\\" + next);
            next += wcslen(next) + 1;
        }
    }
    if (files.empty()) return;

    for (const std::wstring& local : files) {
        const std::string name = localBaseName(toUtf8(local));
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        g_app.fs->upload(toUtf8(local), remoteJoin(g_app.fsPath, name), cancel);
        ++g_app.fsPendingUploads;
    }
    setStatus(L"正在上传 " + std::to_wstring(files.size()) + L" 个文件到 " + toWide(g_app.fsPath) +
              L" …");
    updateButtons();
}

void fsDeleteSelected() {
    const RemoteEntry* entry = selectedFileEntry();
    if (!entry) return;
    std::wstring text = L"确定删除「" + toWide(entry->name) + L"」吗？";
    if (entry->isDir()) text += L"\n目录必须为空才能删除。";
    if (!messageConfirm(L"删除远端文件", text, L"删除", L"取消")) return;
    g_app.fs->remove(entry->path, entry->isDir());
    setStatus(L"正在删除 " + toWide(entry->name) + L" …");
}

// ---------------------------------------------------------------- 单行输入对话框

struct TextPromptState {
    HWND dlg = nullptr;
    HWND edit = nullptr;
    bool ok = false;
};

TextPromptState g_textPrompt;
std::wstring g_textPromptTitle;
std::wstring g_textPromptLabel;
std::wstring g_textPromptValue;

LRESULT CALLBACK textPromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT colorResult = 0;
    if (dlg::handleDialogColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp),
                               colorResult))
        return colorResult;

    switch (msg) {
        case WM_CREATE:
            g_textPrompt.edit =
                addEdit(hwnd, 301, g_textPromptValue, dp(22), dp(66), dp(376), dp(30));
            addButton(hwnd, IDCANCEL, L"取消", dp(222), dp(148), dp(80), dp(32));
            addButton(hwnd, IDOK, L"确定", dp(318), dp(148), dp(80), dp(32), true);
            return 0;
        case WM_PAINT:
            paintDialog(hwnd, g_textPromptTitle, L"", [&](HDC dc) {
                drawText(dc, g_textPromptLabel, RECT{dp(22), dp(42), dp(400), dp(62)},
                         ui::textDim, g_app.tinyFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                paintEditFrame(dc, hwnd, g_textPrompt.edit);
            });
            return 0;
        case WM_DRAWITEM:
            dlg::handleDrawItem(hwnd, *reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_MOUSEMOVE:
            dlg::trackCloseHover(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP:
            if (dlg::isOnCloseButton(hwnd, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}))
                DestroyWindow(hwnd);
            return 0;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (dlg::isOnCloseButton(hwnd, pt)) return HTCLIENT;
            return pt.y < dp(50) ? HTCAPTION : HTCLIENT;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                g_textPromptValue = textOf(g_textPrompt.edit);
                g_textPrompt.ok = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_SETFOCUS:
            if (g_textPrompt.edit) {
                SetFocus(g_textPrompt.edit);
                SendMessageW(g_textPrompt.edit, EM_SETSEL, 0, -1);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_textPrompt.dlg = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool promptTextDialog(const std::wstring& title, const std::wstring& label,
                      std::wstring& value) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = textPromptProc;
        wc.hInstance = g_app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"RshellTextPrompt";
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
        registered = true;
    }
    g_textPrompt = TextPromptState();
    g_textPromptTitle = title;
    g_textPromptLabel = label;
    g_textPromptValue = value;

    HWND dlg = dlg::createDialogWindow(L"RshellTextPrompt", title, dp(420), dp(200));
    if (!dlg) return false;
    g_textPrompt.dlg = dlg;
    runModalLoop(dlg);
    if (g_textPrompt.ok) value = g_textPromptValue;
    return g_textPrompt.ok;
}

void fsMkdirDialog() {
    if (!g_app.fsReady) {
        messageWarn(L"SFTP 未就绪", L"文件通道还没连上，请稍后再试。");
        return;
    }
    std::wstring name;
    if (!promptTextDialog(L"新建目录", L"在当前目录下创建的目录名", name)) return;
    const std::string dirName = trim(toUtf8(name));
    if (dirName.empty()) return;
    if (dirName.find('/') != std::string::npos) {
        messageWarn(L"名称无效", L"目录名不能包含 /。");
        return;
    }
    g_app.fs->mkdir(remoteJoin(g_app.fsPath, dirName));
    setStatus(L"正在创建 " + name + L" …");
}


// ---------------------------------------------------------------- 文件类型图标
//
// 远端文件在本地没有 shell 关联，SHGetFileInfo 取不到图标；而且 SFTP 列表里
// 绝大多数是普通文件，用系统图标反而分不出「这是个可执行文件还是个 txt」。
// 所以按扩展名自己画一套：统一的折角纸张底版 + 类型色 + 一个小符号，
// 全部走 GDI+（和界面卡片同一套抗锯齿），高 DPI 下也清晰。

enum class FileKind { Folder, Link, Code, Exec, Image, Video, Audio, Archive, Doc, Pdf, Data, Other };

std::wstring lowerExtOf(const std::wstring& name) {
    const std::size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return std::wstring();
    std::wstring ext = name.substr(dot + 1);
    for (wchar_t& ch : ext) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return ext;
}

bool extIs(const std::wstring& ext, std::initializer_list<const wchar_t*> candidates) {
    if (ext.empty()) return false;
    for (const wchar_t* candidate : candidates) {
        if (ext == candidate) return true;
    }
    return false;
}

FileKind fileKindOf(const RemoteEntry& entry) {
    if (entry.isDir()) return FileKind::Folder;
    if (entry.isLink()) return FileKind::Link;
    const std::wstring ext = lowerExtOf(toWide(entry.name));
    if (extIs(ext, {L"c", L"h", L"cc", L"cpp", L"cxx", L"hpp", L"hh", L"cs", L"go", L"rs", L"java",
                    L"kt", L"kts", L"py", L"rb", L"php", L"js", L"mjs", L"cjs", L"ts", L"tsx",
                    L"jsx", L"vue", L"svelte", L"swift", L"lua", L"pl", L"sh", L"bash", L"zsh",
                    L"fish", L"ps1", L"psm1", L"bat", L"cmd", L"sql", L"html", L"htm", L"css",
                    L"scss", L"sass", L"less", L"m", L"mm", L"json", L"jsonc", L"yml", L"yaml",
                    L"toml", L"ini", L"conf", L"cfg", L"xml", L"gradle", L"cmake"})) {
        return FileKind::Code;
    }
    if (extIs(ext, {L"exe", L"dll", L"so", L"dylib", L"o", L"obj", L"a", L"lib", L"class",
                    L"wasm", L"bin", L"elf", L"out", L"msi"})) {
        return FileKind::Exec;
    }
    if (extIs(ext, {L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"svg", L"ico", L"tif",
                    L"tiff", L"avif", L"heic", L"psd", L"xcf", L"raw", L"cr2", L"nef"})) {
        return FileKind::Image;
    }
    if (extIs(ext, {L"mp4", L"mkv", L"mov", L"avi", L"webm", L"flv", L"wmv", L"m4v", L"mpg",
                    L"mpeg", L"3gp", L"rmvb", L"vob"})) {
        return FileKind::Video;
    }
    if (extIs(ext, {L"mp3", L"wav", L"flac", L"ogg", L"oga", L"m4a", L"aac", L"opus", L"wma",
                    L"mid", L"midi", L"aiff", L"ape"})) {
        return FileKind::Audio;
    }
    if (extIs(ext, {L"zip", L"tar", L"gz", L"tgz", L"bz2", L"tbz2", L"xz", L"txz", L"7z", L"rar",
                    L"zst", L"lz4", L"cab", L"deb", L"rpm", L"apk", L"jar", L"war", L"iso"})) {
        return FileKind::Archive;
    }
    if (extIs(ext, {L"pdf"})) return FileKind::Pdf;
    if (extIs(ext, {L"csv", L"tsv", L"log", L"db", L"sqlite", L"sqlite3", L"dat", L"parquet",
                    L"dump", L"lock", L"sav", L"bak"})) {
        return FileKind::Data;
    }
    if (extIs(ext, {L"md", L"markdown", L"txt", L"rst", L"adoc", L"org", L"tex", L"doc", L"docx",
                    L"odt", L"rtf", L"pages", L"nfo", L"man"})) {
        return FileKind::Doc;
    }
    return FileKind::Other;
}

std::uint32_t fileKindColor(FileKind kind) {
    switch (kind) {
        case FileKind::Folder: return 0x58A6FF;
        case FileKind::Link: return 0xF778BA;  // 粉色：别和压缩包的金色撞
        case FileKind::Code: return 0x7EE787;
        case FileKind::Exec: return 0xFF9E64;
        case FileKind::Image: return 0xD2A8FF;
        case FileKind::Video: return 0xFF7B72;
        case FileKind::Audio: return 0x56D4DD;
        case FileKind::Archive: return 0xE3B341;
        case FileKind::Doc: return 0xC9D1D9;
        case FileKind::Pdf: return 0xF85149;
        case FileKind::Data: return 0xA5D6FF;
        case FileKind::Other: return ui::textDim;
    }
    return ui::textDim;
}

// 圆头折线：图标里的所有笔画都靠它（圆头比方头更像手绘，小尺寸也更实）
void strokePoints(HDC dc, const std::vector<Gdiplus::PointF>& points, float width,
                  std::uint32_t color) {
    if (points.size() < 2 || width <= 0.0f) return;
    if (!g_gdiplusReady) {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, static_cast<int>(width + 0.5f)),
                             toColorRef(color));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, static_cast<int>(points[0].X), static_cast<int>(points[0].Y), nullptr);
        for (std::size_t i = 1; i < points.size(); ++i) {
            LineTo(dc, static_cast<int>(points[i].X), static_cast<int>(points[i].Y));
        }
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        return;
    }
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(gpColor(color), width);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawLines(&pen, points.data(), static_cast<INT>(points.size()));
}

void fillPoints(HDC dc, const std::vector<Gdiplus::PointF>& points, std::uint32_t color) {
    if (points.size() < 3) return;
    if (!g_gdiplusReady) {
        HBRUSH brush = CreateSolidBrush(toColorRef(color));
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        std::vector<POINT> pts;
        pts.reserve(points.size());
        for (const auto& p : points) {
            pts.push_back(POINT{static_cast<LONG>(p.X), static_cast<LONG>(p.Y)});
        }
        Polygon(dc, pts.data(), static_cast<int>(pts.size()));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
        return;
    }
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(gpColor(color));
    g.FillPolygon(&brush, points.data(), static_cast<INT>(points.size()));
}

// 纸张轮廓：右上角真的折起来（「狗耳朵」），而不是把三角形贴在圆角上——
// 贴上去的做法在 16px 下会被圆角吃掉，看起来就只是一个圆角方块。
void addPagePath(Gdiplus::GraphicsPath& path, float l, float t, float r, float b, float fold,
                 float radius) {
    const float fx = r - fold;
    const float fy = t + fold;
    path.AddLine(l + radius, t, fx, t);   // 上边
    path.AddLine(fx, t, r, fy);           // 折角斜边
    path.AddLine(r, fy, r, b - radius);   // 右边
    path.AddArc(r - radius * 2.0f, b - radius * 2.0f, radius * 2.0f, radius * 2.0f, 0.0f, 90.0f);
    path.AddLine(r - radius, b, l + radius, b);
    path.AddArc(l, b - radius * 2.0f, radius * 2.0f, radius * 2.0f, 90.0f, 90.0f);
    path.AddLine(l, b - radius, l, t + radius);
    path.AddArc(l, t, radius * 2.0f, radius * 2.0f, 180.0f, 90.0f);
    path.CloseFigure();
}

void fillGraphicsPath(HDC dc, Gdiplus::GraphicsPath& path, std::uint32_t color) {
    if (!g_gdiplusReady) return;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(gpColor(color));
    g.FillPath(&brush, &path);
}

// box 取正方形边长作为 16x16 的基准网格：列表里 16px、拖动浮层里 24px，同一份代码。
void paintFileIcon(HDC dc, const RECT& box, FileKind kind) {
    const int side = std::min(box.right - box.left, box.bottom - box.top);
    if (side <= 0) return;
    if (!g_gdiplusReady) {
        fillRoundRect(dc, box, std::max(1, side / 5), fileKindColor(kind));
        return;
    }
    const float unit = static_cast<float>(side) / 16.0f;
    const float x0 =
        static_cast<float>(box.left) + static_cast<float>((box.right - box.left) - side) / 2.0f;
    const float y0 =
        static_cast<float>(box.top) + static_cast<float>((box.bottom - box.top) - side) / 2.0f;
    auto px = [&](float x) { return x0 + x * unit; };
    auto py = [&](float y) { return y0 + y * unit; };
    auto at = [&](float x, float y) { return Gdiplus::PointF(px(x), py(y)); };
    auto boxOf = [&](float l, float t, float r, float b) {
        return RECT{static_cast<int>(px(l) + 0.5f), static_cast<int>(py(t) + 0.5f),
                    static_cast<int>(px(r) + 0.5f), static_cast<int>(py(b) + 0.5f)};
    };
    auto corner = [&](float radius) {
        return std::max(1, static_cast<int>(radius * unit + 0.5f));
    };

    const std::uint32_t color = fileKindColor(kind);
    // 16px 下细节是奢侈品：纸张直接铺满类型色，符号用「挖」出来的深色，
    // 这样即使只有十几个像素，也能一眼看出颜色和轮廓。
    const std::uint32_t pageFill = ui::mix(color, ui::bgPanel, 0.05);
    const std::uint32_t foldFill = ui::mix(color, 0xFFFFFF, 0.45);
    const std::uint32_t ink = ui::mix(color, ui::bgApp, 0.78);
    const std::uint32_t tintFill = ui::mix(color, ui::bgPanel, 0.28);
    const float thin = 1.15f * unit;
    const float thick = 1.45f * unit;

    if (kind == FileKind::Folder) {
        // 文件夹：上面一小块「后背」+ 下面一本「书」
        fillRoundRect(dc, boxOf(2.4f, 3.4f, 8.2f, 6.8f), corner(0.9f), tintFill);
        fillRoundRect(dc, boxOf(2.0f, 5.2f, 14.0f, 13.2f), corner(1.5f), color);
        return;
    }

    // 纸张：比 16x16 略瘦一点（约 0.77 的宽高比），四角圆一点点就行
    const float pageL = 2.6f, pageT = 1.0f, pageR = 13.4f, pageB = 15.0f;
    const float foldSize = 4.4f;
    Gdiplus::GraphicsPath pagePath;
    addPagePath(pagePath, px(pageL), py(pageT), px(pageR), py(pageB), foldSize * unit, 1.1f * unit);
    fillGraphicsPath(dc, pagePath, pageFill);
    // 折角本身：稍亮一档，形成层次
    fillPoints(dc, {at(pageR - foldSize, pageT), at(pageR, pageT + foldSize),
                    at(pageR - foldSize, pageT + foldSize)},
               foldFill);

    switch (kind) {
        case FileKind::Code:
            // "< >"：两个尖括号各留一口气，别在中线粘成一个 X
            strokePoints(dc, {at(6.2f, 6.4f), at(4.6f, 9.0f), at(6.2f, 11.6f)}, thick, ink);
            strokePoints(dc, {at(9.4f, 6.4f), at(11.0f, 9.0f), at(9.4f, 11.6f)}, thick, ink);
            break;
        case FileKind::Exec:
            strokePoints(dc, {at(5.0f, 5.8f), at(7.6f, 9.0f), at(5.0f, 12.2f)}, thick, ink);
            strokePoints(dc, {at(8.6f, 12.2f), at(11.2f, 12.2f)}, thick, ink);
            break;
        case FileKind::Link:
            strokePoints(dc, {at(5.2f, 12.0f), at(10.2f, 6.6f)}, thick, ink);
            strokePoints(dc, {at(6.8f, 6.0f), at(10.6f, 6.0f), at(10.6f, 9.8f)}, thick, ink);
            break;
        case FileKind::Image:
            // 相框 + 挖出太阳和山：直接画太阳+山会被看成「人像」，加个框就稳了
            fillRoundRect(dc, boxOf(4.0f, 5.4f, 12.0f, 13.2f), corner(0.9f), ink);
            fillCircle(dc, static_cast<int>(px(6.9f) + 0.5f), static_cast<int>(py(7.9f) + 0.5f),
                       corner(1.05f), pageFill);
            fillPoints(dc, {at(4.6f, 12.6f), at(7.4f, 9.6f), at(9.6f, 12.6f)}, pageFill);
            fillPoints(dc, {at(9.0f, 12.6f), at(10.6f, 10.4f), at(11.8f, 12.6f)}, pageFill);
            break;
        case FileKind::Video:
            fillPoints(dc, {at(5.8f, 5.8f), at(5.8f, 12.0f), at(11.2f, 8.9f)}, ink);
            break;
        case FileKind::Audio:
            fillCircle(dc, static_cast<int>(px(6.6f) + 0.5f), static_cast<int>(py(10.8f) + 0.5f),
                       corner(1.7f), ink);
            strokePoints(dc, {at(8.3f, 10.8f), at(8.3f, 5.8f), at(11.2f, 6.8f)}, thin, ink);
            break;
        case FileKind::Archive:
            // 包裹：箱体 + 一道「胶带」把页面色挖出来
            fillRoundRect(dc, boxOf(4.4f, 5.2f, 11.6f, 13.0f), corner(0.8f), ink);
            strokePoints(dc, {at(4.6f, 7.9f), at(11.4f, 7.9f)}, 1.3f * unit, pageFill);
            strokePoints(dc, {at(8.0f, 5.4f), at(8.0f, 12.8f)}, 1.3f * unit, pageFill);
            break;
        case FileKind::Data:
            // 三根柱子：一眼就是「数据/统计」
            fillRoundRect(dc, boxOf(4.9f, 9.6f, 6.3f, 12.8f), corner(0.5f), ink);
            fillRoundRect(dc, boxOf(7.2f, 7.4f, 8.6f, 12.8f), corner(0.5f), ink);
            fillRoundRect(dc, boxOf(9.5f, 5.2f, 10.9f, 12.8f), corner(0.5f), ink);
            break;
        case FileKind::Pdf:
            strokePoints(dc, {at(4.8f, 5.8f), at(8.6f, 5.8f)}, thin, ink);
            strokePoints(dc, {at(4.8f, 8.2f), at(11.2f, 8.2f)}, thin, ink);
            fillRoundRect(dc, boxOf(4.8f, 10.2f, 11.2f, 12.9f), corner(0.7f), ink);
            break;
        case FileKind::Doc:
            strokePoints(dc, {at(4.8f, 6.2f), at(11.2f, 6.2f)}, thin, ink);
            strokePoints(dc, {at(4.8f, 8.7f), at(11.2f, 8.7f)}, thin, ink);
            strokePoints(dc, {at(4.8f, 11.2f), at(11.2f, 11.2f)}, thin, ink);
            strokePoints(dc, {at(4.8f, 13.6f), at(8.4f, 13.6f)}, thin, ink);
            break;
        case FileKind::Other:
        default:
            // 不认识的类型：三点省略号，配合「其它」的灰
            fillCircle(dc, static_cast<int>(px(5.7f) + 0.5f), static_cast<int>(py(9.4f) + 0.5f),
                       corner(1.0f), ink);
            fillCircle(dc, static_cast<int>(px(8.0f) + 0.5f), static_cast<int>(py(9.4f) + 0.5f),
                       corner(1.0f), ink);
            fillCircle(dc, static_cast<int>(px(10.3f) + 0.5f), static_cast<int>(py(9.4f) + 0.5f),
                       corner(1.0f), ink);
            break;
    }
}

// 图标总览（开发用）：rshell-gui.exe --icons <out.bmp>
// 把所有类型按 16/24/32/48 各画一遍，一眼就能看出哪一档比例/线条不对。
bool renderIconSheet(const std::wstring& outPath) {
    if (g_app.dpi <= 0) g_app.dpi = wincompat::systemDpi();
    createFonts();

    struct Entry {
        FileKind kind;
        const wchar_t* label;
    };
    const Entry entries[] = {{FileKind::Folder, L"folder"}, {FileKind::Code, L"code"},
                             {FileKind::Exec, L"exec"},     {FileKind::Link, L"link"},
                             {FileKind::Image, L"image"},   {FileKind::Video, L"video"},
                             {FileKind::Audio, L"audio"},   {FileKind::Archive, L"zip"},
                             {FileKind::Doc, L"doc"},       {FileKind::Pdf, L"pdf"},
                             {FileKind::Data, L"data"},     {FileKind::Other, L"other"}};
    const int sizes[] = {16, 24, 32, 48};
    const int columns = static_cast<int>(std::size(entries));
    const int rows = static_cast<int>(std::size(sizes));
    const int margin = dp(46);
    const int cellW = dp(78);
    const int headerH = dp(22);
    const int width = margin + columns * cellW + dp(8);
    int height = headerH;
    for (int r = 0; r < rows; ++r) height += sizes[r] + dp(26);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        return false;
    }
    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ oldBmp = SelectObject(mem, dib);
    fillRect(mem, RECT{0, 0, width, height}, ui::bgPanel);

    for (int c = 0; c < columns; ++c) {
        drawText(mem, entries[c].label,
                 RECT{margin + c * cellW, 0, margin + (c + 1) * cellW, headerH}, ui::textFaint,
                 g_app.tinyFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    int y = headerH;
    for (int r = 0; r < rows; ++r) {
        const int size = sizes[r];
        const int rowH = size + dp(26);
        drawText(mem, std::to_wstring(size) + L"px", RECT{0, y, margin - dp(6), y + rowH},
                 ui::textFaint, g_app.tinyFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        for (int c = 0; c < columns; ++c) {
            const int left = margin + c * cellW + (cellW - size) / 2;
            const int top = y + (rowH - size) / 2;
            paintFileIcon(mem, RECT{left, top, left + size, top + size}, entries[c].kind);
        }
        y += rowH;
    }

    // 存 32bpp BMP：DIB 是自上而下，BMP 文件要求自下而上，逐行倒着写
    bool ok = false;
    std::ofstream out(outPath, std::ios::binary);
    if (out) {
        const int rowBytes = width * 4;
        BITMAPFILEHEADER fileHeader{};
        fileHeader.bfType = 0x4D42;  // "BM"
        fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(rowBytes) * height;
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = width;
        header.biHeight = height;
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;
        header.biSizeImage = static_cast<DWORD>(rowBytes) * height;
        out.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        const char* pixels = static_cast<const char*>(bits);
        for (int row = height - 1; row >= 0; --row) {
            out.write(pixels + static_cast<std::size_t>(row) * rowBytes, rowBytes);
        }
        ok = out.good();
    }

    SelectObject(mem, oldBmp);
    DeleteObject(dib);
    DeleteDC(mem);
    return ok;
}

// ---------------------------------------------------------------- 拖入 / 拖出
//
// 拖入：文件面板接受 WM_DROPFILES，直接上传到当前目录。
// 拖出：交给 dnd::VirtualFileData（虚拟文件：松手后资源管理器才来要数据），
//       这里只负责把事件转成 UI 线程的 FsEvent，以及准备每次拖动的取消开关。

namespace {

// 把拖放层的事件投递到 UI 线程（可能在资源管理器的复制线程上调用）
void postDndEvent(HWND mainWindow, UINT_PTR connectionId, const dnd::Event& event) {
    if (!mainWindow) return;
    auto* message = new FsEvent();
    switch (event.kind) {
        case dnd::EventKind::Started:
            message->kind = FsEventKind::Started;
            message->message = toUtf8(event.name);  // Started 用文件名
            break;
        case dnd::EventKind::Progress:
            message->kind = FsEventKind::Progress;
            break;
        case dnd::EventKind::Done:
            message->kind = FsEventKind::Done;
            message->message = toUtf8(event.name);
            break;
        case dnd::EventKind::Cancelled:
            message->kind = FsEventKind::Cancelled;
            break;
        case dnd::EventKind::Failed:
            message->kind = FsEventKind::Error;
            message->message = event.message;
            break;
    }
    message->path = event.key;
    message->bytes = event.done;
    message->total = event.total;
    message->cancel = event.cancel;
    if (!PostMessageW(mainWindow, WM_FS_EVENT, connectionId, reinterpret_cast<LPARAM>(message))) {
        delete message;
    }
}

// 拖动源：Esc 取消、松手放下；光标用系统默认样式（拖影由目标自己画）
class DropSource : public IDropSource {
public:
    HRESULT QueryInterface(REFIID id, void** out) override {
        if (!out) return E_INVALIDARG;
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return static_cast<ULONG>(++ref_); }
    ULONG Release() override {
        const LONG left = --ref_;
        if (left == 0) delete this;
        return static_cast<ULONG>(left);
    }
    HRESULT QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ~DropSource() = default;
    LONG ref_ = 1;
};

}  // namespace

// 拖出：只声明「要拖哪个远端文件」，下载发生在松手之后（由资源管理器驱动）。
void beginDragOut(HWND hwnd) {
    const RemoteEntry* selected = selectedFileEntry();
    if (!selected || selected->isDir() || !g_app.fsReady) return;
    // 拷贝一份：DoDragDrop 期间可能收到新的目录列表把 fsEntries 换掉
    const RemoteEntry entry = *selected;

    std::vector<dnd::VirtualFile> files;
    dnd::VirtualFile file;
    file.key = entry.path;
    file.name = toWide(entry.name);
    file.size = entry.size;
    file.cancel = std::make_shared<std::atomic<bool>>(false);
    files.push_back(std::move(file));

    const auto fs = g_app.fs;
    dnd::ReaderFactory factory = [fs](const std::string& key, std::string* error) {
        dnd::Reader reader;
        auto source = fs->openReader(key, error);
        if (!source) return reader;
        reader.size = [source, fs]() { return source->size(); };
        reader.read = [source, fs](char* buffer, std::size_t capacity, std::string* readError) {
            return source->read(buffer, capacity, readError);
        };
        reader.close = [source, fs]() { source->close(); };
        return reader;
    };

    const HWND mainWindow = g_app.main;
    const UINT_PTR connectionId = g_app.connectionId;
    auto* data = new dnd::VirtualFileData(std::move(files), std::move(factory),
        [mainWindow, connectionId](const dnd::Event& event) { postDndEvent(mainWindow, connectionId, event); });
    // 「下载不卡 UI」的关键：先声明允许目标后台取数，放置目标才会自己起线程读流
    // 并立刻从 Drop 返回；DoDragDrop 一返回，UI 线程马上回到消息循环，后面的
    // 下载全在后台线程（我们的预取线程 + 目标的复制线程）上跑。
    data->SetAsyncMode(TRUE);
    auto* source = new DropSource();
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
    // 必须在 Release 之前问：TRUE 表示目标已经把取数挪到后台线程，
    // 此刻传输还没结束，进度以 FsEvent 为准。
    BOOL inProgress = FALSE;
    data->InOperation(&inProgress);
    source->Release();
    data->Release();

    if (FAILED(hr) || effect == DROPEFFECT_NONE) {
        setStatus(L"没有放到目标位置，" + toWide(entry.name) + L" 未下载");
        return;
    }
    // 真正落到哪个目录由资源管理器决定；进度从松手那一刻开始在状态栏里走
    if (inProgress == TRUE) {
        setStatus(L"正在后台下载 " + toWide(entry.name) + L"，窗口可以继续操作…");
    } else {
        setStatus(L"正在把 " + toWide(entry.name) + L" 下载到拖放位置…");
    }
    (void)hwnd;
}


// 文件面板右键菜单：上传 / 新建文件夹 / 下载 / 删除 / 复制路径 / 刷新 / 断开
void openFileContextMenu(HWND hwnd, POINT screenPt) {
    const RemoteEntry* entry = selectedFileEntry();
    const bool ready = g_app.fsReady;
    const bool connected = g_app.shell->running() || g_app.connecting;

    std::vector<popup::Item> items;
    auto add = [&items](int id, const wchar_t* text, const wchar_t* shortcut = L"",
                        bool enabled = true, bool danger = false) {
        popup::Item item;
        item.id = id;
        item.text = text;
        item.shortcut = shortcut;
        item.enabled = enabled;
        item.danger = danger;
        items.push_back(std::move(item));
    };
    auto sep = [&items]() {
        popup::Item item;
        item.separator = true;
        items.push_back(std::move(item));
    };

    add(IDM_FS_UPLOAD, L"上传文件…", L"", ready);
    add(IDM_FS_MKDIR, L"新建文件夹…", L"", ready);
    sep();
    add(IDM_FS_DOWNLOAD, L"下载到本地…", L"", ready && entry != nullptr && entry->isFile());
    add(IDM_FS_DELETE, L"删除", L"Delete", ready && entry != nullptr, true);
    add(IDM_FS_COPY_PATH, L"复制路径", L"", entry != nullptr);
    sep();
    add(IDM_FS_GOTO, L"转到目录…", L"Ctrl+L", ready);
    add(IDM_FS_REFRESH, L"刷新", L"F5", ready);
    sep();
    add(IDM_DISCONNECT, L"断开连接", L"", connected, true);

    (void)hwnd;
    const int id = popup::run(g_app.main, screenPt, items);
    if (id > 0) SendMessageW(g_app.main, WM_COMMAND, MAKEWPARAM(id, 0), 0);
}

void paintFileList(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    fillRect(mem, rc, ui::bgPanel);

    if (!g_app.fsReady) {
        const int top = std::max(dp(10), std::min(dp(72), static_cast<int>(rc.bottom) / 5));
        const int center = rc.right / 2;
        paintFileIcon(mem, RECT{center - dp(16), top, center + dp(16), top + dp(32)},
                      FileKind::Folder);
        const std::wstring text = g_app.fsFailed ? L"文件通道连接失败"
                                   : g_app.connecting ? L"正在连接…"
                                                      : L"浏览远端文件";
        drawText(mem, text, RECT{dp(12), top + dp(46), rc.right - dp(12), top + dp(70)},
                 g_app.fsFailed ? ui::danger : ui::textDim, g_app.uiFontBold,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (!g_app.fsError.empty()) {
            drawText(mem, toWide(g_app.fsError), RECT{dp(12), top + dp(78), rc.right - dp(12), rc.bottom},
                     ui::textFaint, g_app.tinyFont, DT_LEFT | DT_TOP | DT_WORDBREAK);
        } else if (!g_app.connecting) {
            drawText(mem, L"连接主机后，在这里浏览文件\n支持上传、下载与拖放传输",
                     RECT{dp(12), top + dp(78), rc.right - dp(12), rc.bottom}, ui::textFaint,
                     g_app.tinyFont, DT_CENTER | DT_TOP | DT_WORDBREAK);
        }
    } else if (!g_app.fsError.empty()) {
        drawText(mem, L"无法读取目录", RECT{dp(12), dp(10), rc.right - dp(12), dp(30)}, ui::danger,
                 g_app.uiFont, DT_LEFT | DT_TOP | DT_SINGLELINE);
        drawText(mem, toWide(g_app.fsError), RECT{dp(12), dp(32), rc.right - dp(12), dp(96)},
                 ui::textDim, g_app.tinyFont, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else if (g_app.fsEntries.empty()) {
        drawText(mem, L"空目录", RECT{dp(12), dp(10), rc.right - dp(12), dp(30)}, ui::textFaint,
                 g_app.tinyFont, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }

    const int count = static_cast<int>(g_app.fsEntries.size());
    for (int i = 0; i < count; ++i) {
        const int top = filePad() + i * fileItemH() - g_app.fsScroll;
        if (top + fileItemH() < 0 || top > rc.bottom) continue;
        RECT item{dp(2), top, rc.right - dp(2), top + fileItemH()};
        const bool selected = i == g_app.fsSel;
        const bool hovered = i == g_app.fsHover;
        if (selected || hovered) {
            fillRoundRect(mem, item, dp(5), selected ? ui::bgActive : ui::bgHover);
            if (selected)
                fillRoundRect(mem, RECT{item.left, top + dp(8), item.left + dp(3),
                                       top + fileItemH() - dp(8)}, dp(2), ui::accent);
        }

        const RemoteEntry& entry = g_app.fsEntries[static_cast<std::size_t>(i)];
        // 类型图标：文件夹 / 代码 / 可执行 / 图片 / 音视频 / 压缩包 / 文档…
        // 同一套画法在拖动浮层里还会用一次（尺寸更大），见 paintFileIcon。
        const int iconSide = dp(16);
        RECT iconBox{dp(7), top + (fileItemH() - iconSide) / 2, dp(7) + iconSide,
                     top + (fileItemH() + iconSide) / 2};
        paintFileIcon(mem, iconBox, fileKindOf(entry));

        const std::wstring name = toWide(entry.name);
        const std::wstring sizeText =
            entry.isDir() ? std::wstring() : toWide(formatBytes(entry.size));
        const int sizeW = sizeText.empty() ? 0 : textWidth(mem, sizeText, g_app.tinyFont) + dp(8);
        RECT nameRc{iconBox.right + dp(8), top, rc.right - dp(8) - sizeW, top + fileItemH()};
        drawText(mem, ellipsize(mem, name, g_app.uiFont, nameRc.right - nameRc.left), nameRc,
                 entry.isDir() ? ui::text : ui::textDim, g_app.uiFont,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!sizeText.empty()) {
            drawText(mem, sizeText,
                     RECT{rc.right - dp(8) - sizeW, top, rc.right - dp(8), top + fileItemH()},
                     ui::textFaint, g_app.tinyFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    const int maxScroll = fileMaxScroll();
    if (maxScroll > 0) {
        const int trackH = std::max<int>(dp(20), rc.bottom - filePad() * 2);
        const int thumbH = std::max(dp(24), trackH * trackH / (trackH + maxScroll));
        const int thumbY = filePad() + (trackH - thumbH) * g_app.fsScroll / maxScroll;
        fillRoundRect(mem, RECT{rc.right - dp(5), thumbY, rc.right - dp(2), thumbY + thumbH},
                      dp(2), ui::mix(ui::textFaint, ui::bgApp, 0.35));
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---- 文件通道（独立 SSH 连接）的生命周期 ----

ConnectionOptions makeRemoteOptions(const Session& session, const std::wstring& password,
                                    const std::wstring& keyPassphrase) {
    ConnectionOptions options;
    options.host = session.host;
    options.port = session.port > 0 ? session.port : 22;
    options.user = session.user;
    options.keyPath = session.keyPath;
    std::string utf8Password = toUtf8(password);
    options.password = utf8Password;
    wipeString(utf8Password);
    std::string utf8KeyPassphrase = toUtf8(keyPassphrase);
    options.keyPassphrase = utf8KeyPassphrase;
    wipeString(utf8KeyPassphrase);
    options.timeoutSec = 15;
    options.allowDefaultKeys = true;

    if (!session.proxyUrl.empty()) {
        std::string proxyError;
        if (!applySessionProxy(options, session, proxyError))
            guiLog("文件通道的代理配置无效：" + proxyError);
    }
    if (options.user.empty()) {
        if (const char* env = std::getenv("USERNAME")) options.user = env;
    }
    const UINT_PTR connectionId = g_app.connectionId;
    options.confirmHostKey = [connectionId](const std::string& hostField, const std::string& fingerprint,
                                const std::string& keyType) {
        return askHostKey(hostField, keyType, fingerprint, connectionId);
    };
    return options;
}

void startRemoteFs(const Session& session, const std::wstring& password,
                   const std::wstring& keyPassphrase) {
    g_app.fsReady = false;
    g_app.fsFailed = false;
    g_app.fsError.clear();
    g_app.fsEntries.clear();
    g_app.fsSel = -1;
    g_app.fsHover = -1;
    g_app.fsScroll = 0;
    g_app.fsPath.clear();
    g_app.fsPendingUploads = 0;
    g_app.stats = RemoteStats();

    const UINT_PTR connectionId = g_app.connectionId;
    g_app.fs->setCallbacks(RemoteFsCallbacks{
        [connectionId](const FsEvent& event) {
            auto* copy = new FsEvent(event);
            if (!PostMessageW(g_app.main, WM_FS_EVENT, connectionId, reinterpret_cast<LPARAM>(copy)))
                delete copy;
        },
        [connectionId](const RemoteStats& stats) {
            auto* copy = new RemoteStats(stats);
            if (!PostMessageW(g_app.main, WM_FS_STATS, connectionId, reinterpret_cast<LPARAM>(copy)))
                delete copy;
        }});
    g_app.fs->setSampleInterval(5);
    g_app.fs->start(makeRemoteOptions(session, password, keyPassphrase));

    layoutMain(g_app.main);
    updateButtons();
}

// ---------------------------------------------------------------- 下载任务
//
// 所有下载（右键「下载到本地」和拖到资源管理器）都登记在这里：
//   状态栏右侧的胶囊显示总进度，点开是逐项列表，每一项都能单独取消。
// 下载任务只保存「远端路径 -> 进度」，真正的字节搬运在文件线程 / 资源管理器里。

constexpr int kMaxVisibleDownloads = 5;  // 列表最多显示几项，其余折成一行说明

bool downloadActive(const App::DownloadTask& task) {
    return task.state == App::DownloadState::Active;
}

std::size_t activeDownloadCount() {
    std::size_t count = 0;
    for (const auto& task : g_app.downloads) {
        if (downloadActive(task)) ++count;
    }
    return count;
}

bool hasFinishedDownloads() {
    return activeDownloadCount() < g_app.downloads.size();
}

App::DownloadTask* findDownloadTask(const std::string& remote, bool upload, bool activeOnly = true) {
    // 从后往前找：同名文件重复传输时，进度事件应该落在最新那一项上；
    // 上传和下载可能同时指向同一个远端路径，方向也必须对得上。
    for (auto it = g_app.downloads.rbegin(); it != g_app.downloads.rend(); ++it) {
        if (it->remote != remote || it->upload != upload) continue;
        if (activeOnly && !downloadActive(*it)) continue;
        return &*it;
    }
    return nullptr;
}

int downloadVisibleRows() {
    return std::min<int>(static_cast<int>(g_app.downloads.size()), kMaxVisibleDownloads);
}

// 展开状态下状态栏上方的面板高度（0 表示不需要面板）
int downloadsPanelHeight() {
    if (!g_app.downloadsOpen) return 0;
    if (g_app.downloads.empty()) return dp(76);
    const int headerH = dp(30);
    const int rowH = dp(34);
    const int moreH = static_cast<int>(g_app.downloads.size()) > kMaxVisibleDownloads
                          ? dp(20)
                          : 0;
    return headerH + downloadVisibleRows() * rowH + moreH + dp(8);
}

// 展开/收起、任务增减之后调用：重排窗口并重画
void syncDownloadLayout() {
    if (!g_app.main) return;
    layoutMain(g_app.main);
    InvalidateRect(g_app.main, nullptr, FALSE);
}

void addDownloadTask(const std::string& remote, const std::wstring& name, std::uint64_t total,
                     bool drag, std::shared_ptr<std::atomic<bool>> cancel, bool upload) {
    App::DownloadTask task;
    task.id = ++g_app.downloadSeq;
    task.remote = remote;
    task.name = name;
    task.total = total;
    task.drag = drag;
    task.upload = upload;
    task.cancel = std::move(cancel);
    g_app.downloads.push_back(std::move(task));
    // 有新任务就把列表展开，让用户马上看到进度和取消按钮
    g_app.downloadsOpen = true;
    syncDownloadLayout();
}

void cancelDownloadTask(int id) {
    for (auto& task : g_app.downloads) {
        if (task.id != id || !downloadActive(task)) continue;
        if (task.cancel) task.cancel->store(true);
        setStatus((task.upload ? L"正在取消上传 " : L"正在取消下载 ") + task.name + L" …");
        InvalidateRect(g_app.main, nullptr, FALSE);
        return;
    }
}

void clearFinishedDownloads() {
    g_app.downloads.erase(
        std::remove_if(g_app.downloads.begin(), g_app.downloads.end(),
                       [](const App::DownloadTask& task) { return !downloadActive(task); }),
        g_app.downloads.end());
    if (g_app.downloads.empty()) g_app.downloadsOpen = false;
    syncDownloadLayout();
}

void stopRemoteFs() {
    for (auto& task : g_app.downloads)
        if (downloadActive(task) && task.cancel) task.cancel->store(true);
    g_app.fs->stop();
    // 连接没了，还在跑的下载不可能继续：全部标成失败，避免列表里挂着永远不会动的项
    for (auto& task : g_app.downloads) {
        if (!downloadActive(task)) continue;
        task.state = App::DownloadState::Failed;
    }
    g_app.fsReady = false;
    g_app.fsFailed = false;
    g_app.fsEntries.clear();
    g_app.fsError.clear();
    g_app.fsPath.clear();
    g_app.fsSel = -1;
    g_app.fsHover = -1;
    g_app.fsScroll = 0;
    g_app.fsPendingUploads = 0;
    g_app.stats = RemoteStats();
    if (g_app.list) InvalidateRect(g_app.list, nullptr, FALSE);
    if (g_app.main) {
        layoutMain(g_app.main);
        updateButtons();
    }
}

// UI 线程处理来自文件工作线程的事件
void handleFsEvent(const FsEvent& event) {
    // ---- 传输任务（上传 + 下载）----
    if (event.kind == FsEventKind::Started) {
        // 拖动下载的任务在松手后才由资源管理器的复制线程登记过来；
        // 取消开关跟着事件走，同路径的旧任务不会串号。
        // 上传没有「拖放落点」的概念，drag 一律为 false。
        addDownloadTask(event.path, toWide(event.message), event.total, !event.upload,
                        event.cancel, event.upload);
        return;
    }
    if (event.kind == FsEventKind::Progress) {
        if (App::DownloadTask* task = findDownloadTask(event.path, event.upload)) {
            task->done = event.bytes;
            if (event.total > 0) task->total = event.total;
            if (g_app.main) InvalidateRect(g_app.main, nullptr, FALSE);
        }
        return;
    }
    if (event.kind == FsEventKind::Cancelled) {
        if (App::DownloadTask* task = findDownloadTask(event.path, event.upload)) {
            task->state = App::DownloadState::Cancelled;
            setStatus((task->upload ? L"已取消上传 " : L"已取消下载 ") + task->name);
            syncDownloadLayout();
        }
        return;
    }
    switch (event.kind) {
        case FsEventKind::Started:
        case FsEventKind::Progress:  // 上面已处理
        case FsEventKind::Cancelled:
            break;
        case FsEventKind::Ready:
            g_app.fsReady = true;
            g_app.fsFailed = false;
            g_app.fsHome = event.path;  // 路径框里的 ~ 展开用
            if (!event.message.empty()) {
                g_app.fsError = event.message;
                if (foregroundSession()) messageWarn(L"文件面板不可用",
                            L"终端已经连上，但 SFTP 子系统打开失败：\n" + toWide(event.message));
            } else if (!event.path.empty()) {
                // shell 通道可能比 SFTP 先连上（并且已经上报过 OSC 7），
                // 这种情况下起点直接对齐终端所在的目录
                const std::string& shellCwd = g_app.termData.cwd();
                fsNavigate(shellCwd.empty() ? event.path : shellCwd);
            }
            break;
        case FsEventKind::Failed:
            g_app.fsReady = false;
            g_app.fsFailed = true;
            g_app.fsError = event.message;
            setStatus(L"文件通道连接失败", true);
            break;
        case FsEventKind::Listing: {
            const std::string previous =
                (g_app.fsSel >= 0 && g_app.fsSel < static_cast<int>(g_app.fsEntries.size()))
                    ? g_app.fsEntries[static_cast<std::size_t>(g_app.fsSel)].name
                    : std::string();
            g_app.fsPath = event.path;
            g_app.fsEntries = event.entries;
            g_app.fsError.clear();
            g_app.fsQuietPath.clear();
            g_app.fsSel = -1;
            for (std::size_t i = 0; i < g_app.fsEntries.size(); ++i) {
                if (!previous.empty() && g_app.fsEntries[i].name == previous) {
                    g_app.fsSel = static_cast<int>(i);
                    break;
                }
            }
            g_app.fsScroll = std::clamp(g_app.fsScroll, 0, fileMaxScroll());
            break;
        }
        case FsEventKind::Done: {
            if (g_app.fsPendingUploads > 0) --g_app.fsPendingUploads;
            App::DownloadTask* task = findDownloadTask(event.path, event.upload);
            if (task) {
                task->state = App::DownloadState::Done;
                if (task->total > 0) task->done = task->total;
                // 拖动下载不知道最终落盘路径（由资源管理器决定），文案要区分开
                setStatus(task->drag ? task->name + L" 已保存到拖放位置"
                                     : toWide(event.message));
                syncDownloadLayout();
            } else {
                setStatus(toWide(event.message));
            }
            // 批量上传时只在全部完成后刷新一次，避免队列里堆一串 listDir
            if ((!task || event.upload) && g_app.fsPendingUploads == 0) fsRefresh();
            break;
        }
        case FsEventKind::Error:
            if (g_app.fsPendingUploads > 0) --g_app.fsPendingUploads;
            if (App::DownloadTask* task = findDownloadTask(event.path, event.upload)) {
                task->state = App::DownloadState::Failed;
                setStatus(toWide(event.message), true);
                syncDownloadLayout();
                break;
            }
            setStatus(toWide(event.message), true);
            // 终端跟随 / 路径框跳转失败（没有权限、目录已删除）只在面板里提示，
            // 不弹模态框：用户可能只是 cd 进了一个进不去的目录，提示够用
            if (!g_app.fsQuietPath.empty() && event.path == g_app.fsQuietPath) {
                g_app.fsQuietPath.clear();
                g_app.fsError = event.message;
                if (g_app.list) InvalidateRect(g_app.list, nullptr, FALSE);
                break;
            }
            if (foregroundSession()) messageError(L"远端操作失败", L"操作没有完成。", toWide(event.message));
            break;
    }
    if (g_app.list) InvalidateRect(g_app.list, nullptr, FALSE);
    if (g_app.main) InvalidateRect(g_app.main, nullptr, FALSE);
    updateButtons();
}


void layoutMain(HWND hwnd) {
    if (!foregroundSession()) return;
    hideStatusHint();
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int pad = dp(14);
    const int titleH = dp(48);
    const int statusH = dp(34);
    const int btnH = dp(36);
    const int sidebarW = std::clamp<int>(rc.right / 4, dp(230), dp(340));

    g_app.titleH = titleH;
    g_app.statusH = statusH;

    // 展开的下载列表贴在状态栏上方：内容区要给它让位，否则会被终端/文件列表挡住
    const int panelH = downloadsPanelHeight();
    g_app.panelTop = rc.bottom - statusH - panelH;

    g_app.tabsRect = RECT{pad, titleH, rc.right - pad, titleH + dp(36)};
    ensureActiveTabVisible(rc.right);
    const int contentTop = titleH + dp(36) + pad;
    const int contentBottom = rc.bottom - statusH - panelH - pad;
    if (contentBottom - contentTop < dp(110)) return;

    // AI 面板：停靠在内容区右侧，或全屏占据内容区
    const int aiWidth = g_app.aiOpen && !g_app.aiFull
                            ? std::clamp<int>(rc.right / 3, dp(360), dp(560))
                            : 0;
    RECT aiRect{};
    if (g_app.aiOpen) {
        if (g_app.aiFull) {
            aiRect = RECT{pad, contentTop, rc.right - pad, rc.bottom};
        } else {
            aiRect = RECT{rc.right - pad - aiWidth, contentTop, rc.right - pad, contentBottom};
        }
    }
    // 只显示当前会话的 AI 窗口，其余隐藏（每个会话各有一份对话）
    for (auto& other : g_tabs) {
        if (other && other->aiTab)
            other->aiTab->show(false);
    }
    if (g_app.aiTab) {
        g_app.aiTab->setBounds(aiRect);
        g_app.aiTab->show(g_app.aiOpen);
    }

    if (g_app.aiFull) {
        // 全屏模式：会话控件全部让位
        if (g_app.list) ShowWindow(g_app.list, SW_HIDE);
        if (g_app.term) ShowWindow(g_app.term, SW_HIDE);
        if (g_app.fsPathEdit) ShowWindow(g_app.fsPathEdit, SW_HIDE);
        if (g_app.searchEdit) ShowWindow(g_app.searchEdit, SW_HIDE);
        if (g_app.status) ShowWindow(g_app.status, SW_HIDE);
        return;
    }

    if (g_app.list) ShowWindow(g_app.list, SW_SHOW);
    if (g_app.term) ShowWindow(g_app.term, SW_SHOW);
    if (g_app.status) ShowWindow(g_app.status, SW_SHOW);
    if (g_app.fsPathEdit) ShowWindow(g_app.fsPathEdit, SW_SHOW);

    // 侧栏：分组标题 + 文件列表 + 底部一个动作按钮（打开会话 / 断开）
    const int headerH = dp(76);
    const int listTop = contentTop + headerH;
    const int buttonY = contentBottom - btnH;
    const int listH = std::max(dp(48), buttonY - dp(8) - listTop);

    g_app.listTop = listTop;
    g_app.sidebarW = sidebarW;
    g_app.contentTop = contentTop;
    g_app.contentBottom = contentBottom;
    // 路径条：标题行左侧（右边留给「上级目录」小按钮），点一下能输入路径
    g_app.fsPathRect = RECT{pad, contentTop + dp(36), pad + sidebarW - dp(36),
                            contentTop + dp(66)};
    if (g_app.fsPathEdit) {
        const RECT& box = g_app.fsPathRect;
        const int editH = std::min<int>(box.bottom - box.top - dp(4), g_app.uiTextH + dp(2));
        MoveWindow(g_app.fsPathEdit, box.left + dp(7), box.top + (box.bottom - box.top - editH) / 2,
                   box.right - box.left - dp(14), editH, TRUE);
        SendMessageW(g_app.fsPathEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.tinyFont), TRUE);
    }

    if (g_app.list) MoveWindow(g_app.list, pad, listTop, sidebarW, listH, TRUE);

    g_app.buttons.clear();
    // 主按钮：未连接时打开会话列表，连上后是断开（id 不变，文字按连接状态在绘制时决定）
    g_app.buttons.push_back(App::UiButton{
        ID_BTN_ACTION, RECT{pad, buttonY, pad + sidebarW, buttonY + btnH}, true, false});
    // 分组标题右侧的「上级目录」小按钮
    const int upSize = dp(30);
    g_app.buttons.push_back(
        App::UiButton{ID_BTN_FS_UP,
                      RECT{pad + sidebarW - upSize, contentTop + dp(36), pad + sidebarW,
                           contentTop + dp(66)},
                      false, true});

    if (g_app.term) {
        const int termX = pad * 2 + sidebarW;
        // 停靠模式下右侧让给 AI 面板
        const int termRight = (g_app.aiOpen && !g_app.aiFull) ? aiRect.left - dp(10)
                                                              : rc.right - pad;
        const int termW = std::max(dp(120), termRight - termX);
        g_app.searchBox = RECT{termX + dp(4), contentTop, termRight - dp(170), contentTop + dp(28)};
        g_app.searchPrev = RECT{termRight - dp(88), contentTop, termRight - dp(60), contentTop + dp(28)};
        g_app.searchNext = RECT{termRight - dp(58), contentTop, termRight - dp(30), contentTop + dp(28)};
        g_app.searchClose = RECT{termRight - dp(28), contentTop, termRight, contentTop + dp(28)};
        if (g_app.searchEdit) {
            const RECT& box = g_app.searchBox;
            const int editH = std::min<int>(box.bottom - box.top - dp(4), g_app.uiTextH);
            MoveWindow(g_app.searchEdit, box.left + dp(7), box.top + (box.bottom - box.top - editH) / 2,
                       std::max(dp(20), static_cast<int>(box.right - box.left) - dp(14)), editH, TRUE);
            SendMessageW(g_app.searchEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);
        }
        if (!g_app.searchOpen && !g_app.shell->running() && !g_app.connecting && selectedSession())
            g_app.buttons.push_back(App::UiButton{IDM_RECONNECT,
                RECT{termRight - dp(108), contentTop, termRight, contentTop + dp(28)}, false, true});
        // 终端上方的旧标题栏已删（标签栏已有会话名）；
        // 只有搜索框或重连按钮需要时才留出这条辅助条
        const bool needTermHeader =
            g_app.searchOpen ||
            (!g_app.shell->running() && !g_app.connecting && selectedSession());
        g_app.termHeaderH = needTermHeader ? dp(38) : 0;
        const int termTop = contentTop + g_app.termHeaderH;
        MoveWindow(g_app.term, termX, termTop, termW,
                   std::max(dp(80), contentBottom - termTop), TRUE);
    }
    // 按钮/标题栏都是自绘的，布局变了要重画
    InvalidateRect(hwnd, nullptr, TRUE);
}

// ---------------------------------------------------------------- 主窗口自绘

// 左侧文件面板：SFTP 浏览器。文件操作都在右键菜单里，底部只留一个动作按钮。
// 双击目录进入、双击文件另存为；F5 刷新，Backspace 上级，Delete 删除。
LRESULT CALLBACK fileListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SessionStateScope visible(findSessionState(g_selectedTabId, false));
    switch (msg) {
        case WM_CREATE:
            DragAcceptFiles(hwnd, TRUE);  // 允许把本地文件拖进来上传
            return 0;
        case WM_PAINT:
            paintFileList(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            g_app.fsScroll = std::clamp(g_app.fsScroll, 0, fileMaxScroll());
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int index = fileIndexFromPoint(pt.x, pt.y);
            if (index != g_app.fsHover) {
                g_app.fsHover = index;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            // 左键按住移动超过系统拖动阈值：开始拖出下载
            if (g_app.fsDragArmed && (wp & MK_LBUTTON)) {
                const int dx = std::abs(pt.x - g_app.fsDragOrigin.x);
                const int dy = std::abs(pt.y - g_app.fsDragOrigin.y);
                if (dx > GetSystemMetrics(SM_CXDRAG) || dy > GetSystemMetrics(SM_CYDRAG)) {
                    g_app.fsDragArmed = false;
                    beginDragOut(hwnd);
                    return 0;
                }
            }
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_app.fsHover != -1) {
                g_app.fsHover = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int index = fileIndexFromPoint(pt.x, pt.y);
            if (index >= 0 && index != g_app.fsSel) {
                g_app.fsSel = index;
                updateButtons();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            const RemoteEntry* entry = selectedFileEntry();
            g_app.fsDragArmed = entry != nullptr && !entry->isDir() && g_app.fsReady;
            g_app.fsDragOrigin = pt;
            return 0;
        }
        case WM_LBUTTONUP:
            g_app.fsDragArmed = false;
            return 0;
        case WM_LBUTTONDBLCLK: {
            const int index = fileIndexFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (index >= 0) {
                g_app.fsSel = index;
                InvalidateRect(hwnd, nullptr, FALSE);
                fsOpenEntry(index);
            }
            return 0;
        }
        case WM_CONTEXTMENU: {
            POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (screen.x == -1 && screen.y == -1) {  // 键盘（Shift+F10）触发
                RECT rc;
                GetClientRect(hwnd, &rc);
                screen = POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
                ClientToScreen(hwnd, &screen);
            }
            // 右键点在某一项上就先选中它，菜单里的「下载 / 删除」才有的放矢
            POINT client = screen;
            ScreenToClient(hwnd, &client);
            const int index = fileIndexFromPoint(client.x, client.y);
            if (index >= 0 && index != g_app.fsSel) {
                g_app.fsSel = index;
                updateButtons();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            openFileContextMenu(hwnd, screen);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            if (notches != 0) {
                g_app.fsScroll = std::clamp(g_app.fsScroll - notches * (fileItemH() * 2), 0,
                                            fileMaxScroll());
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            const int count = static_cast<int>(g_app.fsEntries.size());
            if (wp == VK_UP || wp == VK_DOWN) {
                if (count > 0) {
                    const int base = g_app.fsSel < 0 ? 0 : g_app.fsSel;
                    g_app.fsSel = std::clamp(base + (wp == VK_DOWN ? 1 : -1), 0, count - 1);
                    fileEnsureVisible(g_app.fsSel);
                    updateButtons();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (wp == VK_RETURN) {
                fsOpenEntry(g_app.fsSel);
                return 0;
            }
            if (wp == VK_BACK) {
                fsGoUp();
                return 0;
            }
            if (wp == VK_DELETE) {
                fsDeleteSelected();
                return 0;
            }
            if (wp == VK_F5) {
                fsRefresh();
                return 0;
            }
            // Ctrl+L：跳到目录（资源管理器 / 浏览器的通用习惯；终端里的 Ctrl+L 不受影响）
            if (wp == 'L' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                focusFsPath();
                return 0;
            }
            return 0;
        }
        case WM_DROPFILES: {
            // 拖入：把本地文件上传到当前远端目录（目录略过，RemoteFs 只传文件）
            HDROP drop = reinterpret_cast<HDROP>(wp);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> paths;
            for (UINT i = 0; i < count; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(static_cast<std::size_t>(len) + 1, L'\0');
                DragQueryFileW(drop, i, path.data(), len + 1);
                path.resize(len);
                paths.push_back(path);
            }
            DragFinish(drop);
            if (!g_app.fsReady) {
                messageWarn(L"SFTP 未就绪", L"文件通道还没连上，暂时不能上传。");
                return 0;
            }
            int sent = 0;
            for (const std::wstring& local : paths) {
                const DWORD attrs = GetFileAttributesW(local.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES) continue;
                if (attrs & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::string name = localBaseName(toUtf8(local));
                auto cancel = std::make_shared<std::atomic<bool>>(false);
                g_app.fs->upload(toUtf8(local), remoteJoin(g_app.fsPath, name), cancel);
                ++g_app.fsPendingUploads;
                ++sent;
            }
            if (sent > 0) {
                setStatus(L"正在上传 " + std::to_wstring(sent) + L" 个文件到 " + toWide(g_app.fsPath) +
                          L" …");
            } else {
                messageWarn(L"没有可上传的文件",
                            L"拖进来的都是文件夹。请直接拖文件，或用右键菜单「上传文件…」。");
            }
            updateButtons();
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 主窗口绘制

bool buttonEnabled(int id) {
    const bool busy = g_app.connecting;
    const bool connected = g_app.shell->running();
    switch (id) {
        case ID_BTN_ACTION:
            return connected || !busy;
        case IDM_RECONNECT:
            return !connected && !busy && selectedSession();
        case ID_BTN_FS_UP:
            return g_app.fsReady && !g_app.fsPath.empty() && g_app.fsPath != "/";
        default:
            return true;
    }
}

int hitButton(POINT pt) {
    for (const auto& b : g_app.buttons) {
        if (pt.x >= b.rc.left && pt.x < b.rc.right && pt.y >= b.rc.top && pt.y < b.rc.bottom)
            return b.id;
    }
    return -1;
}

int hitTitleButton(POINT pt) {
    for (int i = 0; i < 4; ++i) {
        const RECT& r = g_app.titleButtons[i];
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) return i;
    }
    return -1;
}

// 标题栏按钮的矢量图标：不依赖任何字体，缺字风险为零
void drawTitleGlyph(HDC dc, int kind, const RECT& rc, std::uint32_t color, bool maximized) {
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, std::max(1, dp(1)), toColorRef(color));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    switch (kind) {
        case 0: {  // 汉堡：三条横线
            const int w = dp(7);
            for (int i = -1; i <= 1; ++i) {
                MoveToEx(dc, cx - w, cy + i * dp(4) + (i > 0 ? 1 : 0), nullptr);
                LineTo(dc, cx + w + 1, cy + i * dp(4) + (i > 0 ? 1 : 0));
            }
            break;
        }
        case 1:  // 最小化
            MoveToEx(dc, cx - dp(5), cy, nullptr);
            LineTo(dc, cx + dp(5) + 1, cy);
            break;
        case 2: {  // 最大化 / 还原
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            if (maximized) {
                Rectangle(dc, cx - dp(6), cy - dp(4), cx + dp(3), cy + dp(4) + 1);
                Rectangle(dc, cx - dp(3), cy - dp(6), cx + dp(6), cy + dp(2) + 1);
            } else {
                Rectangle(dc, cx - dp(5), cy - dp(5), cx + dp(5) + 1, cy + dp(5) + 1);
            }
            SelectObject(dc, oldBrush);
            break;
        }
        default:  // 关闭
            MoveToEx(dc, cx - dp(5), cy - dp(5), nullptr);
            LineTo(dc, cx + dp(5) + 1, cy + dp(5) + 1);
            MoveToEx(dc, cx + dp(5), cy - dp(5), nullptr);
            LineTo(dc, cx - dp(5) + 1, cy + dp(5) + 1);
            break;
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// 标题栏：logo + 应用名 + 当前会话 + 连接状态药丸 + 菜单/最小化/最大化/关闭
void paintTitleBar(HDC dc, const RECT& rc) {
    const int h = g_app.titleH;
    if (h <= 0) return;
    fillRect(dc, RECT{0, 0, rc.right, h}, ui::bgPanel);
    drawHLine(dc, 0, rc.right, h - 1, ui::border);

    const int btnW = dp(46);
    for (int i = 0; i < 4; ++i) {
        g_app.titleButtons[i] =
            RECT{rc.right - btnW * (4 - i), 0, rc.right - btnW * (3 - i), h};
    }

    // 右侧窗口按钮（关闭 hover 变红，是 Windows 用户的肌肉记忆）
    const bool maximized = IsZoomed(g_app.main) != FALSE;
    for (int i = 0; i < 4; ++i) {
        const bool hovered = (g_app.hoverTitleBtn == i);
        const bool pressed = hovered && g_app.pressTitleBtn == i;
        if (hovered) {
            const std::uint32_t hoverBg = (i == 3) ? ui::danger : ui::bgHover;
            fillRect(dc, g_app.titleButtons[i], pressed ? ui::mix(hoverBg, 0x000000, 0.18) : hoverBg);
        }
        std::uint32_t glyph = (i == 3 && hovered) ? 0xFFFFFF : (hovered ? ui::text : ui::textDim);
        drawTitleGlyph(dc, i, g_app.titleButtons[i], glyph, maximized);
    }

    // logo
    const int box = dp(22);
    RECT logo{dp(14), (h - box) / 2, dp(14) + box, (h - box) / 2 + box};
    fillRoundRect(dc, logo, dp(6), ui::mix(ui::accent, ui::bgPanel, 0.85),
                  ui::mix(ui::accent, ui::bgPanel, 0.55));
    drawText(dc, L">_", logo, ui::accent, g_app.uiFontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 应用名
    int x = logo.right + dp(10);
    const int appW = textWidth(dc, L"rshell", g_app.titleFont);
    RECT appRc{x, 0, x + appW + dp(2), h};
    drawText(dc, L"rshell", appRc, ui::text, g_app.titleFont);
    x = appRc.right + dp(10);

    // 连接状态药丸（宽度随文字变化）
    const wchar_t* pillLabel = L"未连接";
    std::uint32_t pillColor = ui::textFaint;
    if (g_app.connecting) {
        pillLabel = L"连接中";
        pillColor = ui::warn;
    } else if (g_app.shell->running()) {
        pillLabel = L"已连接";
        pillColor = ui::success;
    }
    const int pillW = textWidth(dc, pillLabel, g_app.tinyFont) + dp(28);
    const int pillRight = g_app.titleButtons[0].left - dp(8);
    const int pillLeft = pillRight - pillW;

    // 当前会话名（放不下就省略号）
    const int subMaxW = pillLeft - x - dp(12);
    if (subMaxW > dp(36)) {
        fillCircle(dc, x + dp(1), h / 2, dp(2), ui::textFaint);
        x += dp(8);
        const std::wstring subtitle =
            g_app.currentSession.empty() ? L"SSH 工作空间" : toWide(g_app.currentSession);
        RECT subRc{x, 0, x + subMaxW, h};
        drawText(dc, ellipsize(dc, subtitle, g_app.uiFont, subMaxW), subRc, ui::textDim, g_app.uiFont);
    }

    const int pillH = dp(20);
    RECT pill{pillLeft, (h - pillH) / 2, pillRight, (h - pillH) / 2 + pillH};
    fillRoundRect(dc, pill, pillH / 2, ui::mix(pillColor, ui::bgPanel, 0.86),
                  ui::mix(pillColor, ui::bgPanel, 0.6));
    fillCircle(dc, pill.left + dp(9), (pill.top + pill.bottom) / 2, dp(3), pillColor);
    RECT pillText{pill.left + dp(16), pill.top, pill.right - dp(4), pill.bottom};
    drawText(dc, pillLabel, pillText, pillColor, g_app.tinyFont,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// 侧栏按钮：主按钮实心主色；连接后主按钮变“断开”（危险色描边）
void paintButtons(HDC dc) {
    const bool connected = g_app.shell->running();
    for (const auto& b : g_app.buttons) {
        const bool enabled = buttonEnabled(b.id);
        const bool hovered = enabled && g_app.hoverButton == b.id;
        const bool pressed = hovered && g_app.pressButton == b.id;
        std::uint32_t bg = ui::bgCard;
        std::uint32_t fg = ui::textDim;
        std::uint32_t border = ui::borderSoft;

        const bool primarySolid = b.primary && !connected;
        if (primarySolid) {
            if (!enabled) {
                bg = ui::mix(ui::action, ui::bgApp, 0.62);
                fg = ui::mix(0xFFFFFF, ui::bgApp, 0.35);
            } else if (pressed) {
                bg = ui::actionPressed;
                fg = 0xFFFFFF;
            } else {
                bg = hovered ? ui::actionHover : ui::action;
                fg = 0xFFFFFF;
            }
        } else if (b.primary) {
            bg = pressed ? ui::mix(ui::bgCard, 0x000000, 0.15) : (hovered ? ui::bgHover : ui::bgCard);
            fg = ui::mix(ui::danger, ui::text, 0.45);
            border = ui::mix(ui::danger, ui::bgApp, 0.45);
        } else {
            if (!enabled) {
                bg = ui::mix(ui::bgCard, ui::bgApp, 0.5);
                fg = ui::mix(ui::textFaint, ui::bgApp, 0.35);
            } else {
                bg = pressed ? ui::mix(ui::bgHover, 0x000000, 0.12)
                             : (hovered ? ui::bgHover : ui::bgCard);
                fg = hovered ? ui::text : ui::textDim;
            }
        }

        fillRoundRect(dc, b.rc, dp(6), bg, border);

        std::wstring label;
        switch (b.id) {
            case ID_BTN_ACTION:
                label = connected ? L"断开连接" : (selectedSession() ? L"重新连接" : L"打开会话列表…");
                break;
            case IDM_RECONNECT:
                label = L"重新连接";
                break;
            case ID_BTN_FS_UP:
                label = L"↑";
                break;
            default:
                break;
        }
        if (b.id == ID_BTN_FS_UP) {
            drawText(dc, label, b.rc, fg, g_app.uiFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            drawText(dc, label, b.rc, fg, g_app.uiFontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

// 状态栏：左侧状态 + 中间远端指标 + 右侧终端尺寸
std::wstring formatLoad(double value) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.2f", value);
    return buf;
}

std::wstring formatPercent(double value) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d%%", static_cast<int>(value + 0.5));
    return buf;
}

std::wstring formatGigabytes(std::uint64_t kb) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1fG", kb / 1048576.0);
    return buf;
}

// 远端机器指标：负载 / CPU / 内存 / 磁盘 / 采样延迟
std::wstring statsLine() {
    const RemoteStats& s = g_app.stats;
    if (!s.valid) return std::wstring();
    std::wstring out = L"负载 " + formatLoad(s.load1) + L"  " + formatLoad(s.load5) + L"  " +
                       formatLoad(s.load15);
    if (s.cpuPercent >= 0) out += L"   ·   CPU " + formatPercent(s.cpuPercent);
    if (s.memTotalKb > 0) {
        const std::uint64_t used = s.memTotalKb > s.memAvailKb ? s.memTotalKb - s.memAvailKb : 0;
        out += L"   ·   内存 " + formatGigabytes(used) + L"/" + formatGigabytes(s.memTotalKb);
    }
    if (s.diskTotalKb > 0) {
        out += L"   ·   磁盘 " +
               formatPercent(static_cast<double>(s.diskUsedKb) * 100.0 /
                             static_cast<double>(s.diskTotalKb));
    }
    if (s.latencyMs >= 0) out += L"   ·   " + std::to_wstring(s.latencyMs) + L" ms";
    return out;
}

std::size_t failedDownloadCount() {
    return static_cast<std::size_t>(std::count_if(g_app.downloads.begin(), g_app.downloads.end(),
        [](const App::DownloadTask& task) { return task.state == App::DownloadState::Failed; }));
}

// 右侧常驻传输入口：空闲、进行中、失败都使用同一位置。
std::wstring downloadChipText() {
    std::size_t active = 0;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    bool anyUp = false;
    bool anyDown = false;
    for (const auto& task : g_app.downloads) {
        if (!downloadActive(task)) continue;
        ++active;
        done += task.done;
        total += task.total;
        if (task.upload) {
            anyUp = true;
        } else {
            anyDown = true;
        }
    }
    std::wstring text;
    const std::size_t failed = failedDownloadCount();
    if (active > 0) {
        // 方向一眼可见：只有上传/只有下载用单箭头，两者混合用 ⇅
        text = (anyUp && anyDown) ? L"⇅ " : (anyUp ? L"↑ " : L"↓ ");
        text += std::to_wstring(active) + L" 项";
        if (failed > 0) {
            text += L" · ! " + std::to_wstring(failed);
        } else if (total > 0) {
            text += L" · " + formatPercent(static_cast<double>(done) * 100.0 /
                                           static_cast<double>(total));
        } else {
            text += L" · 传输中";
        }
    } else if (failed > 0) {
        text = L"⇅ " + std::to_wstring(failed) + L" 项失败";
    } else if (g_app.downloads.empty()) {
        text = L"⇅ 传输 · 空闲";
    } else if (std::any_of(g_app.downloads.begin(), g_app.downloads.end(),
                         [](const App::DownloadTask& task) { return task.state == App::DownloadState::Cancelled; })) {
        text = L"⇅ 无进行中任务";
    } else {
        text = L"⇅ 已完成";
    }
    text += g_app.downloadsOpen ? L"  ▲" : L"  ▼";
    return text;
}

// 终端参数只用于悬停说明和调整后的短暂提示。
std::wstring terminalInfoText() {
    return std::to_wstring(g_app.termData.cols()) + L" × " +
           std::to_wstring(g_app.termData.rows()) + L"   ·   回滚 " +
           std::to_wstring(g_app.termData.scrollbackCount()) + L"   ·   字号 " +
           std::to_wstring(g_app.fontSize);
}

struct StatusLayout {
    RECT connection{}, content{}, transfer{}, vault{}, reconnect{}, dismiss{};
};

StatusLayout statusGeometry(const RECT& client) {
    StatusLayout layout;
    const int y = client.bottom - g_app.statusH;
    const int top = y + dp(4), bottom = client.bottom - dp(4);
    layout.connection = RECT{dp(24), y, std::clamp<int>(client.right / 4, dp(160), dp(240)), client.bottom};
    layout.transfer = RECT{client.right - dp(182), top, client.right - dp(14), bottom};
    int right = layout.transfer.left - dp(12);
    if (g_app.vault.unlocked()) {
        layout.vault = RECT{right - dp(108), top, right, bottom};
        right = layout.vault.left - dp(12);
    }
    if (!g_app.connecting && !g_app.shell->running() && selectedSession()) {
        layout.reconnect = RECT{right - dp(76), top, right, bottom};
        right = layout.reconnect.left - dp(12);
    }
    const int left = layout.connection.right + dp(16);
    if (g_app.statusError && !g_app.statusText.empty() && right - left >= dp(56)) {
        layout.dismiss = RECT{right - dp(22), top, right, bottom};
        right = layout.dismiss.left - dp(4);
    }
    layout.content = RECT{left, y, std::max(left, right), client.bottom};
    return layout;
}

std::wstring compactStatsText(HDC dc, int width) {
    const RemoteStats& stats = g_app.stats;
    if (!stats.valid || width <= 0) return L"";
    std::vector<std::wstring> parts;
    if (stats.cpuPercent >= 0) parts.push_back(L"CPU " + formatPercent(stats.cpuPercent));
    if (stats.memTotalKb > 0) {
        const auto used = stats.memTotalKb - std::min(stats.memTotalKb, stats.memAvailKb);
        parts.push_back(L"内存 " + formatPercent(100.0 * used / stats.memTotalKb));
    }
    const std::wstring disk = stats.diskTotalKb > 0 ?
        L"磁盘 " + formatPercent(100.0 * stats.diskUsedKb / stats.diskTotalKb) : L"";
    if (!disk.empty()) parts.push_back(disk);
    if (stats.latencyMs >= 0) parts.push_back(L"采样 " + std::to_wstring(stats.latencyMs) + L"ms");
    for (;;) {
        std::wstring text;
        for (const auto& part : parts) { if (!text.empty()) text += L"   "; text += part; }
        if (textWidth(dc, text, g_app.uiFont) <= width || parts.empty()) return text;
        const auto diskPart = std::find(parts.begin(), parts.end(), disk);
        if (!disk.empty() && diskPart != parts.end()) parts.erase(diskPart);
        else parts.pop_back();
    }
}

RECT statusDownloadChip(HWND hwnd) {
    RECT client{};
    if (!hwnd || g_app.statusH <= 0) return client;
    GetClientRect(hwnd, &client);
    return statusGeometry(client).transfer;
}

// 展开的下载列表：贴状态栏上沿整宽停靠，几何在绘制和命中测试之间共用
struct DownloadRowLayout {
    RECT row{};
    RECT cancel{};
    bool cancelEnabled = false;
};

struct DownloadsPanelLayout {
    bool visible = false;
    RECT band{};
    RECT clearButton{};
    RECT closeButton{};
    std::vector<DownloadRowLayout> rows{};
};

DownloadsPanelLayout downloadsPanelGeometry(HWND hwnd) {
    DownloadsPanelLayout layout;
    if (!hwnd || !g_app.downloadsOpen) return layout;
    const int panelH = downloadsPanelHeight();
    if (panelH <= 0) return layout;

    RECT client{};
    GetClientRect(hwnd, &client);
    const int bottom = client.bottom - g_app.statusH;
    layout.visible = true;
    layout.band = RECT{dp(10), bottom - panelH, client.right - dp(10), bottom};

    const int headerH = dp(30);
    const int rowH = dp(34);
    const int btnH = dp(20);
    const int closeSize = dp(18);
    layout.closeButton = RECT{layout.band.right - dp(10) - closeSize,
                              layout.band.top + (headerH - closeSize) / 2,
                              layout.band.right - dp(10),
                              layout.band.top + (headerH - closeSize) / 2 + closeSize};
    if (hasFinishedDownloads()) {
        const int clearW = dp(78);
        layout.clearButton = RECT{layout.closeButton.left - dp(8) - clearW,
                                  layout.band.top + (headerH - btnH) / 2,
                                  layout.closeButton.left - dp(8),
                                  layout.band.top + (headerH - btnH) / 2 + btnH};
    }

    const int rows = downloadVisibleRows();
    for (int i = 0; i < rows; ++i) {
        DownloadRowLayout row;
        const int top = layout.band.top + headerH + i * rowH;
        row.row = RECT{layout.band.left + dp(6), top, layout.band.right - dp(6), top + rowH};
        if (downloadActive(g_app.downloads[static_cast<std::size_t>(i)])) {
            row.cancelEnabled = true;
            row.cancel = RECT{row.row.right - dp(8) - dp(52), top + (rowH - dp(20)) / 2,
                              row.row.right - dp(8), top + (rowH - dp(20)) / 2 + dp(20)};
        }
        layout.rows.push_back(row);
    }
    return layout;
}

// 下载胶囊 / 列表里可点区域（绘制与命中测试共用同一份几何）
struct DownloadHit {
    bool chip = false;
    bool close = false;
    bool clear = false;
    bool inPanel = false;
    int cancelIndex = -1;  // 点在某行的「取消」上
};

DownloadHit hitDownloadUi(HWND hwnd, const POINT& pt) {
    DownloadHit hit;
    const RECT chip = statusDownloadChip(hwnd);
    if (chip.right > chip.left && PtInRect(&chip, pt)) {
        hit.chip = true;
        return hit;
    }
    const DownloadsPanelLayout layout = downloadsPanelGeometry(hwnd);
    if (!layout.visible) return hit;
    if (PtInRect(&layout.band, pt)) hit.inPanel = true;
    if (PtInRect(&layout.closeButton, pt)) {
        hit.close = true;
        return hit;
    }
    if (layout.clearButton.right > layout.clearButton.left &&
        PtInRect(&layout.clearButton, pt)) {
        hit.clear = true;
        return hit;
    }
    for (std::size_t i = 0; i < layout.rows.size(); ++i) {
        if (layout.rows[i].cancelEnabled && PtInRect(&layout.rows[i].cancel, pt)) {
            hit.cancelIndex = static_cast<int>(i);
            break;
        }
    }
    return hit;
}

void paintDownloadsPanel(HDC dc, HWND hwnd) {
    const DownloadsPanelLayout layout = downloadsPanelGeometry(hwnd);
    if (!layout.visible) return;
    fillRoundRect(dc, layout.band, dp(8), ui::bgCard, ui::border);

    const int headerH = dp(30);
    RECT titleRc{layout.band.left + dp(12), layout.band.top, layout.band.left + dp(84),
                 layout.band.top + headerH};
    drawText(dc, L"传输", titleRc, ui::text, g_app.uiFontBold,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const std::size_t active = activeDownloadCount();
    std::wstring summary = std::to_wstring(g_app.downloads.size()) + L" 项";
    if (active > 0) summary += L" · " + std::to_wstring(active) + L" 进行中";
    const int buttonsLeft = layout.clearButton.right > layout.clearButton.left
                                ? static_cast<int>(layout.clearButton.left)
                                : static_cast<int>(layout.closeButton.left);
    RECT summaryRc{titleRc.right, layout.band.top, buttonsLeft - dp(10),
                   layout.band.top + headerH};
    if (summaryRc.right > summaryRc.left) {
        drawText(dc, summary, summaryRc, ui::textFaint, g_app.tinyFont,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (layout.clearButton.right > layout.clearButton.left) {
        drawText(dc, L"清除已完成", layout.clearButton, ui::textDim, g_app.tinyFont,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    drawText(dc, L"✕", layout.closeButton, ui::textDim, g_app.uiFont,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (g_app.downloads.empty()) {
        drawText(dc, L"暂无传输任务", RECT{layout.band.left + dp(12), layout.band.top + headerH,
                 layout.band.right - dp(12), layout.band.bottom - dp(6)}, ui::textDim, g_app.uiFont,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    for (std::size_t i = 0; i < layout.rows.size(); ++i) {
        const App::DownloadTask& task = g_app.downloads[i];
        const DownloadRowLayout& row = layout.rows[i];
        const int rowH = row.row.bottom - row.row.top;
        const bool hover = static_cast<int>(i) == g_app.hoverDownloadCancel;
        if (row.cancelEnabled && hover) fillRoundRect(dc, row.row, dp(6), ui::bgHover);

        // 类型图标：借用文件列表那套的 16px 版本
        const int icon = dp(16);
        RemoteEntry meta;
        meta.name = toUtf8(task.name);
        meta.mode = 0100644u;  // 只用来判扩展名，固定按普通文件处理
        paintFileIcon(dc,
                      RECT{row.row.left + dp(8), row.row.top + (rowH - icon) / 2,
                           row.row.left + dp(8) + icon, row.row.top + (rowH - icon) / 2 + icon},
                      fileKindOf(meta));

        const int right = row.cancelEnabled ? static_cast<int>(row.cancel.left) - dp(10)
                                            : static_cast<int>(row.row.right) - dp(8);
        const int statusLeft = right - dp(150);
        int barRight = statusLeft - dp(12);
        int barLeft = std::max(static_cast<int>(row.row.left) + dp(160), barRight - dp(220));
        if (barLeft > barRight) barLeft = barRight;
        RECT nameRc{row.row.left + dp(30), row.row.top,
                    std::max(barLeft - dp(10), static_cast<int>(row.row.left) + dp(50)),
                    row.row.bottom};
        // 名称前带方向箭头：上传和下载混在一个列表里也能一眼分清
        const std::wstring label = (task.upload ? L"↑ " : L"↓ ") + task.name;
        drawText(dc, ellipsize(dc, label, g_app.uiFont, nameRc.right - nameRc.left), nameRc,
                 ui::text, g_app.uiFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        double ratio = 0.0;
        if (task.total > 0) {
            ratio = static_cast<double>(task.done) / static_cast<double>(task.total);
            ratio = std::clamp(ratio, 0.0, 1.0);
        }
        const int barH = dp(6);
        const int barTop = row.row.top + (rowH - barH) / 2;
        if (barRight - barLeft > dp(24)) {
            fillRoundRect(dc, RECT{barLeft, barTop, barRight, barTop + barH}, barH / 2,
                          ui::bgHover);
            if (ratio > 0.0) {
                const int fillRight =
                    barLeft + std::max(dp(2), static_cast<int>((barRight - barLeft) * ratio));
                std::uint32_t barColor = ui::accent;
                if (task.state == App::DownloadState::Done) barColor = ui::success;
                if (task.state == App::DownloadState::Failed ||
                    task.state == App::DownloadState::Cancelled) {
                    barColor = ui::danger;
                }
                fillRoundRect(dc, RECT{barLeft, barTop, fillRight, barTop + barH}, barH / 2,
                              barColor);
            }
        }

        // 右侧一行状态：百分比 + 已传/总量，或结束状态
        std::wstring status;
        std::uint32_t statusColor = ui::textDim;
        switch (task.state) {
            case App::DownloadState::Active:
                if (task.total > 0) {
                    status = formatPercent(ratio * 100.0) + L" · " +
                             toWide(formatBytes(task.done)) + L" / " +
                             toWide(formatBytes(task.total));
                } else {
                    status = task.upload ? L"上传中…" : L"下载中…";
                }
                break;
            case App::DownloadState::Done:
                status = L"已完成";
                statusColor = ui::success;
                break;
            case App::DownloadState::Cancelled:
                status = L"已取消";
                statusColor = ui::textFaint;
                break;
            case App::DownloadState::Failed:
                status = L"失败";
                statusColor = ui::danger;
                break;
        }
        drawText(dc, status, RECT{statusLeft, row.row.top, right, row.row.bottom}, statusColor,
                 g_app.tinyFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (row.cancelEnabled) {
            fillRoundRect(dc, row.cancel, dp(5), hover ? ui::bgActive : ui::bgPanel,
                          hover ? ui::border : ui::borderSoft);
            drawText(dc, L"取消", row.cancel, hover ? ui::text : ui::textDim, g_app.tinyFont,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // 项数超过显示上限时说明一句，免得用户以为任务丢了
    const std::size_t hidden = g_app.downloads.size() - layout.rows.size();
    if (hidden > 0) {
        RECT moreRc{layout.band.left + dp(30),
                    layout.band.top + headerH + downloadVisibleRows() * dp(34),
                    layout.band.right - dp(12), layout.band.bottom - dp(4)};
        drawText(dc, L"还有 " + std::to_wstring(hidden) + L" 项未显示，可先清除已完成的",
                 moreRc, ui::textFaint, g_app.tinyFont,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

}

std::wstring statusConnectionText() {
    std::wstring text = g_app.connecting ? L"连接中" :
        (g_app.shell->running() ? L"已连接" : (g_app.connectionId ? L"已断开" : L"未连接"));
    if (!g_app.currentSession.empty()) text += L" · " + toWide(g_app.currentSession);
    return text;
}

void paintStatusBar(HDC dc, const RECT& rc) {
    const int h = g_app.statusH;
    if (h <= 0) return;
    const int y = rc.bottom - h;
    const StatusLayout layout = statusGeometry(rc);
    fillRect(dc, RECT{0, y, rc.right, rc.bottom}, ui::bgPanel);
    drawHLine(dc, 0, rc.right, y, ui::border);
    const std::uint32_t dot = g_app.connecting ? ui::warn :
        (g_app.shell->running() ? ui::success : (g_app.statusError ? ui::danger : ui::textFaint));
    fillCircle(dc, dp(14), y + h / 2, dp(3), dot);
    drawText(dc, statusConnectionText(), layout.connection, ui::text, g_app.uiFont,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    fillRect(dc, RECT{layout.content.left - dp(8), y + dp(10), layout.content.left - dp(8) + 1,
                      rc.bottom - dp(10)}, ui::borderSoft);

    const ULONGLONG now = GetTickCount64();
    const bool notice = statusNoticeActive(now);
    const std::wstring content = notice ? g_app.statusText :
        (now < g_app.terminalInfoUntil ? terminalInfoText() :
         compactStatsText(dc, layout.content.right - layout.content.left));
    drawText(dc, content, layout.content, notice && g_app.statusError ? ui::danger : ui::textDim,
             g_app.uiFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (layout.dismiss.right > layout.dismiss.left)
        dlg::paintButton(dc, layout.dismiss, L"×", false, g_app.hoverStatusAction == 2, false, false, true);
    if (layout.reconnect.right > layout.reconnect.left)
        dlg::paintButton(dc, layout.reconnect, L"重新连接", false, g_app.hoverStatusAction == 1, false, false, true);
    if (layout.vault.right > layout.vault.left) {
        fillRoundRect(dc, layout.vault, dp(5), ui::mix(ui::accent, ui::bgPanel, 0.85));
        drawText(dc, L"保险库 已解锁", layout.vault, ui::textDim, g_app.uiFont,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    fillRoundRect(dc, layout.transfer, dp(5), g_app.hoverDownloadChip ? ui::bgHover : ui::bgCard);
    drawText(dc, downloadChipText(), layout.transfer,
             failedDownloadCount() ? ui::danger : (activeDownloadCount() ? ui::accent : ui::textDim),
             g_app.uiFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void hideStatusHint() {
    if (g_app.statusHint) ShowWindow(g_app.statusHint, SW_HIDE);
}

std::wstring statusDetailsText() {
    std::wstring text = statusConnectionText();
    if (statusNoticeActive(GetTickCount64())) text += L"\n" + g_app.statusText.substr(0, 400);
    const RemoteStats& stats = g_app.stats;
    if (stats.valid) {
        text += L"\n" + toWide(stats.host) + L" · " + toWide(stats.os) +
                L" · " + std::to_wstring(stats.cores) + L" 核\n" + statsLine();
        text += L"\n采样耗时包含远端命令执行，不等同于纯网络 RTT。";
    }
    text += L"\n终端 " + terminalInfoText();
    text += g_app.vault.unlocked() ? L"\n保险库已解锁：密钥仍在内存中" :
        (vaultFilePresent() ? L"\n保险库已锁定" : L"\n保险库未创建");
    if (failedDownloadCount()) text += L"\n存在失败传输，点击右侧传输入口查看。";
    return text;
}

LRESULT CALLBACK statusHintProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCDESTROY && g_app.statusHint == hwnd) g_app.statusHint = nullptr;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        fillRoundRect(dc, rc, dp(8), ui::bgCard, ui::border);
        InflateRect(&rc, -dp(12), -dp(10));
        drawText(dc, g_app.statusHintText, rc, ui::text, g_app.uiFont,
                 DT_LEFT | DT_TOP | DT_WORDBREAK);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void showStatusHint(HWND owner, POINT point) {
    RECT client;
    GetClientRect(owner, &client);
    if (point.y < client.bottom - g_app.statusH) return;
    static bool registered = false;
    constexpr wchar_t name[] = L"RshellStatusHint";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = statusHintProc;
        wc.hInstance = g_app.inst;
        wc.lpszClassName = name;
        registered = RegisterClassW(&wc) != 0;
        if (!registered) return;
    }
    if (!g_app.statusHint)
        g_app.statusHint = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            name, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr, g_app.inst, nullptr);
    if (!g_app.statusHint) return;
    g_app.statusHintText = statusDetailsText();
    POINT anchor{point.x, client.bottom - g_app.statusH - dp(6)};
    ClientToScreen(owner, &anchor);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const int width = std::min<int>(dp(400), monitor.rcWork.right - monitor.rcWork.left - dp(20));
    RECT textRc{0, 0, width - dp(24), 0};
    HDC dc = GetDC(owner);
    HGDIOBJ old = SelectObject(dc, g_app.uiFont);
    DrawTextW(dc, g_app.statusHintText.c_str(), -1, &textRc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(owner, dc);
    const int height = std::min<int>(textRc.bottom + dp(20), monitor.rcWork.bottom - monitor.rcWork.top - dp(20));
    const int x = std::clamp<int>(anchor.x, monitor.rcWork.left + dp(10), monitor.rcWork.right - width - dp(10));
    const int y = std::max<int>(monitor.rcWork.top + dp(10), anchor.y - height);
    SetWindowPos(g_app.statusHint, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_app.statusHint, nullptr, FALSE);
}

void paintMain(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    if (rc.right <= 0 || rc.bottom <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    fillRect(mem, rc, ui::bgApp);
    paintTitleBar(mem, rc);
    paintSessionTabs(mem);

    const int pad = dp(14);

    if (g_app.contentTop > 0) {
        fillRoundRect(mem, RECT{pad - dp(6), g_app.contentTop - dp(4),
                               pad + g_app.sidebarW + dp(6), g_app.contentBottom + dp(6)},
                      dp(10), ui::bgPanel, ui::borderSoft);
        drawText(mem, L"远端文件", RECT{pad + dp(4), g_app.contentTop, pad + g_app.sidebarW - dp(58),
                                      g_app.contentTop + dp(28)}, ui::text, g_app.uiFontBold);
        const RECT protocol{pad + g_app.sidebarW - dp(48), g_app.contentTop + dp(4),
                            pad + g_app.sidebarW - dp(2), g_app.contentTop + dp(24)};
        fillRoundRect(mem, protocol, dp(5), ui::bgCard);
        drawText(mem, L"SFTP", protocol, ui::textDim, g_app.tinyFont,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // 终端上方的会话名标题栏已删除：标签栏已经显示会话名。
        // 只有搜索框打开时才需要这条辅助条。
        if (g_app.searchOpen) {
            fillRoundRect(mem, g_app.searchBox, dp(6), dlg::bgInput, ui::accent);
            const std::wstring count = g_app.searchMatches.empty() ? L"0 / 0" :
                std::to_wstring(g_app.searchIndex + 1) + L" / " + std::to_wstring(g_app.searchMatches.size());
            drawText(mem, count, RECT{g_app.searchBox.right + dp(4), g_app.contentTop,
                                     g_app.searchPrev.left - dp(4), g_app.contentTop + dp(28)},
                     g_app.searchMatches.empty() ? ui::textFaint : ui::warn, g_app.tinyFont,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            dlg::paintButton(mem, g_app.searchPrev, L"↑", false, false, false, false, !g_app.searchMatches.empty());
            dlg::paintButton(mem, g_app.searchNext, L"↓", false, false, false, false, !g_app.searchMatches.empty());
            dlg::paintButton(mem, g_app.searchClose, L"×", false, false, false, false, true);
        }
    }

    // 分栏标题：路径条（显示当前远端目录，点一下可以输入新路径）
    if (g_app.contentTop > 0 && g_app.fsPathRect.right > g_app.fsPathRect.left) {
        const RECT box = g_app.fsPathRect;
        const bool canEdit = g_app.fsReady;
        // 凹槽样式：比侧栏底色深半档，hover 时描边转为主色，暗示「可以点」
        fillRoundRect(mem, box, dp(5),
                      canEdit ? dlg::bgInput : ui::bgPanel,
                      canEdit ? (g_app.hoverPathRect || GetFocus() == g_app.fsPathEdit
                                     ? ui::accent : ui::border) : ui::borderSoft);
        std::wstring path = toWide(g_app.fsPath);
        if (path.empty()) path = g_app.fsReady ? L"/" : L"未连接";
        int textRight = box.right - dp(7);
        if (g_app.fsReady) {
            const std::wstring count = std::to_wstring(g_app.fsEntries.size());
            const int countW = textWidth(mem, count, g_app.tinyFont);
            drawText(mem, count,
                     RECT{textRight - countW, box.top, textRight, box.bottom}, ui::textFaint,
                     g_app.tinyFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            textRight -= countW + dp(6);
        }
        drawText(mem, pathEllipsize(mem, path, g_app.tinyFont, textRight - box.left - dp(14)),
                 RECT{box.left + dp(7), box.top, textRight, box.bottom},
                 canEdit ? ui::text : ui::textDim, g_app.tinyFont,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    paintButtons(mem);
    paintStatusBar(mem, rc);
    paintDownloadsPanel(mem, hwnd);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// 自绘按钮没有 HWND，点击后回放成 WM_COMMAND，复用原有命令分发
void activateTitleButton(HWND hwnd, int index) {
    switch (index) {
        case 0:
            openMainMenu(hwnd);
            break;
        case 1:
            ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case 2:
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case 3:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- 保险库入口
//
// 口令只存密码保险库（vault.dat），不再回退到 Windows 凭据管理器。
// 一个菜单项按状态变三种行为：没有文件 -> 新建，有文件且锁定 -> 解锁，
// 已解锁 -> 立刻锁定（DEK 从内存里抹掉，下次用还得重新输入主密码）。
//
// 解锁成功之后把 credential_store 的后端换成保险库（setCredentialVault）；
// 锁定/退出时断开。解锁时顺手清理孤儿条目（会话已删/取消记住密码的）。

// 清理保险库里没有归属的条目：锁定期间删掉的会话、或取消了「记住密码」的会话，
// 其条目只能等解锁后才有机会删（锁定时拿不到条目表）。返回删除条数。
int pruneVaultOrphans() {
    int removed = 0;
    const std::string prefix = "rshell/";
    const std::string keySuffix = "/key-passphrase";
    for (const std::string& entry : g_app.vault.names()) {
        if (entry.rfind(prefix, 0) != 0) continue;
        std::string name = entry.substr(prefix.size());
        const bool keyEntry =
            name.size() > keySuffix.size() &&
            name.compare(name.size() - keySuffix.size(), keySuffix.size(),
                         keySuffix) == 0;
        if (keyEntry) name.resize(name.size() - keySuffix.size());
        const Session* owner = g_app.store.find(name);
        if (owner && (keyEntry ? owner->saveKeyPassphrase : owner->savePassword))
            continue;
        g_app.vault.remove(entry);
        ++removed;
    }
    if (removed == 0) return 0;
    std::string error;
    if (!g_app.vault.save(&error)) {
        guiLog("清理密码保险库里的孤儿条目失败：" + error);
        return 0;
    }
    guiLog("已清理 " + std::to_string(removed) + " 条密码保险库孤儿条目");
    return removed;
}

// 已解锁 -> true；有 vault.dat -> 弹解锁框；没有文件 -> false。
bool ensureVaultUnlocked() {
    if (g_app.vault.unlocked()) return true;
    if (!vaultFilePresent()) return false;
    const VaultPromptResult result = promptVault(VaultPromptMode::Unlock);
    if (!result.ok) return false;
    setCredentialVault(&g_app.vault);
    pruneVaultOrphans();
    guiLog(result.viaTpm ? "密码保险库已用 TPM 解锁" : "密码保险库已解锁");
    InvalidateRect(g_app.main, nullptr, FALSE);
    return true;
}

std::wstring vaultMenuLabel() {
    if (g_app.vault.unlocked()) return L"锁定密码保险库";
    return vaultFilePresent() ? L"解锁密码保险库…" : L"新建密码保险库…";
}

void vaultCommand() {
    if (g_app.vault.unlocked()) {
        // 先断开后端再抹 DEK，避免这中间有代码又走一遍保险库
        setCredentialVault(nullptr);
        g_app.vault.lock();
        guiLog("密码保险库已锁定");
        setStatus(L"密码保险库已锁定，解锁后才能使用已保存的密码");
        InvalidateRect(g_app.main, nullptr, FALSE);  // 状态栏的解锁标记要撤掉
        return;
    }

    const VaultPromptMode mode =
        vaultFilePresent() ? VaultPromptMode::Unlock : VaultPromptMode::Create;
    const VaultPromptResult result = promptVault(mode);
    if (!result.ok) return;

    // 从这里开始，保存/读取口令都由保险库承担
    setCredentialVault(&g_app.vault);
    const int pruned = pruneVaultOrphans();

    const std::wstring count = std::to_wstring(g_app.vault.names().size());
    const std::wstring cleanup =
        pruned > 0 ? L"，已清理 " + std::to_wstring(pruned) + L" 条孤儿条目" : L"";
    if (result.created) {
        guiLog("已创建密码保险库");
        setStatus(L"已创建密码保险库（" + count + L" 条）" +
                  (g_app.vault.hasTpmSlot() ? L"，可用 TPM 解锁" : L""));
    } else {
        guiLog(result.viaTpm ? "密码保险库已用 TPM 解锁" : "密码保险库已解锁");
        setStatus(L"密码保险库已解锁（" + count + L" 条）" + (result.viaTpm ? L" · TPM" : L"") +
                  cleanup);
    }
    InvalidateRect(g_app.main, nullptr, FALSE);
}

void openMainMenu(HWND hwnd) {
    const bool connected = g_app.shell->running();
    const bool busy = g_app.connecting;

    std::vector<popup::Item> items;
    auto add = [&items](int id, const wchar_t* text, const wchar_t* shortcut = L"",
                        bool enabled = true, bool danger = false) {
        popup::Item item;
        item.id = id;
        item.text = text;
        item.shortcut = shortcut;
        item.enabled = enabled;
        item.danger = danger;
        items.push_back(std::move(item));
    };
    auto sep = [&items]() {
        popup::Item item;
        item.separator = true;
        items.push_back(std::move(item));
    };

    add(IDM_OPEN, L"会话管理…", L"Ctrl+O");
    add(IDM_RECONNECT, L"重新连接", L"Ctrl+Shift+R", !connected && !busy && selectedSession());
    add(IDM_CONNECT, connected ? L"断开连接" : L"连接…",
        connected ? L"" : L"Enter", connected || !busy, connected);
    add(IDM_NEW_TAB, L"新建标签", L"Ctrl+T");
    add(IDM_CLOSE_TAB, L"关闭当前标签", L"Ctrl+Shift+W");
    add(IDM_AI_PANEL, L"AI 助手", L"Ctrl+J");
    sep();
    add(IDM_NEW, L"新建会话…", L"Ctrl+N");
    add(IDM_EDIT, L"编辑当前会话…", L"", !busy);
    add(IDM_DELETE, L"删除当前会话…", L"", !busy);
    sep();
    add(IDM_VAULT, vaultMenuLabel().c_str());
    sep();
    add(IDM_COPY, L"复制", L"Ctrl+Shift+C");
    add(IDM_PASTE, L"粘贴", L"Ctrl+V");
    add(IDM_SELECT_ALL, L"全选", L"Ctrl+Shift+A");
    add(IDM_SEARCH, L"搜索终端历史…", L"Ctrl+Shift+F");
    sep();
    add(IDM_ZOOM_IN, L"放大字体", L"Ctrl++");
    add(IDM_ZOOM_OUT, L"缩小字体", L"Ctrl+-");
    add(IDM_ZOOM_RESET, L"重置字体", L"Ctrl+0");
    sep();
    add(IDM_ABOUT, L"关于 rshell");
    add(IDM_EXIT, L"退出");

    const RECT& anchor = g_app.titleButtons[0];
    POINT pt{anchor.left, anchor.bottom + dp(4)};
    ClientToScreen(hwnd, &pt);
    const int id = popup::run(hwnd, pt, items);
    if (id > 0) SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(id, 0), 0);
}

// 菜单里的「新建 / 编辑 / 删除会话」都落到同一个管理窗口：
// 新建 = 打开时表单留空，编辑 = 选中当前会话，删除 = 选中当前会话后删除
void newSessionCommand() { openSessionManager(true); }

void editSessionCommand() { openSessionManager(); }

void deleteSessionCommand() {
    if (g_app.currentSession.empty()) {
        openSessionManager();
        return;
    }
    const std::wstring name = toWide(g_app.currentSession);
    if (!messageConfirm(L"删除会话",
                        L"确定删除会话 “" + name + L"” 吗？（不会影响远端主机）", L"删除",
                        L"取消"))
        return;
    g_app.store.remove(g_app.currentSession);
    g_app.store.save();
    setStatus(L"已删除会话 " + name);
    updateButtons();
}

void applyFontSize(int size) {
    g_app.fontSize = std::clamp(size, 9, 40);
    createFonts();
    layoutTerminal(g_app.term);
    InvalidateRect(g_app.term, nullptr, FALSE);
}

void openTerminalMenu(HWND hwnd, POINT screenPt) {
    std::vector<popup::Item> items;
    auto add = [&](int id, const wchar_t* label, const wchar_t* shortcut, bool enabled = true) {
        popup::Item item;
        item.id = id;
        item.text = label;
        item.shortcut = shortcut;
        item.enabled = enabled;
        items.push_back(std::move(item));
    };
    add(IDM_COPY, L"复制", L"Ctrl+Shift+C", g_app.hasSelection);
    add(IDM_PASTE, L"粘贴", L"Ctrl+V", g_app.shell->running() && IsClipboardFormatAvailable(CF_UNICODETEXT));
    add(IDM_SELECT_ALL, L"全选", L"Ctrl+Shift+A");
    popup::Item separator;
    separator.separator = true;
    items.push_back(separator);
    add(IDM_SEARCH, L"搜索终端历史…", L"Ctrl+Shift+F");
    add(IDM_RECONNECT, L"重新连接", L"Ctrl+Shift+R",
        !g_app.connecting && !g_app.shell->running() && selectedSession());
    const int command = popup::run(hwnd, screenPt, items);
    if (command) PostMessageW(g_app.main, WM_COMMAND, command, 0);
}

// ---------------------------------------------------------------- 多标签会话

struct SessionTabsLayout {
    std::vector<RECT> items;
    RECT previous{}, next{}, add{};
    int capacity = 1;
};

SessionTabsLayout sessionTabsLayout(int width) {
    SessionTabsLayout result;
    const int pad = dp(14), gap = dp(4), button = dp(28);
    const int top = g_app.titleH + dp(3), bottom = top + dp(30);
    const int available = std::max(dp(120), width - pad * 2 - button - gap);
    result.capacity = std::max(1, available / dp(156));
    const bool overflow = static_cast<int>(g_tabs.size()) > result.capacity;
    const int arrows = overflow ? button * 2 + gap * 2 : 0;
    result.capacity = std::max(1, (available - arrows) / dp(156));
    const int count = std::min<int>(result.capacity, static_cast<int>(g_tabs.size()));
    const int tabWidth = std::min(dp(200), (available - arrows) / std::max(1, count));
    for (int i = 0; i < count; ++i)
        result.items.push_back(RECT{pad + i * tabWidth, top, pad + (i + 1) * tabWidth - gap, bottom});
    const int right = width - pad;
    result.add = RECT{right - button, top, right, bottom};
    if (overflow) {
        result.next = RECT{result.add.left - gap - button, top, result.add.left - gap, bottom};
        result.previous = RECT{result.next.left - gap - button, top, result.next.left - gap, bottom};
    }
    return result;
}

void ensureActiveTabVisible(int width) {
    const int capacity = sessionTabsLayout(width).capacity;
    if (static_cast<int>(g_activeTab) < g_app.firstVisibleTab)
        g_app.firstVisibleTab = static_cast<int>(g_activeTab);
    if (static_cast<int>(g_activeTab) >= g_app.firstVisibleTab + capacity)
        g_app.firstVisibleTab = static_cast<int>(g_activeTab) - capacity + 1;
    g_app.firstVisibleTab = std::clamp(g_app.firstVisibleTab, 0,
        std::max(0, static_cast<int>(g_tabs.size()) - capacity));
}

SessionState& sessionAt(std::size_t index) {
    return index == g_activeTab ? static_cast<SessionState&>(g_app) : *g_tabs[index];
}

void paintSessionTabs(HDC dc) {
    if (g_tabs.empty()) return;
    RECT client{};
    GetClientRect(g_app.main, &client);
    const auto layout = sessionTabsLayout(client.right);
    POINT mouse{}; GetCursorPos(&mouse); ScreenToClient(g_app.main, &mouse);
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
        const std::size_t index = static_cast<std::size_t>(g_app.firstVisibleTab) + i;
        if (index >= g_tabs.size()) break;
        const auto& state = sessionAt(index);
        const RECT& rect = layout.items[i];
        const bool active = index == g_activeTab;
        const bool hover = PtInRect(&rect, mouse) != FALSE;
        fillRoundRect(dc, rect, dp(6), active ? ui::bgActive : (hover ? ui::bgHover : ui::bgPanel),
                      active ? ui::accent : ui::borderSoft);
        const auto color = state.connecting ? ui::warn :
            (state.shell->running() ? ui::success : (state.statusError ? ui::danger : ui::textFaint));
        fillCircle(dc, rect.left + dp(11), (rect.top + rect.bottom) / 2, dp(3), color);
        const std::wstring label = state.currentSession.empty() ? L"新标签" : toWide(state.currentSession);
        RECT text{rect.left + dp(22), rect.top, rect.right - dp(28), rect.bottom};
        drawText(dc, label, text, active ? ui::text : ui::textDim,
                 active ? g_app.uiFontBold : g_app.uiFont,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (state.unread && !active)
            fillCircle(dc, rect.right - dp(30), rect.top + dp(7), dp(2), ui::accent);
        RECT close{rect.right - dp(23), rect.top + dp(4), rect.right - dp(3), rect.bottom - dp(4)};
        if (PtInRect(&close, mouse)) fillRoundRect(dc, close, dp(4), ui::bgHover);
        drawText(dc, L"×", close, ui::textDim, g_app.uiFont,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    auto button = [&](const RECT& rect, const wchar_t* label, bool enabled) {
        if (IsRectEmpty(&rect)) return;
        fillRoundRect(dc, rect, dp(5), enabled && PtInRect(&rect, mouse) ? ui::bgHover : ui::bgPanel);
        drawText(dc, label, rect, enabled ? ui::text : ui::textFaint, g_app.uiFontBold,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };
    button(layout.previous, L"‹", g_app.firstVisibleTab > 0);
    button(layout.next, L"›", g_app.firstVisibleTab + layout.capacity < static_cast<int>(g_tabs.size()));
    button(layout.add, L"＋", true);
}

int hitSessionTab(POINT pt, bool* close) {
    RECT client{}; GetClientRect(g_app.main, &client);
    const auto layout = sessionTabsLayout(client.right);
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
        const auto& rect = layout.items[i];
        if (!PtInRect(&rect, pt)) continue;
        if (close) *close = pt.x >= rect.right - dp(26);
        return g_app.firstVisibleTab + static_cast<int>(i);
    }
    return -1;
}

void restoreSessionUi() {
    if (g_app.searchEdit) {
        SetWindowTextW(g_app.searchEdit, g_app.searchQuery.c_str());
        ShowWindow(g_app.searchEdit, g_app.searchOpen ? SW_SHOW : SW_HIDE);
    }
    g_app.unread = false;
    g_app.sbHover = g_app.sbDrag = g_app.fsDragArmed = false;
    g_app.hoverButton = g_app.pressButton = g_app.hoverDownloadCancel = -1;
    g_app.hoverDownloadChip = g_app.hoverPathRect = false;
    g_app.hoverStatusAction = 0;
    g_app.cursorOn = true;
    g_app.swallowChar = -1;
    expireStatusNotices(GetTickCount64());
    layoutMain(g_app.main);
    layoutTerminal(g_app.term);
    if (g_app.searchOpen) refreshTerminalSearch(false);
    std::wstring title = L"rshell";
    if (!g_app.currentSession.empty()) title += L" - " + toWide(g_app.currentSession);
    if (!g_app.termData.title().empty()) title += L" - " + toWide(g_app.termData.title());
    SetWindowTextW(g_app.main, title.c_str());
    updateButtons();
    InvalidateRect(g_app.term, nullptr, FALSE);
    SetFocus(g_app.term);
}

void switchSessionTab(std::size_t index) {
    initializeSessionTabs();
    if (index >= g_tabs.size() || index == g_activeTab) return;
    g_app.searchQuery = textOf(g_app.searchEdit);
    if (HIMC context = ImmGetContext(g_app.term)) {
        ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext(g_app.term, context);
    }
    g_app.imeComposition.clear(); g_app.imeCursorPos = 0;
    g_app.selecting = false;
    SetFocus(g_app.main);
    if (g_app.fsPathEdit) ShowWindow(g_app.fsPathEdit, SW_HIDE);
    if (GetCapture() == g_app.term || GetCapture() == g_app.list) ReleaseCapture();
    g_tabs[g_activeTab] = std::make_unique<SessionState>(std::move(static_cast<SessionState&>(g_app)));
    static_cast<SessionState&>(g_app) = std::move(*g_tabs[index]);
    g_tabs[index].reset();
    g_activeTab = index;
    g_selectedTabId = g_app.tabId;
    restoreSessionUi();
}

void newSessionTab() {
    initializeSessionTabs();
    auto state = std::make_unique<SessionState>();
    state->tabId = g_nextTabId++;
    state->connectionId = g_nextConnectionId++;
    state->termData.setTheme(g_app.termData.theme());
    state->termData.resize(g_app.termData.cols(), g_app.termData.rows());
    g_tabs.push_back(std::move(state));
    switchSessionTab(g_tabs.size() - 1);
}

// ---------------------------------------------------------------- 会话内 AI 面板

std::vector<ai::AiHostInfo> aiHostList() {
    std::vector<ai::AiHostInfo> hosts;
    auto add = [&hosts](SessionState& state) {
        if (!state.shell->running() && !state.fs->ready()) return;
        ai::AiHostInfo info;
        info.label = state.currentSession.empty() ? L"未命名会话" : toWide(state.currentSession);
        if (state.fs->ready()) info.backend = state.fs.get();
        hosts.push_back(std::move(info));
    };
    for (auto& state : g_tabs)
        if (state) add(*state);
    add(static_cast<SessionState&>(g_app));
    return hosts;
}

ai::AiTab* ensureAiPanel(SessionState& state) {
    if (state.aiTab || !g_app.main) return state.aiTab;

    ai::AiTabContext context;
    context.hosts = aiHostList;
    context.vault = &g_app.vault;
    context.ensureVaultUnlocked = []() { return ensureVaultUnlocked(); };
    context.uiFont = g_app.uiFont;
    context.boldFont = g_app.uiFontBold;
    context.monoFont = g_app.font;
    context.captionFont = g_app.tinyFont;
    context.onSettingsSaved = []() {};
    context.onToggleFullScreen = []() { setAiPanelFull(!g_app.aiFull); };
    context.onClose = []() { toggleAiPanel(); };

    state.aiTab = ai::AiTab::create(g_app.main, std::move(context));
    if (state.aiTab) state.aiTab->setFullScreen(state.aiFull);
    return state.aiTab;
}

void toggleAiPanel() {
    if (g_app.aiOpen) {
        g_app.aiOpen = false;
        if (g_app.aiTab) g_app.aiTab->show(false);
    } else {
        if (!ensureAiPanel(g_app)) return;
        g_app.aiOpen = true;
        g_app.aiTab->show(true);
        g_app.aiTab->focus();
    }
    layoutMain(g_app.main);
    InvalidateRect(g_app.main, nullptr, TRUE);
}

void setAiPanelFull(bool full) {
    if (!g_app.aiOpen) return;
    g_app.aiFull = full;
    if (g_app.aiTab) g_app.aiTab->setFullScreen(full);
    layoutMain(g_app.main);
    InvalidateRect(g_app.main, nullptr, TRUE);
}

void disposeSessionState(SessionState& state) {
    for (UINT_PTR timer = kCwdHookTimer1; timer <= kCwdHookTimer4; ++timer)
        KillTimer(g_app.main, state.tabId * 8 + timer);
    if (state.aiTab) {
        // 关掉会话 = 结束这路对话并停掉工作线程
        ai::AiTab::destroy(state.aiTab);
        state.aiTab = nullptr;
    }
    state.connectionId = g_nextConnectionId++;
    for (auto& task : state.downloads) if (task.cancel) task.cancel->store(true);
    state.fs->stop();
    if (!state.connecting) state.shell->close();
    state.currentPassword.clear(); state.currentKeyPassphrase.clear();
}

void closeSessionTab(std::size_t index) {
    if (index >= g_tabs.size()) return;
    auto& state = sessionAt(index);
    const bool transferring = std::any_of(state.downloads.begin(), state.downloads.end(),
        [](const SessionState::DownloadTask& task) { return task.state == SessionState::DownloadState::Active; });
    if (transferring && !messageConfirm(L"关闭标签", L"该标签仍有文件正在传输，关闭会取消这些任务。", L"关闭标签", L"继续传输")) return;
    if (g_tabs.size() == 1) {
        const auto theme = g_app.termData.theme();
        disposeSessionState(g_app);
        static_cast<SessionState&>(g_app) = SessionState();
        g_app.termData.setTheme(theme);
        g_app.tabId = g_nextTabId++;
        g_app.connectionId = g_nextConnectionId++;
        g_selectedTabId = g_app.tabId;
        restoreSessionUi();
        return;
    }
    if (index == g_activeTab) switchSessionTab(index ? index - 1 : index + 1);
    disposeSessionState(*g_tabs[index]);
    g_tabs.erase(g_tabs.begin() + static_cast<std::ptrdiff_t>(index));
    if (g_activeTab > index) --g_activeTab;
    restoreSessionUi();
}

void cycleSessionTab(int direction) {
    if (g_tabs.empty()) return;
    const int count = static_cast<int>(g_tabs.size());
    switchSessionTab(static_cast<std::size_t>((static_cast<int>(g_activeTab) + direction + count) % count));
}

bool handleSessionTabClick(POINT pt) {
    if (!PtInRect(&g_app.tabsRect, pt)) return false;
    bool close = false;
    const int index = hitSessionTab(pt, &close);
    if (index >= 0) {
        if (close) closeSessionTab(static_cast<std::size_t>(index));
        else switchSessionTab(static_cast<std::size_t>(index));
        return true;
    }
    RECT client{}; GetClientRect(g_app.main, &client);
    const auto layout = sessionTabsLayout(client.right);
    if (PtInRect(&layout.add, pt)) { newSessionTab(); openSessionManager(); }
    else if (PtInRect(&layout.previous, pt)) g_app.firstVisibleTab = std::max(0, g_app.firstVisibleTab - 1);
    else if (PtInRect(&layout.next, pt))
        g_app.firstVisibleTab = std::min(std::max(0, static_cast<int>(g_tabs.size()) - layout.capacity), g_app.firstVisibleTab + 1);
    InvalidateRect(g_app.main, nullptr, FALSE);
    return true;
}

LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool sessionMessage = msg == WM_SHELL_DATA || msg == WM_SHELL_CLOSED ||
        msg == WM_CONNECT_OK || msg == WM_CONNECT_FAIL || msg == WM_FS_EVENT || msg == WM_FS_STATS;
    SessionState* target = sessionMessage ? findSessionState(wp, true) : findSessionState(g_selectedTabId, false);
    if (msg == WM_TIMER && wp >= 8) {
        target = findSessionState(wp / 8, false);
        if (!target) return 0;
        wp %= 8;
    }
    if (sessionMessage && (!target || g_applicationClosing.load())) {
        if (msg == WM_SHELL_DATA || msg == WM_SHELL_CLOSED || msg == WM_CONNECT_FAIL)
            delete reinterpret_cast<std::string*>(lp);
        else if (msg == WM_FS_EVENT) delete reinterpret_cast<FsEvent*>(lp);
        else if (msg == WM_FS_STATS) delete reinterpret_cast<RemoteStats*>(lp);
        return 0;
    }
    SessionStateScope context(target);
    switch (msg) {
        case WM_CREATE: {
            g_app.main = hwnd;
            g_applicationClosing.store(false);
            initializeSessionTabs();
            g_app.dpi = wincompat::windowDpi(hwnd);

            g_app.list = CreateWindowExW(
                0, kListClass, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)), g_app.inst, nullptr);

            g_app.term = CreateWindowExW(0, kTermClass, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                         0, 0, 10, 10, hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TERM)),
                                         g_app.inst, nullptr);

            createFonts();
            SetTimer(hwnd, 1, 530, nullptr);
            layoutMain(hwnd);
            return 0;
        }
        case WM_NCCALCSIZE: {
            // 客户区铺满整个窗口，标题栏由 paintTitleBar 自己画；
            // 最大化时向内收边框宽度，否则内容会被屏幕边缘/任务栏切掉。
            // 注意 wp==FALSE 也要返回 0：窗口首次创建时系统只发这一轮，
            // 若交给 DefWindowProc，就会一直保留系统标题栏。
            if (wp) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                if (IsZoomed(hwnd)) {
                    const UINT dpi = static_cast<UINT>(g_app.dpi > 0 ? g_app.dpi : 96);
                    const int frameX = wincompat::systemMetric(SM_CXSIZEFRAME, dpi) +
                                       wincompat::systemMetric(SM_CXPADDEDBORDER, dpi);
                    const int frameY = wincompat::systemMetric(SM_CYSIZEFRAME, dpi) +
                                       wincompat::systemMetric(SM_CXPADDEDBORDER, dpi);
                    params->rgrc[0].left += frameX;
                    params->rgrc[0].right -= frameX;
                    params->rgrc[0].top += frameY;
                    params->rgrc[0].bottom -= frameY;
                }
            }
            return 0;
        }
        case WM_NCHITTEST: {
            POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &client);

            // 自绘按钮区域交回客户区处理
            if (hitTitleButton(client) >= 0 || hitButton(client) >= 0) return HTCLIENT;

            // 去掉非客户区后，缩放边缘要自己判定
            if (!IsZoomed(hwnd)) {
                const int border = dp(6);
                RECT rc;
                GetClientRect(hwnd, &rc);
                const bool left = client.x < border;
                const bool right = client.x >= rc.right - border;
                const bool top = client.y < border;
                const bool bottom = client.y >= rc.bottom - border;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
            }

            if (client.y >= 0 && client.y < g_app.titleH) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paintMain(hwnd);
            return 0;
        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            hideStatusHint();
            if (msg == WM_LBUTTONDOWN && handleSessionTabClick(pt)) return 0;
            if (msg == WM_MOUSEMOVE && PtInRect(&g_app.tabsRect, pt)) InvalidateRect(hwnd, &g_app.tabsRect, FALSE);
            RECT client;
            GetClientRect(hwnd, &client);
            const StatusLayout status = statusGeometry(client);
            const int statusAction = PtInRect(&status.reconnect, pt) ? 1 :
                                    (PtInRect(&status.dismiss, pt) ? 2 : 0);
            const int button = hitButton(pt);
            const int titleButton = hitTitleButton(pt);
            // 下载胶囊 / 列表里的悬停状态
            const DownloadHit downloadHit = hitDownloadUi(hwnd, pt);
            const bool chipHover = downloadHit.chip;
            const int cancelHover = downloadHit.cancelIndex;
            const bool pathHover = g_app.fsReady && PtInRect(&g_app.fsPathRect, pt) != FALSE;
            if (button != g_app.hoverButton || titleButton != g_app.hoverTitleBtn ||
                chipHover != g_app.hoverDownloadChip ||
                cancelHover != g_app.hoverDownloadCancel || pathHover != g_app.hoverPathRect ||
                statusAction != g_app.hoverStatusAction) {
                g_app.hoverStatusAction = statusAction;
                g_app.hoverButton = button;
                g_app.hoverTitleBtn = titleButton;
                g_app.hoverDownloadChip = chipHover;
                g_app.hoverDownloadCancel = cancelHover;
                g_app.hoverPathRect = pathHover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE | (pt.y >= client.bottom - g_app.statusH ? TME_HOVER : 0);
            track.hwndTrack = hwnd;
            track.dwHoverTime = 450;
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MBUTTONUP: {
            const int index = hitSessionTab(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (index >= 0) closeSessionTab(static_cast<std::size_t>(index));
            return 0;
        }
        case WM_MOUSEHOVER:
            showStatusHint(hwnd, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) hideStatusHint();
            break;
        case WM_MOUSELEAVE:
            hideStatusHint();
            if (g_app.hoverStatusAction) {
                g_app.hoverStatusAction = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (g_app.hoverButton != -1 || g_app.hoverTitleBtn != -1 ||
                g_app.hoverDownloadChip || g_app.hoverDownloadCancel != -1 ||
                g_app.hoverPathRect) {
                g_app.hoverButton = -1;
                g_app.hoverTitleBtn = -1;
                g_app.hoverDownloadChip = false;
                g_app.hoverDownloadCancel = -1;
                g_app.hoverPathRect = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            hideStatusHint();
            if (msg == WM_LBUTTONDOWN && handleSessionTabClick(pt)) return 0;
            if (msg == WM_MOUSEMOVE && PtInRect(&g_app.tabsRect, pt)) InvalidateRect(hwnd, &g_app.tabsRect, FALSE);
            RECT client;
            GetClientRect(hwnd, &client);
            const StatusLayout status = statusGeometry(client);
            if (PtInRect(&status.dismiss, pt)) { setStatus(L""); return 0; }
            if (PtInRect(&status.reconnect, pt)) {
                PostMessageW(hwnd, WM_COMMAND, IDM_RECONNECT, 0);
                return 0;
            }
            if (g_app.searchOpen) {
                if (PtInRect(&g_app.searchPrev, pt)) { stepTerminalSearch(-1); return 0; }
                if (PtInRect(&g_app.searchNext, pt)) { stepTerminalSearch(1); return 0; }
                if (PtInRect(&g_app.searchClose, pt)) { showTerminalSearch(false); return 0; }
            }
            if (g_app.fsPathEdit && GetFocus() == g_app.fsPathEdit &&
                !PtInRect(&g_app.fsPathRect, pt)) SetFocus(hwnd);
            // 下载胶囊：展开/收起列表；列表里的取消、清除、关闭
            const DownloadHit downloadHit = hitDownloadUi(hwnd, pt);
            if (downloadHit.chip) {
                g_app.downloadsOpen = !g_app.downloadsOpen;
                syncDownloadLayout();
                return 0;
            }
            if (downloadHit.close) {
                g_app.downloadsOpen = false;
                syncDownloadLayout();
                return 0;
            }
            if (downloadHit.clear) {
                clearFinishedDownloads();
                return 0;
            }
            if (downloadHit.cancelIndex >= 0 &&
                downloadHit.cancelIndex < static_cast<int>(g_app.downloads.size())) {
                cancelDownloadTask(g_app.downloads[static_cast<std::size_t>(
                    downloadHit.cancelIndex)].id);
                return 0;
            }
            if (downloadHit.inPanel) return 0;  // 面板内的点击不要穿到下面
            if (g_app.downloadsOpen) {          // 点面板外面就收起
                g_app.downloadsOpen = false;
                syncDownloadLayout();
            }

            // 路径条：点一下输入新路径（SFTP 没连上时不可编辑）
            if (g_app.fsReady && PtInRect(&g_app.fsPathRect, pt)) {
                focusFsPath();
                return 0;
            }

            const int titleButton = hitTitleButton(pt);
            if (titleButton >= 0) {
                g_app.pressTitleBtn = titleButton;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const int button = hitButton(pt);
            if (button >= 0) {
                if (buttonEnabled(button)) {
                    g_app.pressButton = button;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (g_app.pressTitleBtn >= 0) {
                const int pressed = g_app.pressTitleBtn;
                g_app.pressTitleBtn = -1;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                if (hitTitleButton(pt) == pressed) activateTitleButton(hwnd, pressed);
                return 0;
            }
            if (g_app.pressButton >= 0) {
                const int pressed = g_app.pressButton;
                g_app.pressButton = -1;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                if (hitButton(pt) == pressed) {
                    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(pressed, BN_CLICKED), 0);
                }
                return 0;
            }
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                const DownloadHit downloadHit = hitDownloadUi(hwnd, pt);
                RECT client;
                GetClientRect(hwnd, &client);
                const StatusLayout status = statusGeometry(client);
                if (hitButton(pt) >= 0 || hitTitleButton(pt) >= 0 || downloadHit.chip ||
                    downloadHit.close || downloadHit.clear || downloadHit.cancelIndex >= 0 ||
                    PtInRect(&status.reconnect, pt) || PtInRect(&status.dismiss, pt) ||
                    PtInRect(&g_app.tabsRect, pt) || (g_app.fsReady && PtInRect(&g_app.fsPathRect, pt))) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        }
        case WM_TIMER:
            // 闪烁定时器设在主窗口上（WM_CREATE 里的 SetTimer），之前只在
            // terminalProc 里处理，主窗口把它交给了 DefWindowProc，结果光标
            // 一直亮着不闪。这里统一由主窗口驱动。
            if (wp == 1) {
                expireStatusNotices(GetTickCount64());
                if (g_app.searchDirty) refreshTerminalSearch(false);
                g_app.cursorOn = !g_app.cursorOn;
                if (g_app.term) InvalidateRect(g_app.term, nullptr, FALSE);
            } else if (wp == kCwdHookTimer1) {
                killCwdTimer(kCwdHookTimer1);
                if (!g_app.shell->running()) return 0;
                // 用户已开始输入时不再把初始化命令插进半行命令中。
                if (g_app.lastKeyTick) return 0;
                const bool quiet = g_app.shellGotOutput &&
                    GetTickCount() - g_app.lastShellOutputTick >= 150;
                if (!quiet) {
                    if (++g_app.cwdHookTries < 40) setCwdTimer(kCwdHookTimer1, 150);
                    return 0;
                }
                g_app.cwdHookSnapshot = std::make_unique<Terminal>(g_app.termData);
                g_app.cwdHookStartLine = static_cast<long long>(g_app.termData.scrollbackCount()) +
                    g_app.termData.cursorRow();
                g_app.cwdHookStarted = GetTickCount64();
                g_app.cwdHookDeadline = g_app.cwdHookStarted + 10000;
                g_app.cwdHookPhase = 1;
                g_app.cwdHookAcked = false;
                g_app.holdTermPaint = true;
                if (!g_app.shell->write(cwdHookInput("stty -echo; " + cwdHookAckCommand(1), 1) + "\r")) {
                    abortCwdHook(); return 0;
                }
                setCwdTimer(kCwdHookTimer2, 40);
            } else if (wp == kCwdHookTimer2) {
                killCwdTimer(kCwdHookTimer2);
                if (g_app.cwdHookPhase != 1) return 0;
                if (!g_app.shell->running()) { stopCwdHook(); return 0; }
                if (GetTickCount64() >= g_app.cwdHookDeadline) { abortCwdHook(); return 0; }
                if (!g_app.cwdHookAcked || GetTickCount() - g_app.lastShellOutputTick < 40) {
                    setCwdTimer(kCwdHookTimer2, 40); return 0;
                }
                g_app.cwdHookPhase = 2;
                g_app.cwdHookAcked = false;
                if (!sendCwdHookScript()) { abortCwdHook(); return 0; }
                setCwdTimer(kCwdHookTimer3, 100);
            } else if (wp == kCwdHookTimer3) {
                killCwdTimer(kCwdHookTimer3);
                if (g_app.cwdHookPhase != 2) return 0;
                if (!g_app.shell->running()) { stopCwdHook(); return 0; }
                if (GetTickCount64() >= g_app.cwdHookDeadline) abortCwdHook();
                else setCwdTimer(kCwdHookTimer3, 100);
            } else if (wp == kCwdHookTimer4) {
                killCwdTimer(kCwdHookTimer4);
                if (g_app.cwdHookPhase != 3) return 0;
                if (g_app.cwdHookAcked && GetTickCount() - g_app.lastShellOutputTick >= 40) {
                    finishCwdHook(); return 0;
                }
                if (!g_app.shell->running()) { stopCwdHook(); return 0; }
                if (GetTickCount64() >= g_app.cwdHookDeadline) abortCwdHook();
                else setCwdTimer(kCwdHookTimer4, 40);
            }
            return 0;
        case WM_SIZE:
            layoutMain(hwnd);
            return 0;
        case WM_DPICHANGED: {
            g_app.dpi = HIWORD(wp);
            createFonts();
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layoutMain(hwnd);
            InvalidateRect(g_app.term, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize.x = dp(760);
            info->ptMinTrackSize.y = dp(460);

            // 无边框窗口：最大化尺寸要按工作区 + 边框外扩算，
            // 否则 NCCALCSIZE 内缩后底部会空出一条桌面。
            HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(monitor, &mi)) {
                const UINT dpi = static_cast<UINT>(g_app.dpi > 0 ? g_app.dpi : 96);
                const int frameX = wincompat::systemMetric(SM_CXSIZEFRAME, dpi) +
                                   wincompat::systemMetric(SM_CXPADDEDBORDER, dpi);
                const int frameY = wincompat::systemMetric(SM_CYSIZEFRAME, dpi) +
                                   wincompat::systemMetric(SM_CXPADDEDBORDER, dpi);
                info->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left - frameX;
                info->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top - frameY;
                info->ptMaxSize.x = (mi.rcWork.right - mi.rcWork.left) + frameX * 2;
                info->ptMaxSize.y = (mi.rcWork.bottom - mi.rcWork.top) + frameY * 2;
            }
            return 0;
        }
        case WM_CTLCOLOREDIT:
            if (reinterpret_cast<HWND>(lp) == g_app.fsPathEdit ||
                reinterpret_cast<HWND>(lp) == g_app.searchEdit) {
                HDC dc = reinterpret_cast<HDC>(wp);
                SetTextColor(dc, toColorRef(ui::text));
                SetBkColor(dc, toColorRef(dlg::bgInput));
                return reinterpret_cast<LRESULT>(dlg::editBrush());
            }
            break;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == ID_SEARCH_EDIT && HIWORD(wp) == EN_CHANGE) {
                g_app.searchQuery = textOf(g_app.searchEdit);
                refreshTerminalSearch();
                return 0;
            }
            switch (id) {
                case IDM_NEW_TAB: newSessionTab(); openSessionManager(); return 0;
        case IDM_AI_PANEL: toggleAiPanel(); return 0;
                case IDM_CLOSE_TAB: closeSessionTab(g_activeTab); return 0;
                case IDM_NEXT_TAB: cycleSessionTab(1); return 0;
                case IDM_PREV_TAB: cycleSessionTab(-1); return 0;
                case IDM_SEARCH:
                    showTerminalSearch(true);
                    return 0;
                case IDM_RECONNECT:
                    if (!g_app.connecting && !g_app.shell->running()) {
                        if (selectedSession()) connectStoredSession(g_app.currentSession);
                        else openSessionManager();
                    }
                    return 0;
                case IDM_NEW:
                    newSessionCommand();
                    return 0;
                case IDM_EDIT:
                    editSessionCommand();
                    return 0;
                case IDM_DELETE:
                    deleteSessionCommand();
                    return 0;
                case IDM_VAULT:
                    vaultCommand();
                    return 0;
                case ID_BTN_ACTION:
                case IDM_CONNECT:
                    onConnectCommand();
                    return 0;
                case IDM_OPEN:
                    openSessionManager();
                    return 0;
                case IDM_DISCONNECT:
                    disconnectSession();
                    return 0;
                case ID_BTN_FS_UP:
                    fsGoUp();
                    return 0;
                case IDM_FS_UPLOAD:
                    fsUploadFiles();
                    return 0;
                case IDM_FS_MKDIR:
                    fsMkdirDialog();
                    return 0;
                case IDM_FS_DOWNLOAD:
                    fsOpenEntry(g_app.fsSel);
                    return 0;
                case IDM_FS_DELETE:
                    fsDeleteSelected();
                    return 0;
                case IDM_FS_GOTO:
                    focusFsPath();
                    return 0;
                case IDM_FS_REFRESH:
                    fsRefresh();
                    return 0;
                case IDM_FS_COPY_PATH: {
                    const RemoteEntry* entry = selectedFileEntry();
                    if (!entry) return 0;
                    const std::wstring text = toWide(entry->path);
                    if (copyTextToClipboard(text)) {
                        setStatus(L"已复制路径 " + text);
                    } else {
                        setStatus(L"复制失败：剪贴板正被其它程序占用", true);
                    }
                    return 0;
                }
                case IDM_COPY:
                    doCopy();
                    return 0;
                case IDM_PASTE:
                    doPaste();
                    return 0;
                case IDM_SELECT_ALL:
                    doSelectAll();
                    return 0;
                case IDM_ZOOM_IN:
                    applyFontSize(g_app.fontSize + 2);
                    return 0;
                case IDM_ZOOM_OUT:
                    applyFontSize(g_app.fontSize - 2);
                    return 0;
                case IDM_ZOOM_RESET:
                    applyFontSize(16);
                    return 0;
                case IDM_ABOUT:
                    messageInfo(L"关于 rshell",
                                L"rshell GUI\n\n基于 libssh2 的 SSH 终端与 SFTP 文件管理器。\n"
                                L"会话保存在 ~/.rshell/sessions.ini；勾选「记住密码」的会话，"
                                L"口令存在密码保险库里（%LOCALAPPDATA%\\rshell\\vault.dat）。\n"
                                L"主菜单的「密码保险库」用主密码加密保存口令（本机支持时可用 TPM 解锁）；"
                                L"不勾「记住密码」时口令只留在内存里，断开即失效。\n\n"
                                L"快捷键：Ctrl+Shift+C 复制，Ctrl+V 粘贴，"
                                L"Ctrl++ / Ctrl+- 缩放字体。");
                    return 0;
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    break;
            }
            return 0;
        }
        case WM_SHELL_DATA: {
            std::unique_ptr<std::string> payload(reinterpret_cast<std::string*>(lp));
            if (wp != g_app.connectionId) return 0;
            if (payload) {
                if (std::ofstream* dump = terminalDump()) {
                    dump->write(payload->data(), static_cast<std::streamsize>(payload->size()));
                    dump->flush();
                }
                g_app.shellGotOutput = true;
                g_app.lastShellOutputTick = GetTickCount();
                g_app.termData.feed(*payload);
                for (const auto& ack : g_app.termData.takeShellIntegrationAcks()) handleCwdHookAck(ack);
                if (!foregroundSession()) { g_app.unread = true; InvalidateRect(hwnd, &g_app.tabsRect, FALSE); }
                g_app.searchDirty = g_app.searchOpen;
                flushTerminalResponse();
                // shell 的 OSC 7 钩子会在每次画提示符时把 $PWD 报回来，
                // 目录变了就带着文件面板一起走（单向：面板不反向影响终端）
                if (g_app.termData.takeCwdChange()) syncTerminalCwd();
                InvalidateRect(g_app.term, nullptr, FALSE);
                if (foregroundSession() && !g_app.termData.title().empty()) {
                    SetWindowTextW(hwnd,
                                   (L"rshell - " + toWide(g_app.currentSession) + L" - " +
                                    toWide(g_app.termData.title()))
                                       .c_str());
                }
            }
            return 0;
        }
        case WM_CONNECT_OK: {
            if (wp != g_app.connectionId) return 0;
            g_app.connecting = false;
            if (!g_app.shell->running()) { layoutMain(hwnd); updateButtons(); return 0; }
            layoutMain(hwnd);
            layoutTerminal(g_app.term);
            std::wstring status = L"已连接 " + toWide(g_app.currentSession);
            if (!g_app.shell->fingerprint().empty()) {
                status += L"  ·  " + toWide(g_app.shell->kex());
            }

            // g_app.currentPassword 平时是密文；这里解出一份明文，供写凭据和启动
            // 文件通道共用，本分支结束前抹掉
            std::wstring plainPassword = g_app.currentPassword.reveal();

            // 勾了「记住密码」：连接成功之后才把口令交给密码保险库，
            // 这样被服务器拒绝的口令不会被存下来
            if (!g_app.pendingCredentialTarget.empty()) {
                const Session* pending = g_app.store.find(g_app.pendingCredentialSession);
                std::string error;
                // 临时 UTF-8 副本用完就抹掉，别在堆里留明文
                std::string plain = toUtf8(plainPassword);
                const bool stored = saveCredential(g_app.pendingCredentialTarget, plain, &error);
                wipeString(plain);
                if (stored) {
                    if (pending) {
                        Session copy = *pending;
                        copy.savePassword = true;
                        g_app.store.upsert(copy);
                        g_app.store.save();
                    }
                    status += L"  ·  密码已存入密码保险库";
                } else {
                    guiLog("保存密码失败：" + error);
                    if (foregroundSession()) messageWarn(L"密码没有保存",
                                L"连接已经建立，但口令没能写进 " + activePasswordBackend() +
                                    L"。\n\n" + toWide(error));
                }
                g_app.pendingCredentialTarget.clear();
                g_app.pendingCredentialSession.clear();
            }

            std::wstring plainKeyPassphrase = g_app.currentKeyPassphrase.reveal();
            if (!g_app.pendingKeyCredentialTarget.empty()) {
                const Session* pending = g_app.store.find(g_app.pendingKeyCredentialSession);
                std::string error;
                std::string plain = toUtf8(plainKeyPassphrase);
                const bool stored =
                    saveCredential(g_app.pendingKeyCredentialTarget, plain, &error);
                wipeString(plain);
                if (stored) {
                    if (pending) {
                        Session copy = *pending;
                        copy.saveKeyPassphrase = true;
                        g_app.store.upsert(copy);
                        g_app.store.save();
                    }
                    status += L"  ·  私钥口令已存入密码保险库";
                } else {
                    guiLog("保存私钥口令失败：" + error);
                    if (foregroundSession()) messageWarn(L"私钥口令没有保存",
                                L"连接已经建立，但口令没能写进 " + activePasswordBackend() +
                                    L"。\n\n" + toWide(error));
                }
                g_app.pendingKeyCredentialTarget.clear();
                g_app.pendingKeyCredentialSession.clear();
            }

            setStatus(status);
            if (foregroundSession()) SetWindowTextW(hwnd, (L"rshell - " + toWide(g_app.currentSession)).c_str());
            if (foregroundSession()) SetFocus(g_app.term);
            updateButtons();
            InvalidateRect(g_app.term, nullptr, FALSE);

            // 左侧切到远端文件树，并开始采集机器状态
            if (const Session* session = g_app.store.find(g_app.currentSession)) {
                startRemoteFs(*session, plainPassword, plainKeyPassphrase);
            }

            // 等 rc / motd 输出得差不多了，再往 shell 里注入 OSC 7 钩子，
            // 让文件面板能跟着终端里的 cd 走（RSHELL_NO_CWD_SYNC=1 可关闭）
            stopCwdHook();
            if (!std::getenv("RSHELL_NO_CWD_SYNC")) {
                setCwdTimer(kCwdHookTimer1, 250);
            }
            wipeWideString(plainPassword);
            wipeWideString(plainKeyPassphrase);
            return 0;
        }
        case WM_CONNECT_FAIL: {
            std::unique_ptr<std::string> payload(reinterpret_cast<std::string*>(lp));
            if (wp != g_app.connectionId) return 0;
            stopCwdHook();
            g_app.connecting = false;
            layoutMain(hwnd);
            // 连接都没成功，别把这次输入的口令存进密码保险库
            g_app.pendingCredentialTarget.clear();
            g_app.pendingCredentialSession.clear();
            g_app.pendingKeyCredentialTarget.clear();
            g_app.pendingKeyCredentialSession.clear();
            g_app.currentPassword.clear();
            g_app.currentKeyPassphrase.clear();
            stopRemoteFs();
            updateButtons();
            const std::wstring detail = payload ? toWide(*payload) : L"未知错误";
            // 状态栏只放第一行，完整说明（例如握手失败的算法提示）留在对话框和日志里。
            std::wstring statusText = detail;
            if (const size_t lineEnd = statusText.find(L'\n'); lineEnd != std::wstring::npos) {
                statusText.resize(lineEnd);
            }
            setStatus(L"连接失败：" + statusText, true);
            if (foregroundSession()) messageError(L"连接失败", L"无法建立 SSH 连接，请检查地址、端口和认证信息。", detail);
            return 0;
        }
        case WM_SHELL_CLOSED: {
            std::unique_ptr<std::string> reason(reinterpret_cast<std::string*>(lp));
            if (wp != g_app.connectionId) return 0;
            // 会话结束：注入钩子的定时器可能还在排队，先撤掉
            stopCwdHook();
            g_app.connecting = false;
            {
                const std::string detail = reason && !reason->empty() ? *reason : "连接已断开";
                g_app.termData.feed("\r\n[" + detail + "]\r\n");
                g_app.searchDirty = g_app.searchOpen;
                InvalidateRect(g_app.term, nullptr, FALSE);
                setStatus(toWide(detail), detail.rfind("远端会话已结束", 0) != 0);
                guiLog("会话结束：" + detail);
                if (foregroundSession()) SetWindowTextW(hwnd, L"rshell");
                stopRemoteFs();
                g_app.currentPassword.clear();
                g_app.currentKeyPassphrase.clear();
            }
            layoutMain(hwnd);
            updateButtons();
            return 0;
        }
        case WM_VAULT_TPM_DONE: {
            std::unique_ptr<std::shared_ptr<TpmAttempt>> payload(
                reinterpret_cast<std::shared_ptr<TpmAttempt>*>(lp));
            if (!payload || !*payload) return 0;
            const std::shared_ptr<TpmAttempt> attempt = *payload;
            // 用户已经点了「改用主密码」：这次结果直接丢掉（Vault 析构会清密钥）
            if (attempt->cancelled.load(std::memory_order_acquire)) return 0;
            // 与工作线程的 ready.store(release) 配对：之后读 ok/error/vault 才安全
            (void)attempt->ready.load(std::memory_order_acquire);

            if (attempt->ok) {
                g_app.vault = std::move(attempt->vault);
                g_vaultPrompt.ok = true;
                g_vaultPrompt.viaTpm = true;
                g_vaultPrompt.busy = false;
                if (g_vaultPrompt.dlg) vaultPromptClose(g_vaultPrompt.dlg);
            } else if (g_vaultPrompt.dlg) {
                // TPM 不可用/密钥失效：留在同一个对话框里，直接输入主密码
                vaultPromptSetBusy(g_vaultPrompt.dlg, false, L"");
                std::wstring note =
                    toWide(attempt->error.empty() ? "TPM 解锁失败" : attempt->error);
                note += L"；请输入主密码";
                vaultPromptSetNote(g_vaultPrompt.dlg, note, true);
                if (g_vaultPrompt.password) {
                    SetFocus(g_vaultPrompt.password);
                    SendMessageW(g_vaultPrompt.password, EM_SETSEL, 0, -1);
                }
            }
            if (g_tpmAttempt == attempt) g_tpmAttempt.reset();
            return 0;
        }
        case WM_FS_EVENT: {
            std::unique_ptr<FsEvent> event(reinterpret_cast<FsEvent*>(lp));
            if (wp != 0 && wp != g_app.connectionId) return 0;
            if (event) handleFsEvent(*event);
            return 0;
        }
        case WM_FS_STATS: {
            std::unique_ptr<RemoteStats> stats(reinterpret_cast<RemoteStats*>(lp));
            if (wp != 0 && wp != g_app.connectionId) return 0;
            if (stats) {
                g_app.stats = *stats;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_ASK_HOSTKEY: {
            auto* request = reinterpret_cast<HostKeyRequest*>(lp);
            if (request && !g_applicationClosing.load() &&
                (!request->connectionId || findSessionState(request->connectionId, true))) {
                request->accepted =
                    runHostKeyDialog(request->host, request->keyType, request->fingerprint);
            }
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            g_applicationClosing.store(true);
            for (auto& state : g_tabs) if (state) disposeSessionState(*state);
            disposeSessionState(g_app);
            g_tabs.clear();
            g_app.downloads.clear();
            setCredentialVault(nullptr);  // 进程退出前断开后端，别留悬空指针
            g_app.vault.lock();           // 退出前把 DEK 抹掉，别留在进程内存里
            destroyBackBuffer();
            if (g_app.font) DeleteObject(g_app.font);
            if (g_app.fontBold) DeleteObject(g_app.fontBold);
            if (g_app.uiFont) DeleteObject(g_app.uiFont);
            if (g_app.uiFontBold) DeleteObject(g_app.uiFontBold);
            if (g_app.titleFont) DeleteObject(g_app.titleFont);
            if (g_app.tinyFont) DeleteObject(g_app.tinyFont);
            g_app.font = nullptr;
            g_app.fontBold = nullptr;
            g_app.uiFont = nullptr;
            g_app.uiFontBold = nullptr;
            g_app.titleFont = nullptr;
            g_app.tinyFont = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void applyDarkFrame(HWND hwnd) {
    const BOOL dark = TRUE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE；老版本 Windows 10 用 19
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }
    // Win11：系统圆角 + 描边色，让无边框窗口和系统阴影自然衔接
    const int corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
    const COLORREF borderColor = toColorRef(ui::border);
    DwmSetWindowAttribute(hwnd, 34, &borderColor, sizeof(borderColor));
}

// ---------------------------------------------------------------- 进程加固
//
// 目标：抬高同账户下其它程序"顺手读取"口令的成本，而不是筑一道墙——
// 同用户进程依然可以 OpenProcess + ReadProcessMemory，真正需要"人"参与的秘密
// 只能靠主密码或 Windows Hello。
//
// 全部是尽力而为：策略在当前系统上不支持时 SetProcessMitigationPolicy 会失败，
// 这里忽略返回值，不影响启动。
void hardenProcess() {
    const auto setPolicy = wincompat::api().setMitigation;
    if (!setPolicy) return;  // Win7 不提供这些策略；不影响主密码保险库加密。
    // 不启用 ProcessExtensionPointDisablePolicy：它会阻断自绘终端使用的
    // IMM/TSF 兼容输入路径，导致微软拼音在终端中失效（原生 EDIT 仍可用）。

    // 1) 禁止本进程生成/切换可执行内存：shellcode 注入后没法直接跑起来。
    //    该策略生效后不可撤销。rshell 不依赖 JIT，实测启动和渲染正常。
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode{};
    dynamicCode.ProhibitDynamicCode = 1;   // 不允许 VirtualAlloc(PAGE_EXECUTE_*) / 改可执行属性
    dynamicCode.AllowThreadOptOut = 0;     // 不允许单线程退出该策略
    dynamicCode.AllowRemoteDowngrade = 0;  // 不允许调试器远程降级
    setPolicy(ProcessDynamicCodePolicy, &dynamicCode, sizeof(dynamicCode));

    // 2) 不从不信任的位置加载 DLL：网络路径、低完整性级别目录
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad{};
    imageLoad.NoRemoteImages = 1;
    imageLoad.NoLowMandatoryLabelImages = 1;
    setPolicy(ProcessImageLoadPolicy, &imageLoad, sizeof(imageLoad));

    // 3) 只允许微软签名的二进制：默认关闭。
    //    GUI 会经 COM 加载第三方 shell 扩展（文件对话框、缩略图、杀软 hook），
    //    打开后可能让"另存为/打开"直接失败，所以做成显式开关：
    //      set RSHELL_ENFORCE_MS_SIGNED=1
    if (const char* enforce = std::getenv("RSHELL_ENFORCE_MS_SIGNED")) {
        if (enforce[0] == '1') {
            PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature{};
            signature.MicrosoftSignedOnly = 1;
            setPolicy(ProcessSignaturePolicy, &signature, sizeof(signature));
        }
    }
}

int runGui(HINSTANCE inst, int showCommand) {
    g_app.inst = inst;
    // 在加载任何第三方组件（GDI+、OLE）之前先加固
    hardenProcess();
    // 高 DPI：按监视器 DPI 渲染，避免系统位图缩放导致文字发虚
    wincompat::initializeDpi();
    g_app.dpi = wincompat::systemDpi();
    startGdiplus();
    // 拖出下载要用 OLE 拖放（DoDragDrop）
    const bool oleReady = SUCCEEDED(OleInitialize(nullptr));

    // 离屏渲染：rshell-gui.exe --render <dump> <out.bmp> <cols> <rows>
    //           rshell-gui.exe --icons <out.bmp>   （文件类型图标总览，调图标用）
    // 不起窗口、不连 SSH，直接用同一份绘制代码出图，便于核对渲染结果
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            if (argc >= 3 && std::wstring(argv[1]) == L"--icons") {
                const bool ok = renderIconSheet(argv[2]);
                LocalFree(argv);
                stopGdiplus();
                return ok ? 0 : 2;
            }
            if (argc >= 3 && std::wstring(argv[1]) == L"--ai-paths") {
                const int w = argc >= 4 ? _wtoi(argv[3]) : 460;
                const int h = argc >= 5 ? _wtoi(argv[4]) : 780;
                const bool ok = ai::renderPreview(argv[2], w > 0 ? w : 460, h > 0 ? h : 780, 2);
                LocalFree(argv);
                stopGdiplus();
                return ok ? 0 : 2;
            }
            if (argc >= 3 && std::wstring(argv[1]) == L"--ai-welcome") {
                const int w = argc >= 4 ? _wtoi(argv[3]) : 440;
                const int h = argc >= 5 ? _wtoi(argv[4]) : 780;
                const bool ok = ai::renderPreview(argv[2], w > 0 ? w : 440, h > 0 ? h : 780, 1);
                LocalFree(argv);
                stopGdiplus();
                return ok ? 0 : 2;
            }
            if (argc >= 3 && std::wstring(argv[1]) == L"--ai-preview") {
                // rshell-gui.exe --ai-preview <out.bmp> [宽] [高]
                // 用与真实面板相同的绘制代码渲染一段演示对话，便于核对排版
                const int w = argc >= 4 ? _wtoi(argv[3]) : 440;
                const int h = argc >= 5 ? _wtoi(argv[4]) : 780;
                const bool ok = ai::renderPreview(argv[2], w > 0 ? w : 440, h > 0 ? h : 780);
                LocalFree(argv);
                stopGdiplus();
                return ok ? 0 : 2;
            }
            if (argc >= 5 && std::wstring(argv[1]) == L"--render") {
                const int cols = _wtoi(argv[4]);
                const int rows = argc >= 6 ? _wtoi(argv[5]) : 0;
                const bool ok = renderDumpToBmp(argv[2], argv[3], cols > 0 ? cols : 80,
                                                rows > 0 ? rows : 24);
                LocalFree(argv);
                stopGdiplus();
                return ok ? 0 : 2;
            }
            LocalFree(argv);
        }
    }
    // 终端配色与界面的深蓝灰底色保持一致，
    // 默认的 xterm 纯色在深色 UI 里显得刺眼。
    {
        TerminalTheme theme;
        theme.fg = 0xD8E0EF;
        theme.bg = 0x10141D;
        theme.cursor = 0x98A5FF;
        theme.selection = ui::bgActive;
        const std::uint32_t palette[16] = {
            0x161B22, 0xFF7B72, 0x7EE787, 0xD29922, 0x79C0FF, 0xD2A8FF, 0x56D4DD, 0xC9D1D9,
            0x6E7681, 0xFFA198, 0xA5E3A5, 0xE3B341, 0xA5D6FF, 0xE2C5FF, 0x9FE8EF, 0xF0F6FC,
        };
        for (int i = 0; i < 16; ++i) theme.palette[i] = palette[i];
        g_app.termData.setTheme(theme);
    }

    // libssh2 全局初始化（含 Winsock 启动）必须覆盖所有会话的生命周期
    auto libssh2Runtime = std::make_shared<Libssh2Runtime>();
    g_app.runtime = libssh2Runtime;
    g_app.store = SessionStore::load();
    g_app.store.seedFromConfigIfEmpty();
    if (!g_app.store.loaded() && !g_app.store.sessions().empty()) g_app.store.save();

    // 应用图标：一份 ico 里带 16~256 多档，按用途各取一张（窗口图标大、任务栏小）
    g_app.appIcon = static_cast<HICON>(
        LoadImageW(inst, MAKEINTRESOURCEW(IDI_RSHELL), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                   GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    g_app.appIconSmall = static_cast<HICON>(
        LoadImageW(inst, MAKEINTRESOURCEW(IDI_RSHELL), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!g_app.appIcon) g_app.appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!g_app.appIconSmall) g_app.appIconSmall = g_app.appIcon;

    WNDCLASSEXW listClass{};
    listClass.cbSize = sizeof(listClass);
    listClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    listClass.lpfnWndProc = fileListProc;
    listClass.hInstance = inst;
    listClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    listClass.hbrBackground = nullptr;
    listClass.hIcon = g_app.appIcon;
    listClass.hIconSm = g_app.appIconSmall;
    listClass.lpszClassName = kListClass;
    if (!RegisterClassExW(&listClass)) return 1;

    WNDCLASSEXW termClass{};
    termClass.cbSize = sizeof(termClass);
    termClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    termClass.lpfnWndProc = terminalProc;
    termClass.hInstance = inst;
    termClass.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    termClass.hbrBackground = nullptr;
    termClass.hIcon = g_app.appIcon;
    termClass.hIconSm = g_app.appIconSmall;
    termClass.lpszClassName = kTermClass;
    if (!RegisterClassExW(&termClass)) return 1;

    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.style = CS_HREDRAW | CS_VREDRAW;
    mainClass.lpfnWndProc = mainProc;
    mainClass.hInstance = inst;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    mainClass.hIcon = g_app.appIcon;
    mainClass.hIconSm = g_app.appIconSmall;
    mainClass.lpszClassName = kMainClass;
    if (!RegisterClassExW(&mainClass)) return 1;

    // 初始尺寸随工作区收敛：小屏（1024x768）上 1120x720 会被系统夹住，
    // 客户区尺寸在启动时抖动还会让终端多做一次 resize。
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int initW = std::min(dp(1120), std::max(dp(760), static_cast<int>(work.right - work.left) -
                                                             dp(40)));
    const int initH = std::min(dp(720), std::max(dp(460), static_cast<int>(work.bottom - work.top) -
                                                            dp(40)));
    HWND window = CreateWindowExW(0, kMainClass, L"rshell - SSH 终端",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                  CW_USEDEFAULT, initW, initH, nullptr, nullptr, inst, nullptr);
    if (!window) return 1;
    applyDarkFrame(window);

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    // 布局定稿前会经历几次临时 resize（10x10 → 极小 → 正常），
    // 被顶出的空白行会写进回滚历史，表现为一启动就有“历史”和滚动条。
    g_app.termData.reset();

    // 调试开关：RSHELL_GUI_DEMO=1 时灌一段样例输出，方便离线检查配色/字体/边框对齐
    if (std::getenv("RSHELL_GUI_DEMO")) {
        std::string demo;
        demo += "\x1b[1;32mroot\x1b[0m@\x1b[1;36mrshell\x1b[0m:\x1b[1;34m~/projects/rshell\x1b[0m$ ls\r\n";
        demo += "\x1b[1;34msrc\x1b[0m  \x1b[1;34mtests\x1b[0m  \x1b[32mCMakeLists.txt\x1b[0m  \x1b[33mREADME.md\x1b[0m\r\n";
        demo += "\x1b[1;32mroot\x1b[0m@\x1b[1;36mrshell\x1b[0m:\x1b[1;34m~/projects/rshell\x1b[0m$ ";
        demo += "\x1b[1;35m❯\x1b[0m echo \"中文 / 全角对齐 / TUI 边框\"\r\n";
        demo += "中文 / 全角对齐 / TUI 边框\r\n";
        demo += "\x1b[90m╭──────────────────────────────╮\x1b[0m\r\n";
        demo += "\x1b[90m│\x1b[0m \x1b[1;32m✓\x1b[0m 连接成功   "
                "\x1b[1;33m●\x1b[0m 2 个待更新   \x1b[1;31m✗\x1b[0m 0 失败 \x1b[90m│\x1b[0m\r\n";
        demo += "\x1b[90m╰──────────────────────────────╯\x1b[0m\r\n";
        demo += "\x1b[1;31mred\x1b[0m \x1b[1;32mgreen\x1b[0m \x1b[1;33myellow\x1b[0m "
                "\x1b[1;34mblue\x1b[0m \x1b[1;35mmagenta\x1b[0m \x1b[1;36mcyan\x1b[0m "
                "\x1b[1;37mwhite\x1b[0m\r\n";
        demo += "\x1b[2mdim\x1b[0m \x1b[4munderline\x1b[0m \x1b[7mreverse\x1b[0m\r\n";
        demo += "\x1b[1;32mroot\x1b[0m@\x1b[1;36mrshell\x1b[0m:\x1b[1;34m~/projects/rshell\x1b[0m$ ";
        g_app.termData.feed(demo);
        g_app.currentSession = "demo";
        InvalidateRect(g_app.term, nullptr, FALSE);

        // 顺带造一份文件面板与状态栏的假数据，方便离线检查布局
            g_app.fsReady = true;
        g_app.fsPath = "/home/root/projects/rshell";
        auto addEntry = [](const char* name, std::uint64_t size, bool dir, bool link = false) {
            RemoteEntry e;
            e.name = name;
            e.path = std::string("/home/root/projects/rshell/") + name;
            e.size = size;
            e.mode = link ? (LIBSSH2_SFTP_S_IFLNK | 0777)
                          : (dir ? (LIBSSH2_SFTP_S_IFDIR | 0755)
                                 : (LIBSSH2_SFTP_S_IFREG | 0644));
            e.mtime = 1730000000;
            g_app.fsEntries.push_back(std::move(e));
        };
        addEntry("src", 0, true);
        addEntry("tests", 0, true);
        addEntry("build", 0, true);
        addEntry("CMakeLists.txt", 4121, false);
        addEntry("README.md", 9842, false);
        addEntry("rshell-gui.exe", 442368, false);
        addEntry("deploy.sh", 1024, false);
        addEntry("release-notes.txt", 65536, false);
        // 下面几条纯粹为了在演示模式里把各类图标都摊开看一眼
        addEntry("main.cpp", 15200, false);
        addEntry("libterm.so", 98304, false);
        addEntry("screenshot.png", 2411522, false);
        addEntry("demo-recording.mp4", 14680064, false);
        addEntry("theme.mp3", 3355443, false);
        addEntry("backup-2024.tar.gz", 88604672, false);
        addEntry("metrics.csv", 120884, false);
        addEntry("manual.pdf", 1048576, false);
        addEntry("latest", 0, false, true);
        g_app.fsSel = 3;

        g_app.stats.valid = true;
        g_app.stats.host = "rshell";
        g_app.stats.os = "Ubuntu 24.04 LTS";
        g_app.stats.cores = 8;
        g_app.stats.load1 = 0.42;
        g_app.stats.load5 = 0.31;
        g_app.stats.load15 = 0.25;
        g_app.stats.cpuPercent = 12.0;
        g_app.stats.memTotalKb = 8ULL * 1024 * 1024;
        g_app.stats.memAvailKb = 5ULL * 1024 * 1024;
        g_app.stats.diskTotalKb = 40ULL * 1024 * 1024;
        g_app.stats.diskUsedKb = 17ULL * 1024 * 1024;
        g_app.stats.latencyMs = 23;

        // 下载列表：一条进行中、一条已完成，方便离线核对状态栏胶囊和列表排版
        addDownloadTask("/home/root/projects/rshell/demo-recording.mp4",
                        L"demo-recording.mp4", 14680064, false,
                        std::make_shared<std::atomic<bool>>(false));
        addDownloadTask("/home/root/projects/rshell/backup-2024.tar.gz",
                        L"backup-2024.tar.gz", 88604672, true,
                        std::make_shared<std::atomic<bool>>(false));
        if (g_app.downloads.size() == 2) {
            g_app.downloads[0].done = 14680064 * 47 / 100;
            g_app.downloads[1].done = 88604672;
            g_app.downloads[1].state = App::DownloadState::Done;
        }
        g_app.downloadsOpen = true;
        layoutMain(window);
    }

    updateButtons();
    setStatus(L"就绪：点「打开会话列表…」选择主机");

    ACCEL accels[] = {{static_cast<BYTE>(FVIRTKEY | FCONTROL), static_cast<WORD>('N'), IDM_NEW},
                      {static_cast<BYTE>(FVIRTKEY | FCONTROL), static_cast<WORD>('O'), IDM_OPEN}};
    HACCEL accel = CreateAcceleratorTableW(accels, 2);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) &&
            !(GetKeyState(VK_MENU) & 0x8000) &&
            (msg.hwnd == window || IsChild(window, msg.hwnd))) {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            int command = msg.wParam == 'T' ? IDM_NEW_TAB :
                (msg.wParam == VK_TAB ? (shift ? IDM_PREV_TAB : IDM_NEXT_TAB) :
                 (msg.wParam == 'W' && shift ? IDM_CLOSE_TAB : 0));
            if (command) { SendMessageW(window, WM_COMMAND, command, 0); continue; }
        }
        if (accel && TranslateAcceleratorW(window, accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (accel) DestroyAcceleratorTable(accel);
    if (g_app.appIcon) DestroyIcon(g_app.appIcon);
    if (g_app.appIconSmall && g_app.appIconSmall != g_app.appIcon) DestroyIcon(g_app.appIconSmall);
    g_app.appIcon = nullptr;
    g_app.appIconSmall = nullptr;
    if (oleReady) OleUninitialize();
    stopGdiplus();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int showCommand) {
    return runGui(inst, showCommand);
}






