// ai_tools.h - AI 工具层：定义、读写分级、路径黑名单、凭据脱敏、输出截断
//
// 设计约束（对应产品决策）：
//   - 写操作（run_command / write_file）必须经用户审批；读操作免审但要可见
//   - 读操作免审意味着模型能读任意文件，所以读路径有黑名单，返回内容做凭据脱敏
//   - 所有回灌给模型的内容都经过截断，避免一次命令输出吃掉整个上下文
// 后端由 GUI 注入（复用 SSH/SFTP 连接），测试注入假实现，因此本层不依赖网络。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ai_client.h"
#include "ai_search.h"

namespace rshell::ai {

enum class ToolKind {
    Read,   // 免审批
    Write,  // 必须审批
};

struct DirEntryView {
    std::string name;
    std::uint64_t size = 0;
    bool isDir = false;
};

// 工具的真实执行后端。方法都是同步的，由 Agent 所在的工作线程调用。
class ToolBackend {
public:
    virtual ~ToolBackend() = default;

    struct CommandResult {
        int exitCode = -1;
        std::string output;  // stdout + stderr 合并
        bool truncated = false;
    };

    // host 为空表示「当前目标主机」；非空时按会话名路由到那个标签的连接。
    // 由工具层把模型给的 host 参数原样传下来，具体能不能路由是后端的事。
    virtual CommandResult runCommand(const std::string& command, int timeoutSec,
                                     const std::string& host = {}) = 0;

    virtual bool readFile(const std::string& path, std::size_t maxBytes, std::string& out,
                          bool& truncated, std::string& error,
                          const std::string& host = {}) = 0;

    virtual bool writeFile(const std::string& path, const std::string& content,
                           std::string& error, const std::string& host = {}) = 0;

    virtual bool listDir(const std::string& path, std::vector<DirEntryView>& out,
                         std::string& error, const std::string& host = {}) = 0;

    // 家目录与建目录：AI 对话要存到 ~/.rshell/sessions。
    // 这两个走 SFTP 而不是 exec——libssh2 会话不能同时被两个线程用，
    // 后台线程里发命令会和界面上的文件面板/采样撞车。
    virtual std::string homeDir() { return {}; }
    virtual bool makeDir(const std::string& path, std::string& error) {
        (void)path;
        (void)error;
        return false;
    }
};

struct ToolPolicy {
    std::size_t maxReadBytes = 96u * 1024u;    // 单次读取上限
    std::size_t maxOutputBytes = 32u * 1024u;  // 回灌给模型的单条工具输出上限
    std::size_t maxEntries = 200;              // 单次列目录上限
    int commandTimeoutSec = 60;
    bool redactSecrets = true;
    // 自动审批：低风险写操作直接执行（仍会记在对话里），高风险才弹确认
    bool autoApprove = false;
    // 读路径黑名单（glob，* 可跨 /）。命中时 read_file 直接拒绝。
    std::vector<std::string> deniedReadPaths;
    // 下发命令一律按写操作处理，因此不需要命令黑名单；
    // 但下面这些模式会在审批卡片上标红，提示用户重点确认。
    std::vector<std::string> dangerousCommandPatterns;
    // 自动审批模式下也必须人确认的命令/路径特征（删除、磁盘、账号、服务、敏感目录）
    std::vector<std::string> highRiskPatterns;
    // write_file 目标路径命中这些也需要确认（读不受影响）
    std::vector<std::string> highRiskWritePaths;
};

// 默认策略：内置一份保守的黑名单与危险命令模式
ToolPolicy defaultToolPolicy();

struct ToolResult {
    bool ok = true;
    std::string brief;    // 卡片右上角的短标记：1.9 KB / 退出码 0 / 12 项
    std::string json;     // 回灌给模型的内容（JSON 字符串）
    std::string display;  // 给用户在面板里看的一行说明
    bool truncated = false;
    bool denied = false;  // 被策略拒绝（黑名单等）
};

// 审批/回显用的描述
struct ToolRequestInfo {
    std::string name;
    ToolKind kind = ToolKind::Write;
    std::string summary;    // 一句话，例如「执行命令：systemctl status nginx」
    std::string detail;     // 原文：完整命令 / 文件内容预览
    bool dangerous = false; // 命中危险命令模式
    // 高风险：删除、不可逆、敏感路径、或模型主动标了 risk=high。
    // 自动审批模式下只有高风险才让用户确认。
    bool highRisk = false;
    bool valid = true;      // 参数是否可解析
    std::string problem;    // valid=false 时的原因
};

class ToolRegistry {
public:
    explicit ToolRegistry(ToolBackend& backend, ToolPolicy policy = defaultToolPolicy());

    // 传给模型的功能描述
    std::vector<ToolSpec> specs() const;

    ToolKind kindOf(const std::string& name) const;
    bool known(const std::string& name) const;

    // 生成审批卡片/回显所需的信息；纯解析，不访问远端
    ToolRequestInfo describe(const std::string& name, const std::string& arguments) const;

    // 执行一次工具调用；不抛异常，失败信息写在返回值里
    ToolResult execute(const std::string& name, const std::string& arguments);

    // name_session 的落点：纯本地记账（不碰远端），由 AiTab 写入当前对话的标题/标签
    using SessionNamer = std::function<bool(const std::string& title,
                                            const std::vector<std::string>& tags,
                                            std::string& error)>;
    void setSessionNamer(SessionNamer namer) { sessionNamer_ = std::move(namer); }

    // web_search 的落点：真正的网络请求由 AiTab 注入（这一层保持离线可单测）。
    // 不设置回调 = 不提供这个工具，模型看不到它。
    using SearchFn = std::function<bool(const std::string& query, int maxResults,
                                        std::vector<SearchHit>& out, std::string& error)>;
    void setSearch(SearchFn fn);
    bool searchEnabled() const;

    const ToolPolicy& policy() const { return policy_; }
    void setPolicy(ToolPolicy policy) { policy_ = std::move(policy); }

private:
    ToolResult runCommand(const Json& args);
    ToolResult readFile(const Json& args);
    ToolResult writeFile(const Json& args);
    ToolResult listDir(const Json& args);
    ToolResult nameSession(const Json& args);
    ToolResult webSearch(const Json& args);

    ToolBackend& backend_;
    ToolPolicy policy_;
    SessionNamer sessionNamer_;
    SearchFn search_;
    mutable std::mutex searchMutex_;  // 设置页可能在工具跑着的时候换后端
};

// ---------------------------------------------------------------- 可单测的纯函数

// 读路径是否被策略拒绝；path 中的反斜杠会先归一成 '/'
bool readPathDenied(const std::string& path, const std::vector<std::string>& patterns);

// 替换疑似凭据，返回替换处数。不会改变文本长度以外的语义，只用于回灌模型的内容。
std::size_t redactSecrets(std::string& text);

// 按 UTF-8 边界截断到 maxBytes 以内；发生截断时 truncated 置 true
std::string truncateUtf8(const std::string& text, std::size_t maxBytes, bool& truncated);

}  // namespace rshell::ai
