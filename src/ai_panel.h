// ai_panel.h - AI 对话面板（GDI 自绘，不创建子窗口）
//
// 面板只负责“显示 + 收集输入”，不碰网络、不碰 SSH：
//   - 宿主把工作线程的事件转成这里的 add*/append* 调用（在 UI 线程执行）
//   - 用户点按钮/发送时通过 PanelActions 回调给宿主
// 审批卡片是内联的，不弹模态框，避免打断终端输入。
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rshell::ai {

enum class ItemKind {
    User,
    Assistant,
    Reasoning,
    Tool,
    Approval,
    Notice,
    Error,
};

struct ChatItem {
    ItemKind kind = ItemKind::Notice;
    std::wstring title;
    std::wstring body;
    std::wstring meta;      // 右侧小字：大小、退出码、耗时
    bool streaming = false; // 正在流式输出，标题旁画一个光标
    bool expandable = false;
    bool expanded = false;
    bool danger = false;    // 审批卡片标红（命中危险命令）
    bool ok = true;
    bool awaitApproval = false;
    bool decided = false;
    bool approved = false;
};

struct PanelFonts {
    HFONT ui = nullptr;
    HFONT bold = nullptr;
    HFONT mono = nullptr;
    HFONT caption = nullptr;  // 不能用 small：rpcndr.h 把它定义成 char
};

struct PanelActions {
    std::function<void(const std::wstring& text)> onSend;
    std::function<void()> onCancel;
    std::function<void()> onSettings;
    std::function<void()> onClear;
    std::function<void()> onHost;
    std::function<void()> onToggleFullScreen;
    // 空状态里的按钮（与标题栏「设置」分开：可能只是去解锁保险库）
    std::function<void()> onWelcome;
    // 审批结果：index 是 addApproval 返回的下标
    std::function<void(std::size_t index, bool allowed)> onApproval;
};

class ChatPanel {
public:
    void setFonts(PanelFonts fonts);
    void setCallbacks(PanelActions actions);
    void setBounds(const RECT& bounds);
    RECT bounds() const { return bounds_; }

    void setBusy(bool busy);
    bool busy() const { return busy_; }
    void setStatus(const std::wstring& status);
    void setModelName(const std::wstring& name);
    // 目标主机：可点击时表示还有别的主机可切
    void setHostLabel(std::wstring label, bool clickable);
    void setInputEnabled(bool enabled);
    // 停靠 / 全屏：只影响头部按钮文字
    void setFullScreen(bool full);
    bool fullScreen() const { return fullScreen_; }
    // 空状态：没有消息时显示标题、说明和一个可选按钮
    void setWelcome(std::wstring title, std::wstring subtitle, bool settingsButton, std::wstring buttonLabel = L"打开设置");

    // ---- 内容 ----
    void clear();
    void addUser(const std::wstring& text);
    std::size_t beginAssistant();
    void appendAssistant(std::size_t index, const std::wstring& chunk);
    void finishAssistant(std::size_t index, bool ok = true);
    void appendReasoning(const std::wstring& chunk);
    void addTool(const std::wstring& title, const std::wstring& detail, bool ok,
                 const std::wstring& meta);
    // 返回卡片下标；用户点击后宿主会收到 onApproval
    std::size_t addApproval(const std::wstring& title, const std::wstring& detail, bool danger);
    void resolveApproval(std::size_t index, bool allowed);
    void addNotice(const std::wstring& text, bool error = false);

    // ---- 绘制 / 交互 ----
    void paint(HDC dc);
    bool onMouseMove(POINT pt);
    bool onMouseDown(POINT pt);
    bool onMouseUp(POINT pt);
    bool onMouseWheel(int delta);
    bool onKeyDown(UINT vk, bool shift, bool ctrl);
    bool onChar(wchar_t ch);
    void focusInput() { inputFocused_ = true; }
    void blurInput() { inputFocused_ = false; }
    bool inputFocused() const { return inputFocused_; }
    bool needsCaret() const { return inputFocused_; }
    // 最近一次绘制时光标的客户区位置：输入法合成窗/候选窗要摆在这里
    RECT caretRect() const { return caretRect_; }
    // 供宿主/回归测试观察滚动状态
    int scrollOffset() const { return scrollBack_; }
    bool stickToBottom() const { return stickBottom_; }

    // 供宿主判断是否需要重绘（例如悬停变化）
    bool hasPendingApproval() const;

    const std::vector<ChatItem>& items() const { return items_; }
    std::wstring input() const { return input_; }
    void setInput(std::wstring text);

private:
    enum class Hit { None, Send, Cancel, Settings, Clear, Allow, Deny, Toggle, Host, Full, WelcomeSettings };

    struct HitRect {
        RECT rect{};
        Hit kind = Hit::None;
        std::size_t index = 0;
    };

    int dp(int value) const;
    void measure(HDC dc, int contentWidth);
    void drawItem(HDC dc, const ChatItem& item, const RECT& rc, std::size_t index);
    void drawHeader(HDC dc);
    void drawComposer(HDC dc);
    void drawButton(HDC dc, const RECT& rc, const std::wstring& label, std::uint32_t fill,
                    std::uint32_t textColor, bool hovered);
    int itemHeight(HDC dc, const ChatItem& item, int contentWidth) const;
    void pushHistory(const ChatItem& item);
    void scrollToBottom();
    void clampScroll(int viewportHeight, int contentHeight);

    std::vector<ChatItem> items_;
    std::vector<HitRect> hits_;
    std::vector<int> heights_;
    int measuredWidth_ = -1;
    bool layoutDirty_ = true;
    bool lastItemDirty_ = false;
    PanelFonts fonts_;
    PanelActions actions_;
    RECT bounds_{0, 0, 420, 700};
    int scrollBack_ = 0;  // 距离底部的像素数，0 表示贴底
    bool stickBottom_ = true;  // 是否跟随新内容（用户主动往上滚后关闭）
    int contentHeight_ = 0;
    int viewportHeight_ = 0;

    RECT caretRect_{};
    std::wstring input_;
    std::size_t caret_ = 0;
    bool inputFocused_ = false;
    bool inputEnabled_ = true;
    bool busy_ = false;
    std::wstring status_;
    std::wstring model_;
    std::wstring host_;
    bool hostClickable_ = false;
    bool fullScreen_ = false;
    std::wstring welcomeTitle_;
    std::wstring welcomeSubtitle_;
    bool welcomeSettings_ = false;
    std::wstring welcomeButtonLabel_ = L"打开设置";
    std::size_t reasoningIndex_ = static_cast<std::size_t>(-1);

    Hit hover_ = Hit::None;
    std::size_t hoverIndex_ = 0;
    Hit pressed_ = Hit::None;
    std::size_t pressedIndex_ = 0;
};

// 离屏渲染一段演示对话到 32bpp BMP，用于人工核对界面（rshell-gui.exe --ai-preview <out.bmp>）
// mode: 0=演示对话 1=空状态 2=长路径压力测试
bool renderPreview(const wchar_t* bmpPath, int width, int height, int mode = 0);

}  // namespace rshell::ai
