// config.cpp
#include "config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "util.h"

namespace rshell {

namespace {

// 去掉行内注释（引号内的 # 不算）
std::string stripComment(const std::string& line) {
    bool inSingle = false;
    bool inDouble = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\'' && !inDouble) {
            inSingle = !inSingle;
        } else if (c == '"' && !inSingle) {
            inDouble = !inDouble;
        } else if (c == '#' && !inSingle && !inDouble) {
            return line.substr(0, i);
        }
    }
    return line;
}

std::string unquote(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string word;
    while (iss >> word) out.push_back(unquote(word));
    return out;
}

bool hostPatternMatches(const std::string& pattern, const std::string& alias) {
    return globMatch(toLower(pattern), toLower(alias));
}

}  // namespace

std::string rshellConfigPath() { return homeDirectory() + "/.rshell/config"; }

SshConfig SshConfig::load(const std::string& path) {
    SshConfig cfg;
    if (!path.empty()) {
        cfg.path_ = path;
    } else if (const char* env = std::getenv("RSHELL_SSH_CONFIG")) {
        cfg.path_ = env;
    } else {
        cfg.path_ = homeDirectory() + "/.ssh/config";
    }

    std::ifstream in(utf8ToPath(cfg.path_), std::ios::binary);
    if (!in) {
        log().debug("未找到 SSH 配置文件：" + cfg.path_);
        return cfg;
    }
    cfg.loaded_ = true;

    Block* current = &cfg.global_;
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = trim(stripComment(raw));
        if (line.empty()) continue;

        size_t sep = line.find_first_of(" \t=");
        std::string key = toLower(sep == std::string::npos ? line : line.substr(0, sep));
        std::string value = sep == std::string::npos ? std::string() : trim(line.substr(sep + 1));

        if (key == "host") {
            cfg.blocks_.emplace_back();
            Block& block = cfg.blocks_.back();  // emplace_back 之后重新取引用
            block.patterns = splitWords(value);
            current = &block;
            continue;
        }
        if (key == "match") {
            // Match 条件块暂不支持，跳过其内容直到下一个 Host
            current = nullptr;
            continue;
        }
        if (!current) continue;

        if (key == "hostname") {
            current->values.hostName = unquote(value);
        } else if (key == "user") {
            current->values.user = unquote(value);
        } else if (key == "port") {
            try {
                current->values.port = std::stoi(value);
            } catch (...) {
                log().debug("忽略无效的 Port 配置：" + value);
            }
        } else if (key == "identityfile") {
            current->values.identityFile = expandPath(unquote(value));
        } else if (key == "include") {
            log().debug("忽略 Include 指令：" + value);
        }
    }
    return cfg;
}

bool SshConfig::lookup(const std::string& alias, HostConfig& out) const {
    HostConfig result;
    result.alias = alias;

    auto apply = [](HostConfig& dst, const HostConfig& src) {
        if (dst.hostName.empty()) dst.hostName = src.hostName;
        if (dst.user.empty()) dst.user = src.user;
        if (dst.identityFile.empty()) dst.identityFile = src.identityFile;
        if (dst.port == 0) dst.port = src.port;
    };

    bool matched = false;
    for (const Block& block : blocks_) {
        bool positive = false;
        bool negative = false;
        for (const std::string& pattern : block.patterns) {
            if (!pattern.empty() && pattern[0] == '!') {
                if (hostPatternMatches(pattern.substr(1), alias)) negative = true;
            } else if (hostPatternMatches(pattern, alias)) {
                positive = true;
            }
        }
        if (negative || !positive) continue;
        matched = true;
        apply(result, block.values);
    }

    // 全局段作为兜底：Host 段中的值优先，未设置的项才回落到全局段
    apply(result, global_.values);

    if (result.hostName.empty()) result.hostName = alias;
    out = result;
    return matched;
}

std::vector<std::string> SshConfig::hostNames() const {
    std::vector<std::string> names;
    for (const Block& block : blocks_) {
        for (const std::string& pattern : block.patterns) {
            if (pattern.empty() || pattern[0] == '!' || hasWildcard(pattern)) continue;
            bool dup = false;
            for (const std::string& existing : names) {
                if (existing == pattern) {
                    dup = true;
                    break;
                }
            }
            if (!dup) names.push_back(pattern);
        }
    }
    return names;
}

RshellSettings loadRshellSettings(const std::string& path) {
    RshellSettings settings;
    const std::string file = path.empty() ? rshellConfigPath() : path;
    std::ifstream in(utf8ToPath(file), std::ios::binary);
    if (!in) return settings;

    std::string section;
    std::string aliasName;
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = trim(stripComment(raw));
        if (line.empty()) continue;

        if (line.front() == '[') {
            size_t close = line.find(']');
            std::string name =
                toLower(trim(line.substr(1, close == std::string::npos ? std::string::npos
                                                                       : close - 1)));
            if (startsWith(name, "alias ") || startsWith(name, "alias:")) {
                section = "alias";
                aliasName = trim(name.substr(6));
            } else if (name == "default" || name == "defaults" || name.empty()) {
                section = "default";
            } else {
                section = "unknown";
            }
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = toLower(trim(line.substr(0, eq)));
        std::string value = unquote(trim(line.substr(eq + 1)));
        if (value.empty()) continue;

        auto parseBool = [](const std::string& v) {
            return iequals(v, "true") || iequals(v, "yes") || iequals(v, "on") || v == "1";
        };

        if (section == "default") {
            if (key == "user") {
                settings.defaultUser = value;
            } else if (key == "key" || key == "identityfile") {
                settings.defaultKey = expandPath(value);
            } else if (key == "port") {
                settings.defaultPort = std::atoi(value.c_str());
            } else if (key == "timeout") {
                settings.timeoutSec = std::atoi(value.c_str());
            } else if (key == "progress") {
                settings.progress = parseBool(value);
            } else if (key == "color") {
                settings.color = parseBool(value);
            } else if (key == "downloaddir") {
                settings.downloadDir = expandPath(value);
            }
        } else if (section == "alias" && !aliasName.empty()) {
            RshellSettings::Alias& alias = settings.aliases[aliasName];
            if (key == "host" || key == "hostname") {
                alias.host = value;
            } else if (key == "user") {
                alias.user = value;
            } else if (key == "key" || key == "identityfile") {
                alias.key = expandPath(value);
            } else if (key == "port") {
                alias.port = std::atoi(value.c_str());
            }
        }
    }
    if (settings.timeoutSec <= 0) settings.timeoutSec = 20;
    return settings;
}

RemoteTarget parseRemoteTarget(const std::string& spec) {
    RemoteTarget target;
    std::string rest = trim(spec);
    if (rest.empty()) return target;

    size_t at = rest.find('@');
    if (at != std::string::npos && at > 0 && at + 1 < rest.size()) {
        target.user = rest.substr(0, at);
        rest = rest.substr(at + 1);
    }

    const bool looksLikePath =
        rest[0] == '/' || rest[0] == '~' || rest[0] == '.' || rest[0] == '\\';
    if (looksLikePath) {
        target.path = rest;
        target.hasPath = true;
        return target;
    }

    size_t colon = rest.find(':');
    if (colon != std::string::npos) {
        // 排除 Windows 盘符（C:\dir）以及 IPv6 字面量中的冒号
        const bool driveLetter =
            colon == 1 && std::isalpha(static_cast<unsigned char>(rest[0])) != 0;
        const bool ipv6 = rest.find("::") != std::string::npos || rest.find(']') != std::string::npos;
        if (!driveLetter && !ipv6) {
            target.host = rest.substr(0, colon);
            std::string path = rest.substr(colon + 1);
            if (!path.empty()) {
                target.path = path;
                target.hasPath = true;
            }
            return target;
        }
    }
    target.host = rest;
    return target;
}

unsigned int parseModeString(const std::string& text) {
    std::string s = trim(text);
    if (s.empty()) throw Error("权限字符串为空");
    unsigned int value = 0;
    for (char c : s) {
        if (c < '0' || c > '7')
            throw Error("无法解析权限值：" + text + "（请使用 0644 这样的八进制写法）");
        value = value * 8u + static_cast<unsigned int>(c - '0');
    }
    if (value > 07777) throw Error("权限值超出范围：" + text);
    return value;
}

}  // namespace rshell
