// mcp_http.cpp
#include "mcp_http.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace rshell::mcp {

#ifdef _WIN32

namespace {

constexpr std::size_t kMaxHeaderBytes = 32u * 1024u;
constexpr int kIoTimeoutMs = 15000;
constexpr std::size_t kMaxConnections = 32;

std::string lowerAscii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

// 定长比较，避免用 token 的前缀命中时逐字节提前返回。
bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;  // 键已转小写
    std::string body;
};

bool sendAll(SOCKET socket, const char* data, std::size_t size) {
    while (size > 0) {
        const int chunk = static_cast<int>(std::min<std::size_t>(size, 64u * 1024u));
        const int sent = send(socket, data, chunk, 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

void sendResponse(SOCKET socket, int status, const char* reason, const std::string& body,
                  const std::string& contentType, const std::string& protocolVersion = {}) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " ";
    head += reason;
    head += "\r\nContent-Type: ";
    head += contentType;
    head += "\r\nContent-Length: " + std::to_string(body.size());
    head += "\r\nConnection: close\r\nCache-Control: no-store\r\n";
    if (status == 401) head += "WWW-Authenticate: Bearer\r\n";
    if (!protocolVersion.empty()) head += "MCP-Protocol-Version: " + protocolVersion + "\r\n";
    head += "\r\n";

    if (!sendAll(socket, head.data(), head.size())) return;
    if (!body.empty()) sendAll(socket, body.data(), body.size());
}

// 读一条完整的 HTTP 请求。失败时把原因和应回的 HTTP 状态码带出去。
bool readRequest(SOCKET socket, Request& request, std::size_t maxBodyBytes, int& status,
                 std::string& error) {
    std::string buffer;
    char chunk[8192];

    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        if (buffer.size() > kMaxHeaderBytes) {
            status = 431;
            error = "请求头过大";
            return false;
        }
        const int received = recv(socket, chunk, sizeof(chunk), 0);
        if (received == SOCKET_ERROR) {
            status = 400;
            error = "读取请求失败";
            return false;
        }
        if (received == 0) {
            status = 400;
            error = "连接提前关闭";
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(received));
        headerEnd = buffer.find("\r\n\r\n");
    }

    const std::string head = buffer.substr(0, headerEnd);
    std::size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string::npos) lineEnd = head.size();
    const std::string firstLine = head.substr(0, lineEnd);

    // METHOD SP PATH SP VERSION
    const std::size_t firstSpace = firstLine.find(' ');
    const std::size_t secondSpace =
        firstSpace == std::string::npos ? std::string::npos : firstLine.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
        status = 400;
        error = "请求行格式错误";
        return false;
    }
    request.method = firstLine.substr(0, firstSpace);
    request.path = firstLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);

    std::size_t pos = lineEnd == head.size() ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        std::size_t end = head.find("\r\n", pos);
        if (end == std::string::npos) end = head.size();
        const std::string line = head.substr(pos, end - pos);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            request.headers[lowerAscii(trim(line.substr(0, colon)))] =
                trim(line.substr(colon + 1));
        }
        pos = end == head.size() ? head.size() : end + 2;
    }

    std::size_t contentLength = 0;
    if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
        char* endptr = nullptr;
        const unsigned long long value = std::strtoull(it->second.c_str(), &endptr, 10);
        if (endptr == it->second.c_str() || *endptr != '\0') {
            status = 400;
            error = "Content-Length 无效";
            return false;
        }
        contentLength = static_cast<std::size_t>(value);
    }
    if (contentLength > maxBodyBytes) {
        status = 413;
        error = "请求体超过上限";
        return false;
    }

    std::string body = buffer.substr(headerEnd + 4);
    while (body.size() < contentLength) {
        const int received = recv(socket, chunk, sizeof(chunk), 0);
        if (received <= 0) {
            status = 400;
            error = "读取请求体失败";
            return false;
        }
        body.append(chunk, static_cast<std::size_t>(received));
    }
    if (body.size() > contentLength) body.resize(contentLength);
    request.body = std::move(body);
    return true;
}

bool authorized(const Request& request, const std::string& token) {
    const auto it = request.headers.find("authorization");
    if (it == request.headers.end()) return false;
    const std::string& value = it->second;
    const std::size_t space = value.find(' ');
    if (space == std::string::npos) return false;
    if (!equalsIgnoreCase(value.substr(0, space), "Bearer")) return false;
    return constantTimeEquals(trim(value.substr(space + 1)), token);
}

const char* reasonFor(int status) {
    switch (status) {
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        default: return "Bad Request";
    }
}

// Winsock 进程内只初始化一次，且不配对 WSACleanup：
// stop() 之后可能还有收尾中的连接线程，提前清理会破坏它们手里的 socket。
bool ensureWinsock(std::string& error) {
    static std::once_flag once;
    static bool started = false;
    std::call_once(once, []() {
        WSADATA wsa{};
        started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    });
    if (!started) error = "Winsock 初始化失败";
    return started;
}

}  // namespace

struct HttpServer::Impl {
    HttpConfig config;
    Handler handler;
    std::atomic<bool> running{false};
    SOCKET listenSocket = INVALID_SOCKET;
    std::thread acceptThread;
    std::mutex clientMutex;
    std::set<SOCKET> clients;
    std::vector<std::thread> clientThreads;
    std::atomic<int> boundPort{0};
    std::mutex handlerMutex;  // 协议层不是线程安全的：工具调用串行执行

    void serveClient(SOCKET client) {
        DWORD timeout = kIoTimeoutMs;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));

        Request request;
        int status = 200;
        std::string error;
        if (!readRequest(client, request, config.maxBodyBytes, status, error)) {
            sendResponse(client, status, reasonFor(status), error + "\n", "text/plain");
        } else if (request.method != "POST") {
            sendResponse(client, 405, "Method Not Allowed",
                         "只支持 POST /mcp\n", "text/plain");
        } else if (request.path != "/mcp") {
            sendResponse(client, 404, "Not Found", "路径不存在，MCP 端点是 /mcp\n",
                         "text/plain");
        } else if (!authorized(request, config.token)) {
            sendResponse(client, 401, "Unauthorized", "Bearer token 无效\n", "text/plain");
        } else {
            std::string payload;
            {
                std::lock_guard<std::mutex> lock(handlerMutex);
                if (handler) payload = handler(request.body);
            }
            const auto version = request.headers.find("mcp-protocol-version");
            if (payload.empty()) {
                sendResponse(client, 202, "Accepted", {}, "application/json");
            } else {
                sendResponse(client, 200, "OK", payload, "application/json",
                             version == request.headers.end() ? std::string() : version->second);
            }
        }

        {
            std::lock_guard<std::mutex> lock(clientMutex);
            clients.erase(client);
        }
        closesocket(client);
    }

    void acceptLoop(const std::shared_ptr<Impl>& self) {
        // 先把监听句柄取到局部：stop() 会在别的线程把它置成 INVALID_SOCKET，
        // accept 线程不再直接读这个成员。
        const SOCKET listen = self->listenSocket;
        while (self->running.load()) {
            SOCKET client = accept(listen, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (!self->running.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(self->clientMutex);
                if (!self->running.load() || self->clients.size() >= kMaxConnections) {
                    closesocket(client);
                    continue;
                }
                self->clients.insert(client);
            }
            self->clientThreads.emplace_back(
                [self, client]() { self->serveClient(client); });
        }
    }
};

HttpServer::HttpServer() : impl_(std::make_shared<Impl>()) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(const HttpConfig& config, Handler handler, std::string& error) {
    stop();
    // 每次启动都用新的 Impl：上一次停止时可能还有连接线程在收尾，
    // 它们持有旧 Impl 的共享所有权，不会和新配置互相踩。
    impl_ = std::make_shared<Impl>();
    Impl& state = *impl_;

    if (config.token.empty()) {
        error = "MCP 访问令牌不能为空";
        return false;
    }

    if (!ensureWinsock(error)) return false;

    state.listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (state.listenSocket == INVALID_SOCKET) {
        error = "无法创建监听套接字";
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(state.listenSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(config.port));
    if (InetPtonA(AF_INET, config.bindAddress.c_str(), &address.sin_addr) != 1) {
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    if (bind(state.listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
        SOCKET_ERROR) {
        error = "无法监听 " + config.bindAddress + ":" + std::to_string(config.port) +
                "（端口被占用？）";
        closesocket(state.listenSocket);
        state.listenSocket = INVALID_SOCKET;
        return false;
    }
    if (listen(state.listenSocket, 16) == SOCKET_ERROR) {
        error = "监听失败";
        closesocket(state.listenSocket);
        state.listenSocket = INVALID_SOCKET;
        return false;
    }

    sockaddr_in actual{};
    int length = sizeof(actual);
    if (getsockname(state.listenSocket, reinterpret_cast<sockaddr*>(&actual), &length) == 0) {
        state.boundPort.store(ntohs(actual.sin_port));
    } else {
        state.boundPort.store(config.port);
    }

    state.config = config;
    state.handler = std::move(handler);
    state.running.store(true);
    std::shared_ptr<Impl> self = impl_;
    state.acceptThread = std::thread([self]() { self->acceptLoop(self); });
    return true;
}

void HttpServer::stop() {
    if (!impl_) return;
    Impl& state = *impl_;
    state.running.store(false);

    if (state.listenSocket != INVALID_SOCKET) {
        closesocket(state.listenSocket);
        state.listenSocket = INVALID_SOCKET;
    }
    if (state.acceptThread.joinable()) state.acceptThread.join();

    {
        std::lock_guard<std::mutex> lock(state.clientMutex);
        for (SOCKET client : state.clients) shutdown(client, SD_BOTH);
    }

    // 连接线程不能 join：工具调用可能正通过 SendMessage 等界面线程，
    // 而 stop() 往往就是界面线程发起的。线程持有 Impl 的共享所有权，
    // 让它自己收尾即可。
    for (auto& thread : state.clientThreads) {
        if (thread.joinable()) thread.detach();
    }
    state.clientThreads.clear();

    // 不在这里清空 handler：连接线程可能正持有 handlerMutex 执行工具，
    // 而 stop() 常由界面线程发起（工具又可能正在 SendMessage 回界面）。
    // handler 只持有 State 的 shared_ptr，留在原处不会有悬空问题。
    state.boundPort.store(0);
}

bool HttpServer::running() const { return impl_ && impl_->running.load(); }

int HttpServer::port() const { return impl_ ? impl_->boundPort.load() : 0; }

#else  // !_WIN32

struct HttpServer::Impl {};

HttpServer::HttpServer() : impl_(std::make_shared<Impl>()) {}
HttpServer::~HttpServer() = default;

bool HttpServer::start(const HttpConfig&, Handler, std::string& error) {
    error = "MCP HTTP 服务当前只支持 Windows";
    return false;
}

void HttpServer::stop() {}
bool HttpServer::running() const { return false; }
int HttpServer::port() const { return 0; }

#endif

}  // namespace rshell::mcp
