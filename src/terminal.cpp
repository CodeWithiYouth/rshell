// terminal.cpp
#include "terminal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "util.h"

namespace rshell {

namespace {

std::uint32_t xtermColor(int index, const TerminalTheme& theme) {
    if (index < 16) return theme.palette[index];
    if (index < 232) {
        static const int steps[6] = {0, 95, 135, 175, 215, 255};
        int i = index - 16;
        int r = steps[i / 36];
        int g = steps[(i / 6) % 6];
        int b = steps[i % 6];
        return (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | std::uint32_t(b);
    }
    if (index < 256) {
        int v = 8 + (index - 232) * 10;
        return (std::uint32_t(v) << 16) | (std::uint32_t(v) << 8) | std::uint32_t(v);
    }
    return theme.fg;
}

// 该行是否完全空白（无字符、无自定义底色/反色）
bool isBlankLine(const std::vector<Cell>& line, const TerminalTheme& theme) {
    for (const Cell& c : line) {
        const char32_t ch = c.ch == 0 ? U' ' : c.ch;
        if (ch != U' ') return false;
        if ((c.flags & kFlagReverse) != 0 || c.fg != theme.fg || c.bg != theme.bg) return false;
    }
    return true;
}

// 单个格是否「什么都没画」（重排时用来丢掉折行处的行尾空白）。
// 带底色/反色的空格要保留：TUI 常用整行反色当状态栏。
bool isBlankCell(const Cell& c, const TerminalTheme& theme) {
    const char32_t ch = c.ch == 0 ? U' ' : c.ch;
    return ch == U' ' && (c.flags & kFlagReverse) == 0 && c.fg == theme.fg && c.bg == theme.bg;
}

struct Csi {
    char prefix = 0;
    std::string inter;
    std::vector<int> params;
    char final = 0;

    int param(std::size_t i, int def) const {
        if (i >= params.size() || params[i] < 0) return def;
        return params[i];
    }
};

Csi parseCsiParams(const std::string& body) {
    Csi csi;
    std::size_t i = 0;
    if (i < body.size() && (body[i] == '?' || body[i] == '>' || body[i] == '<' || body[i] == '=')) {
        csi.prefix = body[i++];
    }
    std::string num;
    auto flush = [&]() {
        if (num.empty()) {
            csi.params.push_back(-1);
        } else {
            try {
                csi.params.push_back(std::stoi(num));
            } catch (...) {
                csi.params.push_back(-1);
            }
            num.clear();
        }
    };
    for (; i < body.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(body[i]);
        if (ch >= '0' && ch <= '9') {
            num.push_back(static_cast<char>(ch));
        } else if (ch == ';' || ch == ':') {
            flush();
        } else if (ch >= 0x20 && ch <= 0x2F) {
            csi.inter.push_back(static_cast<char>(ch));
        } else {
            csi.final = static_cast<char>(ch);
        }
    }
    if (!num.empty()) flush();
    return csi;
}

// 解析 38/48/58 的扩展颜色；成功时 i 前进到颜色参数之后
bool parseExtendedColor(const std::vector<int>& params, std::size_t& i, const TerminalTheme& theme,
                        std::uint32_t& out) {
    int mode = (i < params.size()) ? params[i] : -1;
    if (mode == 5) {
        if (i + 1 >= params.size()) return false;
        out = xtermColor(params[i + 1], theme);
        i += 2;
        return true;
    }
    if (mode == 2) {
        if (i + 3 >= params.size()) return false;
        auto comp = [](int v) { return std::uint32_t(std::clamp(v, 0, 255)); };
        out = (comp(params[i + 1]) << 16) | (comp(params[i + 2]) << 8) | comp(params[i + 3]);
        i += 4;
        return true;
    }
    return false;
}

}  // namespace

Terminal::Terminal(int cols, int rows, std::size_t maxScrollback)
    : cols_(std::max(2, cols)), rows_(std::max(1, rows)), maxScrollback_(maxScrollback) {
    fg_ = theme_.fg;
    bg_ = theme_.bg;
    reset();
}

bool Terminal::isWideChar(char32_t ch) { return charWidth(ch) == 2; }

int Terminal::charWidth(char32_t ch) {
    if (ch == 0) return 0;
    // 组合用附加符号：宽度 0
    if ((ch >= 0x0300 && ch <= 0x036F) || (ch >= 0x1AB0 && ch <= 0x1AFF) ||
        (ch >= 0x20D0 && ch <= 0x20FF) || (ch >= 0xFE00 && ch <= 0xFE0F) ||
        (ch >= 0xFE20 && ch <= 0xFE2F) || ch == 0x200B || ch == 0x200C || ch == 0x200D ||
        ch == 0xFEFF || (ch >= 0xE0100 && ch <= 0xE01EF)) {
        return 0;
    }
    // 东亚宽字符
    if (ch < 0x1100) return 1;
    // 表情符号（Emoji_Presentation=Yes，多数终端按两列渲染）
    if ((ch >= 0x231A && ch <= 0x231B) || (ch >= 0x23E9 && ch <= 0x23EC) || ch == 0x23F0 ||
        ch == 0x23F3 || (ch >= 0x25FD && ch <= 0x25FE) || (ch >= 0x2614 && ch <= 0x2615) ||
        (ch >= 0x2648 && ch <= 0x2653) || ch == 0x267F || ch == 0x2693 || ch == 0x26A1 ||
        (ch >= 0x26AA && ch <= 0x26AB) || (ch >= 0x26BD && ch <= 0x26BE) ||
        (ch >= 0x26C4 && ch <= 0x26C5) || ch == 0x26CE || ch == 0x26D4 || ch == 0x26EA ||
        (ch >= 0x26F2 && ch <= 0x26F3) || ch == 0x26F5 || ch == 0x26FA || ch == 0x26FD ||
        ch == 0x2705 || (ch >= 0x270A && ch <= 0x270B) || ch == 0x2728 || ch == 0x274C ||
        ch == 0x274E || (ch >= 0x2753 && ch <= 0x2755) || ch == 0x2757 ||
        (ch >= 0x2795 && ch <= 0x2797) || ch == 0x27B0 || ch == 0x27BF ||
        (ch >= 0x2B1B && ch <= 0x2B1C) || ch == 0x2B50 || ch == 0x2B55 || ch == 0x1F004 ||
        ch == 0x1F0CF || ch == 0x1F18E || (ch >= 0x1F191 && ch <= 0x1F19A) ||
        (ch >= 0x1F1E6 && ch <= 0x1F1FF) || ch == 0x1F201 || ch == 0x1F21A ||
        (ch >= 0x1F232 && ch <= 0x1F23A) || (ch >= 0x1F250 && ch <= 0x1F251) ||
        (ch >= 0x1F300 && ch <= 0x1F64F) || (ch >= 0x1F680 && ch <= 0x1F6FF) ||
        (ch >= 0x1F7E0 && ch <= 0x1F7EB) || (ch >= 0x1F900 && ch <= 0x1F9FF) ||
        (ch >= 0x1FA70 && ch <= 0x1FAFF)) {
        return 2;
    }
    if ((ch >= 0x1100 && ch <= 0x115F) || (ch >= 0x2E80 && ch <= 0x303E) ||
        (ch >= 0x3041 && ch <= 0x33FF) || (ch >= 0x3400 && ch <= 0x4DBF) ||
        (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0xA000 && ch <= 0xA4CF) ||
        (ch >= 0xA960 && ch <= 0xA97F) || (ch >= 0xAC00 && ch <= 0xD7A3) ||
        (ch >= 0xF900 && ch <= 0xFAFF) || (ch >= 0xFE10 && ch <= 0xFE19) ||
        (ch >= 0xFE30 && ch <= 0xFE6F) || (ch >= 0xFF00 && ch <= 0xFF60) ||
        (ch >= 0xFFE0 && ch <= 0xFFE6) || (ch >= 0x20000 && ch <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

Cell Terminal::blankCell() const {
    Cell c;
    c.ch = U' ';
    c.fg = theme_.fg;
    c.bg = theme_.bg;
    c.flags = 0;
    return c;
}

void Terminal::blankLine(std::vector<Cell>& line) const {
    Cell blank = blankCell();
    line.assign(static_cast<std::size_t>(cols_), blank);
}

void Terminal::ensureScreen() {
    screen_.resize(static_cast<std::size_t>(rows_));
    cont_.resize(screen_.size(), 0);
    for (auto& line : screen_) {
        line.resize(static_cast<std::size_t>(cols_), blankCell());
    }
}

void Terminal::reset() {
    screen_.clear();
    cont_.clear();
    history_.clear();
    histCont_.clear();
    altSaved_.clear();
    altScreen_ = false;
    curRow_ = 0;
    curCol_ = 0;
    savedRow_ = 0;
    savedCol_ = 0;
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    wrapPending_ = false;
    cursorVisible_ = true;
    insertMode_ = false;
    bracketedPaste_ = false;
    autoWrap_ = true;
    scrollOffset_ = 0;
    decCursorKeys_ = false;
    focusEvents_ = false;
    mouseMode_ = MouseMode::None;
    mouseSgr_ = false;
    mouseUtf8_ = false;
    mouseUrxvt_ = false;
    g0Charset_ = 'B';
    g1Charset_ = 'B';
    useG1_ = false;
    charsetSlot_ = '(';
    lastPrinted_ = U' ';
    fg_ = theme_.fg;
    bg_ = theme_.bg;
    flags_ = 0;
    state_ = State::Ground;
    csi_.clear();
    osc_.clear();
    utf8Remain_ = 0;
    utf8Pending_ = 0;
    title_.clear();
    cwd_.clear();
    cwdChanged_ = false;
    shellIntegrationAcks_.clear();
    response_.clear();
    ensureScreen();
    dirty_ = true;
}

void Terminal::restoreDisplay(Terminal&& snapshot) {
    const int columns = cols_, lines = rows_;
    std::string cwd = std::move(cwd_), title = std::move(title_), response = std::move(response_);
    auto acks = std::move(shellIntegrationAcks_);
    const bool changed = cwdChanged_;
    *this = std::move(snapshot);
    cwd_ = std::move(cwd); title_ = std::move(title); response_ = std::move(response);
    shellIntegrationAcks_ = std::move(acks);
    cwdChanged_ = changed;
    resize(columns, lines);
    dirty_ = true;
}

void Terminal::resize(int cols, int rows) {
    cols = std::max(2, cols);
    rows = std::max(1, rows);
    if (cols == cols_ && rows == rows_) return;

    // 列宽变化（AI 面板停靠、窗口拉宽/收窄、字号变化）要重新折行，
    // 不能像以前那样把每行截到新宽度——那会把已有输出永久吃掉。
    // 备用屏（TUI）不重排：全屏程序会自己重画，硬折行反而会把画面弄乱。
    if (cols != cols_ && !altScreen_) {
        reflowResize(cols, rows);
        return;
    }

    // 屏幕行从底部对齐保留，被顶出的行进入历史
    std::vector<std::vector<Cell>> old = std::move(screen_);
    screen_.clear();
    const int oldRows = static_cast<int>(old.size());
    if (cont_.size() != old.size()) cont_.resize(old.size(), 0);
    std::vector<char> oldCont = std::move(cont_);
    cont_.clear();
    if (rows < oldRows) {
        const int drop = oldRows - rows;
        // 先吃掉光标下方的空白行，再考虑把顶部行推进历史：
        // 最大化 → 还原时行数会大幅变化，若一律从顶部裁，
        // 少量输出会被整段推入历史，屏幕看起来像被清空了。
        int trailingBlank = 0;
        for (int i = oldRows - 1; i > curRow_ && trailingBlank < drop; --i) {
            if (!isBlankLine(old[static_cast<std::size_t>(i)], theme_)) break;
            ++trailingBlank;
        }
        const int dropBottom = std::min(drop, trailingBlank);
        const int dropTop = drop - dropBottom;
        for (int i = 0; i < dropTop; ++i) {
            const std::vector<Cell>& line = old[static_cast<std::size_t>(i)];
            // 纯空白行不必进历史，否则滚动条位置完全失真
            if (!altScreen_ && !isBlankLine(line, theme_)) {
                history_.push_back(line);
                histCont_.push_back(i < static_cast<int>(oldCont.size()) ? oldCont[static_cast<std::size_t>(i)] : 0);
            }
        }
        for (int i = dropTop; i < oldRows - dropBottom; ++i) {
            screen_.push_back(old[static_cast<std::size_t>(i)]);
            cont_.push_back(i < static_cast<int>(oldCont.size()) ? oldCont[static_cast<std::size_t>(i)] : 0);
        }
        curRow_ = std::max(0, curRow_ - dropTop);
    } else {
        screen_ = std::move(old);
        for (int i = oldRows; i < rows; ++i) screen_.emplace_back();
        cont_ = std::move(oldCont);
    }
    cols_ = cols;
    rows_ = rows;
    for (auto& line : screen_) {
        line.resize(static_cast<std::size_t>(cols_), blankCell());
    }
    cont_.resize(screen_.size(), 0);
    while (history_.size() > maxScrollback_) {
        history_.pop_front();
        if (!histCont_.empty()) histCont_.pop_front();
    }

    curRow_ = std::clamp(curRow_, 0, rows_ - 1);
    curCol_ = std::clamp(curCol_, 0, cols_ - 1);
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    wrapPending_ = false;
    scrollOffset_ = 0;
    dirty_ = true;
}

// 列宽变化时的重排：按「逻辑行」（被软换行拆开的行）重新折行。
// 屏幕和回滚历史一起重排，所以变窄时不会丢字，变宽后能并回原来的一行。
void Terminal::reflowResize(int cols, int rows) {
    if (cont_.size() != screen_.size()) cont_.resize(screen_.size(), 0);
    if (histCont_.size() != history_.size()) histCont_.resize(history_.size(), 0);
    const int oldCols = cols_;

    // 拖窗口边缘时 WM_SIZE 会连着来，历史全量重排太慢（几千行 × 每列一格）。
    // 只重排最近的一段历史，更老的保持原样——它们本来也只是显示，不会再回卷。
    constexpr std::size_t kReflowHistoryRows = 1500;
    const std::size_t keepHistory =
        history_.size() > kReflowHistoryRows ? history_.size() - kReflowHistoryRows : 0;

    std::vector<std::vector<Cell>> merged;  // 重排后的连续行（先历史、后屏幕）
    std::vector<char> mergedCont;
    std::vector<Cell> line;                 // 正在拼的逻辑行
    bool inLine = false;
    int lineStart = 0;
    bool cursorSeen = false;
    std::size_t cursorOffset = 0;
    int cursorNewRow = 0;
    int cursorNewCol = 0;

    auto flushLine = [&]() {
        if (!inLine) return;
        // 逻辑行末尾的默认色空白没有意义，留着会凭空多折出几行
        while (!line.empty() && isBlankCell(line.back(), theme_)) line.pop_back();
        const int length = std::max<int>(1, static_cast<int>(line.size()));
        const int parts = (length + cols - 1) / cols;
        for (int k = 0; k < parts; ++k) {
            std::vector<Cell> row(static_cast<std::size_t>(cols), blankCell());
            for (int c = 0; c < cols; ++c) {
                const std::size_t index = static_cast<std::size_t>(k) * cols + c;
                if (index < line.size()) row[static_cast<std::size_t>(c)] = line[index];
            }
            merged.push_back(std::move(row));
            // 约定：标记表示「本行是上一行的续行」，所以只有第一段是 0
            mergedCont.push_back(k > 0 ? 1 : 0);
        }
        if (cursorSeen) {
            cursorNewRow = lineStart + static_cast<int>(cursorOffset / static_cast<std::size_t>(cols));
            cursorNewCol = static_cast<int>(cursorOffset % static_cast<std::size_t>(cols));
            cursorSeen = false;  // 光标只会落在一条逻辑行里
        }
        line.clear();
        inLine = false;
    };

    auto feedRow = [&](const std::vector<Cell>& src, bool cont, bool cursorRow) {
        if (!cont || !inLine) {
            flushLine();
            inLine = true;
            lineStart = static_cast<int>(merged.size());
        } else {
            // 折行处的行尾空白没有意义，去掉再拼，否则中间会插进一串空格
            while (!line.empty() && isBlankCell(line.back(), theme_)) line.pop_back();
        }
        if (cursorRow) {
            cursorSeen = true;
            cursorOffset =
                line.size() + static_cast<std::size_t>(std::clamp(curCol_, 0, oldCols - 1));
        }
        for (int c = 0; c < oldCols; ++c) {
            line.push_back(c < static_cast<int>(src.size()) ? src[static_cast<std::size_t>(c)]
                                                           : blankCell());
        }
    };

    for (std::size_t i = keepHistory; i < history_.size(); ++i)
        feedRow(history_[i], i > keepHistory && histCont_[i] != 0, false);
    for (int r = 0; r < static_cast<int>(screen_.size()); ++r)
        feedRow(screen_[static_cast<std::size_t>(r)], cont_[static_cast<std::size_t>(r)] != 0,
                r == curRow_);
    flushLine();

    const int total = static_cast<int>(merged.size());

    // 光标下面还有空行就先吃掉它们（和行数变化时的处理保持一致），
    // 否则面板一停靠就会把整屏内容顶进历史，看起来像“跳屏”。
    int deficit = total - rows;
    if (deficit > 0) {
        for (int i = total - 1; i > cursorNewRow && deficit > 0; --i) {
            if (!isBlankLine(merged[static_cast<std::size_t>(i)], theme_)) continue;
            merged.erase(merged.begin() + i);
            mergedCont.erase(mergedCont.begin() + i);
            --deficit;
        }
    }
    const int screenStart = std::max(0, static_cast<int>(merged.size()) - rows);

    // 顶部多出来的行回到历史里；纯空白行照旧不进历史（否则滚动条完全失真），
    // 丢掉空白行时顺带清掉下一行的续行标记，避免下次重排误并。
    std::deque<std::vector<Cell>> keptHistory(history_.begin(),
                                              history_.begin() + static_cast<std::ptrdiff_t>(keepHistory));
    std::deque<char> keptHistoryCont(histCont_.begin(),
                                     histCont_.begin() + static_cast<std::ptrdiff_t>(keepHistory));
    history_ = std::move(keptHistory);
    histCont_ = std::move(keptHistoryCont);
    bool droppedPrevious = keepHistory > 0;  // 分段处当作硬换行
    for (int i = 0; i < screenStart; ++i) {
        const bool blank = isBlankLine(merged[static_cast<std::size_t>(i)], theme_);
        if (blank) {
            droppedPrevious = true;
            continue;
        }
        history_.push_back(merged[static_cast<std::size_t>(i)]);
        histCont_.push_back(droppedPrevious ? 0 : mergedCont[static_cast<std::size_t>(i)]);
        droppedPrevious = false;
    }

    std::vector<std::vector<Cell>> out;
    std::vector<char> outCont;
    for (int i = screenStart; i < static_cast<int>(merged.size()); ++i) {
        out.push_back(merged[static_cast<std::size_t>(i)]);
        outCont.push_back(mergedCont[static_cast<std::size_t>(i)]);
    }
    while (static_cast<int>(out.size()) < rows) {
        out.emplace_back(static_cast<std::size_t>(cols), blankCell());
        outCont.push_back(0);
    }
    while (static_cast<int>(history_.size()) > static_cast<int>(maxScrollback_)) {
        history_.pop_front();
        if (!histCont_.empty()) histCont_.pop_front();
    }

    screen_ = std::move(out);
    cont_ = std::move(outCont);
    cols_ = cols;
    rows_ = rows;
    curRow_ = std::clamp(cursorNewRow - screenStart, 0, rows_ - 1);
    curCol_ = std::clamp(cursorNewCol, 0, cols_ - 1);
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    wrapPending_ = false;
    scrollOffset_ = 0;
    dirty_ = true;
}
const std::vector<Cell>& Terminal::lineAt(std::size_t index) const {
    static const std::vector<Cell> empty;
    if (index < history_.size()) return history_[index];
    std::size_t i = index - history_.size();
    if (i < screen_.size()) return screen_[i];
    return empty;
}

std::string Terminal::plainLine(std::size_t index) const {
    const std::vector<Cell>& line = lineAt(index);
    std::string out;
    for (const Cell& c : line) {
        if (c.flags & kFlagWideTail) continue;
        char32_t ch = c.ch == 0 ? U' ' : c.ch;
        if (ch < 0x80) {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else if (ch < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (ch >> 18)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::vector<TerminalMatch> Terminal::findAll(const std::u32string& query) const {
    std::vector<TerminalMatch> matches;
    if (query.empty()) return matches;
    std::u32string text;
    std::vector<int> columns;
    for (std::size_t line = 0; line < lineCount(); ++line) {
        text.clear();
        columns.clear();
        const auto& cells = lineAt(line);
        for (int col = 0; col < static_cast<int>(cells.size()); ++col) {
            const Cell& cell = cells[static_cast<std::size_t>(col)];
            if (cell.flags & kFlagWideTail) continue;
            text.push_back(cell.ch == 0 ? U' ' : cell.ch);
            columns.push_back(col);
        }
        // 去掉未使用的行尾空格，避免查找空格时把整片空屏当成结果。
        while (!text.empty() && text.back() == U' ') {
            text.pop_back();
            columns.pop_back();
        }
        for (std::size_t pos = text.find(query); pos != std::u32string::npos;
             pos = text.find(query, pos + 1)) {
            const int first = columns[pos];
            const std::size_t last = pos + query.size() - 1;
            const int end = columns[last] + ((cells[columns[last]].flags & kFlagWide) ? 2 : 1);
            matches.push_back(TerminalMatch{line, first, end - first});
        }
    }
    return matches;
}

void Terminal::scrollView(int deltaLines) {
    int maxOffset = static_cast<int>(history_.size());
    scrollOffset_ = std::clamp(scrollOffset_ + deltaLines, 0, maxOffset);
    dirty_ = true;
}

void Terminal::noteOutput() {
    if (scrollOffset_ > 0) scrollOffset_ = 0;
    dirty_ = true;
}

std::string Terminal::takeResponse() {
    std::string out;
    out.swap(response_);
    return out;
}

std::string Terminal::textRange(std::size_t startLine, int startCol, std::size_t endLine,
                                int endCol) const {
    if (startLine > endLine) {
        std::swap(startLine, endLine);
        std::swap(startCol, endCol);
    }
    std::string out;
    for (std::size_t line = startLine; line <= endLine; ++line) {
        const std::vector<Cell>& cells = lineAt(line);
        int from = (line == startLine) ? std::max(0, startCol) : 0;
        int to = (line == endLine) ? std::min(static_cast<int>(cells.size()), endCol)
                                   : static_cast<int>(cells.size());
        std::string row;
        for (int i = from; i < to; ++i) {
            const Cell& c = cells[static_cast<std::size_t>(i)];
            if (c.flags & kFlagWideTail) continue;
            char32_t ch = c.ch == 0 ? U' ' : c.ch;
            if (ch < 0x80) {
                row.push_back(static_cast<char>(ch));
            } else if (ch < 0x800) {
                row.push_back(static_cast<char>(0xC0 | (ch >> 6)));
                row.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
            } else if (ch < 0x10000) {
                row.push_back(static_cast<char>(0xE0 | (ch >> 12)));
                row.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
                row.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
            } else {
                row.push_back(static_cast<char>(0xF0 | (ch >> 18)));
                row.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
                row.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
                row.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
            }
        }
        while (!row.empty() && row.back() == ' ') row.pop_back();
        out += row;
        if (line != endLine) out.push_back('\n');
    }
    return out;
}

void Terminal::decodeUtf8(unsigned char byte) {
    if (utf8Remain_ == 0) {
        if (byte < 0x80) {
            putChar(static_cast<char32_t>(byte));
        } else if ((byte & 0xE0) == 0xC0) {
            utf8Pending_ = byte & 0x1F;
            utf8Remain_ = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            utf8Pending_ = byte & 0x0F;
            utf8Remain_ = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            utf8Pending_ = byte & 0x07;
            utf8Remain_ = 3;
        }
        // 非法起始字节直接丢弃
        return;
    }
    if ((byte & 0xC0) != 0x80) {
        // 非法续字节：丢弃当前序列，重新解析本字节
        utf8Remain_ = 0;
        decodeUtf8(byte);
        return;
    }
    utf8Pending_ = (utf8Pending_ << 6) | (byte & 0x3F);
    if (--utf8Remain_ == 0) {
        putChar(utf8Pending_);
        utf8Pending_ = 0;
    }
}

void Terminal::feed(const std::string& data) {
    for (unsigned char b : data) {
        switch (state_) {
            case State::Ground:
                if (b == 0x1B) {
                    state_ = State::Escape;
                } else if (b < 0x20) {
                    executeControl(b);
                } else if (b != 0x7F) {
                    decodeUtf8(b);
                }
                break;
            case State::Escape:
                utf8Remain_ = 0;
                if (b == '[') {
                    state_ = State::Csi;
                    csi_.clear();
                } else if (b == ']') {
                    state_ = State::Osc;
                    osc_.clear();
                    escPending_ = false;
                } else if (b == 'P') {
                    state_ = State::Dcs;
                    escPending_ = false;
                } else if (b == '_' || b == '^' || b == 'X') {
                    // APC（含 Kitty 图形探测）、PM、SOS：暂不支持的控制字符串
                    // 整段跳过，不缓存负载，直到 ST；不能把 G/参数/base64 当成文本。
                    state_ = State::IgnoredString;
                    escPending_ = false;
                } else if (b == '(' || b == ')' || b == '*' || b == '+' || b == '-' ||
                           b == '.' || b == '/') {
                    charsetSlot_ = static_cast<char>(b);
                    state_ = State::Charset;
                } else {
                    state_ = State::Ground;
                    switch (b) {
                        case '7':
                            saveCursor();
                            break;
                        case '8':
                            restoreCursor();
                            break;
                        case 'D':
                            lineFeed();
                            break;
                        case 'E':
                            carriageReturn();
                            lineFeed();
                            break;
                        case 'M':
                            reverseLineFeed();
                            break;
                        case 'c':
                            reset();
                            break;
                        default:
                            break;  // 键区模式、制表位等忽略
                    }
                }
                break;
            case State::Charset:
                if (charsetSlot_ == ')') {
                    g1Charset_ = static_cast<char>(b);
                } else if (charsetSlot_ == '(') {
                    g0Charset_ = static_cast<char>(b);
                }
                state_ = State::Ground;
                break;
            case State::IgnoredString:
                if (b == 0x18 || b == 0x1A || (escPending_ && b == '\\')) {
                    state_ = State::Ground;
                    escPending_ = false;
                } else {
                    escPending_ = b == 0x1B;
                }
                break;
            case State::Dcs:
                if (b == 0x1B) {
                    escPending_ = true;
                } else if (b == 0x07 || (escPending_ && b == '\\')) {
                    state_ = State::Ground;
                    escPending_ = false;
                } else if (escPending_) {
                    escPending_ = false;
                }
                break;
            case State::Osc:
                if (b == 0x07) {
                    finishOsc();
                    state_ = State::Ground;
                } else if (b == 0x1B) {
                    escPending_ = true;
                } else if (escPending_) {
                    if (b == '\\') {
                        finishOsc();
                        state_ = State::Ground;
                    }
                    escPending_ = false;
                } else if (osc_.size() < 512) {
                    osc_.push_back(static_cast<char>(b));
                }
                break;
            case State::Csi:
                if (b == 0x1B || b == 0x18 || b == 0x1A) {
                    state_ = State::Ground;
                    csi_.clear();
                } else if (b >= 0x40 && b <= 0x7E) {
                    csi_.push_back(static_cast<char>(b));
                    parseCsi();
                } else if (csi_.size() < 128) {
                    csi_.push_back(static_cast<char>(b));
                }
                break;
        }
    }
    if (state_ == State::Ground) utf8Remain_ = 0;
}

// OSC 7 的路径按 URL 规则编码（空格、中文等会写成 %XX），这里只解 %XX，
// 其余字节原样保留：shell 集成脚本通常直接发未编码的 UTF-8 路径。
std::string percentDecode(const std::string& text) {
    auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex(text[i + 1]);
            const int lo = hex(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

void Terminal::finishOsc() {
    const std::string ackPrefix = "777;rshell-cwd;";
    if (osc_.compare(0, ackPrefix.size(), ackPrefix) == 0) {
        if (shellIntegrationAcks_.size() < 8)
            shellIntegrationAcks_.push_back(osc_.substr(ackPrefix.size()));
        return;
    }
    // 窗口标题：OSC 0 ; title / OSC 2 ; title
    if (osc_.size() > 2 && (osc_[0] == '0' || osc_[0] == '2') && osc_[1] == ';') {
        title_ = osc_.substr(2);
        return;
    }
    // 工作目录：OSC 7 ; file://<host>/<path>，shell 每次画提示符时上报
    // （GUI 侧注入了 PROMPT_COMMAND / precmd 钩子，见 gui_main.cpp）。
    static const char kPrefix[] = "7;file://";
    const std::size_t prefixLen = sizeof(kPrefix) - 1;
    if (osc_.size() > prefixLen && osc_.compare(0, prefixLen, kPrefix) == 0) {
        const std::string rest = osc_.substr(prefixLen);
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos) return;  // file:// 后面没有路径
        std::string path = percentDecode(rest.substr(slash));
        if (path.empty() || path[0] != '/') return;
        path = normalizeRemotePath(path);
        if (path != cwd_) {
            cwd_ = path;
            cwdChanged_ = true;
        }
    }
}

void Terminal::executeControl(unsigned char byte) {
    switch (byte) {
        case 0x08:
            backspace();
            break;
        case 0x09:
            tab();
            break;
        case 0x0E:  // SO：切换到 G1
            useG1_ = true;
            break;
        case 0x0F:  // SI：切换回 G0
            useG1_ = false;
            break;
        case 0x0A:
        case 0x0B:
        case 0x0C:
            lineFeed();
            break;
        case 0x0D:
            carriageReturn();
            break;
        default:
            break;  // BEL / SO / SI 等忽略
    }
}

char32_t Terminal::mapCharset(char32_t ch) const {
    const char active = useG1_ ? g1Charset_ : g0Charset_;
    if (active != '0' || ch >= 0x7F) return ch;
    // DEC Special Graphics（ESC ( 0）
    switch (static_cast<int>(ch)) {
        case '_': return U' ';
        case '`': return 0x25C6;  // ◆
        case 'a': return 0x2592;  // ▒
        case 'b': return 0x2409;
        case 'c': return 0x240C;
        case 'd': return 0x240D;
        case 'e': return 0x240A;
        case 'f': return 0x00B0;  // °
        case 'g': return 0x00B1;  // ±
        case 'h': return 0x2424;
        case 'i': return 0x240B;
        case 'j': return 0x2518;  // ┘
        case 'k': return 0x2510;  // ┐
        case 'l': return 0x250C;  // ┌
        case 'm': return 0x2514;  // └
        case 'n': return 0x253C;  // ┼
        case 'o': return 0x23BA;
        case 'p': return 0x23BB;
        case 'q': return 0x2500;  // ─
        case 'r': return 0x23BC;
        case 's': return 0x23BD;
        case 't': return 0x251C;  // ├
        case 'u': return 0x2524;  // ┤
        case 'v': return 0x2534;  // ┴
        case 'w': return 0x252C;  // ┬
        case 'x': return 0x2502;  // │
        case 'y': return 0x2264;  // ≤
        case 'z': return 0x2265;  // ≥
        case '{': return 0x03C0;  // π
        case '|': return 0x2260;  // ≠
        case '}': return 0x00A3;  // £
        case '~': return 0x00B7;  // ·
        default: return ch;
    }
}

void Terminal::putChar(char32_t ch) {
    ch = mapCharset(ch);
    lastPrinted_ = ch;
    int width = charWidth(ch);
    if (width == 0) {
        // U+FE0F：把前一个字符升级为双宽（emoji 呈现），保持列对齐
        if (ch == 0xFE0F && curCol_ > 0) {
            int prev = curCol_ - 1;
            if (prev >= 0 && prev < static_cast<int>(screen_[static_cast<std::size_t>(curRow_)].size())) {
                std::vector<Cell>& line = screen_[static_cast<std::size_t>(curRow_)];
                Cell& base = line[static_cast<std::size_t>(prev)];
                if (!(base.flags & kFlagWide) && !(base.flags & kFlagWideTail) && curCol_ < cols_) {
                    Cell& next = line[static_cast<std::size_t>(curCol_)];
                    if (next.ch == U' ') {
                        base.flags = static_cast<std::uint8_t>(base.flags | kFlagWide);
                        next.ch = U' ';
                        next.fg = base.fg;
                        next.bg = base.bg;
                        next.flags = static_cast<std::uint8_t>(base.flags | kFlagWideTail);
                        ++curCol_;
                        dirty_ = true;
                    }
                }
            }
        }
        return;  // 其它组合字符暂不支持叠加
    }

    if (wrapPending_) {
        carriageReturn();
        lineFeed();
        wrapPending_ = false;
        markContinuation(curRow_);  // 新的一行是上一行折下来的
    }
    if (width == 2 && curCol_ + 1 >= cols_) {
        carriageReturn();
        lineFeed();
        markContinuation(curRow_);
    }
    if (insertMode_) insertCells(curRow_, curCol_, width);

    std::vector<Cell>& row = screen_[static_cast<std::size_t>(curRow_)];
    if (curCol_ < cols_) {
        Cell& cell = row[static_cast<std::size_t>(curCol_)];
        cell.ch = ch;
        cell.fg = fg_;
        cell.bg = bg_;
        cell.flags = static_cast<std::uint8_t>(flags_ | (width == 2 ? kFlagWide : 0));
        if (width == 2 && curCol_ + 1 < cols_) {
            Cell& tail = row[static_cast<std::size_t>(curCol_ + 1)];
            tail.ch = U' ';
            tail.fg = fg_;
            tail.bg = bg_;
            tail.flags = static_cast<std::uint8_t>(flags_ | kFlagWideTail);
        }
    }
    curCol_ += width;
    if (curCol_ >= cols_) {
        curCol_ = cols_ - 1;
        if (autoWrap_) wrapPending_ = true;
    }
    dirty_ = true;
}

void Terminal::lineFeed() {
    wrapPending_ = false;
    if (curRow_ == scrollBottom_) {
        scrollRegionUp(scrollTop_, scrollBottom_, 1);
    } else if (curRow_ < rows_ - 1) {
        ++curRow_;
    }
    dirty_ = true;
}

void Terminal::reverseLineFeed() {
    wrapPending_ = false;
    if (curRow_ == scrollTop_) {
        scrollRegionDown(scrollTop_, scrollBottom_, 1);
    } else if (curRow_ > 0) {
        --curRow_;
    }
    dirty_ = true;
}

void Terminal::carriageReturn() {
    curCol_ = 0;
    wrapPending_ = false;
}

void Terminal::backspace() {
    wrapPending_ = false;
    if (curCol_ > 0) --curCol_;
}

void Terminal::tab() {
    int next = ((curCol_ / 8) + 1) * 8;
    curCol_ = std::min(next, cols_ - 1);
    wrapPending_ = false;
}

void Terminal::scrollRegionUp(int top, int bottom, int count) {
    if (top < 0 || bottom >= rows_ || top > bottom) return;
    count = std::clamp(count, 0, bottom - top + 1);
    for (int i = 0; i < count; ++i) {
        if (!altScreen_ && top == 0 && bottom == rows_ - 1) {
            history_.push_back(screen_[0]);
            histCont_.push_back(cont_.empty() ? 0 : cont_[0]);
            if (history_.size() > maxScrollback_) {
                history_.pop_front();
                if (!histCont_.empty()) histCont_.pop_front();
                if (scrollOffset_ > 0) --scrollOffset_;
            }
        }
        for (int r = top; r < bottom; ++r) {
            screen_[static_cast<std::size_t>(r)] = std::move(screen_[static_cast<std::size_t>(r + 1)]);
            cont_[static_cast<std::size_t>(r)] = cont_[static_cast<std::size_t>(r + 1)];
        }
        blankLine(screen_[static_cast<std::size_t>(bottom)]);
        cont_[static_cast<std::size_t>(bottom)] = 0;
    }
    dirty_ = true;
}

void Terminal::scrollRegionDown(int top, int bottom, int count) {
    if (top < 0 || bottom >= rows_ || top > bottom) return;
    count = std::clamp(count, 0, bottom - top + 1);
    for (int i = 0; i < count; ++i) {
        for (int r = bottom; r > top; --r) {
            screen_[static_cast<std::size_t>(r)] = std::move(screen_[static_cast<std::size_t>(r - 1)]);
            cont_[static_cast<std::size_t>(r)] = cont_[static_cast<std::size_t>(r - 1)];
        }
        blankLine(screen_[static_cast<std::size_t>(top)]);
        cont_[static_cast<std::size_t>(top)] = 0;
    }
    dirty_ = true;
}

void Terminal::eraseCells(int row, int from, int to) {
    if (row < 0 || row >= rows_) return;
    std::vector<Cell>& line = screen_[static_cast<std::size_t>(row)];
    from = std::clamp(from, 0, cols_);
    to = std::clamp(to, 0, cols_);
    Cell blank = blankCell();
    blank.bg = bg_;
    for (int i = from; i < to; ++i) line[static_cast<std::size_t>(i)] = blank;
    dirty_ = true;
}

void Terminal::insertCells(int row, int at, int count) {
    if (row < 0 || row >= rows_ || count <= 0) return;
    std::vector<Cell>& line = screen_[static_cast<std::size_t>(row)];
    at = std::clamp(at, 0, cols_);
    count = std::min(count, cols_ - at);
    for (int i = cols_ - 1; i >= at + count; --i) {
        line[static_cast<std::size_t>(i)] = line[static_cast<std::size_t>(i - count)];
    }
    for (int i = at; i < at + count; ++i) line[static_cast<std::size_t>(i)] = blankCell();
    dirty_ = true;
}

void Terminal::deleteCells(int row, int at, int count) {
    if (row < 0 || row >= rows_ || count <= 0) return;
    std::vector<Cell>& line = screen_[static_cast<std::size_t>(row)];
    at = std::clamp(at, 0, cols_);
    count = std::min(count, cols_ - at);
    for (int i = at; i < cols_ - count; ++i) {
        line[static_cast<std::size_t>(i)] = line[static_cast<std::size_t>(i + count)];
    }
    for (int i = cols_ - count; i < cols_; ++i) line[static_cast<std::size_t>(i)] = blankCell();
    dirty_ = true;
}

void Terminal::insertLines(int at, int count) {
    if (at < scrollTop_ || at > scrollBottom_ || count <= 0) return;
    count = std::min(count, scrollBottom_ - at + 1);
    for (int i = 0; i < count; ++i) {
        for (int r = scrollBottom_; r > at; --r) {
            screen_[static_cast<std::size_t>(r)] = std::move(screen_[static_cast<std::size_t>(r - 1)]);
            cont_[static_cast<std::size_t>(r)] = cont_[static_cast<std::size_t>(r - 1)];
        }
        blankLine(screen_[static_cast<std::size_t>(at)]);
        cont_[static_cast<std::size_t>(at)] = 0;
    }
    curCol_ = 0;
    wrapPending_ = false;
    dirty_ = true;
}

void Terminal::deleteLines(int at, int count) {
    if (at < scrollTop_ || at > scrollBottom_ || count <= 0) return;
    count = std::min(count, scrollBottom_ - at + 1);
    for (int i = 0; i < count; ++i) {
        for (int r = at; r < scrollBottom_; ++r) {
            screen_[static_cast<std::size_t>(r)] = std::move(screen_[static_cast<std::size_t>(r + 1)]);
            cont_[static_cast<std::size_t>(r)] = cont_[static_cast<std::size_t>(r + 1)];
        }
        blankLine(screen_[static_cast<std::size_t>(scrollBottom_)]);
        cont_[static_cast<std::size_t>(scrollBottom_)] = 0;
    }
    curCol_ = 0;
    wrapPending_ = false;
    dirty_ = true;
}

void Terminal::setCursor(int row, int col) {
    curRow_ = std::clamp(row, 0, rows_ - 1);
    curCol_ = std::clamp(col, 0, cols_ - 1);
    wrapPending_ = false;
    dirty_ = true;
}

void Terminal::saveCursor() {
    savedRow_ = curRow_;
    savedCol_ = curCol_;
    savedFg_ = fg_;
    savedBg_ = bg_;
    savedFlags_ = flags_;
    savedWrap_ = wrapPending_;
}

void Terminal::restoreCursor() {
    curRow_ = std::clamp(savedRow_, 0, rows_ - 1);
    curCol_ = std::clamp(savedCol_, 0, cols_ - 1);
    fg_ = savedFg_;
    bg_ = savedBg_;
    flags_ = savedFlags_;
    wrapPending_ = savedWrap_;
    dirty_ = true;
}

void Terminal::enterAltScreen(bool clear) {
    if (altScreen_) {
        if (clear) {
            for (auto& line : screen_) blankLine(line);
            curRow_ = 0;
            curCol_ = 0;
        }
        return;
    }
    altSaved_ = screen_;
    cont_.assign(screen_.size(), 0);  // 备用屏自己重画，不参与重排
    altScreen_ = true;
    // 切屏后先把光标恢复成可见：TUI 通常会自己再发 ?25h/?25l，
    // 万一它隐藏光标后异常退出，也不会把主屏的光标一起带走。
    cursorVisible_ = true;
    if (clear) {
        // ?1049h 同时是 DECSC：把主屏光标位置存进保存槽，退出时用它还原。
        // 少了这一步，vim / less 退出后主屏光标会停在 TUI 最后待过的位置，
        // shell 的提示符就画到屏幕中间甚至盖住已有内容。
        saveCursor();
        for (auto& line : screen_) blankLine(line);
        curRow_ = 0;
        curCol_ = 0;
    }
    dirty_ = true;
}

void Terminal::leaveAltScreen(bool restorePos) {
    if (!altScreen_) return;
    altScreen_ = false;
    // 同上：回到主屏时光标默认可见，别让 TUI 遗留的 ?25l 影响 shell
    cursorVisible_ = true;
    if (!altSaved_.empty()) {
        screen_ = altSaved_;
        altSaved_.clear();
    }
    cont_.assign(screen_.size(), 0);
    if (restorePos) {
        // ?1049l：DECRC，回到进入前的光标位置，主屏内容与光标都对得上
        restoreCursor();
    } else {
        // ?47l / ?1047l 不动光标：沿用当前坐标，只做边界保护
        curRow_ = std::clamp(curRow_, 0, rows_ - 1);
        curCol_ = std::clamp(curCol_, 0, cols_ - 1);
    }
    ensureScreen();
    dirty_ = true;
}

void Terminal::sgr(const std::vector<int>& params) {
    if (params.empty()) {
        fg_ = theme_.fg;
        bg_ = theme_.bg;
        flags_ = 0;
        return;
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        int p = params[i] < 0 ? 0 : params[i];
        if (p == 38 || p == 48) {
            std::uint32_t color = 0;
            std::size_t j = i + 1;
            if (parseExtendedColor(params, j, theme_, color)) {
                if (p == 38) {
                    fg_ = color;
                } else {
                    bg_ = color;
                }
                i = j - 1;
            }
            continue;
        }
        if (p >= 30 && p <= 37) {
            fg_ = theme_.palette[p - 30];
        } else if (p >= 90 && p <= 97) {
            fg_ = theme_.palette[p - 90 + 8];
        } else if (p >= 40 && p <= 47) {
            bg_ = theme_.palette[p - 40];
        } else if (p >= 100 && p <= 107) {
            bg_ = theme_.palette[p - 100 + 8];
        } else {
            switch (p) {
                case 0:
                    fg_ = theme_.fg;
                    bg_ = theme_.bg;
                    flags_ = 0;
                    break;
                case 1:
                    flags_ = static_cast<std::uint8_t>(flags_ | kFlagBold);
                    break;
                case 2:
                    flags_ = static_cast<std::uint8_t>(flags_ | kFlagDim);
                    break;
                case 3:
                    flags_ = static_cast<std::uint8_t>(flags_ | kFlagItalic);
                    break;
                case 4:
                case 21:
                    flags_ = static_cast<std::uint8_t>(flags_ | kFlagUnderline);
                    break;
                case 7:
                    flags_ = static_cast<std::uint8_t>(flags_ | kFlagReverse);
                    break;
                case 22:
                    flags_ = static_cast<std::uint8_t>(flags_ & ~(kFlagBold | kFlagDim));
                    break;
                case 23:
                    flags_ = static_cast<std::uint8_t>(flags_ & ~kFlagItalic);
                    break;
                case 24:
                    flags_ = static_cast<std::uint8_t>(flags_ & ~kFlagUnderline);
                    break;
                case 27:
                    flags_ = static_cast<std::uint8_t>(flags_ & ~kFlagReverse);
                    break;
                case 39:
                    fg_ = theme_.fg;
                    break;
                case 49:
                    bg_ = theme_.bg;
                    break;
                default:
                    break;  // 闪烁、隐藏、删除线等忽略
            }
        }
    }
}

void Terminal::parseCsi() {
    Csi c = parseCsiParams(csi_);
    csi_.clear();
    state_ = State::Ground;

    auto num = [&](std::size_t i, int def) { return c.param(i, def); };
    switch (c.final) {
        case 'A':
            setCursor(curRow_ - std::max(1, num(0, 1)), curCol_);
            break;
        case 'B':
        case 'e':
            setCursor(curRow_ + std::max(1, num(0, 1)), curCol_);
            break;
        case 'C':
        case 'a':
            setCursor(curRow_, curCol_ + std::max(1, num(0, 1)));
            break;
        case 'D':
            setCursor(curRow_, curCol_ - std::max(1, num(0, 1)));
            break;
        case 'E':
            setCursor(curRow_ + std::max(1, num(0, 1)), 0);
            break;
        case 'F':
            setCursor(curRow_ - std::max(1, num(0, 1)), 0);
            break;
        case 'G':
        case '`':
            setCursor(curRow_, std::max(1, num(0, 1)) - 1);
            break;
        case 'H':
        case 'f':
            setCursor(std::max(1, num(0, 1)) - 1, std::max(1, num(1, 1)) - 1);
            break;
        case 'd':
            setCursor(std::max(1, num(0, 1)) - 1, curCol_);
            break;
        case 'Z':
            for (int i = std::max(1, num(0, 1)); i > 0; --i) {
                curCol_ = std::max(0, ((curCol_ - 1) / 8) * 8);
            }
            break;
        case 'J': {
            int mode = num(0, 0);
            if (mode == 0) {
                eraseCells(curRow_, curCol_, cols_);
                for (int r = curRow_ + 1; r < rows_; ++r) eraseCells(r, 0, cols_);
            } else if (mode == 1) {
                for (int r = 0; r < curRow_; ++r) eraseCells(r, 0, cols_);
                eraseCells(curRow_, 0, curCol_ + 1);
            } else if (mode == 2) {
                for (int r = 0; r < rows_; ++r) eraseCells(r, 0, cols_);
                cont_.assign(screen_.size(), 0);
            } else if (mode == 3) {
                history_.clear();
                histCont_.clear();
            }
            break;
        }
        case 'K': {
            int mode = num(0, 0);
            if (mode == 0) {
                eraseCells(curRow_, curCol_, cols_);
            } else if (mode == 1) {
                eraseCells(curRow_, 0, curCol_ + 1);
            } else {
                eraseCells(curRow_, 0, cols_);
            }
            break;
        }
        case '@':
            insertCells(curRow_, curCol_, std::max(1, num(0, 1)));
            break;
        case 'P':
            deleteCells(curRow_, curCol_, std::max(1, num(0, 1)));
            break;
        case 'X':
            eraseCells(curRow_, curCol_, curCol_ + std::max(1, num(0, 1)));
            break;
        case 'L':
            insertLines(curRow_, std::max(1, num(0, 1)));
            break;
        case 'M':
            deleteLines(curRow_, std::max(1, num(0, 1)));
            break;
        case 'S':
            scrollRegionUp(scrollTop_, scrollBottom_, std::max(1, num(0, 1)));
            break;
        case 'T':
            scrollRegionDown(scrollTop_, scrollBottom_, std::max(1, num(0, 1)));
            break;
        case 'b':
            // REP：重复上一个可打印字符
            for (int i = 0; i < std::max(1, num(0, 1)); ++i) putChar(lastPrinted_);
            break;
        case 'm':
            // 只有不带前缀的 CSI ... m 才是 SGR。CSI > 4 ; n m 是 xterm 的
            // modifyOtherKeys 键位设置，若误按 SGR 4;1 解析，会把之后所有空白
            // 画上下划线（屏幕上看起来是一条条横线）；CSI ? ... m 同理不属于 SGR。
            if (c.prefix == 0) sgr(c.params);
            break;
        case 'r': {
            int top = std::max(1, num(0, 1)) - 1;
            int bottom = (c.params.size() > 1 && c.params[1] > 0) ? c.params[1] - 1 : rows_ - 1;
            if (bottom > top && bottom < rows_) {
                scrollTop_ = top;
                scrollBottom_ = bottom;
                setCursor(scrollTop_, 0);
            }
            break;
        }
        case 's':
            saveCursor();
            break;
        case 'u':
            if (c.prefix == '?') {
                // kitty 键盘协议查询：回复 0 表示不支持任何增强标志（不必让程序等超时）
                response_ += "\x1b[?0u";
            } else if (c.prefix == 0) {
                restoreCursor();  // CSI u（xterm/kitty 的恢复光标位置）
            }
            break;
        case 'h':
        case 'l': {
            const bool on = (c.final == 'h');
            for (int mode : c.params) {
                if (mode < 0) continue;
                if (c.prefix == '?') {
                    switch (mode) {
                        case 1:
                            decCursorKeys_ = on;
                            break;
                        case 7:
                            autoWrap_ = on;
                            break;
                        case 25:
                            cursorVisible_ = on;
                            break;
                        case 47:
                        case 1047:
                        case 1049:
                            if (on) {
                                enterAltScreen(mode == 1049);
                            } else {
                                leaveAltScreen(mode == 1049);
                            }
                            break;
                        case 1000:
                            mouseMode_ = on ? MouseMode::X10 : MouseMode::None;
                            break;
                        case 1002:
                            mouseMode_ = on ? MouseMode::Button : MouseMode::None;
                            break;
                        case 1003:
                            mouseMode_ = on ? MouseMode::Any : MouseMode::None;
                            break;
                        case 1005:
                            mouseUtf8_ = on;
                            break;
                        case 1006:
                            mouseSgr_ = on;
                            break;
                        case 1015:
                            mouseUrxvt_ = on;
                            break;
                        case 1004:
                            focusEvents_ = on;
                            break;
                        case 2004:
                            bracketedPaste_ = on;
                            break;
                        default:
                            break;  // 其它（光标闪烁、同步刷新等）忽略
                    }
                } else if (mode == 4) {
                    insertMode_ = on;
                }
            }
            break;
        }
        case 'n': {
            int mode = num(0, 0);
            if (mode == 5) {
                response_ += "\x1b[0n";
            } else if (mode == 6) {
                response_ += "\x1b[" + std::to_string(curRow_ + 1) + ";" +
                             std::to_string(curCol_ + 1) + "R";
            }
            break;
        }
        case 'c':
            if (c.prefix == 0) response_ += "\x1b[?1;2c";
            break;
        case 'p':
            if (c.inter == "!") {
                reset();  // DECSTR 软复位
            } else if (c.inter == "$" && c.prefix == '?') {
                // DECRQM：报告模式状态（0=不支持，1=已设置，2=已重置）。
                // 不回应该查询时，opentui / Neovim 一类程序只能等超时后按“不确定”处理。
                const int mode = num(0, 0);
                int state = 0;
                switch (mode) {
                    case 1: state = decCursorKeys_ ? 1 : 2; break;
                    case 7: state = autoWrap_ ? 1 : 2; break;
                    case 25: state = cursorVisible_ ? 1 : 2; break;
                    case 1000: state = mouseMode_ == MouseMode::X10 ? 1 : 2; break;
                    case 1002: state = mouseMode_ == MouseMode::Button ? 1 : 2; break;
                    case 1003: state = mouseMode_ == MouseMode::Any ? 1 : 2; break;
                    case 1004: state = focusEvents_ ? 1 : 2; break;
                    case 1005: state = mouseUtf8_ ? 1 : 2; break;
                    case 1006: state = mouseSgr_ ? 1 : 2; break;
                    case 1015: state = mouseUrxvt_ ? 1 : 2; break;
                    case 1047:
                    case 1049: state = altScreen_ ? 1 : 2; break;
                    case 2004: state = bracketedPaste_ ? 1 : 2; break;
                    default: state = 0; break;
                }
                response_ += "\x1b[?" + std::to_string(mode) + ";" + std::to_string(state) + "$y";
            }
            break;
        default:
            break;
    }
    dirty_ = true;
}

}  // namespace rshell
