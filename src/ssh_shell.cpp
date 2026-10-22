// ssh_shell.cpp
#include "ssh_shell.h"

#include <chrono>
#include <vector>

#include "util.h"

namespace rshell {

namespace {

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

SshShell::SshShell() = default;

SshShell::~SshShell() { close(); }

void SshShell::setCallbacks(ShellCallbacks callbacks) { callbacks_ = std::move(callbacks); }

void SshShell::start(const ConnectionOptions& options, const std::string& term, int cols, int rows) {
    close();
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
    int idle = 0;
    while (!stop_.load()) {
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!channel_) break;
            auto n = libssh2_channel_read(channel_, buf.data(), buf.size());
            while (n > 0) {
                if (callbacks_.onData) callbacks_.onData(std::string(buf.data(), static_cast<std::size_t>(n)));
                active = true;
                n = libssh2_channel_read(channel_, buf.data(), buf.size());
            }
            if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
                closedReason_ = "读取失败（" + std::to_string(n) + "）";
                break;
            }
            n = libssh2_channel_read_stderr(channel_, buf.data(), buf.size());
            while (n > 0) {
                if (callbacks_.onData) callbacks_.onData(std::string(buf.data(), static_cast<std::size_t>(n)));
                active = true;
                n = libssh2_channel_read_stderr(channel_, buf.data(), buf.size());
            }
            if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
                closedReason_ = "读取 stderr 失败";
                break;
            }
            if (libssh2_channel_eof(channel_) == 1) {
                closedReason_.clear();
                break;
            }
        }
        idle = active ? 0 : idle + 1;
        sleepMs(active ? 0 : (idle > 50 ? 20 : 8));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (channel_) {
            libssh2_channel_close(channel_);
            libssh2_channel_free(channel_);
            channel_ = nullptr;
        }
        if (session_) {
            libssh2_session_set_blocking(session_->raw(), 1);
        }
    }
    running_ = false;
    connected_ = false;
    if (callbacks_.onClosed) callbacks_.onClosed();
}





bool SshShell::writeRaw(const char* data, std::size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!channel_) return false;
    std::size_t off = 0;
    int spins = 0;
    while (off < len) {
        auto written = libssh2_channel_write(channel_, data + off, len - off);
        if (written == LIBSSH2_ERROR_EAGAIN) {
            if (++spins > 2000) return false;
            sleepMs(1);
            continue;
        }
        if (written < 0) return false;
        off += static_cast<std::size_t>(written);
        spins = 0;
    }
    return true;
}

void SshShell::write(const std::string& data) {
    if (data.empty() || !running_.load()) return;
    writeRaw(data.data(), data.size());
}

void SshShell::resize(int cols, int rows) {
    if (!running_.load()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!channel_) return;
    for (int i = 0; i < 100; ++i) {
        int rc = libssh2_channel_request_pty_size(channel_, cols, rows);
        if (rc != LIBSSH2_ERROR_EAGAIN) return;
        sleepMs(2);
    }
}

void SshShell::close() {
    stop_ = true;
    if (reader_.joinable()) reader_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (channel_) {
            libssh2_channel_close(channel_);
            libssh2_channel_free(channel_);
            channel_ = nullptr;
        }
    }
    if (session_) {
        session_->disconnect();
        session_.reset();
    }
    running_ = false;
    connected_ = false;
}

}  // namespace rshell
