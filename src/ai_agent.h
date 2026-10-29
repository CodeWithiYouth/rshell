// ai_agent.h - Agent 循环与审批状态机
//
// 一轮 run() 的流程：
//   请求 → 模型要么给出最终回答，要么给出一批 tool_calls
//        → 读工具直接执行；写工具阻塞等待审批
//        → 工具结果作为 role=tool 消息回灌，再请求
// 审批回调在调用 run() 的工作线程上阻塞执行，GUI 侧需要把请求抛到 UI 线程
// 再等待用户选择（不要在这个回调里直接建窗口）。
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "ai_client.h"
#include "ai_tools.h"

namespace rshell::ai {

enum class ApprovalDecision {
    Allow,  // 本次允许
    Deny,   // 拒绝，但让模型继续想办法
    Abort,  // 终止整个任务
};

struct ApprovalRequest {
    std::string toolName;
    std::string arguments;
    std::string summary;  // 一行摘要，给卡片标题
    std::string detail;   // 原文：完整命令 / 写入内容预览
    bool mutating = true;
    bool dangerous = false;  // 命中危险命令模式，UI 应标红
    bool valid = true;
    std::string problem;  // 参数不可解析时的原因
};

// 返回审批结果；未设置回调时写操作一律按 Deny 处理（安全默认）
using ApprovalFn = std::function<ApprovalDecision(const ApprovalRequest&)>;

struct ToolRecord {
    std::string name;
    std::string summary;
    bool requiredApproval = false;
    ApprovalDecision decision = ApprovalDecision::Allow;
    bool executed = false;
    bool ok = false;
    bool denied = false;  // 被策略或用户拒绝
    bool autoApproved = false;  // 自动审批模式下放行的写操作
    std::string display;  // 完整说明：显示在展开的工具卡片里
    std::string brief;    // 短标记：显示在卡片右上角
};

struct AgentCallbacks {
    std::function<void(const std::string&)> onText;
    std::function<void(const std::string&)> onReasoning;
    // 审批前的通知（可选）：UI 可以先把卡片画出来，ApprovalFn 再阻塞等待点击
    std::function<void(const ApprovalRequest&)> onApprovalNeeded;
    std::function<void(const ToolRecord&)> onToolFinish;
};

struct AgentTurn {
    std::string content;  // 最终回答（中间过程的文本已经通过 onText 流式发出）
    bool finished = false;
    bool cancelled = false;  // 用户取消
    bool aborted = false;    // 审批时选择「终止任务」
    bool stepLimit = false;  // 达到单轮最大步数
    int steps = 0;
    std::vector<ToolRecord> tools;
    std::string error;  // 致命错误（网络、协议、参数）
};

struct AgentConfig {
    int maxSteps = 16;
    // 历史超过该字节数时丢弃最早的完整轮次（system 与最近一轮始终保留）
    std::size_t historyBytes = 512u * 1024u;
    bool trimHistory = true;
};

class Agent {
public:
    Agent(ChatBackend& chat, ToolRegistry& tools, AgentConfig config = AgentConfig());

    void setSystemPrompt(std::string prompt);
    void setCallbacks(AgentCallbacks callbacks);
    void setApproval(ApprovalFn approval);
    void setConfig(AgentConfig config) { config_ = config; }

    void reset();
    void addUserMessage(const std::string& text);

    // 阻塞运行，直到模型给出最终回答、被取消、被终止或出错。
    // 不要与同一个 Agent 并发调用。
    AgentTurn run(const std::function<bool()>& shouldCancel = {});

    const std::vector<Message>& history() const { return history_; }
    std::size_t historyBytes() const;

private:
    void trimHistory();
    Message buildRequestPrefix() const;

    ChatBackend& chat_;
    ToolRegistry& tools_;
    AgentConfig config_;
    std::string systemPrompt_;
    AgentCallbacks callbacks_;
    ApprovalFn approval_;
    std::vector<Message> history_;
};

// 默认系统提示：约束命令风格、禁止猜测、敏感操作先说明
std::string defaultSystemPrompt();

}  // namespace rshell::ai
