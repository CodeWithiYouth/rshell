// ssh_shell.h - 交互式 SSH shell 通道（PTY + 异步读取），供 GUI 终端使用
#pragma once

#include <libssh2.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ssh_session.h"

namespace rshell {

// 回调在工作线程触发，GUI 侧需自行 PostMessage 回 UI 线程
struct ShellCallbacks {
    std::function<void(std::string)> onData;   // 远端输出（stdout+stderr 合并）
    std::function<void()> onClosed;            // 通道结束
};

class SshShell {
public:
    SshShell();
    ~SshShell();

    SshShell(const SshShell&) = delete;
    SshShell& operator=(const SshShell&) = delete;

    // 同步建立连接、申请 PTY 并启动 shell；失败抛 Error
    void start(const ConnectionOptions& options, const std::string& term, int cols, int rows);
    void setCallbacks(ShellCallbacks callbacks);

    // 线程安全：只入队，不在调用线程执行网络 I/O；队列满/断线返回 false。
    bool write(const std::string& data);
    // 线程安全：通知远端窗口大小变化
    void resize(int cols, int rows);
    // 关闭通道并等待读线程退出
    void close();

    bool running() const { return running_.load(); }
    bool connected() const { return connected_.load(); }
    const std::string& fingerprint() const { return fingerprint_; }
    const std::string& kex() const { return kex_; }
    const std::string& serverBanner() const { return banner_; }
    std::string closedReason() const;

private:
    void readerLoop();

    std::unique_ptr<SshSession> session_;
    LIBSSH2_CHANNEL* channel_ = nullptr;
    // 通道 I/O 全部归 reader_；mutex_ 只保护队列、尺寸请求与关闭原因。
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> outgoing_;
    std::size_t queuedBytes_ = 0;
    int resizeCols_ = 0;
    int resizeRows_ = 0;
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    ShellCallbacks callbacks_;
    std::string fingerprint_;
    std::string kex_;
    std::string banner_;
    std::string closedReason_;
};

}  // namespace rshell
