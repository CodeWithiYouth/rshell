// ssh_session.h - 基于 libssh2 的 SSH 会话封装
#pragma once

#include <libssh2.h>

#include <cstdint>
#include <functional>
#include <string>

namespace rshell {

struct ConnectionOptions {
    std::string host;
    int port = 22;
    std::string user;
    std::string password;       // 可空：无密码时交互式询问
    std::string keyPath;        // 可空：私钥路径
    std::string keyPassphrase;  // 可空：私钥口令
    bool allowDefaultKeys = true;  // 依次尝试 ~/.ssh/id_ed25519 等
    bool verifyHostKey = true;     // 使用 known_hosts 校验
    bool strictHostKey = false;    // 未知主机直接拒绝（不询问）
    int timeoutSec = 20;

    // 未知主机时由宿主程序（如 GUI）询问用户；返回 true 表示信任并写入 known_hosts。
    // 未设置时沿用终端交互逻辑（仅 stdin 是 tty 才询问）。
    std::function<bool(const std::string& hostField, const std::string& fingerprint,
                       const std::string& keyType)>
        confirmHostKey;

    ConnectionOptions() = default;
    ConnectionOptions(const ConnectionOptions&) = default;
    ConnectionOptions(ConnectionOptions&&) = default;
    ConnectionOptions& operator=(const ConnectionOptions&) = default;
    ConnectionOptions& operator=(ConnectionOptions&&) = default;
    // 析构时把口令清零：线程闭包、SshSession、RemoteFs 各持一份拷贝，
    // 释放前都抹掉，不让明文原样还回堆里
    ~ConnectionOptions() { wipeSecrets(); }

    // 立刻清零 password / keyPassphrase（可重复调用）
    void wipeSecrets();
};

// 管理一条 SSH 连接的生命周期。所有阻塞 API 都带超时，避免卡死。
class SshSession {
public:
    explicit SshSession(ConnectionOptions options);
    ~SshSession();

    SshSession(const SshSession&) = delete;
    SshSession& operator=(const SshSession&) = delete;

    // 建立 TCP 连接 -> 握手 -> 校验主机密钥 -> 认证
    void connect();
    void disconnect();

    // 执行远端命令，转发 stdout/stderr，stdin 非终端时透传其内容。
    // 返回远端进程退出码；协议错误抛 Error。
    int execute(const std::string& command);

    // 交互式 shell（申请 PTY）。返回远端退出码。
    int interactiveShell(const std::string& term = "xterm-256color");

    LIBSSH2_SESSION* raw() const { return session_; }
    libssh2_socket_t socketFd() const { return sock_; }
    const ConnectionOptions& options() const { return options_; }
    const std::string& serverBanner() const { return banner_; }
    const std::string& fingerprint() const { return fingerprint_; }
    const std::string& keyExchange() const { return kex_; }
    const std::string& hostKeyAlgorithm() const { return hostKeyAlgo_; }

    // 当前终端的行列数（拿不到时返回 80x24）
    static void terminalSize(int& cols, int& rows);

private:
    void openTcp();
    void guardEmptyHost() const;
    void handshake();
    void verifyHostKey();
    void authenticate();

    [[noreturn]] void fail(const std::string& what) const;
    std::string lastErrorText() const;
    std::string sshErrorText(int code) const;

    ConnectionOptions options_;
    LIBSSH2_SESSION* session_ = nullptr;
    libssh2_socket_t sock_ = LIBSSH2_INVALID_SOCKET;
    std::string banner_;
    std::string fingerprint_;
    std::string kex_;
    std::string hostKeyAlgo_;
    bool authenticated_ = false;
};

// libssh2 全局初始化/清理（进程级）
class Libssh2Runtime {
public:
    Libssh2Runtime();
    ~Libssh2Runtime();
    Libssh2Runtime(const Libssh2Runtime&) = delete;
    Libssh2Runtime& operator=(const Libssh2Runtime&) = delete;
};

}  // namespace rshell
