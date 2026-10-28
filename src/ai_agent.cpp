// ai_agent.cpp
#include "ai_agent.h"

#include <algorithm>
#include <utility>

namespace rshell::ai {

namespace {

Message makeAssistant(const std::string& content, std::vector<ToolCall> calls) {
    Message message;
    message.role = Message::Role::Assistant;
    message.content = content;
    message.toolCalls = std::move(calls);
    return message;
}

Message makeTool(const std::string& callId, std::string content) {
    Message message;
    message.role = Message::Role::Tool;
    message.toolCallId = callId;
    message.content = std::move(content);
    return message;
}

Json errorPayload(const std::string& message) {
    Json payload = Json::object();
    payload["error"] = Json::string(message);
    return payload;
}

}  // namespace

std::string defaultSystemPrompt() {
    return
        "你是 rshell 内置的 Linux 服务器运维助手，通过工具在远端主机上执行只读/写入操作。\n"
        "规则：\n"
        "1. 先观察再动手：不清楚环境时先用 list_dir / read_file / run_command 收集信息，不要凭猜测下结论。\n"
        "2. run_command 属于写操作，每次都会由用户确认，所以命令要短、可读、无交互；"
        "不要用 vim / top / 交互式脚本；分页请用 head / tail。\n"
        "3. 修改文件前先用 read_file 读出原内容；write_file 会覆盖整个文件。\n"
        "4. 不要尝试读取私钥、云凭据、密码库等敏感文件，也不要把凭据写进命令。\n"
        "5. 命令失败时解释原因并给出替代方案，不要反复重试同一个失败的调用。\n"
        "6. 回答用简体中文，先给结论，再给必要的细节；不要输出大段与问题无关的日志。";
}

Agent::Agent(ChatBackend& chat, ToolRegistry& tools, AgentConfig config)
    : chat_(chat), tools_(tools), config_(config) {
    systemPrompt_ = defaultSystemPrompt();
}

void Agent::setSystemPrompt(std::string prompt) { systemPrompt_ = std::move(prompt); }

void Agent::setCallbacks(AgentCallbacks callbacks) { callbacks_ = std::move(callbacks); }

void Agent::setApproval(ApprovalFn approval) { approval_ = std::move(approval); }

void Agent::reset() {
    history_.clear();
}

void Agent::addUserMessage(const std::string& text) {
    Message message;
    message.role = Message::Role::User;
    message.content = text;
    history_.push_back(std::move(message));
}

std::size_t Agent::historyBytes() const {
    std::size_t total = systemPrompt_.size();
    for (const Message& message : history_) {
        total += message.content.size() + message.toolCallId.size();
        for (const ToolCall& call : message.toolCalls) {
            total += call.id.size() + call.name.size() + call.arguments.size();
        }
    }
    return total;
}

void Agent::trimHistory() {
    if (!config_.trimHistory) return;
    while (historyBytes() > config_.historyBytes) {
        // 一轮从 User 开始；丢弃 [0, 下一个 User) 这一段，保证不会把
        // assistant 的 tool_calls 和它对应的 tool 回复拆散
        std::size_t nextRound = std::string::npos;
        for (std::size_t i = 1; i < history_.size(); ++i) {
            if (history_[i].role == Message::Role::User) {
                nextRound = i;
                break;
            }
        }
        if (nextRound == std::string::npos) break;  // 只剩最后一轮，不能再裁
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(nextRound));
    }
}

Message Agent::buildRequestPrefix() const {
    Message message;
    message.role = Message::Role::System;
    message.content = systemPrompt_;
    return message;
}

AgentTurn Agent::run(const std::function<bool()>& shouldCancel) {
    AgentTurn turn;

    bool hasUser = false;
    for (const Message& message : history_) {
        if (message.role == Message::Role::User) {
            hasUser = true;
            break;
        }
    }
    if (!hasUser) {
        turn.error = "没有用户消息";
        return turn;
    }

    const std::vector<ToolSpec> specs = tools_.specs();

    for (int step = 0; step < config_.maxSteps; ++step) {
        if (shouldCancel && shouldCancel()) {
            turn.cancelled = true;
            return turn;
        }

        trimHistory();

        std::vector<Message> request;
        request.reserve(history_.size() + 1);
        request.push_back(buildRequestPrefix());
        request.insert(request.end(), history_.begin(), history_.end());

        ChatStreamAccumulator::Callbacks callbacks;
        callbacks.onText = [this](const std::string& chunk) {
            if (callbacks_.onText) callbacks_.onText(chunk);
        };
        callbacks.onReasoning = [this](const std::string& chunk) {
            if (callbacks_.onReasoning) callbacks_.onReasoning(chunk);
        };

        ChatResult result;
        try {
            result = chat_.chat(request, specs, callbacks, shouldCancel);
        } catch (const std::exception& e) {
            turn.error = e.what();
            return turn;
        }

        turn.steps = step + 1;
        if (result.cancelled) {
            turn.cancelled = true;
            return turn;
        }

        // 没有工具调用：这就是最终回答
        if (result.toolCalls.empty()) {
            history_.push_back(makeAssistant(result.content, {}));
            turn.content = result.content;
            turn.finished = true;
            return turn;
        }

        // 有工具调用：先把 assistant 这一条（含 tool_calls）写进历史，
        // 后面每个调用都必须有一条对应的 role=tool 回复，否则下一轮请求会被拒
        history_.push_back(makeAssistant(result.content, result.toolCalls));

        for (const ToolCall& call : result.toolCalls) {
            const ToolRequestInfo info = tools_.describe(call.name, call.arguments);

            ToolRecord record;
            record.name = call.name;
            record.summary = info.summary;
            record.requiredApproval = info.kind == ToolKind::Write;

            if (record.requiredApproval) {
                ApprovalRequest approval;
                approval.toolName = call.name;
                approval.arguments = call.arguments;
                approval.summary = info.summary;
                approval.detail = info.detail;
                approval.mutating = true;
                approval.dangerous = info.dangerous;
                approval.valid = info.valid;
                approval.problem = info.problem;

                if (callbacks_.onApprovalNeeded) callbacks_.onApprovalNeeded(approval);

                // 没有审批回调时按拒绝处理：安全默认，绝不擅自执行写操作
                const ApprovalDecision decision =
                    approval_ ? approval_(approval) : ApprovalDecision::Deny;
                record.decision = decision;

                if (decision == ApprovalDecision::Abort) {
                    record.executed = false;
                    record.denied = true;
                    record.display = "用户终止任务";
                    turn.tools.push_back(record);
                    if (callbacks_.onToolFinish) callbacks_.onToolFinish(record);
                    history_.push_back(
                        makeTool(call.id, errorPayload("用户终止了任务").dump()));
                    turn.aborted = true;
                    turn.content = result.content;
                    return turn;
                }

                if (decision == ApprovalDecision::Deny) {
                    record.executed = false;
                    record.denied = true;
                    record.display = "用户拒绝执行";
                    turn.tools.push_back(record);
                    if (callbacks_.onToolFinish) callbacks_.onToolFinish(record);
                    history_.push_back(makeTool(
                        call.id,
                        errorPayload("用户拒绝执行该操作。不要重复同样的调用，"
                                     "请说明原因或提出不需要该操作的替代方案。")
                            .dump()));
                    continue;
                }
            }

            ToolResult outcome;
            if (!info.valid) {
                outcome.ok = false;
                outcome.json = errorPayload(info.problem).dump();
                outcome.display = info.problem;
            } else {
                outcome = tools_.execute(call.name, call.arguments);
            }

            record.executed = true;
            record.ok = outcome.ok;
            record.denied = outcome.denied;
            record.display = outcome.display;
            record.brief = outcome.brief;
            turn.tools.push_back(record);
            if (callbacks_.onToolFinish) callbacks_.onToolFinish(record);

            history_.push_back(makeTool(call.id, outcome.json));
        }
    }

    turn.stepLimit = true;
    turn.error = "超过单轮最大步数（" + std::to_string(config_.maxSteps) + "），已暂停";
    return turn;
}

}  // namespace rshell::ai
