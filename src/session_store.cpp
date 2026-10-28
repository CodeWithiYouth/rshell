// session_store.cpp
#include "session_store.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "config.h"
#include "util.h"

namespace rshell {

namespace {

std::string defaultPath() {
    if (const char* env = std::getenv("RSHELL_SESSIONS")) return env;
    return homeDirectory() + "/.rshell/sessions.ini";
}

}  // namespace

SessionStore SessionStore::load(const std::string& path) {
    SessionStore store;
    store.path_ = path.empty() ? defaultPath() : path;

    std::ifstream in(utf8ToPath(store.path_), std::ios::binary);
    if (!in) return store;
    store.loaded_ = true;

    Session current;
    bool inSession = false;
    auto flush = [&]() {
        if (!inSession) return;
        if (current.name.empty()) current.name = current.host;
        if (!current.name.empty() && !current.host.empty()) store.sessions_.push_back(current);
        current = Session();
    };

    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[') {
            flush();
            std::size_t end = line.find(']');
            std::string section = line.substr(1, end == std::string::npos ? std::string::npos : end - 1);
            inSession = startsWith(section, "session:") || startsWith(section, "session ");
            if (inSession) {
                current.name = trim(section.substr(8));
            }
            continue;
        }
        if (!inSession) continue;

        std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = toLower(trim(line.substr(0, eq)));
        std::string value = trim(line.substr(eq + 1));
        if (key == "host" || key == "hostname") {
            current.host = value;
        } else if (key == "user") {
            current.user = value;
        } else if (key == "port") {
            current.port = std::atoi(value.c_str());
        } else if (key == "key" || key == "identityfile") {
            current.keyPath = expandPath(value);
        } else if (key == "note" || key == "comment") {
            current.note = value;
        } else if (key == "savepassword" || key == "save-password") {
            current.savePassword = (value == "1" || iequals(value, "true") ||
                                    iequals(value, "yes") || iequals(value, "on"));
        } else if (key == "keepalive" || key == "keep-alive") {
            current.keepAliveSec = std::atoi(value.c_str());
        } else if (key == "proxy") {
            current.proxyUrl = value;
        } else if (key == "proxyuser" || key == "proxy-user") {
            current.proxyUser = value;
        } else if (key == "saveproxypassword" || key == "save-proxy-password") {
            current.saveProxyPassword = (value == "1" || iequals(value, "true") ||
                                         iequals(value, "yes") || iequals(value, "on"));
        } else if (key == "savekeypassphrase" || key == "save-key-passphrase") {
            current.saveKeyPassphrase = (value == "1" || iequals(value, "true") ||
                                         iequals(value, "yes") || iequals(value, "on"));
        }
    }
    flush();

    for (auto& s : store.sessions_) {
        if (s.port <= 0) s.port = 22;
        if (s.name.empty()) s.name = s.host;
    }
    return store;
}

bool SessionStore::save() const {
    fs::path p = utf8ToPath(path_);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);

    std::ostringstream out;
    out << "# rshell GUI 会话列表（可用 RSHELL_SESSIONS 指向其它文件）\n";
    for (const Session& s : sessions_) {
        out << "\n[session:" << s.name << "]\n";
        out << "host=" << s.host << "\n";
        out << "port=" << (s.port > 0 ? s.port : 22) << "\n";
        if (!s.user.empty()) out << "user=" << s.user << "\n";
        if (!s.keyPath.empty()) out << "key=" << s.keyPath << "\n";
        if (!s.note.empty()) out << "note=" << s.note << "\n";
        // 只记标记：密码/私钥口令在密码保险库里，条目名 rshell/<会话名>
        if (s.savePassword) out << "savepassword=1\n";
        if (s.saveKeyPassphrase) out << "savekeypassphrase=1\n";
        // 总是写：0 表示用户明确关掉了，不能靠"缺省即开启"覆盖掉它
        out << "keepalive=" << (s.keepAliveSec > 0 ? s.keepAliveSec : 0) << "\n";
        if (!s.proxyUrl.empty()) {
            out << "proxy=" << s.proxyUrl << "\n";
            if (!s.proxyUser.empty()) out << "proxyuser=" << s.proxyUser << "\n";
            if (s.saveProxyPassword) out << "saveproxypassword=1\n";
        }
    }

    std::ofstream file(p, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const std::string text = out.str();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

void SessionStore::upsert(const Session& session) {
    for (Session& s : sessions_) {
        if (s.name == session.name) {
            s = session;
            return;
        }
    }
    sessions_.push_back(session);
}

bool SessionStore::remove(const std::string& name) {
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (it->name == name) {
            sessions_.erase(it);
            return true;
        }
    }
    return false;
}

const Session* SessionStore::find(const std::string& name) const {
    for (const Session& s : sessions_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

void SessionStore::seedFromConfigIfEmpty() {
    if (!sessions_.empty()) return;

    // 1) ~/.ssh/config 中的 Host
    SshConfig sshConfig = SshConfig::load();
    for (const std::string& alias : sshConfig.hostNames()) {
        HostConfig hc;
        Session s;
        s.name = alias;
        if (sshConfig.lookup(alias, hc)) {
            s.host = hc.hostName.empty() ? alias : hc.hostName;
            s.user = hc.user;
            s.keyPath = hc.identityFile;
            s.port = hc.port > 0 ? hc.port : 22;
        } else {
            s.host = alias;
        }
        sessions_.push_back(s);
    }

    // 2) ~/.rshell/config 中的 [alias]
    RshellSettings settings = loadRshellSettings();
    for (const auto& kv : settings.aliases) {
        Session s;
        s.name = kv.first;
        s.host = kv.second.host;
        s.user = kv.second.user;
        s.keyPath = kv.second.key;
        s.port = kv.second.port > 0 ? kv.second.port : 22;
        if (s.host.empty()) continue;
        bool exists = false;
        for (const Session& existing : sessions_) {
            if (existing.name == s.name) {
                exists = true;
                break;
            }
        }
        if (!exists) sessions_.push_back(s);
    }
}

}  // namespace rshell
