// ai_tab.h - 全局 AI 标签页：子窗口 + 工作线程 + 审批桥接
//
// 一个进程只有一个 AI 标签（关闭即销毁对话）。窗口是主窗口的子窗口，
// 只覆盖内容区，不碰标签栏；对话状态由 AiTab 自己持有。
//
// 线程模型：
//   - 界面线程：所有 ChatPanel 操作、审批点击
//   - 工作线程：Client::chat / 工具执行，事件用 PostMessage 回投
//   - 审批：工作线程阻塞在 condition_variable 上，界面点按钮后唤醒
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ai_panel.h"

namespace rshell {

class Vault;

namespace ai {

// 一个可选的目标主机。backend 是 RemoteFs*，为空表示该会话还没建立文件连接。
struct AiHostInfo {
    // tabId：面板是全局的，靠它把「当前标签 / 一轮对话的目标」对应回具体会话。
    // 用 id 而不是下标，标签增减时不会张冠李戴。
    std::uintptr_t id = 0;
    std::wstring label;
    void* backend = nullptr;  // RemoteFs*，为空表示该标签还没连上
};

struct AiTabContext {
    // 每次需要时重新枚举，避免持有过期指针
    std::function<std::vector<AiHostInfo>()> hosts;
    // 当前激活标签的 id（面板空闲时按它选目标主机）
    std::function<std::uintptr_t()> activeHostId;
    Vault* vault = nullptr;
    // 保险库未解锁时由宿主弹出解锁框；返回 true 表示已解锁
    std::function<bool()> ensureVaultUnlocked;
    // 本机是否存在 vault.dat。没有文件时「解锁」无从谈起，直接走设置页填 Key。
    // 空表示宿主没有提供，按「有保险库」处理。
    std::function<bool()> vaultPresent;
    HFONT uiFont = nullptr;
    HFONT boldFont = nullptr;
    HFONT monoFont = nullptr;
    HFONT captionFont = nullptr;
    std::function<void()> onSettingsSaved;  // 可选：宿主刷新界面
    std::function<void()> onToggleFullScreen;  // 面板头部「全屏/停靠」按钮
    std::function<void()> onClose;             // 面板头部「×」
};

class AiTab {
public:
    static AiTab* create(HWND parent, AiTabContext context);
    static void destroy(AiTab* tab);

    HWND window() const { return window_; }
    void setBounds(const RECT& bounds);
    void show(bool visible);
    void focus();
    void setFullScreen(bool full);
    // 换标签后刷新标题上的主机名
    void refreshHost();
    // 当前目标主机的名字（回归测试用）
    std::wstring hostLabel() const { return currentHostLabel(); }
    // 回归测试用：按会话名解析工具后端，走的是和工具调用完全相同的路径
    void* resolveHostForTest(const std::string& name) const {
        std::string error;
        return backendForName(name, error);
    }
    // 打开设置视图；unlockOnly=true 表示从空状态「解锁」进来
    // （解锁成功且已经读到 Key 就直接返回，不弹表单）
    void startSettings(bool unlockOnly = false);
    // 设置表单是否展开（回归测试用：取消解锁时不能打开）
    bool settingsOpen() const { return settingsOpen_; }

private:
    AiTab() = default;
    ~AiTab() = default;

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

    void initialize(HWND parent);
    void applyTheme();
    void sendMessage(const std::wstring& text);
    void cancelTurn();
    void finishSettings(bool save);
    void createSettingsControls();
    void layoutSettings();
    void startModelFetch();
    void updateHostLabel();
    // 本机有保险库文件且当前锁着（只有这种情况才弹解锁框）
    bool vaultLocked() const;
    void updateKeyNotice();
    void updateWelcome();
    void paint(HDC dc);
    void onPanelApproval(std::size_t index, bool allowed);

    // 工作线程 → 界面线程的事件
    enum class EventKind {
        AssistantBegin,
        Text,
        Reasoning,
        Tool,
        Approval,
        TurnDone,
    };
    struct Event {
        EventKind kind = EventKind::Text;
        std::wstring a, b, c;
        bool ok = true;
        bool flag = false;
        std::shared_ptr<void> payload;  // 审批用的等待槽
    };
    void requestRepaint();
    void positionIme();
    void postEvent(EventKind kind, std::wstring a, std::wstring b = {}, std::wstring c = {},
                   bool ok = true, std::shared_ptr<void> payload = nullptr);
    void handleEvent(Event& event);
    void onTurnFinished(const std::wstring& status, bool ok);

    std::wstring currentHostLabel() const;
    void* currentBackend() const;
    // 一轮对话内锁定发问时的那台主机，避免中途切标签把命令打到别的机器上
    void* turnBackend() const;
    std::uintptr_t activeHostId() const;
    std::wstring hostLabelById(std::uintptr_t id) const;
    void* hostBackendById(std::uintptr_t id) const;
    // 工具里的 host 参数：空 = 当前/本轮锁定的主机；否则按会话名找
    void* backendForName(const std::string& host, std::string& error) const;
    // 系统提示（含当前主机与其它已打开会话）
    std::string buildSystemPrompt();

    struct Impl;
    std::unique_ptr<Impl> impl_;   // 隐藏 Client/Agent/工具链，避免头文件依赖
    HWND window_ = nullptr;
    HWND parent_ = nullptr;
    ChatPanel panel_;
    AiTabContext context_;
    std::vector<std::size_t> hostHits_;
    // 一轮对话里锁定的目标标签；0 表示还没开始新的一轮
    std::uintptr_t turnHostId = 0;
    bool settingsOpen_ = false;
};

}  // namespace ai
}  // namespace rshell
