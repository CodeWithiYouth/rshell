// mcp_http_probe.cpp - 内置 MCP 服务的端到端冒烟测试（回环 socket，不依赖外部 SSH）
//
// 用一个假的 ToolBackend 注册会话，然后真的发 HTTP 请求，验证：
//   握手 / 工具列表 / 只读默认 / 写权限开关 / 命令权限开关 / Bearer 认证 / 方法限制
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "ai_json.h"
#include "ai_tools.h"
#include "mcp_server.h"

using namespace rshell;

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) return;
    ++g_failed;
    std::cerr << "FAIL " << what << "\n";
}

struct FakeBackend : ai::ToolBackend {
    std::map<std::string, std::string> files{{"/etc/hostname", "router\n"}};
    int writes = 0;
    int commands = 0;

    CommandResult runCommand(const std::string&, int, const std::string&) override {
        ++commands;
        CommandResult result;
        result.exitCode = 0;
        result.output = "ok\n";
        return result;
    }

    bool readFile(const std::string& path, std::size_t, std::string& out, bool& truncated,
                  std::string& error, const std::string&) override {
        const auto it = files.find(path);
        if (it == files.end()) {
            error = "没有这个文件";
            return false;
        }
        out = it->second;
        truncated = false;
        return true;
    }

    bool writeFile(const std::string& path, const std::string& content, std::string& error,
                   const std::string&) override {
        (void)error;
        files[path] = content;
        ++writes;
        return true;
    }

    bool listDir(const std::string&, std::vector<ai::DirEntryView>& out, std::string& error,
                 const std::string&) override {
        (void)error;
        out.clear();
        ai::DirEntryView entry;
        entry.name = "hostname";
        entry.size = 7;
        out.push_back(entry);
        return true;
    }

    std::string homeDir() override { return "/root"; }
    bool makeDir(const std::string&, std::string&) override { return true; }
};

struct Response {
    int status = 0;
    std::string body;
};

Response httpRequest(int port, const std::string& method, const std::string& path,
                     const std::string& token, const std::string& body) {
    Response response;
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) return response;

    DWORD timeout = 5000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(socket);
        return response;
    }

    std::string request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    request += "Connection: close\r\nContent-Type: application/json\r\n";
    if (!token.empty()) request += "Authorization: Bearer " + token + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    request += body;
    send(socket, request.data(), static_cast<int>(request.size()), 0);
    shutdown(socket, SD_SEND);

    std::string raw;
    char chunk[4096];
    while (true) {
        const int received = recv(socket, chunk, sizeof(chunk), 0);
        if (received <= 0) break;
        raw.append(chunk, static_cast<std::size_t>(received));
    }
    closesocket(socket);

    const std::size_t lineEnd = raw.find("\r\n");
    if (lineEnd != std::string::npos) {
        const std::string statusLine = raw.substr(0, lineEnd);
        const std::size_t space = statusLine.find(' ');
        if (space != std::string::npos) response.status = std::atoi(statusLine.c_str() + space + 1);
    }
    const std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd != std::string::npos) response.body = raw.substr(headerEnd + 4);
    return response;
}

std::string callBody(int id, const std::string& name, const std::string& arguments) {
    return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + std::to_string(id) +
           ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + name +
           "\",\"arguments\":" + arguments + "}}";
}

bool resultIsError(const std::string& body, bool& isError, std::string& text) {
    ai::Json json;
    if (!ai::Json::parse(body, json)) return false;
    const ai::Json* result = json.find("result");
    if (!result) return false;
    const ai::Json* error = result->find("isError");
    isError = error && error->asBool();
    const ai::Json* content = result->find("content");
    if (content && content->size() > 0) {
        const ai::Json* value = content->items()[0].find("text");
        if (value && value->isString()) text = value->asString();
    }
    return true;
}

}  // namespace

int main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup 失败\n";
        return 2;
    }

    auto backend = std::make_shared<FakeBackend>();
    McpHostResolver resolver = [backend](const std::string& host, McpHostBinding& binding,
                                         std::string& error) {
        if (!host.empty() && host != "fake") {
            error = "没有名为「" + host + "」的会话";
            return false;
        }
        binding.backend = backend;
        binding.label = host.empty() ? "active-host" : host;
        return true;
    };

    const std::string token = "probe-token";
    int approvals = 0;
    bool approveHighRisk = false;
    std::string approvalHost;
    std::string approvalIntent;

    // ---- 默认只读 ----
    McpServer server;
    McpServerConfig config;
    config.port = 0;  // 让系统分配，避免测试并行时抢端口
    config.token = token;
    config.approveHighRisk = [&](const ai::ToolRequestInfo& info, const std::string& hostLabel,
                                 const std::string& intent) {
        (void)info;
        ++approvals;
        approvalHost = hostLabel;
        approvalIntent = intent;
        return approveHighRisk;
    };
    std::string error;
    check(server.start(config, resolver, error), "start MCP server on loopback");
    check(server.running(), "server reports running");
    check(server.port() > 0, "server exposes its bound port");
    const int port = server.port();

    Response unauthorized = httpRequest(port, "POST", "/mcp", "", "{}");
    check(unauthorized.status == 401, "missing token is rejected");

    Response wrong = httpRequest(port, "POST", "/mcp", "nope", "{}");
    check(wrong.status == 401, "wrong token is rejected");

    Response get = httpRequest(port, "GET", "/mcp", token, "");
    check(get.status == 405, "GET /mcp is rejected");

    Response wrongPath = httpRequest(port, "POST", "/other", token, "{}");
    check(wrongPath.status == 404, "only /mcp is served");

    Response init = httpRequest(
        port, "POST", "/mcp", token,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"probe","version":"1"}}})");
    check(init.status == 200 && init.body.find("\"serverInfo\"") != std::string::npos,
          "initialize returns server info");

    Response list = httpRequest(port, "POST", "/mcp", token,
                                R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    check(list.status == 200, "tools/list succeeds");
    check(list.body.find("\"write_file\"") == std::string::npos,
          "write_file is hidden until explicitly enabled");
    check(list.body.find("\"run_command\"") == std::string::npos,
          "run_command is hidden until explicitly enabled");

    Response read = httpRequest(port, "POST", "/mcp", token,
                                callBody(3, "read_file", R"({"path":"/etc/hostname"})"));
    bool isError = true;
    std::string text;
    check(resultIsError(read.body, isError, text), "read_file returns MCP content");
    check(!isError && text.find("router") != std::string::npos, "read_file reads fake backend");

    Response denied = httpRequest(port, "POST", "/mcp", token,
                                  callBody(4, "write_file",
                                           R"({"path":"/tmp/x","content":"x"})"));
    check(resultIsError(denied.body, isError, text) && isError,
          "write_file is rejected while the permission is off");
    check(backend->writes == 0, "denied write never reaches the backend");

    Response missing = httpRequest(port, "POST", "/mcp", token,
                                   callBody(5, "read_file", R"({"path":"/nope"})"));
    check(resultIsError(missing.body, isError, text) && isError,
          "backend failures map to isError");

    Response unknownHost = httpRequest(port, "POST", "/mcp", token,
                                       callBody(6, "list_dir",
                                                R"({"path":"/","host":"ghost"})"));
    check(resultIsError(unknownHost.body, isError, text) && isError &&
              text.find("ghost") != std::string::npos,
          "unknown host names are reported back");

    server.stop();
    check(!server.running(), "stop shuts the listener down");

    // ---- 打开写/命令权限后重启 ----
    config.allowWrite = true;
    config.allowCommand = true;
    check(server.start(config, resolver, error), "restart with write and command permissions");
    const int port2 = server.port();

    Response list2 = httpRequest(port2, "POST", "/mcp", token,
                                 R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})");
    check(list2.body.find("\"write_file\"") != std::string::npos, "write_file becomes visible");
    check(list2.body.find("\"run_command\"") != std::string::npos, "run_command becomes visible");

    Response written = httpRequest(port2, "POST", "/mcp", token,
                                   callBody(8, "write_file",
                                            R"({"path":"/tmp/x","content":"hello"})"));
    check(resultIsError(written.body, isError, text) && !isError, "write_file succeeds");
    check(backend->writes == 1 && backend->files["/tmp/x"] == "hello",
          "write_file reaches the backend");

    Response command = httpRequest(port2, "POST", "/mcp", token,
                                   callBody(9, "run_command",
                                            R"({"command":"uptime","intent":"查看负载"})"));
    check(resultIsError(command.body, isError, text) && !isError,
          "run_command succeeds when enabled");
    check(backend->commands == 1, "run_command reaches the backend");
    check(approvals == 0, "low-risk commands run without approval");

    // 没有 intent 的命令直接拒绝，不进入审批也不执行
    Response noIntent = httpRequest(
        port2, "POST", "/mcp", token,
        callBody(15, "run_command", R"({"command":"uptime"})"));
    check(resultIsError(noIntent.body, isError, text) && isError &&
              text.find("intent") != std::string::npos,
          "run_command without intent is rejected");
    check(approvals == 0 && backend->commands == 1, "missing intent never reaches the backend");

    // ---- 高风险操作复用 ToolPolicy 规则，必须走审批 ----
    approveHighRisk = false;
    Response riskyDenied = httpRequest(
        port2, "POST", "/mcp", token,
        callBody(10, "run_command",
                 R"({"command":"rm -rf /tmp/x","intent":"清理测试目录"})"));
    check(resultIsError(riskyDenied.body, isError, text) && isError,
          "high-risk command is denied when no one approves");
    check(approvals == 1, "high-risk command asks for approval");
    check(approvalHost == "active-host", "approval card carries the resolved host name");
    check(approvalIntent == "清理测试目录", "approval card carries the stated intent");
    check(backend->commands == 1, "denied high-risk command never reaches the backend");

    approveHighRisk = true;
    Response riskyAllowed = httpRequest(
        port2, "POST", "/mcp", token,
        callBody(11, "run_command",
                 R"({"command":"rm -rf /tmp/x","intent":"清理测试目录"})"));
    check(resultIsError(riskyAllowed.body, isError, text) && !isError,
          "approved high-risk command runs");
    check(approvals == 2 && backend->commands == 2, "approval result is honored");

    // 客户端显式指定 host 时，审批卡上写的是那个会话名
    approveHighRisk = false;
    Response namedHost = httpRequest(
        port2, "POST", "/mcp", token,
        callBody(14, "run_command",
                 R"({"command":"rm -rf /tmp/x","intent":"清理测试目录","host":"fake"})"));
    check(resultIsError(namedHost.body, isError, text) && isError,
          "explicit host still asks for approval");
    check(approvals == 3 && approvalHost == "fake",
          "approval card shows the explicitly requested host");

    approveHighRisk = false;
    Response etcDenied = httpRequest(
        port2, "POST", "/mcp", token,
        callBody(12, "write_file", R"({"path":"/etc/passwd","content":"x"})"));
    check(resultIsError(etcDenied.body, isError, text) && isError,
          "high-risk write path needs approval");
    check(approvals == 4 && backend->writes == 1, "denied high-risk write is not applied");

    approveHighRisk = true;
    Response etcAllowed = httpRequest(
        port2, "POST", "/mcp", token,
        callBody(13, "write_file", R"({"path":"/etc/passwd","content":"x"})"));
    check(resultIsError(etcAllowed.body, isError, text) && !isError,
          "approved high-risk write succeeds");
    check(approvals == 5 && backend->writes == 2, "approved write reaches the backend");

    server.stop();
    WSACleanup();

    std::cout << (g_failed == 0 ? "PASS" : "FAIL") << ": " << (g_checks - g_failed) << "/"
              << g_checks << " 项检查通过\n";
    return g_failed == 0 ? 0 : 1;
}
