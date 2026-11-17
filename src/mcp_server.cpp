// mcp_server.cpp
#include "mcp_server.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

#include "mcp_http.h"
#include "mcp_protocol.h"

#ifndef RSHELL_VERSION
#define RSHELL_VERSION "dev"
#endif

namespace rshell {

namespace {

using ai::Json;

// 把一次工具调用路由到 GUI 提供的会话绑定。
class RoutedBackend : public ai::ToolBackend {
public:
    explicit RoutedBackend(McpHostResolver resolver) : resolver_(std::move(resolver)) {}

    // 写操作命中高风险规则时，调用方可以先解析目标主机，把会话名带进审批卡，
    // 然后 pin 住这条绑定再执行，避免用户切标签导致命令跑到别的机器上。
    bool resolveTarget(const std::string& host, McpHostBinding& binding, std::string& error) {
        return bind(host, binding, error);
    }
    void pin(McpHostBinding binding) {
        pinned_ = std::move(binding);
        pinnedActive_ = true;
    }
    void unpin() {
        pinnedActive_ = false;
        pinned_ = McpHostBinding();
    }

    CommandResult runCommand(const std::string& command, int timeoutSec,
                             const std::string& host) override {
        McpHostBinding binding;
        std::string error;
        if (!bind(host, binding, error)) return failure(error);
        return binding.backend->runCommand(command, timeoutSec, {});
    }

    bool readFile(const std::string& path, std::size_t maxBytes, std::string& out,
                  bool& truncated, std::string& error, const std::string& host) override {
        McpHostBinding binding;
        if (!bind(host, binding, error)) return false;
        return binding.backend->readFile(path, maxBytes, out, truncated, error, {});
    }

    bool writeFile(const std::string& path, const std::string& content, std::string& error,
                   const std::string& host) override {
        McpHostBinding binding;
        if (!bind(host, binding, error)) return false;
        return binding.backend->writeFile(path, content, error, {});
    }

    bool listDir(const std::string& path, std::vector<ai::DirEntryView>& out,
                 std::string& error, const std::string& host) override {
        McpHostBinding binding;
        if (!bind(host, binding, error)) return false;
        return binding.backend->listDir(path, out, error, {});
    }

    std::string homeDir() override {
        McpHostBinding binding;
        std::string error;
        if (!bind({}, binding, error)) return {};
        return binding.backend->homeDir();
    }

    bool makeDir(const std::string& path, std::string& error) override {
        McpHostBinding binding;
        if (!bind({}, binding, error)) return false;
        return binding.backend->makeDir(path, error);
    }

private:
    bool bind(const std::string& host, McpHostBinding& binding, std::string& error) {
        if (pinnedActive_) {
            binding = pinned_;
            return true;
        }
        if (!resolver_) {
            error = "MCP 服务没有绑定到 rshell 的会话路由";
            return false;
        }
        if (!resolver_(host, binding, error)) {
            if (error.empty()) error = "找不到可用的主机会话";
            return false;
        }
        if (!binding.backend) {
            error = "目标会话还没有建立文件连接";
            return false;
        }
        return true;
    }

    static CommandResult failure(const std::string& error) {
        CommandResult result;
        result.exitCode = -1;
        result.output = error.empty() ? "命令未能执行" : error;
        return result;
    }

    McpHostResolver resolver_;
    // HTTP 层已经串行化工具调用，所以 pin 只需要普通成员。
    McpHostBinding pinned_;
    bool pinnedActive_ = false;
};

Json stringProperty(const std::string& description) {
    Json property = Json::object();
    property["type"] = Json::string("string");
    property["description"] = Json::string(description);
    return property;
}

Json integerProperty(const std::string& description) {
    Json property = Json::object();
    property["type"] = Json::string("integer");
    property["description"] = Json::string(description);
    return property;
}

bool blankText(const std::string& text) {
    for (char ch : text) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') return false;
    }
    return true;
}

Json schema(std::vector<std::pair<std::string, Json>> properties,
            std::vector<std::string> required) {
    Json props = Json::object();
    for (auto& item : properties) props[item.first] = std::move(item.second);

    Json requiredList = Json::array();
    for (const std::string& name : required) requiredList.push(Json::string(name));

    Json result = Json::object();
    result["type"] = Json::string("object");
    result["properties"] = std::move(props);
    result["required"] = std::move(requiredList);
    return result;
}

const char* kHostDescription =
    "可选：目标会话名（rshell 里已打开的其它标签）。留空表示当前激活的标签。";

class ToolHostImpl : public mcp::ToolHost {
public:
    ToolHostImpl(ai::ToolRegistry& registry, RoutedBackend& routed, bool allowWrite,
                 bool allowCommand, CancelCheck cancelled, McpApproval approve)
        : registry_(registry),
          routed_(routed),
          allowWrite_(allowWrite),
          allowCommand_(allowCommand),
          cancelled_(std::move(cancelled)),
          approve_(std::move(approve)) {}

    std::vector<mcp::ToolDef> tools() const override {
        std::vector<mcp::ToolDef> definitions;

        {
            mcp::ToolDef def;
            def.name = "list_dir";
            def.description =
                "列出 rshell 已连接主机上的目录内容（名称、类型、大小）。只读操作。";
            def.inputSchema = schema({{"path", stringProperty("目录绝对路径，或用 ~ 表示家目录")},
                                      {"host", stringProperty(kHostDescription)}},
                                     {"path"});
            def.readOnly = true;
            definitions.push_back(std::move(def));
        }

        {
            mcp::ToolDef def;
            def.name = "read_file";
            def.description =
                "读取 rshell 已连接主机上的文本文件。只读操作；私钥等敏感路径会被拒绝，"
                "大文件只返回开头部分并标记截断。";
            def.inputSchema = schema(
                {{"path", stringProperty("绝对路径，或用 ~ 表示家目录")},
                 {"max_bytes", integerProperty("最多读取字节数；默认 96 KB")},
                 {"host", stringProperty(kHostDescription)}},
                {"path"});
            def.readOnly = true;
            definitions.push_back(std::move(def));
        }

        if (allowWrite_) {
            mcp::ToolDef def;
            def.name = "write_file";
            def.description =
                "把内容写入 rshell 已连接主机上的文件（覆盖或新建）。"
                "写操作，只有在 rshell 的 MCP 设置里开启写权限后才可用；"
                "命中系统目录、密钥等高风险路径时会在 rshell 里弹确认。";
            def.inputSchema =
                schema({{"path", stringProperty("目标文件绝对路径")},
                        {"content", stringProperty("完整的新文件内容")},
                        {"host", stringProperty(kHostDescription)}},
                       {"path", "content"});
            def.readOnly = false;
            definitions.push_back(std::move(def));
        }

        if (allowCommand_) {
            mcp::ToolDef def;
            def.name = "run_command";
            def.description =
                "在 rshell 已连接主机上执行一条非交互 shell 命令，返回合并后的输出和退出码。"
                "必须同时发送 intent，用一句话说明这条命令要做什么（会显示在 rshell 的审批卡上）。"
                "高危操作，只有在 rshell 的 MCP 设置里开启命令权限后才可用；"
                "删除、重启服务、写系统目录等高风险命令会在 rshell 里弹确认。";
            def.inputSchema =
                schema({{"command", stringProperty("要执行的命令")},
                        {"intent", stringProperty("必填：一句话说明这条命令要做什么，例如「清理 /tmp 下的测试残留」")},
                        {"timeout_sec", integerProperty("超时秒数，默认 60，最大 300")},
                        {"host", stringProperty(kHostDescription)}},
                       {"command", "intent"});
            def.readOnly = false;
            definitions.push_back(std::move(def));
        }

        return definitions;
    }

    std::string call(const std::string& name, const Json& arguments, bool& isError) override {
        if (name != "list_dir" && name != "read_file" && name != "write_file" &&
            name != "run_command") {
            isError = true;
            Json error = Json::object();
            error["error"] = Json::string("未知工具：" + name);
            return error.dump();
        }
        if (!allowed(name)) {
            isError = true;
            Json error = Json::object();
            error["error"] = Json::string(name == "run_command"
                                             ? "run_command 未开启：请在 rshell「工具」菜单里允许 MCP 执行命令"
                                             : "该工具未开启：请在 rshell 的 MCP 设置里允许写操作");
            return error.dump();
        }
        if (cancelled_ && cancelled_()) return stopped(isError);

        // 先校验 intent：每一条命令都必须说明目的，审批卡上要展示给用户。
        std::string intent;
        if (name == "run_command") {
            const Json* value = arguments.find("intent");
            if (!value || !value->isString() || blankText(value->asString())) {
                isError = true;
                return errorJson(
                    "run_command 缺少 intent：请用一句话说明这条命令要做什么，例如「清理 /tmp 下的测试残留」");
            }
            intent = value->asString();
        }

        // 先解析目标会话：审批卡要显示主机名；同时把绑定 pin 住，
        // 防止用户点「允许」的瞬间切了标签，命令落到另一台机器上。
        const std::string argumentsJson = arguments.dump();
        std::string hostArg;
        if (const Json* value = arguments.find("host"); value && value->isString()) {
            hostArg = value->asString();
        }
        McpHostBinding binding;
        std::string resolveError;
        if (!routed_.resolveTarget(hostArg, binding, resolveError)) {
            isError = true;
            return errorJson(resolveError.empty() ? "找不到可用的主机会话" : resolveError);
        }

        // 和 AI 面板共用同一套风险规则：只有 describe() 判定为 highRisk 的
        // 写操作才找人确认，普通命令/普通文件写入直接执行。
        if (registry_.kindOf(name) == ai::ToolKind::Write) {
            const ai::ToolRequestInfo info = registry_.describe(name, argumentsJson);
            if (!info.valid) {
                isError = true;
                return errorJson("参数不完整：" + info.problem);
            }
            if (info.highRisk) {
                if (!approve_) {
                    isError = true;
                    return errorJson("高危操作需要人工确认，但当前没有可用的确认窗口");
                }
                if (!approve_(info, binding.label, intent)) {
                    isError = true;
                    return errorJson("用户拒绝了这次操作");
                }
                if (cancelled_ && cancelled_()) return stopped(isError);
            }
        }

        routed_.pin(std::move(binding));
        struct PinGuard {
            RoutedBackend& backend;
            ~PinGuard() { backend.unpin(); }
        } guard{routed_};

        const ai::ToolResult result = registry_.execute(name, argumentsJson, cancelled_);
        isError = !result.ok;
        return result.json.empty() ? std::string("{}") : result.json;
    }

private:
    static std::string errorJson(const std::string& message) {
        Json error = Json::object();
        error["error"] = Json::string(message);
        return error.dump();
    }

    static std::string stopped(bool& isError) {
        isError = true;
        return errorJson("任务已停止");
    }

    bool allowed(const std::string& name) const {
        if (name == "list_dir" || name == "read_file") return true;
        if (name == "write_file") return allowWrite_;
        if (name == "run_command") return allowCommand_;
        return false;
    }

    ai::ToolRegistry& registry_;
    RoutedBackend& routed_;
    bool allowWrite_ = false;
    bool allowCommand_ = false;
    CancelCheck cancelled_;
    McpApproval approve_;
};

}  // namespace

std::string makeMcpToken() {
    unsigned char bytes[24]{};
    bool ok = false;
#ifdef _WIN32
    ok = BCryptGenRandom(nullptr, bytes, static_cast<ULONG>(sizeof(bytes)),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#endif
    if (!ok) {
        std::random_device device;
        for (unsigned char& byte : bytes) byte = static_cast<unsigned char>(device());
    }

    static const char* kHex = "0123456789abcdef";
    std::string token;
    token.reserve(sizeof(bytes) * 2);
    for (unsigned char byte : bytes) {
        token.push_back(kHex[byte >> 4]);
        token.push_back(kHex[byte & 0x0F]);
    }
    return token;
}

struct McpServer::Impl {
    // 协议与工具链放在独立的 State 里：HTTP 连接线程只持有 State 的
    // shared_ptr，stop() 之后还能安全收尾。
    struct State {
        State(McpHostResolver hostResolver, bool write, bool command, McpApproval approve)
            : resolver(std::move(hostResolver)), allowWrite(write), allowCommand(command) {
            backend = std::make_unique<RoutedBackend>(resolver);

            ai::ToolPolicy policy = ai::defaultToolPolicy();
            policy.autoApprove = true;  // 权限已由 allowWrite/allowCommand 把关
            policy.redactSecrets = true;
            registry = std::make_unique<ai::ToolRegistry>(*backend, std::move(policy));

            host = std::make_unique<ToolHostImpl>(*registry, *backend, allowWrite, allowCommand,
                                                  [this]() { return !running.load(); },
                                                  std::move(approve));
            protocol = std::make_unique<mcp::Protocol>(*host, RSHELL_VERSION);
        }

        std::atomic<bool> running{false};
        McpHostResolver resolver;
        bool allowWrite = false;
        bool allowCommand = false;
        std::unique_ptr<RoutedBackend> backend;
        std::unique_ptr<ai::ToolRegistry> registry;
        std::unique_ptr<ToolHostImpl> host;
        std::unique_ptr<mcp::Protocol> protocol;
    };

    std::shared_ptr<State> state;
    std::unique_ptr<mcp::HttpServer> http;
    int port = 0;
};

McpServer::McpServer() = default;

McpServer::~McpServer() { stop(); }

bool McpServer::start(const McpServerConfig& config, McpHostResolver resolver,
                      std::string& error) {
    stop();

    impl_ = std::make_shared<Impl>();
    impl_->state = std::make_shared<Impl::State>(std::move(resolver), config.allowWrite,
                                                 config.allowCommand, config.approveHighRisk);

    mcp::HttpConfig httpConfig;
    httpConfig.port = config.port;
    httpConfig.token = config.token;

    std::shared_ptr<Impl::State> state = impl_->state;
    state->running.store(true);

    impl_->http = std::make_unique<mcp::HttpServer>();
    if (!impl_->http->start(
            httpConfig,
            [state](const std::string& body) { return state->protocol->handle(body); }, error)) {
        state->running.store(false);
        impl_->http.reset();
        impl_.reset();
        return false;
    }

    impl_->port = impl_->http->port();
    return true;
}

void McpServer::stop() {
    if (!impl_) return;
    if (impl_->state) impl_->state->running.store(false);
    if (impl_->http) {
        impl_->http->stop();
        impl_->http.reset();
    }
    impl_.reset();
}

bool McpServer::running() const {
    return impl_ && impl_->state && impl_->state->running.load() && impl_->http &&
           impl_->http->running();
}

int McpServer::port() const { return impl_ ? impl_->port : 0; }

}  // namespace rshell
