// ai_tab.cpp
#include "ai_tab.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <random>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "ai_agent.h"
#include "ai_client.h"
#include "ai_settings.h"
#include "ai_ssh_backend.h"
#include "ai_tools.h"
#include "remote_fs.h"
#include "ui_theme.h"
#include "util.h"
#include "vault.h"

#include <imm.h>
#include <windowsx.h>

namespace rshell::ai {

namespace {

constexpr wchar_t kAiClass[] = L"RshellAiTab";
constexpr UINT kAiEventMessage = WM_APP + 61;
constexpr UINT kAiFlushMessage = WM_APP + 62;
constexpr UINT kAiModelsMessage = WM_APP + 63;
constexpr UINT_PTR kCaretTimer = 11;
constexpr UINT_PTR kRepaintTimer = 12;
constexpr int kRepaintIntervalMs = 40;  // 流式刷新上限约 25fps

// 设置视图的固定排版（绘制与建控件共用）
constexpr int kProviderTop = 54;
constexpr int kProviderH = 28;
constexpr int kFieldTop = 100;
constexpr int kFieldGap = 66;
constexpr int kEditOffset = 24;
constexpr int kEditHeight = 30;
constexpr int kFetchW = 88;
constexpr int kButtonTop = kFieldTop + kFieldGap * 3 + 4;
constexpr int kButtonHeight = 34;

struct ModelFetchPayload {
    std::vector<std::string> ids;
    std::string error;
};

// 每个对话一个稳定的会话 ID（OpenCode Go 用来做路由与提示缓存）
std::string makeSessionId() {
    std::random_device device;
    std::mt19937_64 generator(device());
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 4; ++i) {
        std::uint64_t value = generator();
        for (int j = 0; j < 16; ++j) {
            out += hex[value & 0xF];
            value >>= 4;
        }
    }
    return out;
}

ApiConfig makeApiConfig(const AiSettings& settings, const std::string& key,
                        const std::string& sessionId) {
    ApiConfig config;
    config.baseUrl = settings.baseUrl;
    config.apiKey = key;
    config.model = settings.model;
    config.userAgent = "rshell-gui/1.2";
    // 只有 OpenCode Go 需要这个头；其它服务商发了也不会被接受方理解
    if (config.baseUrl.find("opencode.ai") != std::string::npos) config.sessionId = sessionId;
    return config;
}

std::wstring toWideText(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string toUtf8Text(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length,
                        nullptr, nullptr);
    return out;
}

int textWidthOf(HDC dc, const std::wstring& text, HFONT font) {
    if (text.empty() || !font) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    return size.cx;
}

void fillRect(HDC dc, const RECT& rc, std::uint32_t color) {
    HBRUSH brush = CreateSolidBrush(toColorRef(color));
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

void drawText(HDC dc, const std::wstring& text, RECT rc, std::uint32_t color, HFONT font,
              UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    if (text.empty() || !font) return;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, toColorRef(color));
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rc, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

// 把当前目标主机的工具调用路由到它的 RemoteFs
class RoutedBackend : public ToolBackend {
public:
    std::function<void*()> resolve;

    CommandResult runCommand(const std::string& command, int timeoutSec) override {
        auto* fs = static_cast<RemoteFs*>(resolve ? resolve() : nullptr);
        if (!fs) return notConnected();
        SshToolBackend backend(*fs);
        return backend.runCommand(command, timeoutSec);
    }

    bool readFile(const std::string& path, std::size_t maxBytes, std::string& out,
                  bool& truncated, std::string& error) override {
        auto* fs = static_cast<RemoteFs*>(resolve ? resolve() : nullptr);
        if (!fs) {
            error = kNotConnected;
            return false;
        }
        SshToolBackend backend(*fs);
        return backend.readFile(path, maxBytes, out, truncated, error);
    }

    bool writeFile(const std::string& path, const std::string& content,
                   std::string& error) override {
        auto* fs = static_cast<RemoteFs*>(resolve ? resolve() : nullptr);
        if (!fs) {
            error = kNotConnected;
            return false;
        }
        SshToolBackend backend(*fs);
        return backend.writeFile(path, content, error);
    }

    bool listDir(const std::string& path, std::vector<DirEntryView>& out,
                 std::string& error) override {
        auto* fs = static_cast<RemoteFs*>(resolve ? resolve() : nullptr);
        if (!fs) {
            error = kNotConnected;
            return false;
        }
        SshToolBackend backend(*fs);
        return backend.listDir(path, out, error);
    }

private:
    static constexpr const char* kNotConnected = "当前没有可用的主机连接（先在会话标签里连上主机）";

    static CommandResult notConnected() {
        CommandResult result;
        result.exitCode = -1;
        result.output = kNotConnected;
        return result;
    }
};

// 工作线程等待界面点击审批
struct ApprovalSlot {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    ApprovalDecision decision = ApprovalDecision::Deny;
};

}  // namespace

// ---------------------------------------------------------------- 实现

struct AiTab::Impl {
    explicit Impl(AiTab* owner) : registry(backend), agent(client, registry) {
        backend.resolve = [owner]() { return owner->currentBackend(); };
    }

    RoutedBackend backend;
    ToolRegistry registry;
    Client client{ApiConfig{}};
    Agent agent;

    std::thread worker;
    std::atomic<bool> cancel{false};
    std::atomic<bool> busy{false};
    bool closing = false;

    std::mutex mutex;
    std::shared_ptr<ApprovalSlot> pending;
    std::shared_ptr<ApprovalSlot> approvalSlot;
    std::size_t approvalIndex = static_cast<std::size_t>(-1);

    std::size_t assistantIndex = static_cast<std::size_t>(-1);
    bool reasoningOpen = false;

    // 流式文本按帧合并：token 先攒在缓冲里，避免每个 token 一次重绘
    std::mutex textMutex;
    std::wstring pendingText;
    std::wstring pendingReasoning;
    std::atomic<bool> flushPosted{false};
    bool repaintPending = false;
    bool imeComposing = false;

    // 双缓冲：直接画到窗口 DC 会先刷底色再描字，流式时肉眼可见闪烁
    HDC backDc = nullptr;
    HBITMAP backBmp = nullptr;
    int backW = 0;
    int backH = 0;

    AiSettings settings;
    std::string apiKey;
    std::string sessionId = makeSessionId();
    bool keyNoticeShown = false;

    // 设置视图
    HWND editBase = nullptr;
    HWND editKey = nullptr;
    HWND modelCombo = nullptr;
    std::string draftProvider;
    std::vector<RECT> providerRects;
    RECT fetchRect{};
    bool fetching = false;
    std::string fetchStatus;
    RECT saveRect{};
    RECT cancelRect{};
};

AiTab* AiTab::create(HWND parent, AiTabContext context) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &AiTab::wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kAiClass;
        if (!RegisterClassExW(&wc)) return nullptr;
        registered = true;
    }

    auto* tab = new AiTab();
    tab->parent_ = parent;
    tab->context_ = std::move(context);
    tab->impl_ = std::make_unique<Impl>(tab);

    tab->window_ = CreateWindowExW(0, kAiClass, L"AI", WS_CHILD | WS_CLIPCHILDREN,
                                   0, 0, 100, 100, parent, nullptr,
                                   GetModuleHandleW(nullptr), tab);
    if (!tab->window_) {
        delete tab;
        return nullptr;
    }
    SetWindowLongPtrW(tab->window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tab));
    tab->initialize(parent);
    return tab;
}

void AiTab::destroy(AiTab* tab) {
    if (!tab) return;
    if (tab->impl_) {
        tab->impl_->closing = true;
        tab->impl_->cancel = true;
        {
            std::lock_guard<std::mutex> lock(tab->impl_->mutex);
            if (tab->impl_->pending) {
                std::lock_guard<std::mutex> slotLock(tab->impl_->pending->mutex);
                tab->impl_->pending->decision = ApprovalDecision::Abort;
                tab->impl_->pending->ready = true;
                tab->impl_->pending->cv.notify_all();
            }
        }
        if (tab->impl_->worker.joinable()) tab->impl_->worker.join();
    }
    if (tab->window_) DestroyWindow(tab->window_);  // WM_NCDESTROY 里 delete
}

void AiTab::initialize(HWND) {
    PanelFonts fonts;
    fonts.ui = context_.uiFont;
    fonts.bold = context_.boldFont;
    fonts.mono = context_.monoFont;
    fonts.caption = context_.captionFont;
    panel_.setFonts(fonts);

    PanelActions actions;
    actions.onSend = [this](const std::wstring& text) { sendMessage(text); };
    actions.onCancel = [this]() { cancelTurn(); };
    actions.onSettings = [this]() { startSettings(false); };
    actions.onWelcome = [this]() { startSettings(true); };
    actions.onClose = [this]() {
        if (context_.onClose) context_.onClose();
    };
    actions.onClear = [this]() {
        if (impl_->busy) return;
        impl_->agent.reset();
        panel_.clear();
        impl_->assistantIndex = static_cast<std::size_t>(-1);
        updateWelcome();
    };
    actions.onHost = [this]() { cycleHost(); };
    actions.onToggleFullScreen = [this]() {
        if (context_.onToggleFullScreen) context_.onToggleFullScreen();
    };
    actions.onApproval = [this](std::size_t index, bool allowed) {
        onPanelApproval(index, allowed);
    };
    panel_.setCallbacks(std::move(actions));

    impl_->settings = loadAiSettings();
    impl_->client.setConfig(makeApiConfig(impl_->settings, std::string(), impl_->sessionId));
    panel_.setModelName(toWideText(impl_->settings.model));

    // Agent 的流式回调 → 界面线程事件
    AgentCallbacks callbacks;
    callbacks.onText = [this](const std::string& chunk) {
        if (!chunk.empty()) postEvent(EventKind::Text, toWideText(chunk));
    };
    callbacks.onReasoning = [this](const std::string& chunk) {
        if (!chunk.empty()) postEvent(EventKind::Reasoning, toWideText(chunk));
    };
    callbacks.onToolFinish = [this](const ToolRecord& record) {
        // title=摘要，meta=短标记，body=完整说明（展开时看）
        postEvent(EventKind::Tool, toWideText(record.summary), toWideText(record.display),
                  toWideText(record.brief), record.ok && !record.denied);
    };
    impl_->agent.setCallbacks(std::move(callbacks));

    if (context_.vault) loadApiKey(*context_.vault, impl_->apiKey);
    updateHostLabel();
    updateWelcome();  // 空状态里已经带「打开设置」，不再重复加一条提示

    SetTimer(window_, kCaretTimer, 500, nullptr);
}

void AiTab::setFullScreen(bool full) {
    panel_.setFullScreen(full);
    InvalidateRect(window_, nullptr, FALSE);
}

void AiTab::setBounds(const RECT& bounds) {
    if (!window_) return;
    MoveWindow(window_, bounds.left, bounds.top, bounds.right - bounds.left,
               bounds.bottom - bounds.top, TRUE);
    RECT client{};
    GetClientRect(window_, &client);
    panel_.setBounds(client);
    if (settingsOpen_) layoutSettings();
    InvalidateRect(window_, nullptr, FALSE);
}

void AiTab::show(bool visible) {
    if (!window_) return;
    if (visible) updateHostLabel();  // 会话可能刚连上
    ShowWindow(window_, visible ? SW_SHOWNA : SW_HIDE);
}

void AiTab::focus() {
    if (!window_) return;
    SetFocus(window_);
    panel_.focusInput();
}

// ---------------------------------------------------------------- 主机

void AiTab::updateHostLabel() {
    const auto hosts = context_.hosts ? context_.hosts() : std::vector<AiHostInfo>();
    const bool clickable = hosts.size() > 1;
    panel_.setHostLabel(currentHostLabel(), clickable);
}

std::wstring AiTab::currentHostLabel() const {
    if (!context_.hosts) return std::wstring();
    const auto hosts = context_.hosts();
    if (hosts.empty()) return L"未连接主机";
    const int index = std::clamp(hostIndex_, 0, static_cast<int>(hosts.size()) - 1);
    return hosts[static_cast<std::size_t>(index)].label;
}

void* AiTab::currentBackend() const {
    if (!context_.hosts) return nullptr;
    const auto hosts = context_.hosts();
    if (hosts.empty()) return nullptr;
    const int index = std::clamp(hostIndex_, 0, static_cast<int>(hosts.size()) - 1);
    return hosts[static_cast<std::size_t>(index)].backend;
}

void AiTab::cycleHost() {
    if (!context_.hosts) return;
    const auto hosts = context_.hosts();
    if (hosts.size() < 2) return;
    hostIndex_ = (hostIndex_ + 1) % static_cast<int>(hosts.size());
    updateHostLabel();
    panel_.addNotice(L"目标主机已切换到 " + currentHostLabel());
    InvalidateRect(window_, nullptr, FALSE);
}

// ---------------------------------------------------------------- 发送 / 取消

void AiTab::updateKeyNotice() {
    if (!impl_->apiKey.empty()) return;
    if (impl_->keyNoticeShown) return;
    impl_->keyNoticeShown = true;
    panel_.addNotice(L"还没有配置 API Key。点击右上角「设置」填写，密钥会保存到本机主密码保险库。");
}

// 本机有保险库文件、并且当前锁着——只有这种情况才值得弹解锁框
bool AiTab::vaultLocked() const {
    return context_.vault && !context_.vault->unlocked() &&
           (!context_.vaultPresent || context_.vaultPresent());
}

void AiTab::updateWelcome() {
    if (impl_->apiKey.empty()) {
        if (vaultLocked()) {
            // Key 可能已经存好了，只是保险库锁着：不误导成「没配置」
            panel_.setWelcome(L"保险库已锁定",
                              L"API Key 保存在本机保险库。点「解锁」输入主密码后即可直接提问。", true,
                              L"解锁");
        } else {
            panel_.setWelcome(
                L"AI 助手",
                L"还没有可用的 API Key。点「打开设置」填写，密钥保存在本机主密码保险库。", true);
        }
    } else {
        panel_.setWelcome(L"AI 助手",
                         L"可以让我查看磁盘、内存、日志，或修改配置。写操作会在执行前请你确认。",
                         false);
    }
}

void AiTab::sendMessage(const std::wstring& text) {
    if (impl_->busy || text.empty()) return;
    if (impl_->apiKey.empty() && vaultLocked() && context_.ensureVaultUnlocked) {
        if (context_.ensureVaultUnlocked() && context_.vault) loadApiKey(*context_.vault, impl_->apiKey);
    }
    if (impl_->apiKey.empty()) {
        updateKeyNotice();
        updateWelcome();
        // 保险库锁着时刚才已经弹过解锁框了，用户取消就停在这里，别再弹第二次、
        // 也别把他丢进设置表单；没有保险库才直接进设置填 Key。
        if (!vaultLocked()) startSettings(true);
        return;
    }
    if (!currentBackend()) {
        panel_.addNotice(L"当前没有可用的主机连接：先在会话标签里连上一台主机，"
                         L"文件面板就绪后 AI 才能读取和执行命令。",
                         true);
        return;
    }

    panel_.addUser(text);
    impl_->agent.addUserMessage(toUtf8Text(text));
    impl_->agent.setSystemPrompt(defaultSystemPrompt() + "\n当前目标主机：" +
                                 toUtf8Text(currentHostLabel()));
    impl_->agent.setApproval([this](const ApprovalRequest& request) {
        auto slot = std::make_shared<ApprovalSlot>();
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->pending = slot;
        }
        postEvent(EventKind::Approval, toWideText(request.summary),
                  toWideText(request.detail),
                  L"[" + currentHostLabel() + L"] " + toWideText(request.toolName),
                  !request.dangerous, slot);

        std::unique_lock<std::mutex> lock(slot->mutex);
        slot->cv.wait(lock, [&]() { return slot->ready; });
        return slot->decision;
    });

    impl_->client.setConfig(makeApiConfig(impl_->settings, impl_->apiKey, impl_->sessionId));
    impl_->busy = true;
    impl_->cancel = false;
    impl_->assistantIndex = static_cast<std::size_t>(-1);
    panel_.setBusy(true);
    panel_.setStatus(L"生成中…");

    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->worker = std::thread([this]() {
        const AgentTurn turn = impl_->agent.run([this]() { return impl_->cancel.load(); });

        std::wstring status;
        bool ok = true;
        if (turn.cancelled) {
            status = L"已停止";
            ok = false;
        } else if (turn.aborted) {
            status = L"已终止任务";
            ok = false;
        } else if (!turn.error.empty()) {
            status = toWideText(turn.error);
            ok = false;
        } else {
            status = L"就绪";
        }
        postEvent(EventKind::TurnDone, status, L"", L"", ok);
    });
}

void AiTab::cancelTurn() {
    if (!impl_->busy) return;
    impl_->cancel = true;

    // 正卡在审批上时，Agent 阻塞在条件变量里，光置 cancel 不会生效；
    // 用 Abort 把它唤醒，才能真的停下来。
    std::shared_ptr<ApprovalSlot> slot;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->pending;
        impl_->pending.reset();
    }
    if (slot) {
        {
            std::lock_guard<std::mutex> lock(slot->mutex);
            slot->decision = ApprovalDecision::Abort;
            slot->ready = true;
        }
        slot->cv.notify_all();
        if (impl_->approvalIndex != static_cast<std::size_t>(-1)) {
            panel_.resolveApproval(impl_->approvalIndex, false);
            impl_->approvalIndex = static_cast<std::size_t>(-1);
        }
    }
    panel_.setStatus(L"正在停止…");
    InvalidateRect(window_, nullptr, FALSE);
}

// ---------------------------------------------------------------- 事件回投

// 自绘输入框没有窗口类帮我们定位，输入法候选窗默认会飘到左上角。
// 每次开始合成时把合成窗与候选窗钉在光标下方。
void AiTab::positionIme() {
    if (!window_) return;
    HIMC context = ImmGetContext(window_);
    if (!context) return;

    RECT caret = panel_.caretRect();
    // 光标下沿再留 2px，候选窗不会压住输入框边框
    POINT anchor{caret.left, caret.bottom + 2};

    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = anchor;
    ImmSetCompositionWindow(context, &composition);

    // 合成串字体必须与界面一致：默认字体更高更宽，候选窗会跟着整体偏移
    if (context_.uiFont) {
        LOGFONTW font{};
        if (GetObjectW(context_.uiFont, sizeof(font), &font))
            ImmSetCompositionFontW(context, &font);
    }

    // CFS_EXCLUDE 告诉输入法「这块别挡」，它会在附近自己挑位置；
    // 只给 CFS_CANDIDATEPOS 时部分输入法会当成绝对左上角，反而贴住光标。
    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = anchor;
    candidate.rcArea = RECT{caret.left, caret.top, caret.left + 2, caret.bottom + 2};
    ImmSetCandidateWindow(context, &candidate);

    ImmReleaseContext(window_, context);
}

void AiTab::requestRepaint() {
    if (impl_->repaintPending) return;
    impl_->repaintPending = true;
    SetTimer(window_, kRepaintTimer, kRepaintIntervalMs, nullptr);
}

void AiTab::postEvent(EventKind kind, std::wstring a, std::wstring b, std::wstring c, bool ok,
                      std::shared_ptr<void> payload) {
    // 文本/思考增量先入缓冲，攒够一帧再交给界面，避免每个 token 一次 PostMessage + 重绘
    if (kind == EventKind::Text || kind == EventKind::Reasoning) {
        {
            std::lock_guard<std::mutex> lock(impl_->textMutex);
            (kind == EventKind::Text ? impl_->pendingText : impl_->pendingReasoning) += a;
        }
        if (!impl_->flushPosted.exchange(true))
            PostMessageW(window_, kAiFlushMessage, 0, 0);
        return;
    }

    auto* event = new Event();
    event->kind = kind;
    event->a = std::move(a);
    event->b = std::move(b);
    event->c = std::move(c);
    event->ok = ok;
    event->payload = std::move(payload);
    PostMessageW(window_, kAiEventMessage, 0, reinterpret_cast<LPARAM>(event));
}

void AiTab::handleEvent(Event& event) {
    const std::size_t none = static_cast<std::size_t>(-1);

    switch (event.kind) {
        case EventKind::Text: {
            if (impl_->assistantIndex == none) impl_->assistantIndex = panel_.beginAssistant();
            panel_.appendAssistant(impl_->assistantIndex, event.a);
            requestRepaint();
            return;
        }
        case EventKind::Reasoning: {
            if (impl_->assistantIndex != none) {
                panel_.finishAssistant(impl_->assistantIndex, true);
                impl_->assistantIndex = none;
            }
            panel_.appendReasoning(event.a);
            requestRepaint();
            return;
        }
        case EventKind::Tool: {
            if (impl_->assistantIndex != none) {
                panel_.finishAssistant(impl_->assistantIndex, true);
                impl_->assistantIndex = none;
            }
            panel_.addTool(event.a, event.b, event.ok, event.c);
            requestRepaint();
            return;
        }
        case EventKind::Approval: {
            if (impl_->assistantIndex != none) {
                panel_.finishAssistant(impl_->assistantIndex, true);
                impl_->assistantIndex = none;
            }
            // postEvent 的 ok 位在这里表示「不危险」
            const std::size_t index = panel_.addApproval(event.a, event.b, !event.ok);
            impl_->approvalIndex = index;
            impl_->approvalSlot = std::static_pointer_cast<ApprovalSlot>(event.payload);
            panel_.setStatus(L"等待确认");
            requestRepaint();
            return;
        }
        case EventKind::TurnDone: {
            if (impl_->assistantIndex != none) {
                panel_.finishAssistant(impl_->assistantIndex, event.ok);
                impl_->assistantIndex = none;
            }
            impl_->busy = false;
            panel_.setBusy(false);
            panel_.setStatus(event.a);
            if (!event.ok && !event.a.empty()) panel_.addNotice(event.a, true);
            requestRepaint();
            return;
        }
        case EventKind::AssistantBegin: return;
    }
}

void AiTab::onPanelApproval(std::size_t index, bool allowed) {
    std::shared_ptr<ApprovalSlot> slot;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->pending;
        impl_->pending.reset();
    }
    if (!slot) return;
    panel_.setStatus(allowed ? L"已允许" : L"已拒绝");
    {
        std::lock_guard<std::mutex> lock(slot->mutex);
        slot->decision = allowed ? ApprovalDecision::Allow : ApprovalDecision::Deny;
        slot->ready = true;
    }
    slot->cv.notify_all();
    (void)index;
}

// ---------------------------------------------------------------- 设置视图

void AiTab::startSettings(bool unlockOnly) {
    if (settingsOpen_) return;
    settingsOpen_ = true;

    bool unlockedNow = false;
    if (impl_->apiKey.empty() && vaultLocked() && context_.ensureVaultUnlocked) {
        // 用户点了「取消」或解锁失败：留在空状态，绝不能跳进设置表单
        // （以前这里忽略返回值，取消后照样打开设置页）
        if (!context_.ensureVaultUnlocked()) {
            settingsOpen_ = false;
            updateWelcome();
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (context_.vault) {
            std::string stored;
            if (loadApiKey(*context_.vault, stored)) {
                impl_->apiKey = stored;
                unlockedNow = true;
            }
        }
    }
    updateWelcome();  // 解锁后可能刚读到已保存的 Key

    // 从空状态点「解锁」进来的：Key 已读到就直接结束，不弹设置表单
    if (unlockOnly && unlockedNow) {
        settingsOpen_ = false;
        panel_.addNotice(L"保险库已解锁，可以直接提问了");
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    createSettingsControls();
    impl_->draftProvider = impl_->settings.provider;
    impl_->fetchStatus.clear();
    SetWindowTextW(impl_->editBase, toWideText(impl_->settings.baseUrl).c_str());
    SetWindowTextW(impl_->editKey, L"");
    SetWindowTextW(impl_->modelCombo, toWideText(impl_->settings.model).c_str());

    layoutSettings();
    HWND controls[] = {impl_->editBase, impl_->editKey, impl_->modelCombo};
    for (HWND control : controls)
        if (control) ShowWindow(control, SW_SHOW);
    SetFocus(impl_->editBase);
    InvalidateRect(window_, nullptr, TRUE);
}

void AiTab::createSettingsControls() {
    auto makeEdit = [this](DWORD style) {
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | style, 0, 0, 10, 10,
                                    window_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (edit && context_.uiFont)
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(context_.uiFont), TRUE);
        return edit;
    };
    if (!impl_->editBase) impl_->editBase = makeEdit(0);
    if (!impl_->editKey) impl_->editKey = makeEdit(ES_PASSWORD);
    if (!impl_->modelCombo) {
        impl_->modelCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWN |
                                                CBS_AUTOHSCROLL,
                                            0, 0, 10, 240, window_, nullptr,
                                            GetModuleHandleW(nullptr), nullptr);
        if (impl_->modelCombo && context_.uiFont)
            SendMessageW(impl_->modelCombo, WM_SETFONT,
                         reinterpret_cast<WPARAM>(context_.uiFont), TRUE);
    }
}

void AiTab::layoutSettings() {
    if (!impl_->editBase) return;
    RECT client{};
    GetClientRect(window_, &client);
    const int left = 24;
    const int width = std::max(120, static_cast<int>(client.right) - left * 2);

    // 预设 chips：自定义 + 内置三家
    impl_->providerRects.clear();
    int chipX = left;
    const std::wstring labels[] = {L"自定义", L"OpenCode Go", L"Command Code", L"Cline"};
    HDC dc = GetDC(window_);
    for (const std::wstring& label : labels) {
        const int chipW = textWidthOf(dc, label, context_.uiFont) + 20;
        impl_->providerRects.push_back(RECT{chipX, kProviderTop, chipX + chipW,
                                            kProviderTop + kProviderH});
        chipX += chipW + 6;
    }
    ReleaseDC(window_, dc);

    MoveWindow(impl_->editBase, left, kFieldTop + kEditOffset, width, kEditHeight, TRUE);
    MoveWindow(impl_->editKey, left, kFieldTop + kFieldGap + kEditOffset, width, kEditHeight,
               TRUE);
    const int comboTop = kFieldTop + kFieldGap * 2 + kEditOffset;
    impl_->fetchRect = RECT{left + width - kFetchW, comboTop, left + width,
                            comboTop + kEditHeight};
    MoveWindow(impl_->modelCombo, left, comboTop, width - kFetchW - 8, 240, TRUE);

    impl_->saveRect = RECT{left, kButtonTop, left + 96, kButtonTop + kButtonHeight};
    impl_->cancelRect = RECT{left + 108, kButtonTop, left + 108 + 96,
                             kButtonTop + kButtonHeight};
}

void AiTab::startModelFetch() {
    if (impl_->fetching) return;

    auto readText = [](HWND handle) {
        wchar_t buffer[2048]{};
        GetWindowTextW(handle, buffer, 2048);
        return toUtf8Text(buffer);
    };

    const std::string base = trim(readText(impl_->editBase));
    std::string key = trim(readText(impl_->editKey));
    if (key.empty()) key = impl_->apiKey;
    if (base.empty()) {
        impl_->fetchStatus = "先填 API 地址";
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (key.empty()) {
        impl_->fetchStatus = "先填 API Key";
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    AiSettings draft = impl_->settings;
    draft.provider = impl_->draftProvider;
    draft.baseUrl = base;
    const std::string url = aiModelsUrl(draft);

    ApiConfig config;
    config.baseUrl = base;
    config.apiKey = key;
    config.userAgent = "rshell-gui/1.2";
    if (base.find("opencode.ai") != std::string::npos) config.sessionId = impl_->sessionId;

    impl_->fetching = true;
    impl_->fetchStatus = "正在获取模型列表…";
    InvalidateRect(window_, nullptr, FALSE);

    // 走工作线程，避免网络慢时把界面卡住；窗口关掉后 PostMessage 会被忽略
    HWND target = window_;
    std::thread([target, url, config]() {
        ModelCatalog catalog = listModels(config, url);
        auto* payload = new ModelFetchPayload{std::move(catalog.ids), catalog.error};
        PostMessageW(target, kAiModelsMessage, 0, reinterpret_cast<LPARAM>(payload));
    }).detach();
}

void AiTab::finishSettings(bool save) {
    if (!settingsOpen_) return;
    auto readText = [](HWND handle) {
        wchar_t buffer[2048]{};
        GetWindowTextW(handle, buffer, 2048);
        return toUtf8Text(buffer);
    };

    if (save) {
        AiSettings settings = impl_->settings;
        settings.baseUrl = trim(readText(impl_->editBase));
        settings.provider = impl_->draftProvider;
        settings.model = trim(readText(impl_->modelCombo));
        if (settings.baseUrl.empty() || settings.model.empty()) {
            panel_.addNotice(L"API 地址和模型都不能为空", true);
            return;
        }
        std::string error;
        if (!saveAiSettings(settings, &error)) {
            panel_.addNotice(toWideText(error), true);
            return;
        }
        const std::string key = trim(readText(impl_->editKey));
        if (!key.empty()) {
            if (!context_.vault || !saveApiKey(*context_.vault, key, &error)) {
                panel_.addNotice(toWideText(error.empty() ? "保险库不可用" : error), true);
                return;
            }
            if (!context_.vault->save(&error)) {
                panel_.addNotice(toWideText(error), true);
                return;
            }
            impl_->apiKey = key;
            impl_->keyNoticeShown = false;
        }

        impl_->settings = settings;
        if (context_.vault) {
            std::string stored;
            if (loadApiKey(*context_.vault, stored)) impl_->apiKey = stored;
        }
        panel_.setModelName(toWideText(settings.model));
        impl_->client.setConfig(makeApiConfig(settings, impl_->apiKey, impl_->sessionId));
        panel_.setStatus(L"就绪");
        // 关键：密钥/模型变了，空状态文案要跟着变，否则还会显示「没有配置 API Key」
        updateWelcome();
        updateHostLabel();
        panel_.addNotice(L"设置已保存");
        if (context_.onSettingsSaved) context_.onSettingsSaved();
    }

    settingsOpen_ = false;
    ShowWindow(impl_->editBase, SW_HIDE);
    ShowWindow(impl_->editKey, SW_HIDE);
    ShowWindow(impl_->modelCombo, SW_HIDE);
    SetFocus(window_);
    panel_.focusInput();
    InvalidateRect(window_, nullptr, TRUE);
}

// ---------------------------------------------------------------- 窗口过程

LRESULT CALLBACK AiTab::wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* tab = reinterpret_cast<AiTab*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        tab = static_cast<AiTab*>(create->lpCreateParams);
        if (tab) tab->window_ = hwnd;  // 创建期就可能收到 WM_SIZE/WM_PAINT
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tab));
    }
    if (tab) return tab->handle(message, wParam, lParam);
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT AiTab::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SIZE: {
            RECT client{};
            GetClientRect(window_, &client);
            panel_.setBounds(client);
            if (settingsOpen_) layoutSettings();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }

        case kAiEventMessage: {
            std::unique_ptr<Event> event(reinterpret_cast<Event*>(lParam));
            if (event) handleEvent(*event);
            return 0;
        }

        case kAiFlushMessage: {
            std::wstring text;
            std::wstring reasoning;
            {
                std::lock_guard<std::mutex> lock(impl_->textMutex);
                text.swap(impl_->pendingText);
                reasoning.swap(impl_->pendingReasoning);
            }
            impl_->flushPosted = false;

            const std::size_t none = static_cast<std::size_t>(-1);
            if (!reasoning.empty()) {
                if (impl_->assistantIndex != none) {
                    panel_.finishAssistant(impl_->assistantIndex, true);
                    impl_->assistantIndex = none;
                }
                panel_.appendReasoning(reasoning);
            }
            if (!text.empty()) {
                if (impl_->assistantIndex == none) impl_->assistantIndex = panel_.beginAssistant();
                panel_.appendAssistant(impl_->assistantIndex, text);
            }
            requestRepaint();
            return 0;
        }

        case kAiModelsMessage: {
            std::unique_ptr<ModelFetchPayload> payload(
                reinterpret_cast<ModelFetchPayload*>(lParam));
            impl_->fetching = false;
            if (!payload) return 0;

            if (payload->ids.empty()) {
                impl_->fetchStatus =
                    payload->error.empty() ? "没有取到模型" : payload->error;
            } else {
                wchar_t current[512]{};
                GetWindowTextW(impl_->modelCombo, current, 512);
                SendMessageW(impl_->modelCombo, CB_RESETCONTENT, 0, 0);
                for (const std::string& id : payload->ids) {
                    const std::wstring wide = toWideText(id);
                    SendMessageW(impl_->modelCombo, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(wide.c_str()));
                }
                // 保留用户原来填的模型（列表里没有就补一条）
                if (current[0] != L'\0') {
                    if (SendMessageW(impl_->modelCombo, CB_FINDSTRINGEXACT,
                                     static_cast<WPARAM>(-1),
                                     reinterpret_cast<LPARAM>(current)) < 0) {
                        SendMessageW(impl_->modelCombo, CB_ADDSTRING, 0,
                                     reinterpret_cast<LPARAM>(current));
                    }
                    SetWindowTextW(impl_->modelCombo, current);
                } else {
                    SetWindowTextW(impl_->modelCombo,
                                   toWideText(payload->ids.front()).c_str());
                }
                impl_->fetchStatus = "已获取 " + std::to_string(payload->ids.size()) + " 个模型";
            }
            InvalidateRect(window_, nullptr, TRUE);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window_, &ps);
            RECT client{};
            GetClientRect(window_, &client);
            const int width = std::max(1, static_cast<int>(client.right));
            const int height = std::max(1, static_cast<int>(client.bottom));

            // 双缓冲：流式刷新时如果直接画窗口 DC，会先刷底色再描字，肉眼可见闪烁
            if (!impl_->backDc) impl_->backDc = CreateCompatibleDC(dc);
            if (!impl_->backBmp || impl_->backW != width || impl_->backH != height) {
                if (impl_->backBmp) DeleteObject(impl_->backBmp);
                impl_->backBmp = CreateCompatibleBitmap(dc, width, height);
                impl_->backW = width;
                impl_->backH = height;
            }
            if (impl_->backDc && impl_->backBmp) {
                HGDIOBJ old = SelectObject(impl_->backDc, impl_->backBmp);
                paint(impl_->backDc);
                BitBlt(dc, 0, 0, width, height, impl_->backDc, 0, 0, SRCCOPY);
                SelectObject(impl_->backDc, old);
            } else {
                paint(dc);
            }
            EndPaint(window_, &ps);
            return 0;
        }

        case WM_ERASEBKGND: return 1;

        case WM_SETFOCUS:
            panel_.focusInput();
            // 立刻重绘一次拿到光标位置：输入法合成窗要按它定位
            InvalidateRect(window_, nullptr, FALSE);
            UpdateWindow(window_);
            positionIme();
            return 0;

        case WM_IME_STARTCOMPOSITION:
            impl_->imeComposing = true;
            panel_.clearPreedit();
            positionIme();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;

        case WM_IME_COMPOSITION: {
            // 组字串自己画：自绘输入框没有 EDIT 代劳，只靠输入法的组字窗口时，
            // 那些只开候选窗的输入法会让用户完全看不到已经打出来的拼音。
            HIMC context = ImmGetContext(window_);
            if (context) {
                if (lParam & GCS_COMPSTR) {
                    std::wstring text;
                    const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
                    if (bytes > 0) {
                        text.resize(static_cast<std::size_t>(bytes) / sizeof(wchar_t));
                        ImmGetCompositionStringW(context, GCS_COMPSTR, text.data(), bytes);
                        while (!text.empty() && text.back() == L'\0') text.pop_back();
                    }
                    // GCS_CURSORPOS：低字是字符下标，只取低字
                    const LONG caret =
                        ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
                    panel_.setPreedit(std::move(text), static_cast<int>(caret & 0xFFFF));
                }
                // 结论串还是得自己取出来上屏
                if (lParam & GCS_RESULTSTR) {
                    const LONG bytes =
                        ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
                    if (bytes > 0) {
                        std::wstring text(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
                        ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), bytes);
                        for (const wchar_t ch : text) panel_.onChar(ch);
                    }
                    panel_.clearPreedit();
                }
                ImmReleaseContext(window_, context);
            }
            InvalidateRect(window_, nullptr, FALSE);
            // 先画一帧再定位：caretRect_ 是绘制时算出来的，否则还是上一帧的位置
            UpdateWindow(window_);
            positionIme();
            return 0;
        }

        case WM_IME_ENDCOMPOSITION:
            impl_->imeComposing = false;
            panel_.clearPreedit();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;

        case WM_IME_SETCONTEXT:
            // 组字串自绘，让输入法别再开一个组字窗口压住输入框
            lParam &= static_cast<LPARAM>(~ISC_SHOWUICOMPOSITIONWINDOW);
            return DefWindowProcW(window_, message, wParam, lParam);

        case WM_IME_NOTIFY:
            // 候选窗刚打开时再钉一次位置，有的输入法在这一刻才读取位置
            if (wParam == IMN_OPENCANDIDATE) positionIme();
            return DefWindowProcW(window_, message, wParam, lParam);

        case WM_KILLFOCUS:
            panel_.blurInput();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;

        case WM_TIMER:
            if (wParam == kRepaintTimer) {
                KillTimer(window_, kRepaintTimer);
                impl_->repaintPending = false;
                InvalidateRect(window_, nullptr, FALSE);
            } else if (wParam == kCaretTimer && panel_.inputFocused()) {
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEMOVE: {
            if (settingsOpen_) return 0;
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (panel_.onMouseMove(pt)) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetFocus(window_);
            if (settingsOpen_) return 0;
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            panel_.onMouseDown(pt);
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (settingsOpen_) {
                // 服务商预设
                const auto& presets = aiProviderPresets();
                for (std::size_t i = 0; i < impl_->providerRects.size(); ++i) {
                    if (!PtInRect(&impl_->providerRects[i], pt)) continue;
                    if (i == 0) {
                        impl_->draftProvider.clear();
                    } else if (i - 1 < presets.size()) {
                        impl_->draftProvider = presets[i - 1].id;
                        SetWindowTextW(impl_->editBase,
                                       toWideText(presets[i - 1].baseUrl).c_str());
                    }
                    impl_->fetchStatus.clear();
                    InvalidateRect(window_, nullptr, FALSE);
                    return 0;
                }
                if (PtInRect(&impl_->fetchRect, pt)) {
                    startModelFetch();
                    return 0;
                }
                if (PtInRect(&impl_->saveRect, pt)) {
                    finishSettings(true);
                    return 0;
                }
                if (PtInRect(&impl_->cancelRect, pt)) {
                    finishSettings(false);
                    return 0;
                }
                return 0;
            }
            if (panel_.onMouseUp(pt)) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (settingsOpen_) return 0;
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (panel_.onMouseWheel(delta)) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            // 合成期间按键归输入法，别让面板把拼音当命令键处理
            if (settingsOpen_ || impl_->imeComposing)
                return DefWindowProcW(window_, message, wParam, lParam);
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (panel_.onKeyDown(static_cast<UINT>(wParam), shift, ctrl)) {
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            return 0;
        }

        case WM_CHAR: {
            if (settingsOpen_ || impl_->imeComposing)
                return DefWindowProcW(window_, message, wParam, lParam);
            const wchar_t ch = static_cast<wchar_t>(wParam);
            if (panel_.onChar(ch)) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }

        case WM_NCDESTROY: {
            KillTimer(window_, kCaretTimer);
            KillTimer(window_, kRepaintTimer);
            if (impl_ && impl_->backBmp) DeleteObject(impl_->backBmp);
            if (impl_ && impl_->backDc) DeleteDC(impl_->backDc);
            SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            delete this;
            return 0;
        }

        default: return DefWindowProcW(window_, message, wParam, lParam);
    }
}

void AiTab::paint(HDC dc) {
    if (!settingsOpen_) {
        panel_.paint(dc);
        return;
    }

    RECT client{};
    GetClientRect(window_, &client);
    fillRect(dc, client, ui::bgApp);

    RECT header{client.left, client.top, client.right, client.top + 42};
    fillRect(dc, header, ui::bgPanel);
    drawText(dc, L"AI 设置", RECT{24, header.top, header.right - 24, header.bottom}, ui::text,
             context_.boldFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const int left = 24;
    const int width = std::max(120, static_cast<int>(client.right) - left * 2);

    // 服务商预设：选一个就把地址填好，只差 Key
    drawText(dc, L"服务商", RECT{left, kProviderTop - 22, left + width, kProviderTop - 2},
             ui::textDim, context_.captionFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const auto& presets = aiProviderPresets();
    const std::wstring chipLabels[] = {L"自定义", L"OpenCode Go", L"Command Code", L"Cline"};
    for (std::size_t i = 0; i < impl_->providerRects.size(); ++i) {
        const bool selected =
            (i == 0) ? impl_->draftProvider.empty()
                     : (i - 1 < presets.size() && impl_->draftProvider == presets[i - 1].id);
        const RECT& chip = impl_->providerRects[i];
        HBRUSH fill = CreateSolidBrush(toColorRef(selected ? ui::bgActive : ui::bgCard));
        FillRect(dc, &chip, fill);
        DeleteObject(fill);
        HBRUSH border = CreateSolidBrush(toColorRef(selected ? ui::accent : ui::borderSoft));
        FrameRect(dc, &chip, border);
        DeleteObject(border);
        drawText(dc, chipLabels[i], chip, selected ? ui::text : ui::textDim,
                 context_.captionFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    const wchar_t* labels[3] = {
        L"API 地址（baseURL）",
        L"API Key（保存在主密码保险库；留空表示不修改）",
        L"模型（可先点「获取模型」再选）",
    };
    for (int i = 0; i < 3; ++i) {
        const int top = kFieldTop + i * kFieldGap;
        drawText(dc, labels[i], RECT{left, top, left + width, top + 20}, ui::textDim,
                 context_.captionFont,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    auto button = [&](const RECT& rc, const std::wstring& text, std::uint32_t fill,
                      std::uint32_t color) {
        HBRUSH brush = CreateSolidBrush(toColorRef(fill));
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        drawText(dc, text, rc, color, context_.captionFont,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };
    button(impl_->fetchRect, impl_->fetching ? L"获取中…" : L"获取模型",
           impl_->fetching ? ui::bgCard : ui::bgCard, ui::accent);
    button(impl_->saveRect, L"保存", ui::action, 0xFFFFFF);
    button(impl_->cancelRect, L"取消", ui::bgCard, ui::textDim);

    // 状态/错误 + 使用说明
    const std::wstring hint =
        !impl_->fetchStatus.empty()
            ? toWideText(impl_->fetchStatus)
            : std::wstring(L"选服务商 → 填 API Key → 获取模型 → 保存");
    drawText(dc, hint,
             RECT{left, kButtonTop + kButtonHeight + 12, left + width,
                  kButtonTop + kButtonHeight + 38},
             impl_->fetchStatus.empty() ? ui::textFaint : ui::accent, context_.captionFont,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

}  // namespace rshell::ai
