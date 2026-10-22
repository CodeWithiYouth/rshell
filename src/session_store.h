// session_store.h - GUI 会话的持久化（~/.rshell/sessions.ini）
#pragma once

#include <string>
#include <vector>

namespace rshell {

struct Session {
    std::string name;    // 显示名，同时是配置段名
    std::string host;    // 主机名或 IP
    std::string user;
    std::string keyPath; // 可空：使用密码或默认密钥
    std::string note;
    int port = 22;
    // 密码本身不写进 sessions.ini：这里只是“已经保存过密码”的标记。
    // 真正的密码由 credential_store.cpp 交给 Windows 凭据管理器或（GUI 解锁后）
    // 密码保险库保管。
    bool savePassword = false;
};

class SessionStore {
public:
    // path 为空时使用 ~/.rshell/sessions.ini（可用 RSHELL_SESSIONS 覆盖）
    static SessionStore load(const std::string& path = std::string());

    bool save() const;

    std::vector<Session>& sessions() { return sessions_; }
    const std::vector<Session>& sessions() const { return sessions_; }
    const std::string& path() const { return path_; }
    bool loaded() const { return loaded_; }

    void upsert(const Session& session);   // 按 name 覆盖或追加
    bool remove(const std::string& name);
    const Session* find(const std::string& name) const;

    // 首次使用时从 ~/.ssh/config 与 ~/.rshell/config 的 alias 生成初始列表
    void seedFromConfigIfEmpty();

private:
    std::string path_;
    std::vector<Session> sessions_;
    bool loaded_ = false;
};

}  // namespace rshell
