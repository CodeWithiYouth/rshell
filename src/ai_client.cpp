// ai_client.cpp
#include "ai_client.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

namespace rshell::ai {

// ------------------------------------------------------------------ SSE 解码

void SseDecoder::feed(std::string_view chunk) {
    if (chunk.empty()) return;
    buffer_.append(chunk.data(), chunk.size());

    // 服务端迟迟不发换行时，直接丢弃，避免无上限增长
    if (buffer_.size() > kMaxBuffered) {
        buffer_.clear();
        return;
    }

    std::size_t consumed = 0;
    for (;;) {
        const std::size_t newline = buffer_.find('\n', consumed);
        if (newline == std::string::npos) break;
        std::string_view line(buffer_.data() + consumed, newline - consumed);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        consumeLine(line);
        consumed = newline + 1;
    }
    if (consumed) buffer_.erase(0, consumed);
}

void SseDecoder::finish() {
    if (!buffer_.empty()) {
        std::string_view line(buffer_);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        consumeLine(line);
        buffer_.clear();
    }
    // 规范要求空行才派发，但服务端直接断开时也要把最后一个事件交出去
    if (!data_.empty()) dispatch();
}

void SseDecoder::consumeLine(std::string_view line) {
    if (line.empty()) {
        dispatch();
        return;
    }
    if (line.front() == ':') return;  // 注释/心跳行

    const std::size_t colon = line.find(':');
    const std::string_view field = colon == std::string_view::npos ? line : line.substr(0, colon);
    std::string_view value = colon == std::string_view::npos
                                 ? std::string_view()
                                 : line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') value.remove_prefix(1);

    if (field == "data") {
        if (!data_.empty()) data_ += '\n';
        data_.append(value);
    } else if (field == "event") {
        event_.assign(value);
    }
    // id / retry 对 OpenAI 兼容接口没有意义，忽略
}

void SseDecoder::dispatch() {
    if (!data_.empty() && handler_) handler_(event_, data_);
    event_.clear();
    data_.clear();
}

// ------------------------------------------------------------------ 流累加

void ChatStreamAccumulator::handle(const std::string& event, const std::string& data) {
    (void)event;
    if (data.empty() || done_) return;
    if (data == "[DONE]") {
        done_ = true;
        return;
    }

    Json root;
    if (!Json::parse(data, root)) {
        // 单个坏块不该中断整次对话：跳过，让后续块继续
        return;
    }

    if (const Json* error = root.find("error")) {
        const Json* message = error->find("message");
        result_.error = message && message->isString() ? message->asString() : error->dump();
        done_ = true;
        return;
    }

    if (const Json* usage = root.find("usage"); usage && usage->isObject()) {
        if (const Json* prompt = usage->find("prompt_tokens"); prompt && prompt->isNumber())
            result_.promptTokens = prompt->asInt();
        if (const Json* completion = usage->find("completion_tokens");
            completion && completion->isNumber())
            result_.completionTokens = completion->asInt();
    }

    const Json* choices = root.find("choices");
    if (!choices || !choices->isArray() || choices->items().empty()) return;
    const Json& choice = choices->items().front();

    if (const Json* reason = choice.find("finish_reason");
        reason && reason->isString() && !reason->asString().empty()) {
        result_.finishReason = reason->asString();
    }

    const Json* delta = choice.find("delta");
    if (!delta || !delta->isObject()) return;

    if (const Json* content = delta->find("content"); content && content->isString()) {
        const std::string& text = content->asString();
        if (!text.empty()) {
            result_.content += text;
            if (callbacks_.onText) callbacks_.onText(text);
        }
    }

    // DeepSeek 用 reasoning_content，另一些实现叫 reasoning
    const Json* reasoning = delta->find("reasoning_content");
    if (!reasoning) reasoning = delta->find("reasoning");
    if (reasoning && reasoning->isString()) {
        const std::string& text = reasoning->asString();
        if (!text.empty()) {
            result_.reasoning += text;
            if (callbacks_.onReasoning) callbacks_.onReasoning(text);
        }
    }

    const Json* toolCalls = delta->find("tool_calls");
    if (!toolCalls || !toolCalls->isArray()) return;

    for (const Json& entry : toolCalls->items()) {
        if (!entry.isObject()) continue;
        const Json* index = entry.find("index");
        const int slot = index && index->isNumber() ? index->asInt() : 0;
        Slot& target = slots_[slot];

        if (const Json* id = entry.find("id"); id && id->isString() && !id->asString().empty())
            target.id = id->asString();

        const Json* function = entry.find("function");
        if (!function || !function->isObject()) continue;

        if (const Json* name = function->find("name"); name && name->isString()) {
            target.name += name->asString();
        }
        const Json* arguments = function->find("arguments");
        if (arguments && arguments->isString()) {
            const std::string& chunk = arguments->asString();
            target.arguments += chunk;
            if (callbacks_.onToolCallDelta && !chunk.empty())
                callbacks_.onToolCallDelta(target.name, chunk);
        }
    }
}

ChatResult ChatStreamAccumulator::take() {
    for (auto& [index, slot] : slots_) {
        ToolCall call;
        call.id = slot.id.empty() ? "call_" + std::to_string(index) : slot.id;
        call.name = slot.name;
        call.arguments = slot.arguments;
        result_.toolCalls.push_back(std::move(call));
    }
    slots_.clear();

    ChatResult out = std::move(result_);
    result_ = ChatResult();
    done_ = false;
    return out;
}

#ifdef _WIN32

namespace {

struct WinHttpCloser {
    void operator()(HINTERNET handle) const {
        if (handle) WinHttpCloseHandle(handle);
    }
};
using Handle = std::unique_ptr<void, WinHttpCloser>;

std::wstring toWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length,
                        nullptr, nullptr);
    return out;
}

std::string lastErrorText() {
    const DWORD code = GetLastError();
    if (code == ERROR_SUCCESS) return std::string();

    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (length && buffer) {
        text = toUtf8(std::wstring(buffer, length));
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    }
    if (buffer) LocalFree(buffer);
    if (text.empty()) text = "错误码 " + std::to_string(code);
    return text;
}

bool isLoopbackHost(const std::string& authority) {
    const std::string host = toLower(authority);
    if (host == "localhost" || host == "127.0.0.1" || host == "[::1]" || host == "::1")
        return true;
    return startsWith(host, "127.");
}

// Win7 没有 AUTOMATIC_PROXY(4)，失败必须回退 DEFAULT_PROXY(0)。
// TLS 1.3 标志在 Win7 上会被拒绝，失败只保留 TLS 1.2。
Handle openSession(const ApiConfig& config) {    constexpr DWORD kAccessAutomaticProxy = 4;  // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY，Win8.1+
    constexpr DWORD kAccessDefaultProxy = 0;    // WINHTTP_ACCESS_TYPE_DEFAULT_PROXY

    const std::wstring agent = toWide(config.userAgent);
    HINTERNET session = WinHttpOpen(agent.c_str(), kAccessAutomaticProxy, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        session = WinHttpOpen(agent.c_str(), kAccessDefaultProxy, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) throw Error("初始化 HTTP 会话失败：" + lastErrorText());

    Handle handle(session);
    WinHttpSetTimeouts(session, config.connectTimeoutMs, config.connectTimeoutMs,
                       config.sendTimeoutMs, config.idleTimeoutMs);

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        DWORD tls12 = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls12, sizeof(tls12));
    }
    return handle;
}

std::string readAll(HINTERNET request) {
    std::string out;
    char buffer[8192];
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &got)) break;
        if (got == 0) break;
        out.append(buffer, got);
        if (out.size() > 1024u * 1024u) break;  // 模型列表比错误体重，给 1 MB
    }
    return out;
}

struct SimpleUrl {
    std::wstring host;
    std::wstring path;
    int port = 443;
    bool https = true;
};

// 只拆 host/port/path，不像 Client::parseUrl 那样补 /chat/completions
bool parseHttpUrl(const std::string& text, SimpleUrl& out, std::string& error) {
    std::string rest = trim(text);
    if (rest.empty()) {
        error = "地址为空";
        return false;
    }
    if (startsWith(rest, "https://")) {
        rest = rest.substr(8);
        out.https = true;
        out.port = 443;
    } else if (startsWith(rest, "http://")) {
        rest = rest.substr(7);
        out.https = false;
        out.port = 80;
    } else {
        out.https = true;
        out.port = 443;
    }

    const std::size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);

    if (!authority.empty() && authority.back() != ']') {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            const std::string port = authority.substr(colon + 1);
            if (!port.empty() && std::all_of(port.begin(), port.end(),
                                             [](unsigned char c) { return c >= '0' && c <= '9'; })) {
                const int value = std::atoi(port.c_str());
                if (value > 0 && value < 65536) {
                    out.port = value;
                    authority.erase(colon);
                }
            }
        }
    }
    if (authority.empty()) {
        error = "地址缺少主机名";
        return false;
    }
    if (!out.https && !isLoopbackHost(authority)) {
        error = "模型列表地址必须使用 HTTPS（本机地址除外）";
        return false;
    }
    out.host = toWide(authority);
    out.path = toWide(path.empty() ? "/" : path);
    return true;
}

}  // namespace

ModelCatalog listModels(const ApiConfig& config, const std::string& url) {
    ModelCatalog result;
    if (config.apiKey.empty()) {
        result.error = "先填写 API Key";
        return result;
    }

    SimpleUrl target;
    std::string error;
    if (!parseHttpUrl(url, target, error)) {
        result.error = error;
        return result;
    }

    try {
        Handle session = openSession(config);
        Handle connection(WinHttpConnect(session.get(), target.host.c_str(),
                                         static_cast<INTERNET_PORT>(target.port), 0));
        if (!connection) {
            result.error = "连接失败：" + lastErrorText();
            return result;
        }
        Handle request(WinHttpOpenRequest(connection.get(), L"GET", target.path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          target.https ? WINHTTP_FLAG_SECURE : 0));
        if (!request) {
            result.error = "创建请求失败：" + lastErrorText();
            return result;
        }

        std::wstring headers = L"Accept: application/json\r\n";
        headers += L"Authorization: Bearer " + toWide(config.apiKey) + L"\r\n";
        if (!config.sessionId.empty())
            headers += L"x-opencode-session: " + toWide(config.sessionId) + L"\r\n";
        if (!WinHttpSendRequest(request.get(), headers.c_str(),
                                static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0,
                                0)) {
            result.error = "发送请求失败：" + lastErrorText();
            return result;
        }
        if (!WinHttpReceiveResponse(request.get(), nullptr)) {
            result.error = "读取响应失败：" + lastErrorText();
            return result;
        }

        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        const std::string body = readAll(request.get());
        if (status < 200 || status >= 300) {
            result.error = "HTTP " + std::to_string(status) +
                           (body.empty() ? std::string() : "：" + body.substr(0, 200));
            return result;
        }

        Json root;
        std::string parseError;
        if (!Json::parse(body, root, &parseError)) {
            result.error = "模型列表不是合法 JSON：" + parseError;
            return result;
        }
        const Json* list = root.find("data");
        if (!list) list = root.find("models");
        if (!list || !list->isArray()) {
            result.error = "模型列表格式不认识（缺少 data 数组）";
            return result;
        }

        for (const Json& item : list->items()) {
            if (!item.isObject()) continue;
            const Json* id = item.find("id");
            if (!id || !id->isString() || id->asString().empty()) continue;

            // 有 supported_endpoints 的（Command Code）只保留支持 chat/completions 的，
            // 否则选了 Claude 会在 /chat/completions 上直接 400
            if (const Json* endpoints = item.find("supported_endpoints");
                endpoints && endpoints->isArray()) {
                bool supported = false;
                for (const Json& entry : endpoints->items()) {
                    if (entry.isString() &&
                        entry.asString().find("chat/completions") != std::string::npos) {
                        supported = true;
                        break;
                    }
                }
                if (!supported) continue;
            }
            result.ids.push_back(id->asString());
        }
        result.ok = true;
        if (result.ids.empty()) result.error = "模型列表为空";
        return result;
    } catch (const std::exception& e) {
        result.error = e.what();
        return result;
    }
}

Client::Client(ApiConfig config) : config_(std::move(config)) {}

Client::~Client() = default;

Client::Url Client::parseUrl(const std::string& baseUrl) {
    Url url;
    std::string rest = trim(baseUrl);
    if (rest.empty()) throw Error("未配置 API 地址");

    if (startsWith(rest, "https://")) {
        rest = rest.substr(8);
        url.https = true;
        url.port = 443;
    } else if (startsWith(rest, "http://")) {
        rest = rest.substr(7);
        url.https = false;
        url.port = 80;
    } else {
        url.https = true;
        url.port = 443;
    }

    const std::size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? std::string() : rest.substr(slash);

    // 端口只认最后一处冒号，且不能落在 IPv6 的方括号里
    if (!authority.empty() && authority.back() == ']') {
        // [::1] 不带端口
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            const std::string port = authority.substr(colon + 1);
            if (!port.empty() && std::all_of(port.begin(), port.end(),
                                             [](unsigned char c) { return c >= '0' && c <= '9'; })) {
                const int value = std::atoi(port.c_str());
                if (value > 0 && value < 65536) {
                    url.port = value;
                    authority.erase(colon);
                }
            }
        }
    }

    while (!path.empty() && path.back() == '/') path.pop_back();
    if (!endsWith(path, "/chat/completions")) path += "/chat/completions";
    if (path.empty() || path.front() != '/') path.insert(path.begin(), '/');

    if (authority.empty()) throw Error("API 地址缺少主机名");

    // 明文 HTTP 只允许本机（Ollama / vLLM 常见），其它地址一律要求 HTTPS，
    // 避免 API Key 在网络上裸奔
    if (!url.https && !isLoopbackHost(authority)) {
        throw Error("AI 接口必须使用 HTTPS（本机地址除外）");
    }

    url.host = toWide(authority);
    url.path = toWide(path);
    return url;
}

std::string Client::buildBody(const ApiConfig& config, const std::vector<Message>& messages,
                              const std::vector<ToolSpec>& tools) {
    Json body = Json::object();
    body["model"] = Json::string(config.model);
    body["stream"] = Json::boolean(true);
    if (config.includeUsage) {
        body["stream_options"]["include_usage"] = Json::boolean(true);
    }

    Json list = Json::array();
    for (const Message& message : messages) {
        Json entry = Json::object();
        switch (message.role) {
            case Message::Role::System: entry["role"] = Json::string("system"); break;
            case Message::Role::User: entry["role"] = Json::string("user"); break;
            case Message::Role::Assistant: entry["role"] = Json::string("assistant"); break;
            case Message::Role::Tool: entry["role"] = Json::string("tool"); break;
        }

        const bool hasToolCalls = !message.toolCalls.empty();
        // assistant 带 tool_calls 时，部分实现不接受空 content 字段
        if (!message.content.empty() || !hasToolCalls) {
            entry["content"] = Json::string(message.content);
        }
        if (!message.toolCallId.empty()) {
            entry["tool_call_id"] = Json::string(message.toolCallId);
        }
        if (hasToolCalls) {
            Json calls = Json::array();
            for (const ToolCall& call : message.toolCalls) {
                Json item = Json::object();
                item["id"] = Json::string(call.id);
                item["type"] = Json::string("function");
                item["function"]["name"] = Json::string(call.name);
                item["function"]["arguments"] = Json::string(call.arguments);
                calls.push(std::move(item));
            }
            entry["tool_calls"] = std::move(calls);
        }
        list.push(std::move(entry));
    }
    body["messages"] = std::move(list);

    if (!tools.empty()) {
        Json specs = Json::array();
        for (const ToolSpec& tool : tools) {
            Json item = Json::object();
            item["type"] = Json::string("function");
            item["function"]["name"] = Json::string(tool.name);
            item["function"]["description"] = Json::string(tool.description);
            item["function"]["parameters"] = tool.parameters;
            specs.push(std::move(item));
        }
        body["tools"] = std::move(specs);
        // 逐步确认的安全模型下，并行调用会让审批队列难以表达，明确关闭
        body["parallel_tool_calls"] = Json::boolean(false);
    }

    return body.dump();
}

ChatResult Client::chat(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
                        ChatStreamAccumulator::Callbacks callbacks,
                        const std::function<bool()>& shouldCancel) {
    if (config_.apiKey.empty()) throw Error("未配置 API Key：请在设置中填写");
    if (config_.model.empty()) throw Error("未配置模型名称");

    const Url url = parseUrl(config_.baseUrl);
    Handle session = openSession(config_);

    Handle connection(WinHttpConnect(session.get(), url.host.c_str(),
                                     static_cast<INTERNET_PORT>(url.port), 0));
    if (!connection) throw Error("连接 AI 服务失败：" + lastErrorText());

    Handle request(WinHttpOpenRequest(connection.get(), L"POST", url.path.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      url.https ? WINHTTP_FLAG_SECURE : 0));
    if (!request) throw Error("创建 HTTP 请求失败：" + lastErrorText());

    const std::string body = buildBody(config_, messages, tools);

    // 显式 identity：WINHTTP_OPTION_DECOMPRESSION 要 Win8.1+，Win7 上解不了 gzip
    std::wstring headers = L"Content-Type: application/json\r\n";
    headers += L"Accept: text/event-stream\r\n";
    headers += L"Accept-Encoding: identity\r\n";
    headers += L"Authorization: Bearer " + toWide(config_.apiKey) + L"\r\n";
    // OpenCode Go 用这个头做路由和提示缓存；别的服务商不认识也无所谓，
    // 所以只在调用方显式给了 sessionId 时才发
    if (!config_.sessionId.empty()) {
        headers += L"x-opencode-session: " + toWide(config_.sessionId) + L"\r\n";
    }

    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                            const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0)) {
        throw Error("发送 AI 请求失败：" + lastErrorText());
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        throw Error("读取 AI 响应失败：" + lastErrorText());
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        const std::string text = readAll(request.get());
        std::string detail = text;
        Json parsed;
        if (Json::parse(text, parsed)) {
            if (const Json* error = parsed.find("error")) {
                const Json* message = error->find("message");
                if (message && message->isString()) detail = message->asString();
            }
        }
        if (detail.size() > 400) detail.resize(400);
        throw Error("AI 服务返回 HTTP " + std::to_string(status) +
                    (detail.empty() ? std::string() : "：" + detail));
    }

    ChatStreamAccumulator accumulator(std::move(callbacks));
    SseDecoder decoder;
    decoder.setHandler([&accumulator](const std::string& event, const std::string& data) {
        accumulator.handle(event, data);
    });

    bool cancelled = false;
    char buffer[16384];
    for (;;) {
        if (shouldCancel && shouldCancel()) {
            cancelled = true;
            break;
        }
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &got)) {
            // 取消时句柄可能已被外部关闭，不把这种情况当错误上报
            if (shouldCancel && shouldCancel()) {
                cancelled = true;
                break;
            }
            throw Error("读取 AI 响应失败：" + lastErrorText());
        }
        if (got == 0) break;
        decoder.feed(std::string_view(buffer, got));
    }
    decoder.finish();

    ChatResult result = accumulator.take();
    result.cancelled = cancelled;
    if (!result.error.empty()) throw Error("AI 服务返回错误：" + result.error);
    return result;
}

#else  // !_WIN32

Client::Client(ApiConfig config) : config_(std::move(config)) {}
Client::~Client() = default;

ChatResult Client::chat(const std::vector<Message>&, const std::vector<ToolSpec>&,
                        ChatStreamAccumulator::Callbacks, const std::function<bool()>&) {
    throw Error("AI 功能目前仅支持 Windows");
}

#endif

}  // namespace rshell::ai
