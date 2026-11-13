// ai_session.cpp
#include "ai_session.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include "ai_json.h"
#include "util.h"

namespace rshell::ai {

namespace {

std::tm localTime(std::int64_t stamp) {
    std::time_t t = static_cast<std::time_t>(stamp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

// 按 UTF-8 边界截断，不把汉字劈成半个
std::string cutUtf8(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) return text;
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + "…";
}

std::string trimRight(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.pop_back();
    return text;
}

}  // namespace

std::string sessionDir(const std::string& home) {
    std::string base = home;
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) base.pop_back();
    if (base.empty()) return ".rshell/sessions";
    return base + "/.rshell/sessions";
}

std::string sessionSlug(const std::string& title) {
    std::string slug;
    for (unsigned char ch : title) {
        if (ch < 0x80 && std::isalnum(ch)) {
            slug.push_back(static_cast<char>(std::tolower(ch)));
        } else if (!slug.empty() && slug.back() != '-') {
            // 连续的空白、标点、中文折叠成一个分隔符
            slug.push_back('-');
        }
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.size() > 24) {
        slug.resize(24);
        while (!slug.empty() && slug.back() == '-') slug.pop_back();
    }
    if (slug.empty()) slug = "session";
    return slug;
}

std::string sessionFileName(const SessionRecord& record, std::int64_t stamp) {
    const std::tm tm = localTime(stamp);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buffer) + "-" + sessionSlug(record.title) + ".json";
}

bool isSessionFileName(const std::string& name) {
    // 20261030-141233-foo.json 至少这么长；前缀必须是时间戳，否则不算我们的文件
    if (name.size() < 20) return false;
    if (name.find_first_of("/\\") != std::string::npos) return false;
    if (name.compare(name.size() - 5, 5, ".json") != 0) return false;
    if (name.size() >= 10 && name.compare(name.size() - 10, 10, ".meta.json") == 0) return false;
    for (int i = 0; i < 8; ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[static_cast<std::size_t>(i)]))) return false;
    if (name[8] != '-') return false;
    for (int i = 9; i < 15; ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[static_cast<std::size_t>(i)]))) return false;
    return name[15] == '-';
}

std::int64_t sessionStampFromName(const std::string& name) {
    if (!isSessionFileName(name)) return 0;
    std::tm tm{};
    tm.tm_year = std::stoi(name.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(name.substr(4, 2)) - 1;
    tm.tm_mday = std::stoi(name.substr(6, 2));
    tm.tm_hour = std::stoi(name.substr(9, 2));
    tm.tm_min = std::stoi(name.substr(11, 2));
    tm.tm_sec = std::stoi(name.substr(13, 2));
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    return t == static_cast<std::time_t>(-1) ? 0 : static_cast<std::int64_t>(t);
}

std::string sessionStampText(std::int64_t stamp) {
    if (stamp <= 0) return {};
    const std::tm tm = localTime(stamp);
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%02d-%02d %02d:%02d", tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min);
    return buffer;
}

std::string serializeSession(const SessionRecord& record) {
    Json root = Json::object();
    root["version"] = Json::number(1);
    root["title"] = Json::string(record.title);
    Json tags = Json::array();
    for (const std::string& tag : record.tags) tags.push(Json::string(tag));
    root["tags"] = std::move(tags);
    root["host"] = Json::string(record.host);
    root["created"] = Json::number(static_cast<double>(record.created));
    root["updated"] = Json::number(static_cast<double>(record.updated));
    Json messages = Json::array();
    for (const SessionMessage& message : record.messages) {
        Json item = Json::object();
        item["role"] = Json::string(message.user ? "user" : "assistant");
        item["text"] = Json::string(message.text);
        messages.push(std::move(item));
    }
    root["messages"] = std::move(messages);
    return root.dump(2) + "\n";
}

bool parseSession(const std::string& json, SessionRecord& out, std::string* error) {
    Json root;
    std::string parseError;
    if (!Json::parse(json, root, &parseError) || !root.isObject()) {
        if (error) *error = parseError.empty() ? "不是合法的 JSON 对象" : parseError;
        return false;
    }
    SessionRecord record;
    if (const Json* title = root.find("title"); title && title->isString())
        record.title = title->asString();
    if (const Json* host = root.find("host"); host && host->isString()) record.host = host->asString();
    if (const Json* created = root.find("created"); created && created->isNumber())
        record.created = static_cast<std::int64_t>(created->asNumber());
    if (const Json* updated = root.find("updated"); updated && updated->isNumber())
        record.updated = static_cast<std::int64_t>(updated->asNumber());
    if (const Json* tags = root.find("tags"); tags && tags->isArray()) {
        for (const Json& tag : tags->items())
            if (tag.isString() && !tag.asString().empty()) record.tags.push_back(tag.asString());
    }
    if (const Json* messages = root.find("messages"); messages && messages->isArray()) {
        for (const Json& item : messages->items()) {
            if (!item.isObject()) continue;
            SessionMessage message;
            const Json* role = item.find("role");
            message.user = !(role && role->isString() && role->asString() == "assistant");
            const Json* text = item.find("text");
            if (text && text->isString()) message.text = text->asString();
            if (message.text.empty()) continue;  // 只有 tool_calls 的助手消息
            record.messages.push_back(std::move(message));
        }
    }
    // 标题可能是空的（老文件或模型没起名）：从第一条用户消息兜底，列表里才有东西可看
    if (record.title.empty()) {
        for (const SessionMessage& message : record.messages) {
            if (message.user) {
                std::string first = message.text.substr(0, message.text.find('\n'));
                record.title = cutUtf8(trimRight(first), 48);
                break;
            }
        }
    }
    out = std::move(record);
    return true;
}

// ---------------------------------------------------------------- 远端读写

bool ensureSessionDir(ToolBackend& backend, std::string& dir, std::string& error) {
    if (!dir.empty()) return true;
    // 家目录与建目录都走 SFTP：后台线程里不能碰 exec（libssh2 会话不是线程安全的）
    const std::string home = backend.homeDir();
    if (home.empty()) {
        error = "拿不到远端家目录（SFTP 未就绪）";
        return false;
    }
    dir = sessionDir(home);
    if (!backend.makeDir(dir, error)) {
        error = "建不了 " + dir + (error.empty() ? "" : "：" + error);
        return false;
    }
    return true;
}

std::vector<StoredSession> readSessions(ToolBackend& backend, std::string& dir, int limit,
                                        std::string& error) {
    error.clear();
    std::vector<StoredSession> entries;
    if (!ensureSessionDir(backend, dir, error)) return entries;
    std::vector<DirEntryView> files;
    if (!backend.listDir(dir, files, error, "")) {
        if (error.empty()) error = "无法列出历史会话目录";
        return entries;
    }

    std::vector<std::string> names;
    for (const DirEntryView& file : files)
        if (!file.isDir && isSessionFileName(file.name)) names.push_back(file.name);
    std::sort(names.begin(), names.end());

    for (auto it = names.rbegin();
         it != names.rend() && static_cast<int>(entries.size()) < limit; ++it) {
        std::string readError;
        StoredSession entry;
        if (!readSessionFile(backend, dir, *it, entry, readError)) {
            error = readError;
            continue;
        }
        entries.push_back(std::move(entry));
    }
    if (!entries.empty()) error.clear();
    return entries;
}

bool readSessionFile(ToolBackend& backend, const std::string& dir, const std::string& file,
                     StoredSession& out, std::string& error) {
    error.clear();
    if (dir.empty() || !isSessionFileName(file) || file.find_first_of("/\\") != std::string::npos) {
        error = "无效的对话文件名";
        return false;
    }
    std::string text;
    bool truncated = false;
    if (!backend.readFile(dir + "/" + file, kMaxSessionFileBytes, text, truncated, error, "")) {
        if (error.empty()) error = "无法读取对话文件 " + file;
        return false;
    }
    if (truncated) {
        error = "对话记录超过 512 KiB，未载入不完整内容";
        return false;
    }
    StoredSession entry;
    if (!parseSession(text, entry.record, &error)) return false;
    if (entry.record.messages.empty()) {
        error = "对话记录中没有可载入的消息";
        return false;
    }
    entry.file = file;
    entry.messageCount = entry.record.messages.size();
    entry.summaryAvailable = true;
    out = std::move(entry);
    return true;
}

std::vector<StoredSession> listSessions(ToolBackend& backend, std::string& dir, int limit,
                                       std::string& error) {
    error.clear();
    std::vector<StoredSession> entries;
    if (!ensureSessionDir(backend, dir, error)) return entries;
    std::vector<DirEntryView> files;
    if (!backend.listDir(dir, files, error, "")) {
        if (error.empty()) error = "无法列出历史会话目录";
        return entries;
    }
    std::vector<std::string> names;
    for (const auto& file : files) if (!file.isDir) names.push_back(file.name);
    std::sort(names.begin(), names.end());
    for (auto it = names.rbegin();
         it != names.rend() && static_cast<int>(entries.size()) < limit; ++it) {
        if (!isSessionFileName(*it)) continue;
        StoredSession entry;
        entry.file = *it;
        entry.record.title = it->substr(16, it->size() - 21);
        if (entry.record.title == "session" || entry.record.title.empty())
            entry.record.title = "未命名对话";
        entry.record.created = entry.record.updated = sessionStampFromName(*it);
        const std::string metadata = it->substr(0, it->size() - 5) + ".meta.json";
        if (std::binary_search(names.begin(), names.end(), metadata)) {
            std::string text, readError;
            bool truncated = false;
            Json root;
            if (backend.readFile(dir + "/" + metadata, 16u * 1024u, text, truncated, readError, "") &&
                !truncated && Json::parse(text, root, nullptr) && root.isObject()) {
                const Json* count = root.find("message_count");
                const Json* title = root.find("title");
                SessionRecord summary;
                if (title && title->isString() && !title->asString().empty() &&
                    count && count->isNumber() && count->asNumber() >= 0 &&
                    count->asNumber() <= 1000000 && parseSession(text, summary, nullptr)) {
                    entry.record = std::move(summary);
                    entry.record.messages.clear();
                    entry.messageCount = static_cast<std::size_t>(count->asNumber());
                    entry.summaryAvailable = true;
                }
            }
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool writeSessionFile(ToolBackend& backend, const std::string& dir, const std::string& file,
                      const SessionRecord& record, std::string& error) {
    error.clear();
    if (dir.empty() || !isSessionFileName(file)) {
        error = "缺少目录或文件名";
        return false;
    }
    const std::string serialized = serializeSession(record);
    if (serialized.size() > kMaxSessionFileBytes) {
        error = "对话记录超过 512 KiB，未保存；原有远端记录保持不变，当前聊天可继续";
        return false;
    }
    if (!backend.writeFile(dir + "/" + file, serialized, error, "")) return false;
    // 每份对话一个小摘要，互不竞争共享索引；摘要失败也不能使正文保存失效。
    Json metadata = Json::object();
    metadata["version"] = Json::number(1);
    metadata["title"] = Json::string(cutUtf8(record.title, 240));
    metadata["host"] = Json::string(cutUtf8(record.host, 96));
    metadata["created"] = Json::number(static_cast<double>(record.created));
    metadata["updated"] = Json::number(static_cast<double>(record.updated));
    Json tags = Json::array();
    for (const auto& tag : record.tags) {
        if (tags.size() == 8) break;
        tags.push(Json::string(cutUtf8(tag, 64)));
    }
    metadata["tags"] = std::move(tags);
    metadata["message_count"] = Json::number(static_cast<double>(record.messages.size()));
    const std::string name = file.substr(0, file.size() - 5) + ".meta.json";
    std::string metadataError;
    try {
        if (backend.writeFile(dir + "/" + name, metadata.dump() + "\n", metadataError, "")) return true;
    } catch (const std::exception& e) {
        metadataError = e.what();
    } catch (...) {
        metadataError = "摘要写入中断";
    }
    error = "对话已保存，历史列表摘要暂未更新" +
            (metadataError.empty() ? std::string() : "：" + metadataError);
    return true;
}

}  // namespace rshell::ai
