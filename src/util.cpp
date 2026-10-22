// util.cpp
#include "util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace rshell {

// ---------------------------------------------------------------- 字符串
std::string trim(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && toLower(a) == toLower(b);
}

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string base64Encode(const unsigned char* data, size_t len) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < len) {
        unsigned int v = (static_cast<unsigned int>(data[i]) << 16) |
                         (static_cast<unsigned int>(data[i + 1]) << 8) |
                         static_cast<unsigned int>(data[i + 2]);
        out += table[(v >> 18) & 0x3F];
        out += table[(v >> 12) & 0x3F];
        out += table[(v >> 6) & 0x3F];
        out += table[v & 0x3F];
        i += 3;
    }
    if (i < len) {
        unsigned int v = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<unsigned int>(data[i + 1]) << 8;
        out += table[(v >> 18) & 0x3F];
        out += table[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? table[(v >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

// ---------------------------------------------------------------- 本地路径
namespace {

bool isEnvNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string envValue(const std::string& name) {
    if (name.empty()) return {};
    const char* v = std::getenv(name.c_str());
    return v ? std::string(v) : std::string();
}

std::string expandVariables(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] == '$' && i + 1 < in.size()) {
            if (in[i + 1] == '{') {
                size_t close = in.find('}', i + 2);
                if (close != std::string::npos) {
                    out += envValue(in.substr(i + 2, close - i - 2));
                    i = close + 1;
                    continue;
                }
            } else if (isEnvNameChar(in[i + 1])) {
                size_t j = i + 1;
                while (j < in.size() && isEnvNameChar(in[j])) ++j;
                out += envValue(in.substr(i + 1, j - i - 1));
                i = j;
                continue;
            }
        }
        if (in[i] == '%' && i + 1 < in.size()) {
            size_t close = in.find('%', i + 1);
            if (close != std::string::npos && close > i + 1) {
                std::string name = in.substr(i + 1, close - i - 1);
                bool valid = std::all_of(name.begin(), name.end(), isEnvNameChar);
                if (valid) {
                    out += envValue(name);
                    i = close + 1;
                    continue;
                }
            }
        }
        out += in[i++];
    }
    return out;
}

}  // namespace

std::string expandPath(const std::string& in) {
    if (in.empty()) return in;
    std::string out = in;
    if (out == "~") {
        out = homeDirectory();
    } else if (startsWith(out, "~/") || startsWith(out, "~\\")) {
        out = homeDirectory() + out.substr(1);
    }
    return expandVariables(out);
}

fs::path utf8ToPath(const std::string& utf8) { return fs::u8path(utf8); }

std::string pathToUtf8(const fs::path& p) { return p.u8string(); }

std::string homeDirectory() {
#ifdef _WIN32
    std::string profile = envValue("USERPROFILE");
    if (!profile.empty()) return profile;
    std::string drive = envValue("HOMEDRIVE");
    std::string path = envValue("HOMEPATH");
    if (!drive.empty() || !path.empty()) return drive + path;
#else
    std::string home = envValue("HOME");
    if (!home.empty()) return home;
#endif
    return ".";
}

// ---------------------------------------------------------------- 远端路径
// ---------------------------------------------------------------- 粘贴文本
std::string normalizePasteText(const std::string& text, bool bracketedPaste) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            out.push_back('\r');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;  // CRLF 只算一次回车
        } else if (c == '\n') {
            out.push_back('\r');
        } else if (c == '\x1b' && bracketedPaste) {
            continue;  // 见 util.h 里的说明：防止内容伪造 ESC[201~ 提前结束粘贴
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// OSC 7 钩子注入脚本。printf 的格式串里只能有一层反斜杠：远端 printf 收到 \033
// 才会吐出 ESC，写成 \\033 就变成字面文本，屏幕上会露出 "\033[1A" 这种东西。
std::string buildCwdHookScript(int eraseLines) {
    const int n = eraseLines < 1 ? 1 : eraseLines;
    std::string script = "printf '\\033[" + std::to_string(n) + "A\\r\\033[J'; ";
    script +=
        // 钩子本体：每次画提示符前把 $PWD 用 OSC 7 报给本端（file://host/path）
        "__rshell_cwd(){ printf '\\033]7;file://%s%s\\033\\\\' \"${HOSTNAME:-localhost}\" \"$PWD\"; }; "
        "if [ -n \"${BASH_VERSION-}\" ]; then "
        "PROMPT_COMMAND=\"__rshell_cwd${PROMPT_COMMAND:+; $PROMPT_COMMAND}\"; "
        "elif [ -n \"${ZSH_VERSION-}\" ]; then precmd_functions+=(__rshell_cwd); fi; "
        "__rshell_cwd; printf '\\r\\033[2K'";
    return script;
}

// 收尾命令：stty echo 恢复回显，printf 把光标拉回行首并清掉当前行。
// 注意字符串里的 \\r / \\033 是给远端 printf 的转义文本（运行时是反斜杠 + 字母），
// 绝不能写成 "\r" / "\033"，那是真实的 CR / ESC 字节，会把这一行拆成两行。
std::string buildCwdHookRestoreEcho() { return "stty echo; printf '\\r\\033[2K'"; }

bool isAbsoluteRemotePath(const std::string& p) { return !p.empty() && p[0] == '/'; }

std::string normalizeRemotePath(const std::string& p) {
    if (p.empty()) return ".";
    bool absolute = p[0] == '/';
    std::vector<std::string> stack;
    for (const auto& part : split(p, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!stack.empty() && stack.back() != "..") {
                stack.pop_back();
            } else if (!absolute) {
                stack.push_back("..");
            }
            continue;
        }
        stack.push_back(part);
    }
    if (stack.empty()) return absolute ? "/" : ".";
    std::string out = absolute ? "/" : "";
    out += join(stack, "/");
    return out;
}

std::string remoteJoin(const std::string& base, const std::string& rel) {
    if (rel.empty()) return normalizeRemotePath(base);
    if (isAbsoluteRemotePath(rel)) return normalizeRemotePath(rel);
    if (base.empty()) return normalizeRemotePath(rel);
    if (base == "/") return normalizeRemotePath("/" + rel);
    return normalizeRemotePath(base + "/" + rel);
}

std::string remoteBaseName(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (p == "/" || p.empty()) return p.empty() ? "." : "/";
    size_t pos = p.find_last_of('/');
    return (pos == std::string::npos) ? p : p.substr(pos + 1);
}

std::string localBaseName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

std::string remoteDirName(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

void validateRemotePath(const std::string& p) {
    if (p.empty()) throw Error("远端路径不能为空");
    for (unsigned char c : p) {
        if (c < 0x20 || c == 0x7F) throw Error("远端路径包含非法控制字符");
    }
}

// ---------------------------------------------------------------- glob
bool globMatch(std::string_view pattern, std::string_view text) {
    size_t pi = 0, ti = 0;
    size_t star = std::string_view::npos;
    size_t starText = 0;
    while (ti < text.size()) {
        bool matched = false;
        if (pi < pattern.size()) {
            if (pattern[pi] == '\\' && pi + 1 < pattern.size()) {
                if (pattern[pi + 1] == text[ti]) {
                    pi += 2;
                    ++ti;
                    matched = true;
                }
            } else if (pattern[pi] == '?' || pattern[pi] == text[ti]) {
                ++pi;
                ++ti;
                matched = true;
            } else if (pattern[pi] == '*') {
                star = pi++;
                starText = ti;
                matched = true;
            }
        }
        if (matched) continue;
        if (star != std::string_view::npos) {
            pi = star + 1;
            ti = ++starText;
            continue;
        }
        return false;
    }
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

bool hasWildcard(std::string_view s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\') {
            ++i;
            continue;
        }
        if (s[i] == '*' || s[i] == '?') return true;
    }
    return false;
}

// ---------------------------------------------------------------- 格式化
std::string formatBytes(std::uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return buf;
}

std::string formatRate(double bytesPerSecond) {
    if (!(bytesPerSecond > 0.0) || !std::isfinite(bytesPerSecond)) return "--";
    return formatBytes(static_cast<std::uint64_t>(bytesPerSecond)) + "/s";
}

std::string formatDuration(std::uint64_t seconds) {
    char buf[32];
    if (seconds < 3600) {
        std::snprintf(buf, sizeof(buf), "%02llu:%02llu",
                      static_cast<unsigned long long>(seconds / 60),
                      static_cast<unsigned long long>(seconds % 60));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu",
                      static_cast<unsigned long long>(seconds / 3600),
                      static_cast<unsigned long long>((seconds % 3600) / 60),
                      static_cast<unsigned long long>(seconds % 60));
    }
    return buf;
}

std::string formatProgress(std::uint64_t done, std::uint64_t total,
                           double bytesPerSecond, int barWidth) {
    if (barWidth < 4) barWidth = 4;
    if (total > 0 && done > total) done = total;
    double ratio = (total == 0) ? 0.0 : static_cast<double>(done) / static_cast<double>(total);

    int filled = static_cast<int>(ratio * barWidth);
    if (filled > barWidth) filled = barWidth;
    std::string bar;
    bar.reserve(static_cast<size_t>(barWidth) + 2);
    bar += '[';
    bar.append(static_cast<size_t>(filled), '=');
    if (filled < barWidth) {
        bar += '>';
        bar.append(static_cast<size_t>(barWidth - filled - 1), ' ');
    }
    bar += ']';

    char head[128];
    std::snprintf(head, sizeof(head), "%5.1f%% %s %s/%s", ratio * 100.0, bar.c_str(),
                  formatBytes(done).c_str(), formatBytes(total).c_str());

    std::ostringstream oss;
    oss << head;
    if (bytesPerSecond > 0.0) {
        oss << "  " << formatRate(bytesPerSecond);
        if (total > done) {
            double remain = static_cast<double>(total - done) / bytesPerSecond;
            oss << "  ETA " << formatDuration(static_cast<std::uint64_t>(remain + 0.5));
        }
    }
    return oss.str();
}

std::string formatMode(unsigned int mode) {
    std::string out(10, '-');
    unsigned int type = mode & 0170000;
    out[0] = (type == 0040000)   ? 'd'
             : (type == 0120000) ? 'l'
             : (type == 0010000) ? 'p'
             : (type == 0140000) ? 's'
                                 : '-';
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) {
        if (mode & (1u << (8 - i))) out[static_cast<size_t>(i) + 1] = rwx[i];
    }
    if (mode & 04000) out[3] = (out[3] == 'x') ? 's' : 'S';
    if (mode & 02000) out[6] = (out[6] == 'x') ? 's' : 'S';
    if (mode & 01000) out[9] = (out[9] == 'x') ? 't' : 'T';
    return out;
}

std::string formatTimestamp(std::int64_t unixSeconds) {
    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0) return "-";
#else
    if (localtime_r(&t, &tm) == nullptr) return "-";
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

// ---------------------------------------------------------------- 终端
bool stdoutIsTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

bool stdinIsTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

void initConsole() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

std::string readLine(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) throw Error("输入已结束");
    return line;
}

std::string readPassword(const std::string& prompt) {
    if (!stdinIsTty()) {
        std::string line;
        if (!std::getline(std::cin, line)) throw Error("输入已结束");
        return line;
    }
    std::cout << prompt << std::flush;
    std::string password;
#ifdef _WIN32
    while (true) {
        int ch = _getch();
        if (ch == '\r' || ch == '\n') break;
        if (ch == 3) {  // Ctrl+C
            std::cout << "\n";
            throw Error("已取消");
        }
        if (ch == '\b') {
            if (!password.empty()) {
                password.pop_back();
                std::cout << "\b \b" << std::flush;
            }
            continue;
        }
        if (ch == 0 || ch == 0xE0) {  // 功能键的两字节序列，丢弃
            _getch();
            continue;
        }
        password.push_back(static_cast<char>(ch));
        std::cout << '*' << std::flush;
    }
    std::cout << "\n";
#else
    termios oldt{};
    bool restore = false;
    if (tcgetattr(STDIN_FILENO, &oldt) == 0) {
        termios newt = oldt;
        newt.c_lflag &= static_cast<tcflag_t>(~ECHO);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) == 0) restore = true;
    }
    std::string line;
    bool ok = static_cast<bool>(std::getline(std::cin, line));
    if (restore) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    std::cout << "\n";
    if (!ok) throw Error("输入已结束");
    password = line;
#endif
    return password;
}

double monotonicSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

std::string isoNow() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// ---------------------------------------------------------------- 日志
Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::info(const std::string& msg) {
    if (level_ == LogLevel::Quiet) return;
    std::cerr << msg << std::endl;
}

void Logger::debug(const std::string& msg) {
    if (level_ != LogLevel::Verbose) return;
    std::cerr << dim("[debug] " + msg) << std::endl;
}

void Logger::warn(const std::string& msg) {
    if (level_ == LogLevel::Quiet) return;
    std::cerr << yellow("[警告] " + msg) << std::endl;
}

void Logger::error(const std::string& msg) {
    std::cerr << red("[错误] " + msg) << std::endl;
}

namespace {
bool colorEnabled() {
    static const bool enabled = [] {
#ifdef _WIN32
        return _isatty(_fileno(stderr)) != 0;
#else
        return isatty(STDERR_FILENO) != 0;
#endif
    }();
    return enabled;
}
}  // namespace

std::string colorize(const std::string& text, const char* ansiCode) {
    if (!colorEnabled()) return text;
    return std::string("\x1b[") + ansiCode + "m" + text + "\x1b[0m";
}

std::string green(const std::string& s) { return colorize(s, "32"); }
std::string red(const std::string& s) { return colorize(s, "31"); }
std::string yellow(const std::string& s) { return colorize(s, "33"); }
std::string cyan(const std::string& s) { return colorize(s, "36"); }
std::string bold(const std::string& s) { return colorize(s, "1"); }
std::string dim(const std::string& s) { return colorize(s, "2"); }

}  // namespace rshell


