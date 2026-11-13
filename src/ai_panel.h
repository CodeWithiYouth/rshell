// ai_panel.h - AI 对话面板（GDI 自绘，不创建子窗口）
//
// 面板只负责“显示 + 收集输入”，不碰网络、不碰 SSH：
//   - 宿主把工作线程的事件转成这里的 add*/append* 调用（在 UI 线程执行）
//   - 用户点按钮/发送时通过 PanelActions 回调给宿主
// 审批卡片是内联的，不弹模态框，避免打断终端输入。
#pragma once

#include <cstddef>
#include <memory>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rshell {
namespace md { struct Style; class LayoutCache; }
}  // namespace rshell

namespace rshell::ai {

// 思考过程的换行缓存：流式追加时只从"最后一个未完成行"继续切，
// 绘制变成逐行 TextOut（见 ai_panel.cpp）。
struct WrapCache;

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
    // 助手消息的排版缓存：内容不变时只重放，流式时只重排变化的块
    mutable std::shared_ptr<md::LayoutCache> mdCache;
    // 思考过程的换行缓存：默认展开的长文本如果每帧重排整段，拖动窗口会卡死
    mutable std::shared_ptr<WrapCache> wrapCache;
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
    // 历史会话：远端 ~/.rshell/sessions/ 里的旧对话（按钮只在连了主机时出现）
    std::function<void()> onHistory;
    std::function<void()> onHost;
    std::function<void()> onToggleFullScreen;
    // 复制了代码块之后通知宿主：请安排一次重绘把我恢复成“复制”
    std::function<void()> onCopied;
    // 空状态里的按钮（与标题栏「设置」分开：可能只是去解锁保险库）
    std::function<void()> onWelcome;
    std::function<void()> onClose;
    // 审批结果：index 是 addApproval 返回的下标
    std::function<void(std::size_t index, bool allowed)> onApproval;
};

class ChatPanel {
public:
    ~ChatPanel();
    void setFonts(PanelFonts fonts);
    void setCallbacks(PanelActions actions);
    void setBounds(const RECT& bounds);
    RECT bounds() const { return bounds_; }
    // 消息区 / 输入区 / 最后一个气泡的矩形（客户区坐标，给局部重绘用）
    RECT viewRect() const;
    RECT composerRect() const;
    RECT lastItemRect() const;
    // 内容是否超过一屏（超过且贴底时，新行会让整段上移）
    // 诊断 / 回归测试：所有思考条目累计切了多少行。
    // 文本没变化时重绘不应再增加（防止退回"每帧整段重排"）。
    std::size_t reasoningLinesCut() const;
    // 文本选择：Ctrl+C 复制选中的对话内容
    bool hasSelection() const { return hasSelection_; }
    bool copySelection();
    void clearSelection();
    // 回归测试用：当前选中的文本
    const std::wstring& selectedText() const { return selectedText_; }
    // 回归测试用：点一下消息文本并拖到终点，覆盖真实的选择路径
    bool selectText(POINT from, POINT to);
    // 回归测试用：按“行号 + 行内偏移”直接建选区（行数据在 paint 后才有）
    bool selectLineRange(std::size_t firstLine, int firstCh, std::size_t lastLine, int lastCh);
    std::size_t selectableLineCount() const { return selectLines_.size(); }
    // 回归测试用：代码块复制按钮的矩形（没绘制过时为空）
    RECT codeBlockCopyRect(std::size_t itemIndex, std::size_t blockIndex) const;
    // 回归测试用：第 N 条可选文本行的屏幕矩形
    RECT selectLineRect(std::size_t line) const;
    // 面板内容是否超过一屏（用于切换滚动条显示）
    bool contentOverflows() const { return contentHeight_ > viewportHeight_; }
    int contentHeight() const { return contentHeight_; }
    RECT itemRectForTest(std::size_t index) const;

    void setBusy(bool busy);
    bool busy() const { return busy_; }
    void setStatus(const std::wstring& status);
    const std::wstring& statusForTest() const { return status_; }
    void setModelName(const std::wstring& name);
    // 目标主机：可点击时表示还有别的主机可切
    void setHostLabel(std::wstring label, bool clickable);
    // 「历史」按钮只在能读到远端历史时出现（面板自己不知道连没连上）
    void setHistoryAvailable(bool available) { historyAvailable_ = available; }
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
    // dirty 非空时只重画与它相交的条目（流式刷新只失效最后一个气泡）
    void paint(HDC dc, const RECT* dirty = nullptr);
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
    // 输入法组字串（拼音）。自绘输入框没有 EDIT 代画，必须自己画，
    // 否则有些输入法只开候选窗、不开组字窗，用户就看不到已经打出来的拼音。
    void setPreedit(std::wstring text, int caretPos);
    void clearPreedit();
    std::wstring preedit() const { return preedit_; }
    // 最近一次绘制时光标的客户区位置：输入法合成窗/候选窗要摆在这里
    RECT caretRect() const { return caretRect_; }
    // 最近一次绘制时「历史」按钮的位置（宿主拿它当弹出列表的锚点）
    RECT historyRect() const { return historyRect_; }
    // 供宿主/回归测试观察滚动状态
    int scrollOffset() const { return scrollBack_; }
    bool stickToBottom() const { return stickBottom_; }

    // 供宿主判断是否需要重绘（例如悬停变化）
    bool hasPendingApproval() const;

    const std::vector<ChatItem>& items() const { return items_; }
    std::wstring input() const { return input_; }
    void setInput(std::wstring text);

private:
    enum class Hit { None, Send, Cancel, Settings, Clear, History, Allow, Deny, Toggle, Host, Full, WelcomeSettings, Close, CopyCode };

    struct HitRect {
        RECT rect{};
        Hit kind = Hit::None;
        std::size_t index = 0;
        std::size_t sub = 0;  // 同一条消息里的第几个按钮（代码块复制）
    };

    int dp(int value) const;
    void measure(HDC dc, int contentWidth);
    void drawItem(HDC dc, const ChatItem& item, const RECT& rc, std::size_t index);
    void drawHeader(HDC dc);
    void drawComposer(HDC dc);
    void drawButton(HDC dc, const RECT& rc, const std::wstring& label, std::uint32_t fill,
                    std::uint32_t textColor, bool hovered);
    int itemHeight(HDC dc, const ChatItem& item, int contentWidth) const;
    // 工具卡片标题占几行（1 或 2）：摘要长的时候给两行，别只剩一句被截断的命令
    int toolTitleLines(HDC dc, const ChatItem& item, int contentWidth) const;
    void rebuildDerivedFonts();
    md::Style markdownStyle(HDC dc) const;
    void pushHistory(const ChatItem& item);
    // 收集一条消息里可选择的文本行（绘制时调用，坐标是屏幕坐标）
    void collectSelectLines(HDC dc, const ChatItem& item, const RECT& rc, std::size_t index);
    // 重建所有可见消息的可选文本行（有选择/刚开始选择时才调用）
    void rebuildSelectLines(HDC dc);
    // 在文本下面画选区高亮（必须在卡片背景之后、文字之前）
    void paintSelectionForItem(HDC dc, std::size_t index);
    bool selectionPoint(POINT pt, std::size_t& line, int& ch) const;
    void updateSelection(POINT pt);
    void rebuildSelectedText();
    void copyCodeBlock(std::size_t itemIndex, std::size_t blockIndex);
    void scrollToBottom();
    void clampScroll(int viewportHeight, int contentHeight);

    std::vector<ChatItem> items_;
    std::vector<HitRect> hits_;
    // 鼠标选择的文本行（每次绘制重建，只包含可见区域）
    struct SelectLine {
        std::size_t item = 0;
        int top = 0;
        int bottom = 0;
        int left = 0;
        HFONT font = nullptr;
        std::wstring text;
        // 每个字符结束处的 x（相对 left）。只在真的要命中/高亮时才计算，
        // 否则每帧给几百行算一遍 GDI 宽度，流式会被拖慢。
        mutable std::vector<int> dx;
        mutable bool dxReady = false;
        bool hardBreak = true;
    };
    HDC measureDc();
    void ensureLineDx(const SelectLine& line) const;
    std::vector<SelectLine> selectLines_;
    std::size_t selAnchorLine_ = 0;
    std::size_t selFocusLine_ = 0;
    int selAnchorCh_ = 0;
    int selFocusCh_ = 0;
    bool selecting_ = false;
    bool hasSelection_ = false;
    std::wstring selectedText_;
    // 代码块复制按钮的短暂反馈
    std::size_t copyFlashItem_ = static_cast<std::size_t>(-1);
    std::size_t copyFlashBlock_ = static_cast<std::size_t>(-1);
    ULONGLONG copyFlashUntil_ = 0;
    std::size_t hoverSub_ = 0;
    std::vector<int> heights_;
    int measuredWidth_ = -1;
    bool layoutDirty_ = true;
    bool lastItemDirty_ = false;
    PanelFonts fonts_;
    HFONT headingFonts_[3]{};
    HFONT italicFont_ = nullptr;
    PanelActions actions_;
    RECT bounds_{0, 0, 420, 700};
    int scrollBack_ = 0;  // 距离底部的像素数，0 表示贴底
    bool stickBottom_ = true;  // 是否跟随新内容（用户主动往上滚后关闭）
    int contentHeight_ = 0;
    int viewportHeight_ = 0;

    RECT caretRect_{};
    RECT historyRect_{};  // 「历史」按钮，宿主据此定位弹出列表
    std::wstring input_;
    std::size_t caret_ = 0;
    std::wstring preedit_;       // 输入法组字串，插在 caret_ 位置显示
    std::size_t preeditCaret_ = 0;  // 组字串内部的光标（拼音回改时用）
    bool inputFocused_ = false;
    bool inputEnabled_ = true;
    bool busy_ = false;
    std::wstring status_;
    std::wstring model_;
    std::wstring host_;
    bool hostClickable_ = false;
    bool historyAvailable_ = false;
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
    std::size_t pressedSub_ = 0;
    HDC measureDc_ = nullptr;  // 懒计算字符宽度用的内存 DC
    // 可选文本行的缓存时间戳：内容/滚动/尺寸没变就不重建
    int selectStampContent_ = -1;
    int selectStampScroll_ = -1;
    int selectStampWidth_ = -1;
    std::size_t selectStampItems_ = 0;
    std::size_t selectStampTail_ = 0;
};

#ifdef RSHELL_ENABLE_DIAGNOSTICS
// 离屏渲染一段演示对话到 32bpp BMP，用于人工核对界面（rshell-gui.exe --ai-preview <out.bmp>）
// mode: 0=演示对话 1=空状态 2=长路径压力测试 3=Markdown
bool renderPreview(const wchar_t* bmpPath, int width, int height, int mode = 0);
#endif

}  // namespace rshell::ai
