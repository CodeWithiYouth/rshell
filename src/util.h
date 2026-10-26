// util.h - 与平台/SSH 无关的通用工具
#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rshell {

namespace fs = std::filesystem;

// rshell 统一的错误类型：main() 捕获后打印 message 并以非零码退出
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what) : std::runtime_error(what) {}
};

// ---------------------------------------------------------------- 字符串
std::string trim(std::string_view s);
std::string toLower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool startsWith(std::string_view s, std::string_view prefix);
bool endsWith(std::string_view s, std::string_view suffix);
std::vector<std::string> split(std::string_view s, char sep);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string replaceAll(std::string s, std::string_view from, std::string_view to);
std::string base64Encode(const unsigned char* data, size_t len);

// ---------------------------------------------------------------- 本地路径
// 展开 ~ / ~/xxx、$VAR、${VAR}、%VAR%
std::string expandPath(const std::string& in);
fs::path utf8ToPath(const std::string& utf8);
std::string pathToUtf8(const fs::path& p);
std::string homeDirectory();

// ---------------------------------------------------------------- 远端 POSIX 路径
// 都不会访问远端，只做纯字符串运算
std::string remoteJoin(const std::string& base, const std::string& rel);
std::string remoteBaseName(const std::string& path);
// 取本地路径的文件名（同时接受 / 与 \ 作为分隔符）
std::string localBaseName(const std::string& path);
std::string remoteDirName(const std::string& path);
bool isAbsoluteRemotePath(const std::string& p);
std::string normalizeRemotePath(const std::string& p);
// 验证路径中不含控制字符，避免把奇怪内容写进日志/终端
void validateRemotePath(const std::string& p);

// ---------------------------------------------------------------- glob
// 支持 * ? 以及反斜杠转义，语义与 POSIX fnmatch 的常见子集一致
bool globMatch(std::string_view pattern, std::string_view text);
bool hasWildcard(std::string_view s);

// ---------------------------------------------------------------- 格式化
std::string formatBytes(std::uint64_t bytes);          // 1.2 MiB
std::string formatRate(double bytesPerSecond);         // 3.4 MiB/s
std::string formatDuration(std::uint64_t seconds);     // 01:23 / 1:02:03
std::string formatProgress(std::uint64_t done, std::uint64_t total,
                           double bytesPerSecond, int barWidth = 28);
std::string formatMode(unsigned int mode);             // -rw-r--r-- / drwxr-xr-x
std::string formatTimestamp(std::int64_t unixSeconds); // 2026-10-20 14:30

// ---------------------------------------------------------------- 终端
bool stdoutIsTty();
bool stdinIsTty();
void initConsole();  // Windows: UTF-8 代码页 + 打开 ANSI 转义
std::string readLine(const std::string& prompt);
std::string readPassword(const std::string& prompt);

double monotonicSeconds();
std::string isoNow();

// ---------------------------------------------------------------- 日志
enum class LogLevel { Quiet, Normal, Verbose };

class Logger {
public:
    static Logger& instance();
    void setLevel(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_; }
    bool verbose() const { return level_ == LogLevel::Verbose; }
    // 输出到 stderr，避免污染 stdout 上的数据流
    void info(const std::string& msg);
    void debug(const std::string& msg);
    void warn(const std::string& msg);
    void error(const std::string& msg);

private:
    LogLevel level_ = LogLevel::Normal;
};

inline Logger& log() { return Logger::instance(); }

// 终端颜色（非 tty 时自动禁用）
std::string colorize(const std::string& text, const char* ansiCode);
std::string green(const std::string& s);
std::string red(const std::string& s);
std::string yellow(const std::string& s);
std::string cyan(const std::string& s);
std::string bold(const std::string& s);
std::string dim(const std::string& s);

// 粘贴文本归一化：CRLF / LF / CR 统一成 \r（远端 shell 的一次回车）。
// bracketedPaste 时再剥掉 ESC —— 括号粘贴靠 ESC[201~ 分界，内容自带该序列
// 就能提前"越狱"，让后面的文本被远端当成用户输入直接执行。
std::string normalizePasteText(const std::string& text, bool bracketedPaste);

// 构造"注入 OSC 7 钩子"的远程命令。注入第 1 步的 `stty -echo` 一定会被远端 tty
// 回显，而且可能不止一行（多行提示符、PROMPT_COMMAND 打印、shell 报错都算），
// 所以开头先用 ESC[<n>A 回到注入起点行首、再 ESC[J 清到屏尾，一次擦干净。
// eraseLines 是"注入起点 → 当前光标"差的行数，由调用方用终端模型数出来（≥1）。
std::string buildCwdHookScript(int eraseLines);

// 注入收尾：恢复回显并清空当前行。和 buildCwdHookScript 一样，printf 的格式串里
// 只能有一层反斜杠。如果这里写成了真实的 CR 字节，远端 tty 的 ICRNL 会把它变成
// 换行，`printf '` 的单引号就悬空，bash 直接掉进 PS2 续行——屏幕上会多出一个 "> "。
std::string buildCwdHookRestoreEcho();
// 初始化输入包装：Bash 仅移除带本次唯一标记的当前历史项，不改用户历史设置。
std::string buildCwdHookInput(const std::string& command, const std::string& historyTag);

}  // namespace rshell
