// ai_panel.cpp
#include "ai_panel.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include <objidl.h>
#include <ole2.h>
#include <gdiplus.h>

#include "md_render.h"
#include "ui_theme.h"

namespace rshell::ai {

namespace {

using namespace rshell::ui;

constexpr int kItemGap = 10;
constexpr int kPad = 14;
constexpr int kHeaderH = 62;  // 两行：标题/操作 + 主机/状态
constexpr int kComposerH = 74;
constexpr int kBtnH = 32;
constexpr int kSmallBtnW = 76;

// GDI+ 只用来画抗锯齿圆角；宿主已经初始化过时这里不会重复生效
ULONG_PTR g_token = 0;
bool g_ready = false;

void ensureGdiplus() {
    if (g_ready) return;
    Gdiplus::GdiplusStartupInput input;
    g_ready = Gdiplus::GdiplusStartup(&g_token, &input, nullptr) == Gdiplus::Ok;
}

Gdiplus::Color gp(std::uint32_t c, int alpha = 255) {
    return Gdiplus::Color(static_cast<BYTE>(alpha), static_cast<BYTE>((c >> 16) & 0xFF),
                          static_cast<BYTE>((c >> 8) & 0xFF), static_cast<BYTE>(c & 0xFF));
}

void fillRect(HDC dc, const RECT& rc, std::uint32_t color) {
    HBRUSH brush = CreateSolidBrush(toColorRef(color));
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

void fillRoundRect(HDC dc, const RECT& rc, int radius, std::uint32_t color,
                   std::uint32_t borderColor = 0) {
    ensureGdiplus();
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    if (!g_ready) {
        HBRUSH brush = CreateSolidBrush(toColorRef(color));
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
        return;
    }

    const float r = static_cast<float>(std::min(radius, std::min(w, h) / 2));
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    const float d = r * 2.0f;
    const float x = static_cast<float>(rc.left);
    const float y = static_cast<float>(rc.top);
    if (r <= 0.0f) {
        path.AddRectangle(Gdiplus::RectF(x, y, static_cast<float>(w), static_cast<float>(h)));
    } else {
        path.AddArc(x, y, d, d, 180.0f, 90.0f);
        path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
        path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
        path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }
    Gdiplus::SolidBrush brush(gp(color));
    graphics.FillPath(&brush, &path);
    if (borderColor) {
        Gdiplus::Pen pen(gp(borderColor), 1.0f);
        graphics.DrawPath(&pen, &path);
    }
}

void strokeRoundRect(HDC dc, const RECT& rc, int radius, std::uint32_t color) {
    ensureGdiplus();
    if (!g_ready) {
        HPEN pen = CreatePen(PS_SOLID, 1, toColorRef(color));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        return;
    }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    const float r = static_cast<float>(std::min(radius, std::min(w, h) / 2));
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    const float d = r * 2.0f;
    const float x = static_cast<float>(rc.left);
    const float y = static_cast<float>(rc.top);
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    Gdiplus::Pen pen(gp(color), 1.0f);
    graphics.DrawPath(&pen, &path);
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

int textWidth(HDC dc, const std::wstring& text, HFONT font) {
    if (text.empty() || !font) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    return size.cx;
}

int lineHeight(HDC dc, HFONT font) {
    if (!font) return 18;
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old);
    return metrics.tmHeight > 0 ? metrics.tmHeight : 18;
}

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

// 保留尾部：路径的文件名比开头更有信息量
std::wstring ellipsizeTail(HDC dc, const std::wstring& text, HFONT font, int maxWidth) {
    if (text.empty() || maxWidth <= 0) return std::wstring();
    if (textWidth(dc, text, font) <= maxWidth) return text;
    const std::wstring dots = L"…";
    int lo = 0;
    int hi = static_cast<int>(text.size());
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        const std::wstring candidate =
            dots + text.substr(text.size() - static_cast<std::size_t>(mid));
        if (textWidth(dc, candidate, font) <= maxWidth)
            lo = mid;
        else
            hi = mid - 1;
    }
    if (lo <= 0) return dots;
    return dots + text.substr(text.size() - static_cast<std::size_t>(lo));
}

// 按宽度折行后的高度
int wrappedHeight(HDC dc, const std::wstring& text, HFONT font, int width) {
    if (text.empty() || width <= 0) return 0;
    RECT rc{0, 0, width, 0};
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rc,
              DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    SelectObject(dc, old);
    return rc.bottom - rc.top;
}

}  // namespace

// ---------------------------------------------------------------- 基础

int ChatPanel::dp(int value) const {
    // 面板不自己算 DPI：宿主按 96 基准给出逻辑像素，这里沿用固定缩放，
    // 真正的高 DPI 由宿主在 setBounds 时给更大的像素尺寸（字号已按 DPI 创建）。
    return value;
}

namespace {
HFONT cloneScaledFont(HFONT base, double factor) {
    if (!base) return nullptr;
    LOGFONTW info{};
    if (!GetObjectW(base, sizeof(info), &info)) return nullptr;
    info.lfHeight = static_cast<LONG>(static_cast<double>(info.lfHeight) * factor);
    info.lfWeight = FW_BOLD;
    return CreateFontIndirectW(&info);
}
HFONT cloneItalicFont(HFONT base) {
    if (!base) return nullptr;
    LOGFONTW info{};
    if (!GetObjectW(base, sizeof(info), &info)) return nullptr;
    info.lfItalic = TRUE;
    return CreateFontIndirectW(&info);
}
}  // namespace

ChatPanel::~ChatPanel() {
    for (HFONT font : headingFonts_) {
        if (font) DeleteObject(font);
    }
    if (italicFont_) DeleteObject(italicFont_);
}

void ChatPanel::rebuildDerivedFonts() {
    for (HFONT& font : headingFonts_) {
        if (font) {
            DeleteObject(font);
            font = nullptr;
        }
    }
    if (italicFont_) {
        DeleteObject(italicFont_);
        italicFont_ = nullptr;
    }
    headingFonts_[0] = cloneScaledFont(fonts_.ui, 1.45);
    headingFonts_[1] = cloneScaledFont(fonts_.ui, 1.24);
    headingFonts_[2] = cloneScaledFont(fonts_.ui, 1.10);
    italicFont_ = cloneItalicFont(fonts_.ui);
}

void ChatPanel::setFonts(PanelFonts fonts) {
    fonts_ = fonts;
    if (!fonts_.caption) fonts_.caption = fonts_.ui;
    if (!fonts_.bold) fonts_.bold = fonts_.ui;
    if (!fonts_.mono) fonts_.mono = fonts_.ui;
    rebuildDerivedFonts();
}

md::Style ChatPanel::markdownStyle(HDC dc) const {
    md::Style style;
    style.normal = fonts_.ui;
    style.bold = fonts_.bold;
    style.italic = italicFont_ ? italicFont_ : fonts_.ui;
    style.mono = fonts_.mono;
    for (int i = 0; i < 3; ++i) style.heading[i] = headingFonts_[i];
    style.text = toColorRef(ui::text);
    style.dim = toColorRef(ui::textDim);
    style.accent = toColorRef(ui::accent);
    style.codeFg = toColorRef(0xBED7C3);
    style.codeBg = toColorRef(0x121A16);
    style.border = toColorRef(ui::borderSoft);
    style.bodyHeight = lineHeight(dc, fonts_.ui);
    return style;
}

void ChatPanel::setCallbacks(PanelActions actions) { actions_ = std::move(actions); }

void ChatPanel::setBounds(const RECT& bounds) {
    bounds_ = bounds;
    if (stickBottom_)
        scrollBack_ = 0;
    else
        scrollBack_ = std::min(scrollBack_, std::max(0, contentHeight_ - viewportHeight_));
}

void ChatPanel::setBusy(bool busy) { busy_ = busy; }

void ChatPanel::setStatus(const std::wstring& status) { status_ = status; }

void ChatPanel::setModelName(const std::wstring& name) { model_ = name; }

void ChatPanel::setHostLabel(std::wstring label, bool clickable) {
    host_ = std::move(label);
    hostClickable_ = clickable;
}

void ChatPanel::setInputEnabled(bool enabled) { inputEnabled_ = enabled; }

void ChatPanel::setFullScreen(bool full) { fullScreen_ = full; }

void ChatPanel::setWelcome(std::wstring title, std::wstring subtitle, bool settingsButton,
                            std::wstring buttonLabel) {
    welcomeTitle_ = std::move(title);
    welcomeSubtitle_ = std::move(subtitle);
    welcomeSettings_ = settingsButton;
    welcomeButtonLabel_ = std::move(buttonLabel);
}

void ChatPanel::clear() {
    layoutDirty_ = true;
    items_.clear();
    hits_.clear();
    heights_.clear();
    scrollBack_ = 0;
    reasoningIndex_ = static_cast<std::size_t>(-1);
}

void ChatPanel::scrollToBottom() {
    scrollBack_ = 0;
    stickBottom_ = true;
}

void ChatPanel::pushHistory(const ChatItem& item) {
    items_.push_back(item);
    layoutDirty_ = true;
}

void ChatPanel::addUser(const std::wstring& text) {
    ChatItem item;
    item.kind = ItemKind::User;
    item.title = L"你";
    item.body = text;
    pushHistory(item);
    scrollToBottom();
}

std::size_t ChatPanel::beginAssistant() {
    ChatItem item;
    item.kind = ItemKind::Assistant;
    item.title = L"Agent";
    item.streaming = true;
    pushHistory(item);
    if (stickBottom_) scrollToBottom();
    return items_.size() - 1;
}

void ChatPanel::appendAssistant(std::size_t index, const std::wstring& chunk) {
    if (index >= items_.size()) return;
    items_[index].body += chunk;
    lastItemDirty_ = true;
    if (stickBottom_) scrollToBottom();
}

void ChatPanel::finishAssistant(std::size_t index, bool ok) {
    if (index >= items_.size()) return;
    items_[index].streaming = false;
    items_[index].ok = ok;
    if (!ok) items_[index].kind = ItemKind::Error;
}

void ChatPanel::appendReasoning(const std::wstring& chunk) {
    if (reasoningIndex_ == static_cast<std::size_t>(-1) || reasoningIndex_ >= items_.size() ||
        items_[reasoningIndex_].kind != ItemKind::Reasoning) {
        ChatItem item;
        item.kind = ItemKind::Reasoning;
        item.title = L"思考过程";
        item.expandable = true;
        item.expanded = true;  // 思考过程默认展开
        pushHistory(item);
        reasoningIndex_ = items_.size() - 1;
    }
    items_[reasoningIndex_].body += chunk;
    lastItemDirty_ = true;
    if (stickBottom_) scrollToBottom();
}

void ChatPanel::addTool(const std::wstring& title, const std::wstring& detail, bool ok,
                        const std::wstring& meta) {
    ChatItem item;
    item.kind = ItemKind::Tool;
    item.title = title;
    item.body = detail;
    item.meta = meta;
    item.ok = ok;
    item.expandable = !detail.empty();
    item.expanded = false;
    pushHistory(item);
    reasoningIndex_ = static_cast<std::size_t>(-1);  // 下一段思考另起一条
    if (stickBottom_) scrollToBottom();
}

std::size_t ChatPanel::addApproval(const std::wstring& title, const std::wstring& detail,
                                   bool danger) {
    ChatItem item;
    item.kind = ItemKind::Approval;
    item.title = title;
    item.body = detail;
    item.danger = danger;
    item.awaitApproval = true;
    item.expandable = !detail.empty();
    item.expanded = true;
    pushHistory(item);
    scrollToBottom();
    return items_.size() - 1;
}

void ChatPanel::resolveApproval(std::size_t index, bool allowed) {
    if (index >= items_.size() || items_[index].kind != ItemKind::Approval) return;
    layoutDirty_ = true;
    items_[index].awaitApproval = false;
    items_[index].decided = true;
    items_[index].approved = allowed;
    if (!allowed) items_[index].expanded = false;
}

void ChatPanel::addNotice(const std::wstring& text, bool error) {
    ChatItem item;
    item.kind = error ? ItemKind::Error : ItemKind::Notice;
    item.body = text;
    pushHistory(item);
    if (stickBottom_) scrollToBottom();
}

bool ChatPanel::hasPendingApproval() const {
    for (const ChatItem& item : items_) {
        if (item.kind == ItemKind::Approval && item.awaitApproval) return true;
    }
    return false;
}

void ChatPanel::setInput(std::wstring text) {
    input_ = std::move(text);
    caret_ = input_.size();
}

void ChatPanel::setPreedit(std::wstring text, int caretPos) {
    preedit_ = std::move(text);
    const std::size_t limit = preedit_.size();
    const int pos = caretPos < 0 ? 0 : caretPos;
    preeditCaret_ = std::min<std::size_t>(static_cast<std::size_t>(pos), limit);
}

void ChatPanel::clearPreedit() {
    preedit_.clear();
    preeditCaret_ = 0;
}

// ---------------------------------------------------------------- 布局与绘制

// 工具卡片标题占几行：卡片宽减去图标、边距和右侧元信息（结果大小/退出码）
int ChatPanel::toolTitleLines(HDC dc, const ChatItem& item, int width) const {
    int metaW = 0;
    if (!item.meta.empty()) {
        metaW = std::min(textWidth(dc, item.meta, fonts_.caption),
                         std::max(40, width * 2 / 5)) + 10;
    }
    const int titleWidth = std::max(40, width - 20 - 18 - 10 - metaW);
    return textWidth(dc, item.title, fonts_.ui) > titleWidth ? 2 : 1;
}
int ChatPanel::itemHeight(HDC dc, const ChatItem& item, int width) const {    const int bodyFont = lineHeight(dc, fonts_.ui);
    const int labelFont = lineHeight(dc, fonts_.caption);

    switch (item.kind) {
        case ItemKind::Notice:
        case ItemKind::Error:
            return std::max(bodyFont, wrappedHeight(dc, item.body, fonts_.caption, width));

        case ItemKind::User:
            return labelFont + 4 + wrappedHeight(dc, item.body, fonts_.ui, width - 20) + 16;

        case ItemKind::Assistant: {
            const int body = md::layout(dc, 0, 0, width, item.body, markdownStyle(dc), false);
            return labelFont + 4 + std::max(body, bodyFont) + 8;
        }

        case ItemKind::Reasoning: {
            const int header = labelFont + 2;
            if (!item.expanded) return header;
            return header + wrappedHeight(dc, item.body, fonts_.caption, width - 20) + 12;
        }

        case ItemKind::Tool: {
            const int lines = toolTitleLines(dc, item, width);
            const int header = bodyFont * lines + 12;
            if (!item.expanded) return header;
            return header + wrappedHeight(dc, item.body, fonts_.mono, width - 20) + 12;
        }

        case ItemKind::Approval: {
            int height = 10 + wrappedHeight(dc, item.title, fonts_.bold, width - 24) + 8;
            if (item.expanded && !item.body.empty())
                height += std::min(wrappedHeight(dc, item.body, fonts_.mono, width - 24), 160) + 8;
            height += item.awaitApproval ? kBtnH + 14 : lineHeight(dc, fonts_.caption) + 10;
            return height;
        }
    }
    return bodyFont;
}

void ChatPanel::measure(HDC dc, int contentWidth) {
    heights_.clear();
    heights_.reserve(items_.size());
    contentHeight_ = 0;
    for (const ChatItem& item : items_) {
        const int h = itemHeight(dc, item, contentWidth);
        heights_.push_back(h);
        contentHeight_ += h + kItemGap;
    }
    if (!items_.empty()) contentHeight_ -= kItemGap;
}

void ChatPanel::paint(HDC dc, const RECT* dirty) {    hits_.clear();

    const int width = bounds_.right - bounds_.left;
    if (width <= 0) return;

    ensureGdiplus();
    RECT full = bounds_;
    fillRect(dc, full, bgApp);  // 圆角外露出的就是主窗口底色

    // 圆角卡片：把后续绘制裁进圆角区域，四角自然透明，和左右两块卡片一致
    const int cardRadius = 10;
    HRGN cardShape = CreateRoundRectRgn(full.left, full.top, full.right + 1, full.bottom + 1,
                                        cardRadius * 2, cardRadius * 2);
    const int savedDc = SaveDC(dc);
    if (cardShape) SelectClipRgn(dc, cardShape);
    fillRect(dc, full, bgPanel);

    RECT header{full.left, full.top, full.right, full.top + kHeaderH};
    RECT composer{full.left, full.bottom - kComposerH, full.right, full.bottom};
    RECT view{full.left, header.bottom, full.right, composer.top};

    const int contentWidth = width - kPad * 2;
    // 消息区用更深的底色，正文和气泡才「浮」得起来（以前整块面板一个颜色，
    // 文字和背景贴在一起看不清层次）
    fillRect(dc, view, bgApp);
    // 只有结构变化或宽度变化才重新测量全部条目；流式追加时只重量最后一条。
    // 否则每个 token 都会对所有历史消息做一遍 DrawText 计算高度。
    if (layoutDirty_ || measuredWidth_ != contentWidth || heights_.size() != items_.size()) {
        measure(dc, contentWidth);
        measuredWidth_ = contentWidth;
        layoutDirty_ = false;
        lastItemDirty_ = false;
    } else if (lastItemDirty_ && !items_.empty()) {
        const int h = itemHeight(dc, items_.back(), contentWidth);
        contentHeight_ += h - heights_.back();
        heights_.back() = h;
        lastItemDirty_ = false;
    }
    viewportHeight_ = view.bottom - view.top;
    const int maxScroll = std::max(0, contentHeight_ - viewportHeight_);
    if (stickBottom_) scrollBack_ = 0;
    scrollBack_ = std::clamp(scrollBack_, 0, maxScroll);

    // 消息区裁剪，避免滚动时画到标题栏/输入框上（用 Intersect，别把外层的圆角裁掉）
    const int messageClip = SaveDC(dc);
    IntersectClipRect(dc, view.left, view.top, view.right, view.bottom);

    // 内容不足一屏时从顶部开始排，避免只有一条提示时上方一大片空白
    int y = contentHeight_ <= viewportHeight_ ? view.top
                                              : view.bottom + scrollBack_ - contentHeight_;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const int h = heights_[i];
        RECT rc{view.left + kPad, y, view.right - kPad, y + h};
        if (rc.bottom >= view.top && rc.top <= view.bottom) {
            // 局部重绘：不相交的「纯文本」条目直接跳过（Markdown 是这里最贵的部分）。
            // 工具卡/审批卡即使不重画也要保留命中区，否则点不动，所以不跳。
            const ItemKind kind = items_[i].kind;
            const bool cheapToSkip = kind == ItemKind::Assistant || kind == ItemKind::User ||
                                     kind == ItemKind::Notice || kind == ItemKind::Error;
            const bool outside = dirty != nullptr &&
                                 (dirty->bottom < rc.top || dirty->top > rc.bottom ||
                                  dirty->right < rc.left || dirty->left > rc.right);
            if (!(outside && cheapToSkip)) drawItem(dc, items_[i], rc, i);
        }
        y += h + kItemGap;
    }
    RestoreDC(dc, messageClip);  // 标题栏/输入框在裁剪区外，得先把消息区的裁剪撤掉

    // 空状态：标题 + 说明 + 可选的「打开设置」按钮
    if (items_.empty() && !welcomeTitle_.empty()) {
        const int centerX = (view.left + view.right) / 2;
        const int blockTop = view.top + std::max(dp(60), viewportHeight_ / 4);
        RECT titleRect{view.left + kPad, blockTop, view.right - kPad, blockTop + dp(30)};
        drawText(dc, welcomeTitle_, titleRect, text, fonts_.bold,
                 DT_CENTER | DT_TOP | DT_SINGLELINE);
        RECT subRect{view.left + kPad, titleRect.bottom + dp(8), view.right - kPad,
                     titleRect.bottom + dp(8) + dp(60)};
        drawText(dc, welcomeSubtitle_, subRect, textFaint, fonts_.ui,
                 DT_CENTER | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL);
        if (welcomeSettings_) {
            const int w = dp(120);
            RECT button{centerX - w / 2, subRect.top + dp(56), centerX + w / 2,
                        subRect.top + dp(56) + kBtnH};
            drawButton(dc, button, welcomeButtonLabel_, action, 0xFFFFFF,
                       hover_ == Hit::WelcomeSettings);
            hits_.push_back({button, Hit::WelcomeSettings, 0});
        }
    }

    // 滚动条：只在内容超出时显示
    if (maxScroll > 0) {
        const int trackTop = view.top + 4;
        const int trackHeight = viewportHeight_ - 8;
        const int thumbHeight =
            std::max(24, static_cast<int>(static_cast<double>(viewportHeight_) * viewportHeight_ /
                                         contentHeight_));
        const double ratio = static_cast<double>(maxScroll - scrollBack_) / maxScroll;
        const int thumbTop = trackTop + static_cast<int>((trackHeight - thumbHeight) * ratio);
        RECT track{view.right - 4, trackTop, view.right, trackTop + trackHeight};
        fillRect(dc, track, bgPanel);
        RECT thumb{view.right - 4, thumbTop, view.right, thumbTop + thumbHeight};
        fillRect(dc, thumb, borderSoft);
    }

    drawHeader(dc);
    drawComposer(dc);

    // 恢复裁剪并描边：现在是一张和左右两块一致的圆角卡片
    RestoreDC(dc, savedDc);
    if (cardShape) DeleteObject(cardShape);
    strokeRoundRect(dc, RECT{full.left, full.top, full.right - 1, full.bottom - 1}, cardRadius,
                    border);
}

void ChatPanel::drawHeader(HDC dc) {
    RECT header{bounds_.left, bounds_.top, bounds_.right, bounds_.top + kHeaderH};
    fillRect(dc, header, bgPanel);
    RECT line{header.left, header.bottom - 1, header.right, header.bottom};
    fillRect(dc, line, borderSoft);

    const int rowH = 32;
    RECT row1{header.left, header.top, header.right, header.top + rowH};
    RECT row2{header.left, header.top + rowH, header.right, header.bottom - 2};

    // 第一行：标题 + 操作按钮（窄面板下也不会挤到标题）
    int left = row1.left + kPad;
    drawText(dc, L"AI Agent", RECT{left, row1.top, left + 120, row1.bottom}, text, fonts_.bold,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    int right = row1.right - kPad;
    const int btnW = 48;

    // 关闭按钮放最右，宽度窄一点，和三个操作按钮区分开
    RECT closeRect{right - 32, row1.top + 7, right, row1.bottom - 5};
    drawButton(dc, closeRect, L"×", bgCard, textDim, hover_ == Hit::Close);
    hits_.push_back({closeRect, Hit::Close, 0});
    right = closeRect.left - 6;

    auto addButton = [&](const wchar_t* label, Hit hit, bool hovered) {
        RECT rect{right - btnW, row1.top + 7, right, row1.bottom - 5};
        drawButton(dc, rect, label, bgCard, textDim, hovered);
        hits_.push_back({rect, hit, 0});
        right = rect.left - 6;
    };
    // 「新建」= 开一段新对话（旧对话留在「历史」里，不会删远端文件）
    addButton(L"新建", Hit::Clear, hover_ == Hit::Clear);
    addButton(L"设置", Hit::Settings, hover_ == Hit::Settings);
    addButton(fullScreen_ ? L"停靠" : L"全屏", Hit::Full, hover_ == Hit::Full);

    // 第二行：左边主机 chip，右边模型名或状态
    const int secondTop = row2.top + 2;
    const int secondBottom = row2.bottom - 2;
    int secondRight = row2.right - kPad;

    const std::wstring status = !status_.empty() ? status_ : (busy_ ? L"生成中…" : L"");
    if (!status.empty()) {
        const int w = std::min(textWidth(dc, status, fonts_.caption), 140);
        drawText(dc, ellipsize(dc, status, fonts_.caption, w),
                 RECT{secondRight - w, secondTop, secondRight, secondBottom},
                 busy_ ? accent : textFaint, fonts_.caption,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        secondRight -= w + 10;
    } else if (!model_.empty()) {
        const int w = std::min(textWidth(dc, model_, fonts_.caption), 160);
        drawText(dc, ellipsize(dc, model_, fonts_.caption, w),
                 RECT{secondRight - w, secondTop, secondRight, secondBottom}, textFaint,
                 fonts_.caption, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        secondRight -= w + 10;
    }

    // 目标主机：点击在已连接的主机之间轮换
    historyRect_ = RECT{};
    if (!host_.empty() && historyAvailable_) {
        const int available = std::max(60, secondRight - left);
        const int chipW = std::min(textWidth(dc, host_, fonts_.caption) + 18, available);
        RECT chip{left, secondTop, left + chipW, secondBottom};
        fillRoundRect(dc, chip, 5, hover_ == Hit::Host ? bgHover : bgCard, borderSoft);
        drawText(dc, ellipsize(dc, host_, fonts_.caption, chipW - 14), chip, accent,
                 fonts_.caption, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (hostClickable_) hits_.push_back({chip, Hit::Host, 0});

        // 「历史」只在连上主机时出现：对话存在远端，没有连接就读不到
        const int histW = 38;
        RECT hist{chip.right + 6, secondTop, chip.right + 6 + histW, secondBottom};
        if (hist.right < secondRight - 4) {
            drawButton(dc, hist, L"历史", bgCard, textDim, hover_ == Hit::History);
            hits_.push_back({hist, Hit::History, 0});
            historyRect_ = hist;
        }
    }
}

void ChatPanel::drawComposer(HDC dc) {
    RECT composer{bounds_.left, bounds_.bottom - kComposerH, bounds_.right, bounds_.bottom};
    fillRect(dc, composer, bgPanel);
    RECT line{composer.left, composer.top, composer.right, composer.top + 1};
    fillRect(dc, line, borderSoft);

    const int btnW = 72;
    RECT button{composer.right - kPad - btnW, composer.top + 20, composer.right - kPad,
                composer.top + 20 + kBtnH};

    RECT box{composer.left + kPad, composer.top + 18, button.left - 10,
             composer.top + 18 + kBtnH};
    fillRoundRect(dc, box, 6, bgApp, borderSoft);

    RECT editRect = box;
    editRect.left += 10;
    editRect.right -= 10;

    // 输入过长时向左滚动，保证光标（以及组字串）可见
    const std::wstring before = input_.substr(0, caret_);
    const int caretX = textWidth(dc, before, fonts_.ui);
    const int preeditW = preedit_.empty() ? 0 : textWidth(dc, preedit_, fonts_.ui);
    const int avail = editRect.right - editRect.left - 2;
    int offset = 0;
    if (caretX + preeditW > avail) offset = caretX + preeditW - avail;

    const std::uint32_t inkColor = inputEnabled_ ? text : textFaint;
    if (input_.empty() && preedit_.empty()) {
        drawText(dc, inputEnabled_ ? L"描述你的问题，Enter 发送" : L"先连接一台主机",
                 editRect, textFaint, fonts_.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        HRGN clip = CreateRectRgn(editRect.left, editRect.top, editRect.right, editRect.bottom);
        SelectClipRgn(dc, clip);
        RECT drawRect = editRect;
        drawRect.left -= offset;
        if (preedit_.empty()) {
            drawText(dc, input_, drawRect, inkColor, fonts_.ui,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else {
            // 三段拼起来：已输入前半 + 组字串（带下划线）+ 已输入后半。
            // 单行框总是从同一条基线开始画，所以只用左边界平移即可。
            int x = drawRect.left;
            auto segment = [&](const std::wstring& text, bool underlined) {
                if (text.empty()) return;
                const int width = textWidth(dc, text, fonts_.ui);
                RECT rc{x, editRect.top, x + width + dp(2), editRect.bottom};
                drawText(dc, text, rc, inkColor, fonts_.ui,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                if (underlined && width > 0) {
                    // 下划线告诉用户这段还没上屏
                    fillRect(dc, RECT{x, editRect.bottom - dp(5), x + width,
                                      editRect.bottom - dp(5) + std::max(1, dp(1))},
                             inkColor);
                }
                x += width;
            };
            segment(before, false);
            segment(preedit_, true);
            segment(input_.substr(caret_), false);
        }
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
    }

    // 光标：空输入框时也要画，否则看起来像没聚焦；位置同时给输入法用。
    // 组字期间光标落在拼音内部（回改拼音时会在中间），并且常亮不闪。
    {
        const int inner = preedit_.empty()
                              ? 0
                              : textWidth(dc, preedit_.substr(0, preeditCaret_), fonts_.ui);
        const int x = editRect.left + caretX + inner - offset;
        caretRect_ = RECT{x, editRect.top + 4, x + 1, editRect.bottom - 4};
        if (inputFocused_ && (!preedit_.empty() || (GetTickCount64() / 500) % 2 == 0))
            fillRect(dc, caretRect_, text);
    }

    if (busy_) {
        drawButton(dc, button, L"停止", danger, 0xFFFFFF, hover_ == Hit::Cancel);
        hits_.push_back({button, Hit::Cancel, 0});
    } else {
        const bool enabled = inputEnabled_ && !input_.empty();
        drawButton(dc, button, L"发送", enabled ? action : bgCard,
                   enabled ? 0xFFFFFF : textFaint, hover_ == Hit::Send && enabled);
        if (enabled) hits_.push_back({button, Hit::Send, 0});
    }
}

void ChatPanel::drawButton(HDC dc, const RECT& rc, const std::wstring& label, std::uint32_t fill,
                           std::uint32_t textColor, bool hovered) {
    fillRoundRect(dc, rc, 6, hovered ? mix(fill, 0xFFFFFF, 0.12) : fill);
    drawText(dc, label, rc, textColor, fonts_.ui, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void ChatPanel::drawItem(HDC dc, const ChatItem& item, const RECT& rc, std::size_t index) {
    const int cap = lineHeight(dc, fonts_.caption);
    const int body = lineHeight(dc, fonts_.ui);

    switch (item.kind) {
        case ItemKind::Notice:
        case ItemKind::Error: {
            RECT text = rc;
            drawText(dc, item.body, text, item.kind == ItemKind::Error ? danger : textFaint,
                     fonts_.caption, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL);
            return;
        }

        case ItemKind::User: {
            fillRoundRect(dc, rc, 8, bgCard, borderSoft);
            RECT inner = rc;
            inner.left += 10;
            inner.right -= 10;
            inner.top += 6;
            drawText(dc, item.title, RECT{inner.left, inner.top, inner.right, inner.top + cap},
                     accent, fonts_.caption, DT_LEFT | DT_TOP | DT_SINGLELINE);
            RECT bodyRect{inner.left, inner.top + cap + 4, inner.right, rc.bottom - 6};
            drawText(dc, item.body, bodyRect, text, fonts_.ui,
                     DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL);
            return;
        }

        case ItemKind::Assistant: {
            drawText(dc, item.title,
                     RECT{rc.left, rc.top, rc.right, rc.top + cap}, success, fonts_.caption,
                     DT_LEFT | DT_TOP | DT_SINGLELINE);
            int textLeft = rc.left;
            if (item.streaming) {
                const int titleW = textWidth(dc, item.title, fonts_.caption);
                drawText(dc, L"▍", RECT{rc.left + titleW + 4, rc.top, rc.right, rc.top + cap},
                         accent, fonts_.caption, DT_LEFT | DT_TOP | DT_SINGLELINE);
            }
            RECT bodyRect{textLeft, rc.top + cap + 4, rc.right, rc.bottom};
            md::layout(dc, bodyRect.left, bodyRect.top, bodyRect.right - bodyRect.left,
                        item.body, markdownStyle(dc), true);
            return;
        }

        case ItemKind::Reasoning: {
            const std::wstring label =
                item.expanded ? L"思考过程（点击收起）" : L"思考过程（点击展开）";
            drawText(dc, label, RECT{rc.left, rc.top, rc.right, rc.top + cap}, textDim,
                     fonts_.caption, DT_LEFT | DT_TOP | DT_SINGLELINE);
            if (item.expanded) {
                RECT inner{rc.left + 10, rc.top + cap + 2, rc.right - 10, rc.bottom};
                drawText(dc, item.body, inner, textDim, fonts_.caption,
                         DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);
            }
            hits_.push_back({rc, Hit::Toggle, index});
            return;
        }

        case ItemKind::Tool: {
            fillRoundRect(dc, rc, 8, bgCard, border);
            RECT inner = rc;
            inner.left += 10;
            inner.right -= 10;
            inner.top += 6;

            const std::wstring glyph = item.ok ? L"✓" : L"✕";
            const std::uint32_t glyphColor = item.ok ? success : danger;
            drawText(dc, glyph, RECT{inner.left, inner.top, inner.left + 16, inner.top + body},
                     glyphColor, fonts_.ui, DT_LEFT | DT_TOP | DT_SINGLELINE);

            int right = inner.right;
            if (!item.meta.empty()) {
                // 元信息最多占卡片 40%，再长就省略，避免把标题挤成负宽度
                const int cardWidth = inner.right - inner.left;
                const int metaW = std::min(textWidth(dc, item.meta, fonts_.caption),
                                           std::max(40, cardWidth * 2 / 5));
                const std::wstring meta = ellipsize(dc, item.meta, fonts_.caption, metaW);
                drawText(dc, meta, RECT{inner.right - metaW, inner.top, inner.right, inner.top + body},
                         textFaint, fonts_.caption, DT_RIGHT | DT_TOP | DT_SINGLELINE);
                right = inner.right - metaW - 10;
            }
            const int titleLeft = inner.left + 18;
            const int titleWidth = std::max(40, right - titleLeft);
            std::wstring title = item.title;
            // 「读取 /very/long/path」：保留动词，路径留尾部（文件名最关键）
            const int titleLines = toolTitleLines(dc, item, rc.right - rc.left);
            const std::size_t split = title.find(L' ');
            if (titleLines == 1 && split != std::wstring::npos && split + 1 < title.size() &&
                (title[split + 1] == L'/' || title[split + 1] == L'~')) {
                const std::wstring prefix = title.substr(0, split + 1);
                const std::wstring path = title.substr(split + 1);
                const int prefixW = std::min(textWidth(dc, prefix, fonts_.ui), titleWidth / 2);
                title = prefix +
                        ellipsizeTail(dc, path, fonts_.ui, std::max(20, titleWidth - prefixW));
            } else if (titleLines == 1) {
                title = ellipsize(dc, title, fonts_.ui, titleWidth);
            }
            drawText(dc, title,
                     RECT{titleLeft, inner.top, titleLeft + titleWidth,
                          inner.top + body * titleLines},
                     text, fonts_.ui,
                     DT_LEFT | DT_TOP |
                         (titleLines == 2 ? (DT_WORDBREAK | DT_END_ELLIPSIS) : DT_SINGLELINE));

            if (item.expanded) {
                RECT bodyRect{inner.left + 18, inner.top + body + 4, inner.right, rc.bottom - 6};
                fillRect(dc, bodyRect, bgApp);
                RECT padded = bodyRect;
                padded.left += 8;
                padded.right -= 8;
                padded.top += 6;
                drawText(dc, item.body, padded, textDim, fonts_.mono,
                         DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);
            }
            if (item.expandable) hits_.push_back({rc, Hit::Toggle, index});
            return;
        }

        case ItemKind::Approval: {
            const std::uint32_t borderColor = item.danger ? danger : accent;
            fillRoundRect(dc, rc, 8, bgCard, borderColor);
            RECT inner = rc;
            inner.left += 12;
            inner.right -= 12;
            inner.top += 8;
            inner.bottom -= 8;

            // 标题折行：路径是确认时最关键的信息，不能省略
            const int titleHeight = wrappedHeight(dc, item.title, fonts_.bold,
                                                  inner.right - inner.left);
            RECT titleRect{inner.left, inner.top, inner.right, inner.top + titleHeight};
            // 必须带 DT_EDITCONTROL：否则超长无空格路径不会断行，会被裁掉
            drawText(dc, item.title, titleRect, item.danger ? danger : accent, fonts_.bold,
                     DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL);
            inner.top += titleHeight + 8;

            if (item.expanded && !item.body.empty()) {
                RECT bodyRect{inner.left, inner.top, inner.right, rc.bottom - 6};
                fillRect(dc, bodyRect, bgApp);
                RECT padded = bodyRect;
                padded.left += 8;
                padded.right -= 8;
                padded.top += 6;
                padded.bottom = std::min(padded.bottom, padded.top + 150);
                drawText(dc, item.body, padded, textDim, fonts_.mono,
                         DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);
                inner.top = padded.bottom + 6;
            }

            if (item.awaitApproval) {
                RECT allow{inner.left, rc.bottom - kBtnH - 8, inner.left + kSmallBtnW,
                           rc.bottom - 8};
                RECT deny{allow.right + 8, allow.top, allow.right + 8 + kSmallBtnW, allow.bottom};
                drawButton(dc, allow, L"允许", action, 0xFFFFFF,
                           hover_ == Hit::Allow && hoverIndex_ == index);
                drawButton(dc, deny, L"拒绝", bgPanel, danger,
                           hover_ == Hit::Deny && hoverIndex_ == index);
                hits_.push_back({allow, Hit::Allow, index});
                hits_.push_back({deny, Hit::Deny, index});
            } else if (item.decided) {
                const std::wstring label = item.approved ? L"已允许" : L"已拒绝";
                drawText(dc, label,
                         RECT{inner.left, rc.bottom - cap - 8, inner.right, rc.bottom - 8},
                         item.approved ? success : danger, fonts_.caption,
                         DT_LEFT | DT_TOP | DT_SINGLELINE);
            }
            return;
        }
    }
}

// ---------------------------------------------------------------- 交互

bool ChatPanel::onMouseMove(POINT pt) {
    Hit hit = Hit::None;
    std::size_t index = 0;
    for (const HitRect& rect : hits_) {
        if (PtInRect(&rect.rect, pt)) {
            hit = rect.kind;
            index = rect.index;
            break;
        }
    }
    if (hit == hover_ && index == hoverIndex_) return false;
    hover_ = hit;
    hoverIndex_ = index;
    return true;
}

bool ChatPanel::onMouseDown(POINT pt) {
    for (const HitRect& rect : hits_) {
        if (PtInRect(&rect.rect, pt)) {
            pressed_ = rect.kind;
            pressedIndex_ = rect.index;
            return true;
        }
    }
    // 点击输入框：聚焦
    RECT composer{bounds_.left, bounds_.bottom - kComposerH, bounds_.right, bounds_.bottom};
    if (PtInRect(&composer, pt)) {
        inputFocused_ = true;
        return true;
    }
    inputFocused_ = false;
    return false;
}

bool ChatPanel::onMouseUp(POINT pt) {
    const Hit pressed = pressed_;
    const std::size_t pressedIndex = pressedIndex_;
    pressed_ = Hit::None;
    if (pressed == Hit::None) return false;

    for (const HitRect& rect : hits_) {
        if (!PtInRect(&rect.rect, pt)) continue;
        if (rect.kind != pressed || rect.index != pressedIndex) return false;
        switch (pressed) {
            case Hit::Send:
                if (inputEnabled_ && !input_.empty()) {
                    auto action = actions_.onSend;
                    std::wstring text = input_;
                    input_.clear();
                    caret_ = 0;
                    if (action) action(text);
                }
                return true;
            case Hit::Cancel:
                if (actions_.onCancel) actions_.onCancel();
                return true;
            case Hit::Settings:
                if (actions_.onSettings) actions_.onSettings();
                return true;
            case Hit::Clear:
                if (actions_.onClear) actions_.onClear();
                return true;
            case Hit::History:
                if (actions_.onHistory) actions_.onHistory();
                return true;
            case Hit::Host:
                if (actions_.onHost) actions_.onHost();
                return true;
            case Hit::Full:
                if (actions_.onToggleFullScreen) actions_.onToggleFullScreen();
                return true;
            case Hit::Close:
                if (actions_.onClose) actions_.onClose();
                return true;
            case Hit::WelcomeSettings:
                if (actions_.onWelcome)
                    actions_.onWelcome();
                else if (actions_.onSettings)
                    actions_.onSettings();
                return true;
            case Hit::Allow:
            case Hit::Deny: {
                if (rect.index >= items_.size()) return true;
                const bool allowed = pressed == Hit::Allow;
                resolveApproval(rect.index, allowed);
                if (actions_.onApproval) actions_.onApproval(rect.index, allowed);
                return true;
            }
            case Hit::Toggle:
                if (rect.index < items_.size()) {
                    items_[rect.index].expanded = !items_[rect.index].expanded;
                    layoutDirty_ = true;
                }
                return true;
            default: return false;
        }
    }
    return false;
}

bool ChatPanel::onMouseWheel(int delta) {
    const int maxScroll = std::max(0, contentHeight_ - viewportHeight_);
    if (maxScroll <= 0) return false;
    // delta > 0 = 滚轮向上 = 看更早的内容 = 离底部更远
    constexpr int kStep = 60;
    scrollBack_ = std::clamp(scrollBack_ + (delta * kStep) / WHEEL_DELTA, 0, maxScroll);
    stickBottom_ = scrollBack_ == 0;
    return true;
}

bool ChatPanel::onKeyDown(UINT vk, bool shift, bool ctrl) {
    if (!inputFocused_) return false;

    switch (vk) {
        case VK_BACK:
            if (caret_ > 0 && !input_.empty()) {
                input_.erase(caret_ - 1, 1);
                --caret_;
            }
            return true;
        case VK_DELETE:
            if (caret_ < input_.size()) input_.erase(caret_, 1);
            return true;
        case VK_LEFT:
            if (caret_ > 0) --caret_;
            return true;
        case VK_RIGHT:
            if (caret_ < input_.size()) ++caret_;
            return true;
        case VK_HOME:
            caret_ = 0;
            return true;
        case VK_END:
            caret_ = input_.size();
            return true;
        case VK_RETURN:
            if (shift) return true;  // 单行输入框：Shift+Enter 不发送
            if (!busy_ && inputEnabled_ && !input_.empty()) {
                auto action = actions_.onSend;
                std::wstring text = input_;
                input_.clear();
                caret_ = 0;
                if (action) action(text);
            }
            return true;
        case VK_ESCAPE:
            if (busy_ && actions_.onCancel) actions_.onCancel();
            return true;
        case 'V':
            if (!ctrl) return false;
            if (!OpenClipboard(nullptr)) return true;
            {
                HANDLE data = GetClipboardData(CF_UNICODETEXT);
                if (data) {
                    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
                    if (text) {
                        std::wstring pasted(text);
                        // 粘贴按单行处理：换行换成空格
                        std::replace(pasted.begin(), pasted.end(), L'\r', L' ');
                        std::replace(pasted.begin(), pasted.end(), L'\n', L' ');
                        input_.insert(caret_, pasted);
                        caret_ += pasted.size();
                        GlobalUnlock(data);
                    }
                }
                CloseClipboard();
            }
            return true;
        default: return false;
    }
}

bool ChatPanel::onChar(wchar_t ch) {
    if (!inputFocused_ || !inputEnabled_ || busy_) return false;
    if (ch < 0x20 || ch == 0x7F) return false;
    input_.insert(caret_, 1, ch);
    ++caret_;
    return true;
}

// ---------------------------------------------------------------- 离屏预览

namespace {

HFONT previewFont(int px, int weight, const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfHeight = -px;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, face);
    return CreateFontIndirectW(&lf);
}

bool writeBmp(const wchar_t* path, const void* bits, int width, int height) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const int rowBytes = width * 4;
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
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
    for (int row = height - 1; row >= 0; --row)
        out.write(pixels + static_cast<std::size_t>(row) * rowBytes, rowBytes);
    return out.good();
}

}  // namespace

bool renderPreview(const wchar_t* bmpPath, int width, int height, int mode) {
    if (width <= 0 || height <= 0) return false;
    ensureGdiplus();

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // 负值 = 自上而下，写入 BMP 时再倒序
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !bits) return false;

    HGDIOBJ oldBmp = SelectObject(mem, dib);

    PanelFonts fonts;
    fonts.ui = previewFont(14, FW_NORMAL, L"Segoe UI");
    fonts.bold = previewFont(14, FW_SEMIBOLD, L"Segoe UI");
    fonts.mono = previewFont(13, FW_NORMAL, L"Consolas");
    fonts.caption = previewFont(12, FW_NORMAL, L"Segoe UI");

    ChatPanel panel;
    panel.setFonts(fonts);
    panel.setBounds(RECT{0, 0, width, height});
    panel.setModelName(L"deepseek-chat");
    panel.setHostLabel(L"8.148.6.178", true);
    panel.setStatus(L"就绪");

    if (mode == 3) {
        // Markdown 压力测试：标题、行内样式、列表、引用、代码块、表格
        panel.addUser(L"帮我总结一下磁盘占用");
        const std::size_t index = panel.beginAssistant();
        panel.appendAssistant(
            index,
            L"## 结论\n\n根分区 **已用 92%**，主要是 `/var/lib/docker`。\n\n"
            L"### 建议\n\n1. 先清理悬空镜像\n2. 再压缩日志\n\n"
            L"- `docker system prune -af`\n- 压缩 `/var/log/*.gz`\n\n"
            L"> 删除前建议先 `docker images` 确认。\n\n"
            L"```bash\ndocker system df\njournalctl --vacuum-time=7d\n```\n\n"
            L"| 路径 | 占用 | 建议 |\n| --- | ---: | :---: |\n"
            L"| /var/lib/docker | 61 GB | 清理 |\n| /var/log | 8 GB | 轮转 |\n"
            L"| /home | 3 GB | 保留 |\n\n---\n\n处理完预计可释放 **约 40 GB**。");
        panel.finishAssistant(index, true);
        panel.paint(mem);
        const bool okMd = writeBmp(bmpPath, bits, width, height);
        SelectObject(mem, oldBmp);
        DeleteObject(dib);
        DeleteDC(mem);
        DeleteObject(fonts.ui);
        DeleteObject(fonts.bold);
        DeleteObject(fonts.mono);
        DeleteObject(fonts.caption);
        return okMd;
    }

    if (mode == 2) {
        // 长路径压力测试：单行标题、消息正文、审批卡各放一条超长无空格路径
        const std::wstring path =
            L"/var/lib/docker/overlay2/9f8a7c6b5d4e3f2a1b0c9d8e7f6a5b4c3d2e1f0a/diff/usr/local/lib/"
            L"python3.11/site-packages/some/deeply/nested/module/configuration_file.yaml";
        panel.addUser(L"帮我看看 " + path + L" 的配置");
        const std::size_t index = panel.beginAssistant();
        panel.appendAssistant(index, L"这个文件是 " + path + L"，里面配置了 upstream 地址。");
        panel.finishAssistant(index, true);
        panel.addTool(L"读取 " + path, L"server: 127.0.0.1:8080", true, L"1.2 KB");
        panel.addApproval(L"写入 " + path, L"- proxy_pass http://127.0.0.1:3000;\n+ proxy_pass http://127.0.0.1:8080;", false);
        panel.paint(mem);
        const bool okPath = writeBmp(bmpPath, bits, width, height);
        SelectObject(mem, oldBmp);
        DeleteObject(dib);
        DeleteDC(mem);
        DeleteObject(fonts.ui);
        DeleteObject(fonts.bold);
        DeleteObject(fonts.mono);
        DeleteObject(fonts.caption);
        return okPath;
    }

    if (mode == 1) {
        panel.setWelcome(L"AI 助手", L"还没有可用的 API Key。点「打开设置」填写；如果之前保存过，打开设置会先解锁保险库。", true);
        panel.paint(mem);
        const bool okWelcome = writeBmp(bmpPath, bits, width, height);
        SelectObject(mem, oldBmp);
        DeleteObject(dib);
        DeleteDC(mem);
        DeleteObject(fonts.ui);
        DeleteObject(fonts.bold);
        DeleteObject(fonts.mono);
        DeleteObject(fonts.caption);
        return okWelcome;
    }

    panel.addUser(L"nginx 一直返回 502，帮我查一下");
    const std::size_t answer = panel.beginAssistant();
    panel.appendReasoning(
        L"先确认 nginx 是否在运行，再检查 upstream 的端口是否有进程监听，"
        L"最后看错误日志里的具体报错。");
    panel.addTool(L"读取 /etc/nginx/sites-enabled/app.conf",
                  L"server {\n    listen 80;\n    location / {\n"
                  L"        proxy_pass http://127.0.0.1:3000;\n    }\n}",
                  true, L"1.2 KB");
    panel.addTool(L"systemctl is-active app", L"inactive", false, L"退出码 3");
    const std::size_t approval = panel.addApproval(
        L"写入 /etc/nginx/sites-enabled/app.conf",
        L"- proxy_pass http://127.0.0.1:3000;\n+ proxy_pass http://127.0.0.1:8080;",
        false);
    (void)approval;
    panel.appendAssistant(
        answer,
        L"502 的原因是 upstream 端口不通：app.conf 里转发到 3000，但应用服务已经停掉了。\n"
        L"建议先把应用拉起来（systemctl start app），确认端口后再决定是否改配置。");
    panel.finishAssistant(answer, true);
    panel.addNotice(L"提示：写操作会在卡片里显示原文，确认后才会执行。");

    panel.paint(mem);

    const bool ok = writeBmp(bmpPath, bits, width, height);

    SelectObject(mem, oldBmp);
    DeleteObject(dib);
    DeleteDC(mem);
    DeleteObject(fonts.ui);
    DeleteObject(fonts.bold);
    DeleteObject(fonts.mono);
    DeleteObject(fonts.caption);
    return ok;
}

// ---------------------------------------------------------------- 局部重绘区域

RECT ChatPanel::viewRect() const {
    return RECT{bounds_.left, bounds_.top + kHeaderH, bounds_.right,
                bounds_.bottom - kComposerH};
}

RECT ChatPanel::composerRect() const {
    return RECT{bounds_.left, bounds_.bottom - kComposerH, bounds_.right, bounds_.bottom};
}

// 最后一个气泡的「从它顶部到消息区底部」：流式追加只会往下长，
// 上面的历史消息一个像素都不会变，没必要跟着重画。
RECT ChatPanel::lastItemRect() const {
    const RECT view = viewRect();
    if (items_.empty() || heights_.size() != items_.size()) return view;
    int above = 0;
    for (std::size_t i = 0; i + 1 < heights_.size(); ++i) above += heights_[i] + kItemGap;
    const int viewport = view.bottom - view.top;
    const int y = contentHeight_ <= viewport ? view.top
                                             : view.bottom + scrollBack_ - contentHeight_;
    const int top = y + above;
    return RECT{view.left, std::max<int>(top, view.top), view.right, view.bottom};
}

}  // namespace rshell::ai
