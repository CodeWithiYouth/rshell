// mcp_protocol.cpp
#include "mcp_protocol.h"

#include <string>

namespace rshell::mcp {

namespace {

using ai::Json;

Json idOf(const Json& message) {
    if (const Json* id = message.find("id")) return *id;
    return Json();
}

std::string makeResult(const Json& id, Json result) {
    Json out = Json::object();
    out["jsonrpc"] = Json::string("2.0");
    out["id"] = id;
    out["result"] = std::move(result);
    return out.dump();
}

std::string makeError(const Json& id, int code, const std::string& message) {
    Json error = Json::object();
    error["code"] = Json::number(code);
    error["message"] = Json::string(message);

    Json out = Json::object();
    out["jsonrpc"] = Json::string("2.0");
    out["id"] = id;
    out["error"] = std::move(error);
    return out.dump();
}

// 客户端给的版本我们认识就原样回；不认识就回自己的，让客户端决定是否继续。
std::string negotiateVersion(const std::string& requested) {
    static const char* kSupported[] = {"2024-11-05", "2025-03-26", "2025-06-18",
                                       "2025-11-25"};
    for (const char* version : kSupported)
        if (requested == version) return requested;
    return Protocol::kFallbackVersion;
}

Json textContent(const std::string& text) {
    Json content = Json::object();
    content["type"] = Json::string("text");
    content["text"] = Json::string(text);
    return content;
}

}  // namespace

Protocol::Protocol(ToolHost& host, std::string serverVersion)
    : host_(host), version_(std::move(serverVersion)) {}

std::string Protocol::handle(const std::string& message) {
    Json request;
    std::string parseError;
    if (!Json::parse(message, request, &parseError)) {
        return makeError(Json(), -32700, "无法解析 JSON：" + parseError);
    }
    if (!request.isObject()) {
        return makeError(Json(), -32600, "请求必须是 JSON 对象");
    }

    const Json* methodValue = request.find("method");
    const bool notification = request.find("id") == nullptr;

    // 没有 method 的对象通常是客户端对我们通知/响应的回包，直接忽略。
    if (!methodValue || !methodValue->isString() || methodValue->asString().empty()) {
        if (notification) return {};
        return makeError(idOf(request), -32600, "缺少 method");
    }

    const std::string method = methodValue->asString();
    const Json id = idOf(request);

    if (notification) {
        if (method == "notifications/initialized") initialized_ = true;
        return {};  // 包括 notifications/cancelled：当前没有可取消的长任务
    }

    if (method == "initialize") return makeResult(id, initializeResult(request));
    if (method == "ping") return makeResult(id, Json::object());
    if (method == "tools/list") return makeResult(id, toolsResult());
    if (method == "tools/call") return callTool(id, request);

    return makeError(id, -32601, "未知方法：" + method);
}

ai::Json Protocol::initializeResult(const Json& request) const {
    std::string requested;
    if (const Json* params = request.find("params")) {
        if (const Json* version = params->find("protocolVersion")) {
            if (version->isString()) requested = version->asString();
        }
    }

    Json tools = Json::object();
    tools["listChanged"] = Json::boolean(false);

    Json capabilities = Json::object();
    capabilities["tools"] = std::move(tools);

    Json info = Json::object();
    info["name"] = Json::string("rshell");
    info["version"] = Json::string(version_);

    Json result = Json::object();
    result["protocolVersion"] = Json::string(negotiateVersion(requested));
    result["capabilities"] = std::move(capabilities);
    result["serverInfo"] = std::move(info);
    result["instructions"] = Json::string(
        "rshell 把已连接的 SSH 会话暴露为 MCP 工具。host 参数留空表示当前激活的标签，"
        "填会话名可以操作其它已打开的主机。");
    return result;
}

ai::Json Protocol::toolsResult() const {
    Json list = Json::array();
    for (const ToolDef& def : host_.tools()) {
        Json tool = Json::object();
        tool["name"] = Json::string(def.name);
        tool["description"] = Json::string(def.description);
        tool["inputSchema"] = def.inputSchema;

        Json annotations = Json::object();
        annotations["readOnlyHint"] = Json::boolean(def.readOnly);
        annotations["destructiveHint"] = Json::boolean(!def.readOnly);
        annotations["openWorldHint"] = Json::boolean(false);
        tool["annotations"] = std::move(annotations);

        list.push(std::move(tool));
    }

    Json result = Json::object();
    result["tools"] = std::move(list);
    return result;
}

std::string Protocol::callTool(const ai::Json& id, const ai::Json& request) {
    const Json* params = request.find("params");
    if (!params || !params->isObject()) {
        return makeError(id, -32602, "tools/call 缺少 params");
    }
    const Json* name = params->find("name");
    if (!name || !name->isString() || name->asString().empty()) {
        return makeError(id, -32602, "tools/call 缺少工具名");
    }

    // 规范要求 arguments 是对象；有些客户端会序列化成字符串，这里都兼容。
    Json arguments = Json::object();
    if (const Json* value = params->find("arguments")) {
        if (value->isObject()) {
            arguments = *value;
        } else if (value->isString()) {
            Json parsed;
            if (Json::parse(value->asString(), parsed) && parsed.isObject()) {
                arguments = std::move(parsed);
            }
        }
    }

    bool isError = false;
    std::string text = host_.call(name->asString(), arguments, isError);

    Json result = Json::object();
    result["content"] = Json::array();
    result["content"].push(textContent(text.empty() ? (isError ? "工具执行失败" : "完成") : text));
    result["isError"] = Json::boolean(isError);
    return makeResult(id, std::move(result));
}

}  // namespace rshell::mcp
