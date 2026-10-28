// ai_client.h - OpenAI 兼容 /chat/completions 的流式客户端
//
// Win7 兼容要点：
//   - WinHttpOpen 的 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 是 Win8.1+，失败时回退 DEFAULT_PROXY
//   - TLS 1.3 标志在 Win7 的 Schannel 上不被接受，设置失败时只保留 TLS 1.2
//   - 不启用 WINHTTP_OPTION_DECOMPRESSION（Win8.1+），改用 Accept-Encoding: identity
// 网络与解析分层：SseDecoder / ChatStreamAccumulator 不依赖 socket，可离线单测。
#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ai_json.h"

namespace rshell::ai {

// ------------------------------------------------------------------ 消息模型

struct ToolCall {
    std::string id;         // 服务端给的 call id，回灌 tool 结果时必须原样带回
    std::string name;
    std::string arguments;  // JSON 字符串，模型可能分片输出，由累加器拼好
};

struct ToolSpec {
    std::string name;
    std::string description;
    Json parameters = Json::object();  // JSON Schema
};

struct Message {
    enum class Role { System, User, Assistant, Tool };

    Role role = Role::User;
    std::string content;
    std::string toolCallId;            // role == Tool 时必填
    std::vector<ToolCall> toolCalls;   // role == Assistant 时可能有
};

// ------------------------------------------------------------------ SSE 解码

// 把任意切分的字节流还原成 SSE 事件。
// 只处理 OpenAI 实际用到的字段（event / data），忽略注释行与 id / retry。
// 连续多个 data: 行按规范用 '\n' 拼接。
class SseDecoder {
public:
    using Handler = std::function<void(const std::string& event, const std::string& data)>;

    void setHandler(Handler handler) { handler_ = std::move(handler); }

    // 喂入一块 HTTP 响应体；缓冲区上限 8 MiB，防止服务端不发换行撑爆内存
    void feed(std::string_view chunk);
    // 流结束时调用：把末尾没有空行结尾的残留事件也派发出去
    void finish();

private:
    void consumeLine(std::string_view line);
    void dispatch();

    static constexpr std::size_t kMaxBuffered = 8u * 1024u * 1024u;

    std::string buffer_;
    std::string event_;
    std::string data_;
    Handler handler_;
};

// ------------------------------------------------------------------ 流累加

struct ChatResult {
    std::string content;
    std::string reasoning;  // DeepSeek 的 reasoning_content / 部分实现的 reasoning
    std::vector<ToolCall> toolCalls;
    std::string finishReason;
    int promptTokens = 0;
    int completionTokens = 0;
    bool cancelled = false;
    std::string error;  // 流里返回的 error 对象（由调用方决定是否抛异常）
};

// 把 chat.completion.chunk 事件拼成完整回复。
// 同一 index 的 tool_call 可能分多块到达，name 与 arguments 都要追加。
class ChatStreamAccumulator {
public:
    struct Callbacks {
        std::function<void(const std::string&)> onText;
        std::function<void(const std::string&)> onReasoning;
        std::function<void(const std::string& name, const std::string& argumentsDelta)> onToolCallDelta;
    };

    explicit ChatStreamAccumulator(Callbacks callbacks = {}) : callbacks_(std::move(callbacks)) {}

    // 处理一个 SSE 事件；data 为 "[DONE]" 时只标记结束
    void handle(const std::string& event, const std::string& data);
    // 取走结果（会把内部状态清空，便于复用）
    ChatResult take();

    bool done() const { return done_; }

private:
    struct Slot {
        std::string id;
        std::string name;
        std::string arguments;
        bool reported = false;
    };

    Callbacks callbacks_;
    std::map<int, Slot> slots_;
    ChatResult result_;
    bool done_ = false;
};

// ------------------------------------------------------------------ 网络客户端

// 对话后端接口：真实实现是 Client；单测里换成脚本化实现，避免联网
class ChatBackend {
public:
    virtual ~ChatBackend() = default;
    virtual ChatResult chat(const std::vector<Message>& messages,
                            const std::vector<ToolSpec>& tools,
                            ChatStreamAccumulator::Callbacks callbacks,
                            const std::function<bool()>& shouldCancel) = 0;
};

struct ApiConfig {
    // 末尾是否带 /v1 都可以；带不带 /chat/completions 也都能识别
    std::string baseUrl = "https://api.openai.com/v1";
    std::string apiKey;
    std::string model = "gpt-4o-mini";
    std::string userAgent = "rshell-gui/1.1";
    int connectTimeoutMs = 15000;
    int sendTimeoutMs = 30000;
    int idleTimeoutMs = 600000;  // 流式长连接的空闲超时，不是总时长
    bool includeUsage = true;
};

class Client : public ChatBackend {
public:
    explicit Client(ApiConfig config);
    ~Client() override;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // 发起一次流式补全。协议错误、网络错误、HTTP 非 2xx 都抛 rshell::Error。
    // shouldCancel 每收到一块数据检查一次；返回 true 时中断，结果里 cancelled=true。
    // 只在调用线程内使用：GUI 应放到工作线程，不要多线程共享同一个 Client。
    ChatResult chat(const std::vector<Message>& messages,
                    const std::vector<ToolSpec>& tools,
                    ChatStreamAccumulator::Callbacks callbacks,
                    const std::function<bool()>& shouldCancel = {}) override;

    const ApiConfig& config() const { return config_; }
    // 设置页保存后热更新，不必重建 Client（Agent 持有它的引用）
    void setConfig(ApiConfig config) { config_ = std::move(config); }

private:
    struct Url {
        std::wstring host;
        std::wstring path;
        int port = 443;
        bool https = true;
    };

    static Url parseUrl(const std::string& baseUrl);
    static std::string buildBody(const ApiConfig& config,
                                 const std::vector<Message>& messages,
                                 const std::vector<ToolSpec>& tools);

    ApiConfig config_;
};

}  // namespace rshell::ai
