// config.h - ~/.ssh/config 与 rshell 自身配置文件解析
#pragma once

#include <map>
#include <string>
#include <vector>

namespace rshell {

// 单个 Host 段落中与连接相关的配置项（空字符串/0 表示未设置）
struct HostConfig {
    std::string alias;      // 查询时使用的名字
    std::string hostName;   // HostName，缺省为 alias
    std::string user;
    std::string identityFile;
    int port = 0;

    bool empty() const {
        return hostName.empty() && user.empty() && identityFile.empty() && port == 0;
    }
};

class SshConfig {
public:
    // path 为空时使用 ~/.ssh/config（可用环境变量 RSHELL_SSH_CONFIG 覆盖）
    static SshConfig load(const std::string& path = std::string());

    // 返回是否存在匹配块；不存在时 out 保持默认值
    bool lookup(const std::string& alias, HostConfig& out) const;

    // 所有可用的 Host 名称（用于补全/提示），不含通配模式
    std::vector<std::string> hostNames() const;

    const std::string& path() const { return path_; }
    bool loaded() const { return loaded_; }

private:
    struct Block {
        std::vector<std::string> patterns;
        HostConfig values;
    };

    std::string path_;
    bool loaded_ = false;
    Block global_;
    std::vector<Block> blocks_;
};

// rshell 自己的默认值（~/.rshell/config）
struct RshellSettings {
    std::string defaultUser;
    std::string defaultKey;
    int defaultPort = 0;
    int timeoutSec = 20;
    bool progress = true;
    bool color = true;
    std::string downloadDir;  // get 未指定本地路径时的存放目录，空表示当前目录

    struct Alias {
        std::string host;
        std::string user;
        std::string key;
        int port = 0;
    };
    std::map<std::string, Alias> aliases;
};

// path 为空时使用 ~/.rshell/config
RshellSettings loadRshellSettings(const std::string& path = std::string());
std::string rshellConfigPath();

// 解析 "user@host:port/path" 或 "/path" 形式的目标。
struct RemoteTarget {
    std::string host;   // 为空表示使用配置中的默认主机
    std::string user;
    int port = 0;
    std::string path;   // 可能为空
    bool hasPath = false;
};

// 例：deploy@example.com:/var/log   -> {deploy, example.com, 0, /var/log}
//     /var/log                     -> {空, 空, 0, /var/log}
//     webbox                       -> {空, 空, 0, 空}（由调用方按 alias 解析）
RemoteTarget parseRemoteTarget(const std::string& spec);

// "0644" / "644" / "u+x" 中的前两种（八进制）支持
unsigned int parseModeString(const std::string& text);

}  // namespace rshell
