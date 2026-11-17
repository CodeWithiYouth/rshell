// mcp_server.h - 把 rshell 已连接的会话暴露成 MCP 工具（GUI 内置）
//
// 只在 127.0.0.1 上监听，Bearer token 认证；工具执行复用 GUI 现有的
// RemoteFs（第二条 SSH 连接），因此终端、文件面板和 MCP 看到的是同一台机器。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "ai_tools.h"

namespace rshell {

// 一次工具调用期间必须同时持有 backend 和 keepAlive：
// backend 是 SshToolBackend（内部只存 RemoteFs&），keepAlive 是 RemoteFs 的
// shared_ptr，标签被关掉时由它保证对象不在调用中途析构。
struct McpHostBinding {
    std::shared_ptr<ai::ToolBackend> backend;
    std::shared_ptr<void> keepAlive;
    std::string label;  // 解析后的会话名，显示在审批卡上
};

// host 为空表示「当前激活标签」。返回 false 时 error 说明原因。
using McpHostResolver =
    std::function<bool(const std::string& host, McpHostBinding& binding, std::string& error)>;

// 命中 ToolPolicy 的高风险规则时，把审批信息交给宿主（GUI 弹确认框）。
// hostLabel 是解析后的目标会话名；返回 true = 用户允许。
// 不设置时高危操作一律拒绝。
using McpApproval =
    std::function<bool(const ai::ToolRequestInfo& info, const std::string& hostLabel,
                       const std::string& intent)>;

struct McpServerConfig {
    int port = 8765;
    std::string token;
    bool allowWrite = false;    // write_file
    bool allowCommand = false;  // run_command（等于远程执行能力，默认关闭）
    // 低风险写操作直接执行；只有 describe().highRisk 才调用它。
    McpApproval approveHighRisk;
};

// 生成 48 个十六进制字符的随机 token（CNG；不可用时退化为时间种子）。
std::string makeMcpToken();

class McpServer {
public:
    McpServer();
    ~McpServer();
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    bool start(const McpServerConfig& config, McpHostResolver resolver, std::string& error);
    void stop();
    bool running() const;
    int port() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace rshell
