// ssh_shell.cpp
#include "ssh_shell.h"

#include <chrono>
#include <algorithm>
#include <vector>

#include "util.h"
#include "credential_store.h"

namespace rshell {

SshShell::SshShell() = default;

SshShell::~SshShell() { close(); }

void SshShell::setCallbacks(ShellCallbacks callbacks) { callbacks_ = std::move(callbacks); }

void SshShell::start(const ConnectionOptions& options, const std::string& term, int cols, int rows) {
    close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closedReason_.clear();
    }
    session_ = std::make_unique<SshSession>(options);
    session_->connect();
    fingerprint_ = session_->fingerprint();
    kex_ = session_->keyExchange();
    banner_ = session_->serverBanner();

    LIBSSH2_SESSION* raw = session_->raw();
    channel_ = libssh2_channel_open_session(raw);
    if (!channel_) throw Error("无法打开 SSH 会话通道");

    const int c = cols > 0 ? cols : 80;
    const int r = rows > 0 ? rows : 24;
    if (libssh2_channel_request_pty_ex(channel_, term.c_str(),
                                       static_cast<unsigned int>(term.size()), nullptr, 0, c, r,
                                       0, 0) != 0) {
        throw Error("远端拒绝分配伪终端");
    }
    if (libssh2_channel_shell(channel_) != 0) throw Error("远端拒绝启动 shell");

    libssh2_session_set_blocking(raw, 0);
    connected_ = true;
    running_ = true;
    stop_ = false;
    reader_ = std::thread([this]() { readerLoop(); });
}

void SshShell::readerLoop() {
    std::vector<char> buf(64 * 1024);
    std::string pending;
    std::string reason;
    std::size_t offset = 0;
    std::size_t attemptSize = 0;
    int cols = 0, rows = 0;
    auto lastProgress = std::chrono::steady_clock::now();
    while (!stop_.load()) {
        bool active = false;
        bool readPending = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending.empty() && cols == 0 && resizeCols_ > 0) {
                cols = resizeCols_;
                rows = resizeRows_;
                resizeCols_ = 0;
            }
            if (cols == 0 && pending.empty() && !outgoing_.empty()) {
                pending = std::move(outgoing_.front());
                outgoing_.pop_front();
                offset = 0;
                lastProgress = std::chrono::steady_clock::now();
            }
        }
        if (!pending.empty()) {
            // EAGAIN 后必须用相同的地址和长度重试，期间继续读取远端输出。
            if (attemptSize == 0) attemptSize = std::min<std::size_t>(16384, pending.size() - offset);
            const auto n = libssh2_channel_write(channel_, pending.data() + offset, attemptSize);
            if (n > 0) {
                offset += static_cast<std::size_t>(n);
                attemptSize = 0;
                lastProgress = std::chrono::steady_clock::now();
                active = true;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    queuedBytes_ -= static_cast<std::size_t>(n);
                }
                if (offset == pending.size()) wipeString(pending);
            } else if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
                reason = "发送失败（" + std::to_string(n) + "）";
                break;
            } else if (std::chrono::steady_clock::now() - lastProgress > std::chrono::seconds(30)) {
                reason = "发送超时：远端持续未接收数据";
                break;
            }
        }
        if (cols > 0) {
            const int rc = libssh2_channel_request_pty_size(channel_, cols, rows);
            if (rc != LIBSSH2_ERROR_EAGAIN) cols = 0;
        }
        for (int stream = 0; stream < 2 && reason.empty(); ++stream) {
            // 限制单轮读取量，连续输出时也给发送、resize 和退出留出机会。
            for (int batch = 0; batch < 8 && !stop_.load(); ++batch) {
                const auto n = libssh2_channel_read_ex(channel_, stream, buf.data(), buf.size());
                if (n > 0) {
                    if (callbacks_.onData)
                        callbacks_.onData(std::string(buf.data(), static_cast<std::size_t>(n)));
                    active = true;
                    if (batch == 7) readPending = true;
                } else {
                    if (n < 0 && n != LIBSSH2_ERROR_EAGAIN)
                        reason = "网络读取失败（" + std::to_string(n) + "）";
                    break;
                }
            }
        }
        if (!reason.empty()) break;
        if (!readPending && libssh2_channel_eof(channel_) == 1) {
            const int status = libssh2_channel_get_exit_status(channel_);
            reason = "远端会话已结束（退出码 " + std::to_string(status) + "）";
            break;
        }
        if (!active) {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(8), [&]() {
                return stop_.load() || (pending.empty() && cols == 0 &&
                       (!outgoing_.empty() || resizeCols_ > 0));
            });
        }
    }
    wipeString(pending);
    if (channel_) {
        libssh2_channel_close(channel_);
        libssh2_channel_free(channel_);
        channel_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::string& data : outgoing_) wipeString(data);
        outgoing_.clear();
        queuedBytes_ = 0;
        resizeCols_ = 0;
        closedReason_ = reason;
        running_ = false;
        connected_ = false;
    }
    if (!stop_.load() && callbacks_.onClosed) callbacks_.onClosed();
}

std::string SshShell::closedReason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closedReason_;
}

bool SshShell::write(const std::string& data) {
    if (data.empty()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    constexpr std::size_t maxQueuedBytes = 8 * 1024 * 1024;
    if (!running_.load() || stop_.load() || data.size() > maxQueuedBytes - queuedBytes_)
        return false;
    outgoing_.push_back(data);
    queuedBytes_ += data.size();
    wake_.notify_one();
    return true;
}

void SshShell::resize(int cols, int rows) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load() || stop_.load()) return;
    resizeCols_ = std::max(1, cols);
    resizeRows_ = std::max(1, rows);
    wake_.notify_one();
}

void SshShell::close() {
    stop_ = true;
    wake_.notify_one();
    if (reader_.joinable()) reader_.join();
    if (channel_) {
        libssh2_channel_close(channel_);
        libssh2_channel_free(channel_);
        channel_ = nullptr;
    }
    if (session_) {
        session_->disconnect();
        session_.reset();
    }
    running_ = false;
    connected_ = false;
}

}  // namespace rshell
