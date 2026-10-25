// terminal.h - 与平台无关的 VT100/xterm 终端模拟器（供 GUI 渲染）
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace rshell {

// 单元格属性位
enum CellFlag : std::uint8_t {
    kFlagBold = 1u << 0,
    kFlagDim = 1u << 1,
    kFlagItalic = 1u << 2,
    kFlagUnderline = 1u << 3,
    kFlagReverse = 1u << 4,
    kFlagWide = 1u << 5,      // 占两列的首字符
    kFlagWideTail = 1u << 6,  // 宽字符的第二列
};

struct Cell {
    char32_t ch = U' ';
    std::uint32_t fg = 0;  // 0xRRGGBB
    std::uint32_t bg = 0;
    std::uint8_t flags = 0;
};

struct TerminalMatch {
    std::size_t line = 0;
    int column = 0;
    int width = 0;
};

// 终端配色（0-15 基础色由调用方给出，其余按 xterm 标准生成）
struct TerminalTheme {
    std::uint32_t fg = 0xD0D0D0;
    std::uint32_t bg = 0x101418;
    std::uint32_t cursor = 0xE6E6E6;
    std::uint32_t selection = 0x2A4B6B;
    std::uint32_t palette[16] = {
        0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
        0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
    };
};

// 鼠标上报模式（CSI ?1000/1002/1003）
enum class MouseMode {
    None = 0,
    X10,     // 1000：仅按下
    Button,  // 1002：按下 + 拖动
    Any,     // 1003：任何移动
};

class Terminal {
public:
    explicit Terminal(int cols = 80, int rows = 24, std::size_t maxScrollback = 5000);

    void setTheme(const TerminalTheme& theme) { theme_ = theme; }
    const TerminalTheme& theme() const { return theme_; }

    void reset();
    void resize(int cols, int rows);
    // 解析远端输出；必须在 UI 线程调用（会触发重绘标记）
    void feed(const std::string& data);

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int cursorRow() const { return curRow_; }
    int cursorCol() const { return curCol_; }
    // 光标的显示/隐藏完全由应用的 DECSET/DECRST 25（cursorVisible_）决定：
    // 备用屏（vim / btop / htop 等 TUI）同样要按应用要求画光标，
    // 否则 TUI 里会永远没有光标（它们自己用 ?25h / ?25l 控制）。
    bool cursorVisible() const { return cursorVisible_; }
    bool bracketedPaste() const { return bracketedPaste_; }
    // 应用光标键模式（CSI ?1h）：方向键应发送 SS3 序列
    bool applicationCursorKeys() const { return decCursorKeys_; }
    // 焦点上报（CSI ?1004h）
    bool focusReporting() const { return focusEvents_; }
    // 鼠标上报
    MouseMode mouseMode() const { return mouseMode_; }
    bool mouseSgr() const { return mouseSgr_; }
    bool mouseUtf8() const { return mouseUtf8_; }
    bool mouseUrxvt() const { return mouseUrxvt_; }
    const std::string& title() const { return title_; }
    // 工作目录：远端 shell 通过 OSC 7（file://host/path）上报，GUI 用它让文件面板
    // 跟着终端里的 cd 走。takeCwdChange() 是消费式的：同一条提示符反复上报同一目录
    // 时不会重复触发，面板也不会被反复重列。
    const std::string& cwd() const { return cwd_; }
    bool takeCwdChange() {
        const bool changed = cwdChanged_;
        cwdChanged_ = false;
        return changed;
    }
    // 私有 shell 集成确认帧，分包解析后交给 GUI，不参与终端显示。
    std::vector<std::string> takeShellIntegrationAcks() {
        std::vector<std::string> result;
        result.swap(shellIntegrationAcks_);
        return result;
    }
    // 撤去本端初始化命令的临时画面，保留最新目录、标题和终端尺寸。
    void restoreDisplay(Terminal&& snapshot);

    // ---- 渲染访问 --------------------------------------------------------
    // 行索引连续：0..scrollbackCount()-1 是回滚历史，其后的 rows() 行是当前屏幕
    std::size_t scrollbackCount() const { return history_.size(); }
    std::size_t lineCount() const { return history_.size() + screen_.size(); }
    const std::vector<Cell>& lineAt(std::size_t index) const;
    // 视图是否滚动到了历史（0 表示贴底）
    int scrollOffset() const { return scrollOffset_; }
    void scrollView(int deltaLines);   // 正数向上翻历史
    void scrollToBottom() { scrollOffset_ = 0; }
    // 有输出时若用户没有上翻，则自动贴底
    void noteOutput();

    // 需要回写给远端的字节（如光标位置查询响应）
    std::string takeResponse();

    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // ---- 文本选择 --------------------------------------------------------
    std::string textRange(std::size_t startLine, int startCol, std::size_t endLine,
                          int endCol) const;
    // 取某行的可见文本（去掉行尾空白），用于调试/测试
    std::string plainLine(std::size_t index) const;
    // 精确匹配可见字符，跳过宽字符尾格；返回历史与当前屏幕的网格位置。
    std::vector<TerminalMatch> findAll(const std::u32string& query) const;

    // ---- 供单元测试使用 --------------------------------------------------
    static bool isWideChar(char32_t ch);
    static int charWidth(char32_t ch);

private:
    void ensureScreen();
    void blankLine(std::vector<Cell>& line) const;
    Cell blankCell() const;

    void putChar(char32_t ch);
    char32_t mapCharset(char32_t ch) const;
    void lineFeed();
    void reverseLineFeed();
    void carriageReturn();
    void backspace();
    void tab();
    void scrollRegionUp(int top, int bottom, int count);
    void scrollRegionDown(int top, int bottom, int count);
    void eraseCells(int row, int from, int to);  // [from, to)
    void insertCells(int row, int at, int count);
    void deleteCells(int row, int at, int count);
    void insertLines(int at, int count);
    void deleteLines(int at, int count);

    void sgr(const std::vector<int>& params);
    void setCursor(int row, int col);
    void saveCursor();
    void restoreCursor();
    void enterAltScreen(bool clear);
    // restorePos：xterm 的 ?1049l 会把光标恢复到进入备用屏之前的位置（DECRC），
    // shell 的提示符才接着原来那一行走；?47l / ?1047l 按规范不动光标。
    void leaveAltScreen(bool restorePos);

    void parseCsi();
    void finishOsc();  // OSC 收尾：窗口标题（0/2）与工作目录（7）
    void executeControl(unsigned char byte);
    void decodeUtf8(unsigned char byte);

    // 解析状态
    enum class State { Ground, Escape, Csi, Osc, Charset, Dcs };
    State state_ = State::Ground;
    std::string csi_;   // CSI 参数字节
    std::string osc_;   // OSC 内容
    std::uint32_t utf8Pending_ = 0;
    int utf8Remain_ = 0;
    bool escPending_ = false;  // OSC/DCS 内的 ESC，用于识别 ST

    // 屏幕
    int cols_ = 80;
    int rows_ = 24;
    std::size_t maxScrollback_ = 5000;
    std::vector<std::vector<Cell>> screen_;
    std::deque<std::vector<Cell>> history_;
    std::vector<std::vector<Cell>> altSaved_;
    bool altScreen_ = false;

    int curRow_ = 0;
    int curCol_ = 0;
    int savedRow_ = 0;
    int savedCol_ = 0;
    std::uint32_t savedFg_ = 0xD0D0D0;
    std::uint32_t savedBg_ = 0x101418;
    std::uint8_t savedFlags_ = 0;
    bool savedWrap_ = false;
    int scrollTop_ = 0;
    int scrollBottom_ = 23;
    bool wrapPending_ = false;
    bool cursorVisible_ = true;
    bool insertMode_ = false;
    bool bracketedPaste_ = false;
    bool autoWrap_ = true;
    int scrollOffset_ = 0;
    bool decCursorKeys_ = false;   // CSI ?1h
    bool focusEvents_ = false;     // CSI ?1004h
    MouseMode mouseMode_ = MouseMode::None;
    bool mouseSgr_ = false;   // ?1006
    bool mouseUtf8_ = false;  // ?1005
    bool mouseUrxvt_ = false; // ?1015
    // 字符集：G0/G1 设计（'B' = ASCII，'0' = DEC Special Graphics）
    char g0Charset_ = 'B';
    char g1Charset_ = 'B';
    bool useG1_ = false;  // SO/SI
    char charsetSlot_ = '(';
    // REP（CSI b）需要记住上一个可打印字符
    char32_t lastPrinted_ = U' ';

    // 当前 SGR 状态
    std::uint32_t fg_ = 0xD0D0D0;
    std::uint32_t bg_ = 0x101418;
    std::uint8_t flags_ = 0;

    TerminalTheme theme_;
    std::string response_;
    std::string title_;
    std::string cwd_;        // 最近一次 OSC 7 上报的目录（空 = 还没收到）
    bool cwdChanged_ = false;  // 有新目录待 GUI 消费
    std::vector<std::string> shellIntegrationAcks_;
    bool dirty_ = true;
};

}  // namespace rshell
