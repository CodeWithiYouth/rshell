// ai_tools.cpp
#include "ai_tools.h"

#include <algorithm>
#include <cctype>

#include "util.h"

namespace rshell::ai {

namespace {

bool isTokenChar(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-';
}

bool startsWithAt(const std::string& text, std::size_t pos, const char* prefix) {
    const std::size_t length = std::char_traits<char>::length(prefix);
    return pos + length <= text.size() && text.compare(pos, length, prefix) == 0;
}

// PEM 私钥块：从 BEGIN 到匹配的 END 整段替换
std::size_t redactPemBlocks(std::string& text) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t begin = text.find("-----BEGIN ", pos);
        if (begin == std::string::npos) break;
        const std::size_t headerEnd = text.find("-----", begin + 11);
        if (headerEnd == std::string::npos) break;
        const std::string header = text.substr(begin, headerEnd + 5 - begin);
        if (header.find("PRIVATE KEY") == std::string::npos) {
            pos = headerEnd + 5;
            continue;
        }
        const std::size_t end = text.find("-----END ", headerEnd + 5);
        if (end == std::string::npos) {
            text.replace(begin, text.size() - begin, header + "<redacted>");
            ++count;
            break;
        }
        const std::size_t endLine = text.find("-----", end + 9);
        const std::size_t stop = endLine == std::string::npos ? text.size() : endLine + 5;
        text.replace(begin, stop - begin, header + "<redacted>");
        ++count;
        pos = begin + header.size() + 10;
    }
    return count;
}

// 固定前缀的令牌：命中后吞掉紧跟的令牌字符，只保留前缀
std::size_t redactPrefixedTokens(std::string& text) {
    static const char* const kPrefixes[] = {
        "AKIA", "ASIA", "ghp_", "gho_", "ghu_", "ghs_", "ghr_",
        "github_pat_", "glpat-", "xoxb-", "xoxp-", "xoxa-", "xoxr-", "xoxs-",
        "sk-", "AIza",
    };

    std::string out;
    out.reserve(text.size());
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const bool boundary = i == 0 || !isTokenChar(static_cast<unsigned char>(text[i - 1]));

        const char* matched = nullptr;
        if (boundary) {
            for (const char* prefix : kPrefixes) {
                if (startsWithAt(text, i, prefix)) {
                    matched = prefix;
                    break;
                }
            }
        }
        if (!matched) {
            out += text[i++];
            continue;
        }

        const std::size_t prefixLength = std::char_traits<char>::length(matched);
        std::size_t end = i + prefixLength;
        while (end < text.size() && isTokenChar(static_cast<unsigned char>(text[end]))) ++end;
        // 太短的（例如正文里的 "sk-"）不当作令牌
        if (end - (i + prefixLength) < 12) {
            out += text[i++];
            continue;
        }
        out.append(matched, prefixLength);
        out += "<redacted>";
        ++count;
        i = end;
    }
    text.swap(out);
    return count;
}

// JWT：eyJ 开头、至少两个点
std::size_t redactJwt(std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const bool boundary = i == 0 || !isTokenChar(static_cast<unsigned char>(text[i - 1]));
        if (!boundary || !startsWithAt(text, i, "eyJ")) {
            out += text[i++];
            continue;
        }
        std::size_t end = i;
        int dots = 0;
        while (end < text.size()) {
            const char c = text[end];
            if (c == '.') {
                ++dots;
                ++end;
            } else if (isTokenChar(static_cast<unsigned char>(c))) {
                ++end;
            } else {
                break;
            }
        }
        if (dots < 2 || end - i < 30) {
            out += text[i++];
            continue;
        }
        out += "eyJ<redacted>";
        ++count;
        i = end;
    }
    text.swap(out);
    return count;
}

// key = value 形式：password=xxx、api_key: yyy、Authorization: Bearer zzz
std::size_t redactAssignments(std::string& text) {
    static const char* const kKeywords[] = {
        "password", "passwd", "secret", "token", "api_key", "apikey",
        "api-key", "access_key", "secret_key", "private_key", "authorization",
    };

    // 先收集区间，再倒序替换，避免边改边算导致下标漂移
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    std::size_t lineStart = 0;
    while (lineStart < text.size()) {
        std::size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = text.size();

        const std::string lower = toLower(text.substr(lineStart, lineEnd - lineStart));
        std::size_t searchFrom = 0;
        while (searchFrom < lower.size()) {
            std::size_t hit = std::string::npos;
            std::size_t keywordLength = 0;
            for (const char* candidate : kKeywords) {
                const std::size_t found = lower.find(candidate, searchFrom);
                if (found != std::string::npos && (hit == std::string::npos || found < hit)) {
                    hit = found;
                    keywordLength = std::char_traits<char>::length(candidate);
                }
            }
            if (hit == std::string::npos) break;

            // 词边界：关键词前面不能紧贴标识符字符（tokenizer= 不算 token=）
            if (hit > 0 && isTokenChar(static_cast<unsigned char>(lower[hit - 1]))) {
                searchFrom = hit + 1;
                continue;
            }
            std::size_t cursor = hit + keywordLength;
            while (cursor < lower.size() && (lower[cursor] == ' ' || lower[cursor] == '\t')) ++cursor;
            if (cursor >= lower.size() || (lower[cursor] != '=' && lower[cursor] != ':')) {
                searchFrom = hit + 1;
                continue;
            }
            ++cursor;
            while (cursor < lower.size() && (lower[cursor] == ' ' || lower[cursor] == '\t')) ++cursor;
            if (cursor >= lower.size()) break;

            std::size_t valueBegin = lineStart + cursor;
            const char quote = (text[valueBegin] == '"' || text[valueBegin] == '\'')
                                   ? text[valueBegin]
                                   : '\0';
            if (quote) ++valueBegin;

            std::size_t valueEnd = valueBegin;
            if (quote) {
                while (valueEnd < lineEnd && text[valueEnd] != quote) ++valueEnd;
            } else {
                while (valueEnd < lineEnd && text[valueEnd] != ' ' && text[valueEnd] != '\t' &&
                       text[valueEnd] != ',' && text[valueEnd] != ';' && text[valueEnd] != '&' &&
                       text[valueEnd] != '"' && text[valueEnd] != '\'') {
                    ++valueEnd;
                }
            }

            if (valueEnd > valueBegin && valueEnd - valueBegin >= 4 &&
                text.compare(valueBegin, valueEnd - valueBegin, "***") != 0) {
                // Authorization: Bearer xxx —— 冒号后的值只是 scheme，
                // 真正的凭据在下一段，要连着一起替换
                if (!quote) {
                    const std::string scheme =
                        toLower(text.substr(valueBegin, valueEnd - valueBegin));
                    if (scheme == "bearer" || scheme == "basic" || scheme == "token") {
                        std::size_t next = valueEnd;
                        while (next < lineEnd && (text[next] == ' ' || text[next] == '\t')) ++next;
                        std::size_t last = next;
                        while (last < lineEnd && text[last] != ' ' && text[last] != '\t' &&
                               text[last] != ',' && text[last] != ';' && text[last] != '&' &&
                               text[last] != '"' && text[last] != '\'') {
                            ++last;
                        }
                        if (last > next) valueEnd = last;
                    }
                }
                ranges.emplace_back(valueBegin, valueEnd);
            }
            searchFrom = cursor + 1;
        }
        lineStart = lineEnd + 1;
    }

    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        text.replace(it->first, it->second - it->first, "***");
    }
    return ranges.size();
}

bool containsCommandPattern(const std::string& command,
                            const std::vector<std::string>& patterns) {
    const std::string lower = toLower(command);
    for (const std::string& pattern : patterns) {
        if (pattern.empty()) continue;
        if (lower.find(toLower(pattern)) != std::string::npos) return true;
    }
    return false;
}

std::string firstLine(const std::string& text, std::size_t limit) {
    const std::size_t newline = text.find('\n');
    std::string line = newline == std::string::npos ? text : text.substr(0, newline);
    if (line.size() > limit) line = line.substr(0, limit) + "…";
    return line;
}

std::string contentPreview(const std::string& content, std::size_t maxBytes) {
    bool truncated = false;
    std::string preview = truncateUtf8(content, maxBytes, truncated);
    if (truncated) preview += "\n…（预览已截断）";
    return preview;
}

Json makeError(const std::string& message) {
    Json error = Json::object();
    error["error"] = Json::string(message);
    return error;
}

}  // namespace

// ---------------------------------------------------------------- 策略

ToolPolicy defaultToolPolicy() {
    ToolPolicy policy;

    policy.deniedReadPaths = {
        // 私钥与证书
        "*/.ssh/id_*", "*/.ssh/*.pem", "*/id_rsa*", "*/id_dsa*", "*/id_ecdsa*", "*/id_ed25519*",
        "*.pem", "*.key", "*.p12", "*.pfx",
        // 各类凭据文件
        "*/.aws/credentials", "*/.aws/config", "*/.azure/*", "*/.config/gcloud/*",
        "*/.netrc", "*/.git-credentials", "*/.docker/config.json", "*/.kube/config",
        "*/.npmrc", "*/.pypirc", "*/.gem/credentials",
        // rshell 自己的密钥库
        "*/.rshell/*", "*/vault.dat", "*/vault.dat.*",
        // 系统敏感文件
        "/etc/shadow", "/etc/gshadow", "/etc/sudoers", "/etc/sudoers.d/*",
    };

    policy.dangerousCommandPatterns = {
        "rm -rf /", "rm -rf /*", "mkfs", "dd if=", "> /dev/sd", "of=/dev/sd",
        "shutdown", "reboot", "halt", "init 0", "chmod 777 /", "chown -r /",
        ":(){", "sudo su", "passwd", "userdel", "iptables -f", "curl | sh", "wget | sh",
        "truncate -s 0", "> /etc/",
    };

    // 自动审批模式下仍然必须人来点的操作：删除类、不可逆、账号/服务/防火墙、
    // 包卸载、容器删除。宁可多问一句，也不让模型自己决定「这条 rm 安不安全」。
    policy.highRiskPatterns = {
        // 删除 / 覆盖
        "rm ", "rmdir", "unlink", "shred", "truncate", "-delete",
        // 磁盘 / 分区
        "mkfs", "dd if=", "dd of=", "fdisk", "parted", "wipefs", "> /dev/sd", "of=/dev/sd",
        // 账号 / 权限 / 提权
        "userdel", "groupdel", "chpasswd", "visudo", "sudoers", "chmod 777", "chown -r",
        "setfacl", "usermod",
        // 服务 / 防火墙 / 关机
        "systemctl stop", "systemctl disable", "systemctl mask", "systemctl restart",
        "iptables", "nft ", "ufw ", "shutdown", "reboot", "halt", "init 0",
        "kill -9", "pkill", "killall",
        // 包管理卸载
        "apt remove", "apt purge", "apt-get remove", "apt-get purge", "yum remove",
        "dnf remove", "apk del", "snap remove", "pip uninstall",
        // 容器 / 编排
        "docker rm", "docker rmi", "docker prune", "docker volume rm", "docker system prune",
        "kubectl delete", "helm uninstall",
        // 版本控制 / 数据库
        "git clean", "git reset --hard", "drop table", "drop database", "truncate table",
        // 写系统目录（读不受影响）
        "> /etc/", ">> /etc/", "tee /etc/", "> /boot/", "tee /boot/",
    };
    // write_file 的目标路径命中这些就需要确认（读文件不受限）
    policy.highRiskWritePaths = {
        "/etc/*", "/boot/*", "/usr/lib/systemd/*", "/lib/systemd/*", "*/.ssh/*",
        "*/sudoers", "*/sudoers.d/*", "*/authorized_keys", "*/crontab*", "*/cron.d/*",
        "*/.bashrc", "*/.profile", "*/.bash_profile",
    };

    return policy;
}

// ---------------------------------------------------------------- 纯函数

bool readPathDenied(const std::string& path, const std::vector<std::string>& patterns) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    for (const std::string& pattern : patterns) {
        if (pattern.empty()) continue;
        if (globMatch(pattern, normalized)) return true;
        // 相对路径与绝对路径都要能命中：~/.ssh/id_rsa 不带前导 /
        if (!normalized.empty() && normalized.front() != '/' && globMatch(pattern, "/" + normalized))
            return true;
    }
    return false;
}

std::size_t redactSecrets(std::string& text) {
    std::size_t count = 0;
    count += redactPemBlocks(text);
    count += redactPrefixedTokens(text);
    count += redactJwt(text);
    count += redactAssignments(text);
    return count;
}

std::string truncateUtf8(const std::string& text, std::size_t maxBytes, bool& truncated) {
    truncated = false;
    if (text.size() <= maxBytes) return text;
    truncated = true;
    std::size_t cut = maxBytes;
    // 退到 UTF-8 字符边界，避免把多字节字符劈开
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut);
}

// ---------------------------------------------------------------- 工具注册

ToolRegistry::ToolRegistry(ToolBackend& backend, ToolPolicy policy)
    : backend_(backend), policy_(std::move(policy)) {}

std::vector<ToolSpec> ToolRegistry::specs() const {
    std::vector<ToolSpec> specs;

    {
        ToolSpec spec;
        spec.name = "run_command";
        spec.description =
            "在远程主机上执行一条 shell 命令，返回 stdout 与 stderr 合并后的文本和退出码。"
            "写操作，需要用户确认。命令应尽量短、无交互（不要用 vim/top 这类全屏程序），"
            "需要分页时自行加 | head 或 | tail。";
        Json properties = Json::object();
        properties["command"] = Json::object();
        properties["command"]["type"] = Json::string("string");
        properties["command"]["description"] = Json::string("要执行的命令");
        properties["timeout_sec"] = Json::object();
        properties["timeout_sec"]["type"] = Json::string("integer");
        properties["timeout_sec"]["description"] = Json::string("超时秒数，默认 60，最大 300");
        properties["host"] = Json::object();
        properties["host"]["type"] = Json::string("string");
        properties["host"]["description"] = Json::string(
            "可选：目标会话名（rshell 里已打开的其它标签）。留空表示当前标签的主机；"
            "要操作别的标签时填它的会话名，例如 us-vps。");
        properties["risk"] = Json::object();
        properties["risk"]["type"] = Json::string("string");
        properties["risk"]["description"] = Json::string(
            "可选：这次操作会删除数据、不可逆或影响线上服务时填 high，"
            "强制用户确认；其余情况不用填。");
        spec.parameters["type"] = Json::string("object");
        spec.parameters["properties"] = std::move(properties);
        spec.parameters["required"].push(Json::string("command"));
        specs.push_back(std::move(spec));
    }

    {
        ToolSpec spec;
        spec.name = "read_file";
        spec.description =
            "读取远程主机上的一个文本文件。读操作，不需要用户确认；"
            "私钥、云凭据等敏感路径会被自动拒绝。大文件只返回开头部分并标记截断。";
        Json properties = Json::object();
        properties["path"] = Json::object();
        properties["path"]["type"] = Json::string("string");
        properties["path"]["description"] = Json::string("绝对路径，或用 ~ 表示家目录");
        properties["max_bytes"] = Json::object();
        properties["max_bytes"]["type"] = Json::string("integer");
        properties["max_bytes"]["description"] = Json::string("最多读取字节数");
        properties["host"] = Json::object();
        properties["host"]["type"] = Json::string("string");
        properties["host"]["description"] = Json::string(
            "可选：目标会话名（rshell 里已打开的其它标签）。留空表示当前标签的主机；"
            "要操作别的标签时填它的会话名，例如 us-vps。");
        properties["risk"] = Json::object();
        properties["risk"]["type"] = Json::string("string");
        properties["risk"]["description"] = Json::string(
            "可选：这次操作会删除数据、不可逆或影响线上服务时填 high，"
            "强制用户确认；其余情况不用填。");
        spec.parameters["type"] = Json::string("object");
        spec.parameters["properties"] = std::move(properties);
        spec.parameters["required"].push(Json::string("path"));
        specs.push_back(std::move(spec));
    }

    {
        ToolSpec spec;
        spec.name = "list_dir";
        spec.description = "列出远程目录下的条目（名称、类型、大小）。读操作，不需要确认。";
        Json properties = Json::object();
        properties["path"] = Json::object();
        properties["path"]["type"] = Json::string("string");
        properties["path"]["description"] = Json::string("目录路径");
        properties["host"] = Json::object();
        properties["host"]["type"] = Json::string("string");
        properties["host"]["description"] = Json::string(
            "可选：目标会话名（rshell 里已打开的其它标签）。留空表示当前标签的主机；"
            "要操作别的标签时填它的会话名，例如 us-vps。");
        properties["risk"] = Json::object();
        properties["risk"]["type"] = Json::string("string");
        properties["risk"]["description"] = Json::string(
            "可选：这次操作会删除数据、不可逆或影响线上服务时填 high，"
            "强制用户确认；其余情况不用填。");
        spec.parameters["type"] = Json::string("object");
        spec.parameters["properties"] = std::move(properties);
        spec.parameters["required"].push(Json::string("path"));
        specs.push_back(std::move(spec));
    }

    {
        ToolSpec spec;
        spec.name = "write_file";
        spec.description =
            "把内容写入远程文件（覆盖或新建）。写操作，需要用户确认并会显示内容预览。"
            "修改配置文件前，建议先用 read_file 读原内容。";
        Json properties = Json::object();
        properties["path"] = Json::object();
        properties["path"]["type"] = Json::string("string");
        properties["path"]["description"] = Json::string("目标文件绝对路径");
        properties["content"] = Json::object();
        properties["content"]["type"] = Json::string("string");
        properties["content"]["description"] = Json::string("完整的新文件内容");
        properties["host"] = Json::object();
        properties["host"]["type"] = Json::string("string");
        properties["host"]["description"] = Json::string(
            "可选：目标会话名（rshell 里已打开的其它标签）。留空表示当前标签的主机；"
            "要操作别的标签时填它的会话名，例如 us-vps。");
        properties["risk"] = Json::object();
        properties["risk"]["type"] = Json::string("string");
        properties["risk"]["description"] = Json::string(
            "可选：这次操作会删除数据、不可逆或影响线上服务时填 high，"
            "强制用户确认；其余情况不用填。");
        spec.parameters["type"] = Json::string("object");
        spec.parameters["properties"] = std::move(properties);
        spec.parameters["required"].push(Json::string("path"));
        spec.parameters["required"].push(Json::string("content"));
        specs.push_back(std::move(spec));
    }

    return specs;
}

ToolKind ToolRegistry::kindOf(const std::string& name) const {
    if (name == "read_file" || name == "list_dir") return ToolKind::Read;
    return ToolKind::Write;
}

bool ToolRegistry::known(const std::string& name) const {
    return name == "run_command" || name == "read_file" || name == "list_dir" ||
           name == "write_file";
}

namespace {
// 可选的 host 参数：留空表示「当前目标主机」
std::string hostArg(const Json& args) {
    const Json* host = args.find("host");
    return host && host->isString() ? host->asString() : std::string();
}

// 审批卡片/摘要的前缀：「在 us-vps 上」；没指定 host 时为空
std::string targetHostPrefix(const Json& args) {
    const std::string host = hostArg(args);
    return host.empty() ? std::string() : ("在 " + host + " 上");
}
}  // namespace

ToolRequestInfo ToolRegistry::describe(const std::string& name, const std::string& arguments) const {
    ToolRequestInfo info;
    info.name = name;
    info.kind = kindOf(name);

    if (!known(name)) {
        info.valid = false;
        info.problem = "未知工具：" + name;
        return info;
    }

    Json args;
    std::string error;
    if (!Json::parse(arguments, args, &error) || !args.isObject()) {
        info.valid = false;
        info.problem = "工具参数不是合法 JSON：" + (error.empty() ? arguments : error);
        return info;
    }

    const Json* path = args.find("path");
    const std::string pathText = path && path->isString() ? path->asString() : std::string();
    // 指定了别的会话时，审批卡片上要写清楚「在哪台机器上」
    const std::string onHost = targetHostPrefix(args);
    // 模型自己判断这条有风险时，也可以主动要求用户确认（risk=high）
    if (const Json* risk = args.find("risk");
        risk && risk->isString() && toLower(risk->asString()) == "high") {
        info.highRisk = true;
    }

    if (name == "run_command") {
        const Json* command = args.find("command");
        const std::string text = command && command->isString() ? command->asString() : std::string();
        if (text.empty()) {
            info.valid = false;
            info.problem = "命令为空";
            return info;
        }
        info.summary = onHost + "执行命令：" + firstLine(text, 100);
        info.detail = text;
        info.dangerous = containsCommandPattern(text, policy_.dangerousCommandPatterns);
        // 删除、磁盘、账号、服务、包卸载、写系统目录 → 自动审批也要问
        info.highRisk = info.highRisk || info.dangerous ||
                        containsCommandPattern(text, policy_.highRiskPatterns);
    } else if (name == "write_file") {
        const Json* content = args.find("content");
        const std::string text = content && content->isString() ? content->asString() : std::string();
        if (pathText.empty()) {
            info.valid = false;
            info.problem = "缺少目标路径";
            return info;
        }
        info.summary = onHost + "写入 " + pathText + "（" + formatBytes(text.size()) + "）";
        info.detail = contentPreview(text, 4096);
        // 覆盖系统目录/密钥/计划任务属于敏感写入
        for (const std::string& pattern : policy_.highRiskWritePaths) {
            if (globMatch(pattern, pathText)) {
                info.highRisk = true;
                break;
            }
        }
    } else if (name == "read_file") {
        info.summary = onHost + "读取 " + pathText;
        info.detail = pathText;
    } else {  // list_dir
        info.summary = onHost + "列出目录 " + pathText;
        info.detail = pathText;
    }
    return info;
}

ToolResult ToolRegistry::execute(const std::string& name, const std::string& arguments) {
    Json args;
    std::string error;
    if (!Json::parse(arguments, args, &error) || !args.isObject()) {
        ToolResult result;
        result.ok = false;
        result.json = makeError("工具参数不是合法 JSON：" + (error.empty() ? arguments : error)).dump();
        result.display = "参数错误";
        return result;
    }

    try {
        if (name == "run_command") return runCommand(args);
        if (name == "read_file") return readFile(args);
        if (name == "write_file") return writeFile(args);
        if (name == "list_dir") return listDir(args);
    } catch (const std::exception& e) {
        ToolResult result;
        result.ok = false;
        result.json = makeError(std::string("工具执行异常：") + e.what()).dump();
        result.display = "执行异常";
        return result;
    }

    ToolResult result;
    result.ok = false;
    result.json = makeError("未知工具：" + name).dump();
    result.display = "未知工具";
    return result;
}

ToolResult ToolRegistry::runCommand(const Json& args) {
    const Json* command = args.find("command");
    if (!command || !command->isString() || command->asString().empty()) {
        ToolResult result;
        result.ok = false;
        result.json = makeError("缺少 command 参数").dump();
        result.display = "缺少命令";
        return result;
    }

    int timeout = policy_.commandTimeoutSec;
    if (const Json* value = args.find("timeout_sec"); value && value->isNumber())
        timeout = std::clamp(value->asInt(), 5, 300);

    const std::string text = command->asString();
    ToolBackend::CommandResult raw = backend_.runCommand(text, timeout, hostArg(args));

    bool truncated = raw.truncated;
    std::string output = raw.output;
    if (output.size() > policy_.maxOutputBytes) {
        output = truncateUtf8(output, policy_.maxOutputBytes, truncated);
        truncated = true;
    }

    std::size_t redacted = 0;
    if (policy_.redactSecrets) redacted = redactSecrets(output);

    Json payload = Json::object();
    payload["exit_code"] = Json::number(raw.exitCode);
    payload["output"] = Json::string(output);
    payload["truncated"] = Json::boolean(truncated);
    if (redacted) payload["redacted"] = Json::number(static_cast<double>(redacted));

    ToolResult result;
    result.ok = raw.exitCode == 0;
    result.json = payload.dump();
    result.truncated = truncated;
    // 卡片右上角只放短标记；完整说明留在 display（展开时显示）
    result.brief = "退出码 " + std::to_string(raw.exitCode) + (truncated ? " · 截断" : "");
    result.display = "命令退出码 " + std::to_string(raw.exitCode) +
                     (truncated ? "（输出已截断）" : std::string()) +
                     (redacted ? "（已隐藏 " + std::to_string(redacted) + " 处凭据）" : std::string());
    return result;
}

ToolResult ToolRegistry::readFile(const Json& args) {
    const Json* path = args.find("path");
    if (!path || !path->isString() || path->asString().empty()) {
        ToolResult result;
        result.ok = false;
        result.json = makeError("缺少 path 参数").dump();
        result.display = "缺少路径";
        return result;
    }
    const std::string target = path->asString();

    if (readPathDenied(target, policy_.deniedReadPaths)) {
        ToolResult result;
        result.ok = false;
        result.denied = true;
        result.json = makeError("该路径被安全策略禁止读取（可能包含私钥或凭据）：" + target).dump();
        result.display = "已拒绝：敏感路径";
        return result;
    }

    std::size_t limit = policy_.maxReadBytes;
    if (const Json* value = args.find("max_bytes"); value && value->isNumber()) {
        limit = std::min<std::size_t>(static_cast<std::size_t>(std::max(0, value->asInt())),
                                      policy_.maxReadBytes);
        if (limit == 0) limit = policy_.maxReadBytes;
    }

    std::string content;
    bool truncated = false;
    std::string error;
    if (!backend_.readFile(target, limit, content, truncated, error, hostArg(args))) {
        ToolResult result;
        result.ok = false;
        result.json = makeError(error.empty() ? "读取失败" : error).dump();
        result.display = "读取失败：" + target;
        return result;
    }

    std::size_t redacted = 0;
    if (policy_.redactSecrets) redacted = redactSecrets(content);

    Json payload = Json::object();
    payload["path"] = Json::string(target);
    payload["content"] = Json::string(content);
    payload["truncated"] = Json::boolean(truncated);
    if (redacted) payload["redacted"] = Json::number(static_cast<double>(redacted));

    ToolResult result;
    result.json = payload.dump();
    result.truncated = truncated;
    result.brief = formatBytes(content.size()) + (truncated ? " · 截断" : "");
    result.display = "已读取 " + target + "（" + formatBytes(content.size()) + "）" +
                     (truncated ? "（已截断）" : std::string()) +
                     (redacted ? "（已隐藏 " + std::to_string(redacted) + " 处凭据）" : std::string());
    return result;
}

ToolResult ToolRegistry::writeFile(const Json& args) {
    const Json* path = args.find("path");
    const Json* content = args.find("content");
    if (!path || !path->isString() || path->asString().empty() || !content || !content->isString()) {
        ToolResult result;
        result.ok = false;
        result.json = makeError("缺少 path 或 content 参数").dump();
        result.display = "参数不完整";
        return result;
    }

    const std::string target = path->asString();
    const std::string& text = content->asString();
    if (text.size() > 256u * 1024u) {
        ToolResult result;
        result.ok = false;
        result.json = makeError("单次写入不能超过 256 KB").dump();
        result.display = "内容过大";
        return result;
    }

    std::string error;
    if (!backend_.writeFile(target, text, error, hostArg(args))) {
        ToolResult result;
        result.ok = false;
        result.json = makeError(error.empty() ? "写入失败" : error).dump();
        result.display = "写入失败：" + target;
        return result;
    }

    Json payload = Json::object();
    payload["path"] = Json::string(target);
    payload["bytes_written"] = Json::number(static_cast<double>(text.size()));

    ToolResult result;
    result.json = payload.dump();
    result.brief = formatBytes(text.size());
    result.display = "已写入 " + target + "（" + formatBytes(text.size()) + "）";
    return result;
}

ToolResult ToolRegistry::listDir(const Json& args) {
    const Json* path = args.find("path");
    if (!path || !path->isString() || path->asString().empty()) {
        ToolResult result;
        result.ok = false;
        result.json = makeError("缺少 path 参数").dump();
        result.display = "缺少路径";
        return result;
    }
    const std::string target = path->asString();

    std::vector<DirEntryView> entries;
    std::string error;
    if (!backend_.listDir(target, entries, error, hostArg(args))) {
        ToolResult result;
        result.ok = false;
        result.json = makeError(error.empty() ? "列目录失败" : error).dump();
        result.display = "列目录失败：" + target;
        return result;
    }

    const bool truncated = entries.size() > policy_.maxEntries;
    if (truncated) entries.resize(policy_.maxEntries);

    Json list = Json::array();
    for (const DirEntryView& entry : entries) {
        Json item = Json::object();
        item["name"] = Json::string(entry.name);
        item["type"] = Json::string(entry.isDir ? "dir" : "file");
        if (!entry.isDir) item["size"] = Json::number(static_cast<double>(entry.size));
        list.push(std::move(item));
    }

    Json payload = Json::object();
    payload["path"] = Json::string(target);
    payload["entries"] = std::move(list);
    payload["truncated"] = Json::boolean(truncated);

    ToolResult result;
    result.json = payload.dump();
    result.truncated = truncated;
    result.brief = std::to_string(entries.size()) + " 项" + (truncated ? " · 截断" : "");
    result.display = "已列出 " + target + "（" + std::to_string(entries.size()) + " 项" +
                     (truncated ? "，已截断" : "") + "）";
    return result;
}

}  // namespace rshell::ai
