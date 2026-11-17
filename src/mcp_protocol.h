// mcp_protocol.h - MCP（Model Context Protocol）JSON-RPC 2.0 协议层
//
// 只负责「消息 → 消息」：不碰 socket、不碰 SSH，便于离线单测。
// 传输层见 mcp_http.h，宿主接线见 mcp_server.h。
//
// 当前实现 streamable HTTP 的无状态子集：
//   initialize / notifications/initialized / ping / tools/list / tools/call
// 工具执行结果按 MCP 规范放进 content[type=text]，失败时 isError=true。
#pragma once

#include <string>
#include <vector>

#include "ai_json.h"

namespace rshell::mcp {

// identity：工具在 tools/list 里对客户端展示的元数据。
struct ToolDef {
    std::string name;
    std::string description;
    ai::Json inputSchema = ai::Json::object();
    bool readOnly = true;  // 只用于 annotations 提示，不参与权限判断
};

// 工具执行的落点。实现方通常包装 ai::ToolRegistry。
class ToolHost {
public:
    virtual ~ToolHost() = default;

    virtual std::vector<ToolDef> tools() const = 0;

    // 返回给客户端看的文本（一般是 JSON）。isError=true 表示这次调用失败。
    virtual std::string call(const std::string& name, const ai::Json& arguments,
                             bool& isError) = 0;
};

class Protocol {
public:
    // 客户端请求的版本不认识时回退到这一版
    static constexpr const char* kFallbackVersion = "2025-06-18";

    Protocol(ToolHost& host, std::string serverVersion);

    // 处理一条完整的 JSON-RPC 消息。
    // 返回空串表示不需要回应（通知，或客户端发来的 response）。
    std::string handle(const std::string& message);

private:
    ai::Json initializeResult(const ai::Json& request) const;
    ai::Json toolsResult() const;
    std::string callTool(const ai::Json& id, const ai::Json& request);

    ToolHost& host_;
    std::string version_;
    bool initialized_ = false;
};

}  // namespace rshell::mcp
