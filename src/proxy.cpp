// proxy.cpp
#include "proxy.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rshell {

namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

void closeSocket(Socket sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    ::close(sock);
#endif
}

std::string socketErrorText() {
#ifdef _WIN32
    return "Winsock 错误 " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

bool sendAll(Socket sock, const char* data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        const int n = static_cast<int>(::send(sock, data + sent, static_cast<int>(length - sent), 0));
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool recvAll(Socket sock, char* data, std::size_t length) {
    std::size_t got = 0;
    while (got < length) {
        const int n = static_cast<int>(::recv(sock, data + got, static_cast<int>(length - got), 0));
        if (n <= 0) return false;
        got += static_cast<std::size_t>(n);
    }
    return true;
}

std::string base64(const std::string& text) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int value = 0;
    int bits = -6;
    for (const unsigned char c : text) {
        value = (value << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out += table[(value >> bits) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > -6) out += table[((value << 8) >> (bits + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

// 非阻塞 connect + select 超时，和直连路径保持一致
Socket dial(const std::string& host, int port, int timeoutSec, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string service = std::to_string(port);
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0) {
        error = "无法解析代理地址：" + host;
        return kInvalidSocket;
    }

    const int seconds = std::max(1, timeoutSec);
    std::string lastError = "没有可用地址";
    Socket connected = kInvalidSocket;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        Socket sock = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == kInvalidSocket) {
            lastError = socketErrorText();
            continue;
        }

#ifdef _WIN32
        u_long nonBlocking = 1;
        ioctlsocket(sock, FIONBIO, &nonBlocking);
#else
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
        int rc = ::connect(sock, it->ai_addr, static_cast<int>(it->ai_addrlen));
        if (rc != 0) {
#ifdef _WIN32
            const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            const bool pending = errno == EINPROGRESS;
#endif
            if (!pending) {
                lastError = socketErrorText();
                closeSocket(sock);
                continue;
            }

            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            fd_set errorSet;
            FD_ZERO(&errorSet);
            FD_SET(sock, &errorSet);
            timeval tv{};
            tv.tv_sec = seconds;
#ifdef _WIN32
            const int selected = select(0, nullptr, &writeSet, &errorSet, &tv);
#else
            const int selected = select(sock + 1, nullptr, &writeSet, &errorSet, &tv);
#endif
            if (selected <= 0) {
                lastError = selected == 0 ? "连接代理超时" : socketErrorText();
                closeSocket(sock);
                continue;
            }
            int soError = 0;
#ifdef _WIN32
            int length = sizeof(soError);
#else
            socklen_t length = sizeof(soError);
#endif
            getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &length);
            if (soError != 0) {
                lastError = "连接代理失败";
                closeSocket(sock);
                continue;
            }
        }

#ifdef _WIN32
        u_long blocking = 0;
        ioctlsocket(sock, FIONBIO, &blocking);
        DWORD ms = static_cast<DWORD>(seconds) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
        fcntl(sock, F_SETFL, flags);
        timeval tv{};
        tv.tv_sec = seconds;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        int one = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        connected = sock;
        break;
    }
    freeaddrinfo(result);
    if (connected == kInvalidSocket) error = "连接代理 " + host + ":" + service + " 失败：" + lastError;
    return connected;
}

bool socks5Handshake(Socket sock, const ProxyConfig& proxy, const std::string& host, int port,
                     std::string& error) {
    const bool needAuth = !proxy.user.empty();
    std::vector<char> greeting{0x05, static_cast<char>(needAuth ? 2 : 1), 0x00};
    if (needAuth) greeting.push_back(0x02);
    if (!sendAll(sock, greeting.data(), greeting.size())) {
        error = "发送代理握手失败";
        return false;
    }

    char reply[2]{};
    if (!recvAll(sock, reply, 2)) {
        error = "读取代理握手响应失败";
        return false;
    }
    if (reply[0] != 0x05) {
        error = "代理返回的不是 SOCKS5";
        return false;
    }
    if (reply[1] == 0x02) {
        if (proxy.user.size() > 255 || proxy.password.size() > 255) {
            error = "代理用户名或密码过长";
            return false;
        }
        std::vector<char> packet{0x01, static_cast<char>(proxy.user.size())};
        packet.insert(packet.end(), proxy.user.begin(), proxy.user.end());
        packet.push_back(static_cast<char>(proxy.password.size()));
        packet.insert(packet.end(), proxy.password.begin(), proxy.password.end());
        if (!sendAll(sock, packet.data(), packet.size())) {
            error = "发送代理认证失败";
            return false;
        }
        char authReply[2]{};
        if (!recvAll(sock, authReply, 2) || authReply[1] != 0x00) {
            error = "代理认证被拒绝";
            return false;
        }
    } else if (reply[1] != 0x00) {
        error = "代理不接受可用的认证方式";
        return false;
    }

    if (host.size() > 255) {
        error = "目标主机名过长";
        return false;
    }
    std::vector<char> request{0x05, 0x01, 0x00, 0x03,
                              static_cast<char>(host.size())};
    request.insert(request.end(), host.begin(), host.end());
    request.push_back(static_cast<char>((port >> 8) & 0xFF));
    request.push_back(static_cast<char>(port & 0xFF));
    if (!sendAll(sock, request.data(), request.size())) {
        error = "发送代理请求失败";
        return false;
    }

    char head[4]{};
    if (!recvAll(sock, head, 4)) {
        error = "读取代理响应失败";
        return false;
    }
    if (head[0] != 0x05) {
        error = "代理响应格式错误";
        return false;
    }
    if (head[1] != 0x00) {
        static const char* reasons[] = {"成功",    "一般性失败", "规则不允许", "网络不可达",
                                        "主机不可达", "连接被拒绝", "TTL 过期",  "不支持的命令",
                                        "不支持的地址类型"};
        const int index = static_cast<int>(static_cast<unsigned char>(head[1]));
        error = std::string("代理拒绝连接：") +
                (index >= 0 && index < 9 ? reasons[index] : "未知原因");
        return false;
    }

    int skip = 0;
    if (head[3] == 0x01) {
        skip = 4;
    } else if (head[3] == 0x03) {
        char length = 0;
        if (!recvAll(sock, &length, 1)) {
            error = "代理响应截断";
            return false;
        }
        skip = static_cast<int>(static_cast<unsigned char>(length));
    } else if (head[3] == 0x04) {
        skip = 16;
    } else {
        error = "代理返回未知地址类型";
        return false;
    }
    std::vector<char> discard(static_cast<std::size_t>(skip) + 2);
    if (!recvAll(sock, discard.data(), discard.size())) {
        error = "代理响应截断";
        return false;
    }
    return true;
}

bool httpConnectHandshake(Socket sock, const ProxyConfig& proxy, const std::string& host, int port,
                          std::string& error) {
    std::string request = "CONNECT " + host + ":" + std::to_string(port) +
                          " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) + "\r\n";
    if (!proxy.user.empty()) {
        request += "Proxy-Authorization: Basic " + base64(proxy.user + ":" + proxy.password) +
                   "\r\n";
    }
    request += "\r\n";
    if (!sendAll(sock, request.data(), request.size())) {
        error = "发送代理请求失败";
        return false;
    }

    std::string response;
    char buffer[512];
    while (response.find("\r\n\r\n") == std::string::npos) {
        const int n = static_cast<int>(::recv(sock, buffer, sizeof(buffer), 0));
        if (n <= 0) {
            error = "代理响应截断";
            return false;
        }
        response.append(buffer, static_cast<std::size_t>(n));
        if (response.size() > 8192) break;
    }
    if (response.rfind("HTTP/", 0) != 0) {
        error = "代理没有返回 HTTP 响应";
        return false;
    }
    const std::size_t space = response.find(' ');
    if (space == std::string::npos || space + 4 > response.size() ||
        response.compare(space + 1, 3, "200") != 0) {
        std::string status = space == std::string::npos
                                 ? std::string()
                                 : response.substr(space + 1, std::min<std::size_t>(40, response.size() - space - 1));
        while (!status.empty() && (status.back() == '\r' || status.back() == '\n')) status.pop_back();
        error = "代理拒绝 CONNECT；" + status;
        return false;
    }
    return true;
}

}  // namespace

bool parseProxyUrl(const std::string& text, ProxyConfig& out, std::string& error) {
    out = ProxyConfig();
    std::string rest = text;
    // 去掉首尾空白
    const auto first = rest.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return true;  // 空串 = 不使用代理
    }
    rest = rest.substr(first);
    const auto last = rest.find_last_not_of(" \t\r\n");
    rest = rest.substr(0, last + 1);

    if (rest.rfind("socks5://", 0) == 0 || rest.rfind("socks5h://", 0) == 0) {
        out.kind = ProxyConfig::Kind::Socks5;
        rest = rest.substr(rest.find("://") + 3);
    } else if (rest.rfind("http://", 0) == 0) {
        out.kind = ProxyConfig::Kind::HttpConnect;
        rest = rest.substr(7);
    } else if (rest.rfind("https://", 0) == 0) {
        error = "代理本身不支持 HTTPS（CONNECT 是明文隧道），请用 http://";
        return false;
    } else {
        out.kind = ProxyConfig::Kind::Socks5;  // 裸 host:port 默认按 SOCKS5
    }

    const std::size_t at = rest.rfind('@');
    if (at != std::string::npos) {
        const std::string userinfo = rest.substr(0, at);
        rest = rest.substr(at + 1);
        const std::size_t colon = userinfo.find(':');
        if (colon == std::string::npos) {
            out.user = userinfo;
        } else {
            out.user = userinfo.substr(0, colon);
            out.password = userinfo.substr(colon + 1);
        }
    }

    const std::size_t colon = rest.rfind(':');
    if (colon == std::string::npos) {
        error = "代理地址缺少端口，例如 socks5://127.0.0.1:1080";
        return false;
    }
    out.host = rest.substr(0, colon);
    const std::string portText = rest.substr(colon + 1);
    if (out.host.empty() || portText.empty()) {
        error = "代理地址格式不对，应为 host:port";
        return false;
    }
    for (const char c : portText) {
        if (c < '0' || c > '9') {
            error = "代理端口不是数字";
            return false;
        }
    }
    const int port = std::atoi(portText.c_str());
    if (port <= 0 || port > 65535) {
        error = "代理端口超出范围";
        return false;
    }
    out.port = port;
    return true;
}

std::string proxyToUrl(const ProxyConfig& proxy) {
    if (!proxy.enabled()) return std::string();
    const std::string scheme =
        proxy.kind == ProxyConfig::Kind::HttpConnect ? "http://" : "socks5://";
    return scheme + proxy.host + ":" + std::to_string(proxy.port);
}

std::intptr_t connectThroughProxy(const ProxyConfig& proxy, const std::string& host, int port,
                                  int timeoutSec, std::string& error) {
    if (!proxy.enabled()) {
        error = "没有配置代理";
        return -1;
    }

    Socket sock = dial(proxy.host, proxy.port, timeoutSec, error);
    if (sock == kInvalidSocket) return -1;

    const bool ok = proxy.kind == ProxyConfig::Kind::HttpConnect
                        ? httpConnectHandshake(sock, proxy, host, port, error)
                        : socks5Handshake(sock, proxy, host, port, error);
    if (!ok) {
        closeSocket(sock);
        return -1;
    }
    return static_cast<std::intptr_t>(sock);
}

}  // namespace rshell
