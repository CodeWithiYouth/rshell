// ai_session.h - AI 对话记录（存到远端主机的 ~/.rshell/sessions/）
//
// 为什么存远端：rshell 的 AI 是「在这台机器上干活」的助手，对话上下文天然属于那台主机。
// 换台机器连同一台服务器，打开面板还能接着上次的思路；
// 本机只留 UI 布局和 API Key，不落对话内容。
//
// 这一层只有纯字符串/时间处理：怎么读写由 ai_tab 通过 SSH/SFTP 完成，
// 所以可以在单元测试里直接跑。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ai_tools.h"

namespace rshell::ai {

// 一条对话记录。只存纯文本轮次：工具卡片不进文件，
// 免得把命令输出（可能几十 KB）一起搬到远端。
struct SessionMessage {
    bool user = true;   // true = 用户，false = 助手
    std::string text;   // UTF-8
};

struct SessionRecord {
    std::string title;              // 模型起的中文标题
    std::vector<std::string> tags;  // 模型打的标签
    std::string host;               // 会话名，只用于展示
    std::int64_t created = 0;       // Unix 秒
    std::int64_t updated = 0;
    std::vector<SessionMessage> messages;
};

// 远端目录：<home>/.rshell/sessions
std::string sessionDir(const std::string& home);

// 标题 → 文件名里的 slug。只留 ASCII 字母数字，其余折叠成 '-'；
// 中文标题会退化成 "session"。
std::string sessionSlug(const std::string& title);

// 文件名：20261030-141233-<slug>.json。
// 同一个对话反复保存要沿用第一次的名字，由调用方把名字记下来。
std::string sessionFileName(const SessionRecord& record, std::int64_t stamp);

// 目录里只认这种名字，其它文件一律忽略（用户可能在同一目录放别的东西）
bool isSessionFileName(const std::string& name);

// 文件名里的时间（本地时区，解析失败返回 0）与展示用的 "10-30 14:12"
std::int64_t sessionStampFromName(const std::string& name);
std::string sessionStampText(std::int64_t stamp);

std::string serializeSession(const SessionRecord& record);
bool parseSession(const std::string& json, SessionRecord& out, std::string* error);

// ---------------------------------------------------------------- 远端读写
//
// 后端由调用方注入（生产是 SSH/SFTP，测试是假实现），这一层只管
// 「建目录 → 列文件 → 读最新 / 写一个文件」，线程与事件调度在 AiTab 里。

struct StoredSession {
    std::string file;  // 文件名（不是全路径）
    SessionRecord record;
};

// 取远端家目录并建好 <home>/.rshell/sessions；dir 作为缓存传给后续调用
bool ensureSessionDir(ToolBackend& backend, std::string& dir, std::string& error);

// 读最近 limit 段对话（文件名带时间戳，字典序就是时间序）
std::vector<StoredSession> readSessions(ToolBackend& backend, std::string& dir, int limit,
                                        std::string& error);

// 写一段对话（覆盖同名文件）
bool writeSessionFile(ToolBackend& backend, const std::string& dir, const std::string& file,
                      const SessionRecord& record, std::string& error);

}  // namespace rshell::ai
