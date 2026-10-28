// proxy.h - SSH 连接经由 SOCKS5 / HTTP CONNECT 代理
//
// 只做「把 socket 连出去」这一件事：libssh2 的握手、密钥交换和认证完全不变。
// 终端、SFTP 面板和 AI 工具都走 SshSession，所以接入点只有一处。
#pragma once

#include <cstdint>
#include <string>

namespace rshell {

struct ProxyConfig {
    enum class Kind { None, Socks5, HttpConnect };

    Kind kind = Kind::None;
    std::string host;
    int port = 0;
    std::string user;
    std::string password;  // 只在内存里，持久化走密码保险库

    bool enabled() const { return kind != Kind::None && !host.empty() && port > 0; }
};

// 解析 "socks5://[user:pass@]host:port" / "http://[user:pass@]host:port"
// 也接受 "socks5h://"（等同 socks5）和裸 "host:port"（按 SOCKS5）。
// 空字符串表示不使用代理。失败时返回 false 并写 error。
bool parseProxyUrl(const std::string& text, ProxyConfig& out, std::string& error);

// 回写成 "socks5://host:port" / "http://host:port"（不含密码）；None 返回空串
std::string proxyToUrl(const ProxyConfig& proxy);

// 经代理连接到 host:port（host 交给代理解析，便于远端 DNS）。
// 成功返回已连接的 socket（阻塞模式、已设超时和 TCP_NODELAY），失败返回 -1。
std::intptr_t connectThroughProxy(const ProxyConfig& proxy, const std::string& host, int port,
                                  int timeoutSec, std::string& error);

}  // namespace rshell
