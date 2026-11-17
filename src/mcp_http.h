// mcp_http.h - MCP 的 Streamable HTTP 传输（只监听本机回环地址）
//
// 只实现规范里有状态子集的「无状态」用法：
//   POST /mcp  Accept: application/json
// 每个请求独立处理，响应始终是 application/json；不做 SSE、不做会话 ID。
// 认证用启动时生成的 Bearer token，绑定地址默认 127.0.0.1。
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace rshell::mcp {

struct HttpConfig {
    std::string bindAddress = "127.0.0.1";
    int port = 8765;  // 0 = 让系统分配
    std::string token;  // 必填；空串时拒绝启动
    std::size_t maxBodyBytes = 4u * 1024u * 1024u;
};

class HttpServer {
public:
    // 返回空串 = 这条消息不需要响应（通知），传输层回 202。
    using Handler = std::function<std::string(const std::string& body)>;

    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start(const HttpConfig& config, Handler handler, std::string& error);
    void stop();

    bool running() const;
    int port() const;  // 实际监听端口（配置 0 时由系统分配）

private:
    struct Impl;
    // 连接线程在 stop() 后仍可能收尾（不能再 join：宿主 stop 可能由界面线程
    // 发起，而工具调用正卡在 SendMessage 回界面），因此 Impl 用共享所有权。
    std::shared_ptr<Impl> impl_;
};

}  // namespace rshell::mcp
