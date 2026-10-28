// remote_fs.cpp
#include "remote_fs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <libssh2_sftp.h>

#include "credential_store.h"
#include "sftp_client.h"
#include "util.h"

namespace rshell {

namespace {

// 采样脚本：只用 POSIX sh 支持的语法，尽量兼容 busybox / dash。
// ##X 是分段标记，解析时按标记切块；两次读 /proc/stat 用来算 CPU 占用率。
const char* kSampleScript =
    "LC_ALL=C\n"
    "echo '##HOST'; hostname 2>/dev/null || uname -n 2>/dev/null\n"
    "echo '##OS'; (sed -n 's/^PRETTY_NAME=\"\\{0,1\\}\\(.*\\)\"\\{0,1\\}$/\\1/p' /etc/os-release "
    "2>/dev/null | head -n 1) || uname -sr 2>/dev/null\n"
    "echo '##UP'; cut -d. -f1 /proc/uptime 2>/dev/null\n"
    "echo '##LOAD'; cut -d' ' -f1-3 /proc/loadavg 2>/dev/null\n"
    "echo '##CORES'; (nproc 2>/dev/null || grep -c '^processor' /proc/cpuinfo 2>/dev/null)\n"
    "echo '##CPU'; grep '^cpu ' /proc/stat 2>/dev/null\n"
    "sleep 0.3\n"
    "echo '##CPU2'; grep '^cpu ' /proc/stat 2>/dev/null\n"
    "echo '##MEM'; grep -E '^(MemTotal|MemAvailable|MemFree):' /proc/meminfo 2>/dev/null\n"
    "echo '##DISK'; df -kP / 2>/dev/null | tail -n 1\n"
    "echo '##END'\n";

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char ch : text) {
        if (ch == '\n') {
            if (!current.empty() && current.back() == '\r') current.pop_back();
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

// /proc/stat 的 cpu 行：cpu user nice system idle iowait irq softirq steal ...
bool parseCpuLine(const std::string& line, std::uint64_t& idleTicks, std::uint64_t& totalTicks) {
    std::istringstream in(line);
    std::string tag;
    if (!(in >> tag) || tag != "cpu") return false;
    std::uint64_t value = 0;
    std::vector<std::uint64_t> fields;
    while (in >> value) fields.push_back(value);
    if (fields.size() < 4) return false;
    totalTicks = 0;
    for (std::uint64_t v : fields) totalTicks += v;
    idleTicks = fields[3] + (fields.size() > 4 ? fields[4] : 0);  // idle + iowait
    return true;
}

}  // namespace

bool RemoteEntry::isDir() const { return (mode & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR; }
bool RemoteEntry::isLink() const { return (mode & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFLNK; }
bool RemoteEntry::isFile() const { return (mode & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFREG; }

RemoteFs::RemoteFs() = default;

RemoteFs::~RemoteFs() { stop(); }

void RemoteFs::setCallbacks(RemoteFsCallbacks callbacks) { callbacks_ = std::move(callbacks); }

void RemoteFs::post(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
}

// 外部线程（OLE 复制线程）要读远端数据，但 libssh2 会话只能由文件线程碰。
// 这里把活儿排进同一个队列，然后等它做完。断开连接时 worker 已经退出，
// 队列里的任务不会有人跑，靠超时脱身，避免把资源管理器卡死。
bool RemoteFs::runSync(Task task, int timeoutMs) {
    if (!running_.load()) return false;
    struct Job {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    auto job = std::make_shared<Job>();
    Task payload = std::move(task);
    post([job, payload]() {
        if (payload) payload();
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->done = true;
        }
        job->cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(job->mutex);
    return job->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [&job]() { return job->done; });
}

void RemoteFs::start(const ConnectionOptions& options) {
    stop();

    options_ = options;
    // 口令单独以密文保存；options_ 里只留 host/port/user/keyPath 这类非敏感字段
    password_.assignUtf8(options_.password);
    wipeString(options_.password);
    keyPassphrase_.assignUtf8(options_.keyPassphrase);
    wipeString(options_.keyPassphrase);
    // 建连超时压短一点：断开时 worker 最多等这么久
    if (options_.timeoutSec > 10 || options_.timeoutSec <= 0) options_.timeoutSec = 10;
    running_ = true;
    ready_ = false;
    sftpReady_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.clear();
    }
    worker_ = std::thread([this]() { workerLoop(); });
}

void RemoteFs::stop() {
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.clear();
    }
    sftp_.reset();
    if (session_) {
        session_->disconnect();
        session_.reset();
    }
    ready_ = false;
    sftpReady_ = false;
    statsPending_ = false;
    password_.clear();  // 断开后不再需要这两份口令
    keyPassphrase_.clear();
}

void RemoteFs::listDir(const std::string& path) {
    post([this, path]() {
        FsEvent event;
        event.kind = FsEventKind::Listing;
        event.path = path;
        if (!sftp_) {
            event.kind = FsEventKind::Error;
            event.message = "SFTP 尚未就绪";
            if (callbacks_.onEvent) callbacks_.onEvent(event);
            return;
        }
        try {
            std::vector<RemoteEntry> entries;
            for (const SftpEntry& item : sftp_->listDir(path)) {
                RemoteEntry e;
                e.name = item.name;
                e.path = item.path.empty() ? remoteJoin(path, item.name) : item.path;
                e.size = item.size;
                e.mode = item.mode;
                e.mtime = item.mtime;
                entries.push_back(std::move(e));
            }
            // 目录在前，其余按名字排序（不区分大小写）
            std::sort(entries.begin(), entries.end(),
                      [](const RemoteEntry& a, const RemoteEntry& b) {
                          if (a.isDir() != b.isDir()) return a.isDir();
                          return toLower(a.name) < toLower(b.name);
                      });
            event.entries = std::move(entries);
        } catch (const std::exception& ex) {
            event.kind = FsEventKind::Error;
            event.message = ex.what();
        }
        if (callbacks_.onEvent) callbacks_.onEvent(event);
    });
}

void RemoteFs::mkdir(const std::string& path) {
    post([this, path]() {
        FsEvent event;
        event.kind = FsEventKind::Done;
        event.path = path;
        try {
            if (!sftp_) throw Error("SFTP 尚未就绪");
            sftp_->mkdir(path, 0755);
            event.message = "已创建 " + remoteBaseName(path);
        } catch (const std::exception& ex) {
            event.kind = FsEventKind::Error;
            event.message = ex.what();
        }
        if (callbacks_.onEvent) callbacks_.onEvent(event);
    });
}

void RemoteFs::remove(const std::string& path, bool isDir) {
    post([this, path, isDir]() {
        FsEvent event;
        event.kind = FsEventKind::Done;
        event.path = path;
        try {
            if (!sftp_) throw Error("SFTP 尚未就绪");
            if (isDir)
                sftp_->rmdir(path);
            else
                sftp_->removeFile(path);
            event.message = "已删除 " + remoteBaseName(path);
        } catch (const std::exception& ex) {
            event.kind = FsEventKind::Error;
            event.message = ex.what();
        }
        if (callbacks_.onEvent) callbacks_.onEvent(event);
    });
}

void RemoteFs::rename(const std::string& from, const std::string& to) {
    post([this, from, to]() {
        FsEvent event;
        event.kind = FsEventKind::Done;
        event.path = to;
        try {
            if (!sftp_) throw Error("SFTP 尚未就绪");
            sftp_->rename(from, to);
            event.message = "已重命名为 " + remoteBaseName(to);
        } catch (const std::exception& ex) {
            event.kind = FsEventKind::Error;
            event.message = ex.what();
        }
        if (callbacks_.onEvent) callbacks_.onEvent(event);
    });
}

void RemoteFs::download(const std::string& remotePath, const std::string& localPath,
                        std::shared_ptr<std::atomic<bool>> cancel) {
    post([this, remotePath, localPath, cancel]() {
        FsEvent event;
        event.kind = FsEventKind::Done;
        event.path = remotePath;
        bool cancelled = false;
        try {
            if (!sftp_) throw Error("SFTP 尚未就绪");
            TransferOptions options;
            options.showProgress = false;
            // 每 ~100ms 把已传输字节数抛回 UI 线程，供进度条使用
            options.onProgress = [this, remotePath](std::uint64_t done, std::uint64_t total) {
                FsEvent progress;
                progress.kind = FsEventKind::Progress;
                progress.path = remotePath;
                progress.bytes = done;
                progress.total = total;
                if (callbacks_.onEvent) callbacks_.onEvent(progress);
            };
            if (cancel) {
                options.cancelled = [cancel]() { return cancel->load(); };
            }
            const TransferResult result = sftp_->download(remotePath, localPath, options);
            event.message =
                "已下载 " + remoteBaseName(remotePath) + "（" + formatBytes(result.bytes) + "）";
        } catch (const std::exception& ex) {
            // 用户取消时删掉半截文件，避免留下一个看着像成功的残片
            cancelled = cancel && cancel->load();
            if (cancelled) {
                std::error_code ec;
                fs::remove(utf8ToPath(localPath), ec);
            }
            event.kind = cancelled ? FsEventKind::Cancelled : FsEventKind::Error;
            event.message = cancelled ? ("已取消下载 " + remoteBaseName(remotePath))
                                      : std::string("下载失败：") + ex.what();
        }
        if (callbacks_.onEvent) callbacks_.onEvent(event);
    });
}

// ---------------------------------------------------------------- 拖动下载：按需读

struct RemoteFs::Reader::State {
    std::string path;
    LIBSSH2_SFTP_HANDLE* handle = nullptr;  // 只在文件线程上访问
    std::uint64_t size = 0;
    bool opened = false;
    bool exhausted = false;
    bool closed = false;
};

RemoteFs::Reader::Reader(RemoteFs& fs, std::string path)
    : fs_(fs), state_(std::make_shared<State>()) {
    state_->path = std::move(path);
}

RemoteFs::Reader::~Reader() { close(); }

std::uint64_t RemoteFs::Reader::size() const { return state_ ? state_->size : 0; }

bool RemoteFs::Reader::open(std::string* error) {
    struct Job {
        std::shared_ptr<State> state;
        std::string error;
    };
    auto job = std::make_shared<Job>();
    job->state = state_;
    RemoteFs* fs = &fs_;
    const bool done = fs->runSync([job, fs]() {
        auto& st = *job->state;
        try {
            if (!fs->sftp_) throw Error("SFTP 尚未就绪");
            std::uint64_t size = 0;
            st.handle = fs->sftp_->openReadHandle(st.path, size);
            st.size = size;
            st.opened = true;
        } catch (const std::exception& ex) {
            job->error = ex.what();
        }
    });
    if (!done) {
        if (error) *error = "等待文件通道超时";
        return false;
    }
    if (!job->error.empty()) {
        if (error) *error = job->error;
        return false;
    }
    return true;
}

std::size_t RemoteFs::Reader::read(char* buffer, std::size_t capacity, std::string* error) {
    if (error) error->clear();
    if (!state_ || !buffer || capacity == 0) return 0;
    if (state_->exhausted || state_->closed) return 0;

    struct Job {
        std::shared_ptr<State> state;
        std::vector<char> data;  // 数据先落在任务自己身上，超时返回也不会写坏调用方的缓冲区
        std::size_t got = 0;
        std::string error;
    };
    auto job = std::make_shared<Job>();
    job->state = state_;
    job->data.resize(capacity);
    RemoteFs* fs = &fs_;
    const bool done = fs->runSync([job, fs]() {
        auto& st = *job->state;
        try {
            if (!st.handle || st.closed) throw Error("远端文件已关闭");
            job->got = fs->sftp_->readHandle(st.handle, job->data.data(), job->data.size());
            if (job->got == 0) st.exhausted = true;
        } catch (const std::exception& ex) {
            job->error = ex.what();
            st.exhausted = true;
        }
    }, 30000);
    if (!done) {
        if (error) *error = "读取超时";
        return 0;
    }
    if (!job->error.empty()) {
        if (error) *error = job->error;
        return 0;
    }
    if (job->got == 0) return 0;
    memcpy(buffer, job->data.data(), job->got);
    return job->got;
}

void RemoteFs::Reader::close() {
    if (!state_) return;
    state_->closed = true;
    if (!state_->opened) return;
    state_->opened = false;
    if (!state_->handle) return;
    struct Job {
        std::shared_ptr<State> state;
    };
    auto job = std::make_shared<Job>();
    job->state = state_;
    RemoteFs* fs = &fs_;
    fs->runSync([job, fs]() {
        auto& st = *job->state;
        if (st.handle) {
            if (fs->sftp_) fs->sftp_->closeReadHandle(st.handle);
            st.handle = nullptr;
        }
    }, 5000);
}

std::shared_ptr<RemoteFs::Reader> RemoteFs::openReader(const std::string& remotePath,
                                                       std::string* error) {
    auto reader = std::shared_ptr<Reader>(new Reader(*this, remotePath));
    if (!reader->open(error)) return nullptr;
    return reader;
}

void RemoteFs::upload(const std::string& localPath, const std::string& remotePath,
                      std::shared_ptr<std::atomic<bool>> cancel) {
    post([this, localPath, remotePath, cancel]() {
        // 先报一次 Started，UI 才有东西可显示（总字节取本地文件大小，
        // 取不到就留 0，进度条退化成「上传中…」而不是一直空着）
        FsEvent started;
        started.kind = FsEventKind::Started;
        started.path = remotePath;
        started.upload = true;
        started.message = localBaseName(localPath);
        started.cancel = cancel;
        {
            std::error_code ec;
            started.total = fs::file_size(utf8ToPath(localPath), ec);
            if (ec) started.total = 0;
        }
        if (callbacks_.onEvent) callbacks_.onEvent(started);

        FsEvent event;
        event.kind = FsEventKind::Done;
        event.path = remotePath;
        event.upload = true;
        bool cancelled = false;
        // 取消时只在「目标原本不存在」的情况删掉半截文件：
        // 覆盖已有文件后再删会把用户原来的远端文件一起弄丢。
        std::string targetPath;
        bool targetExisted = false;
        try {
            if (!sftp_) throw Error("SFTP 尚未就绪");
            targetPath = sftp_->resolveRemoteTarget(remotePath, localPath);
            SftpEntry before;
            targetExisted = sftp_->stat(targetPath, before);
            TransferOptions options;
            options.showProgress = false;
            // 和下载一样按 ~100ms 把已传字节抛回 UI 线程
            options.onProgress = [this, remotePath](std::uint64_t done, std::uint64_t total) {
                FsEvent progress;
                progress.kind = FsEventKind::Progress;
                progress.path = remotePath;
                progress.upload = true;
                progress.bytes = done;
                progress.total = total;
                if (callbacks_.onEvent) callbacks_.onEvent(progress);
            };
            if (cancel) options.cancelled = [cancel]() { return cancel->load(); };
            const TransferResult result = sftp_->upload(localPath, remotePath, options);
            event.message =
                "已上传 " + localBaseName(localPath) + "（" + formatBytes(result.bytes) + "）";
        } catch (const std::exception& ex) {
            // 和下载对称：取消后把远端半截文件删掉，不然远端会留一个像样的残片
            cancelled = cancel && cancel->load();
            if (cancelled && sftp_ && !targetExisted && !targetPath.empty()) {
                try {
                    sftp_->removeFile(targetPath);
                } catch (...) {
                }
            }
            event.kind = cancelled ? FsEventKind::Cancelled : FsEventKind::Error;
            event.message = cancelled ? ("已取消上传 " + localBaseName(localPath))
                                      : std::string("上传失败：") + ex.what();
        }
        if (callbacks_.onEvent) callbacks_.onEvent(event);
    });
}

void RemoteFs::sampleNow() { statsPending_ = true; }

void RemoteFs::workerLoop() {
    // ---- 建立独立连接 ----
    try {
        // 只有建连这一小段需要明文口令：从密文里取出来，用完整即抹掉
        ConnectionOptions conn = options_;
        std::string utf8Password = password_.revealUtf8();
        conn.password = utf8Password;
        wipeString(utf8Password);
        std::string utf8KeyPassphrase = keyPassphrase_.revealUtf8();
        conn.keyPassphrase = utf8KeyPassphrase;
        wipeString(utf8KeyPassphrase);
        session_ = std::make_unique<SshSession>(conn);
        session_->connect();
    } catch (const std::exception& ex) {
        FsEvent event;
        event.kind = FsEventKind::Failed;
        event.message = ex.what();
        if (callbacks_.onEvent) callbacks_.onEvent(event);
        running_ = false;
        ready_ = false;
        session_.reset();
        return;
    }

    if (!running_.load()) return;

    // SFTP 打不开不算致命：终端还能用，只是左侧文件面板不可用
    std::string sftpError;
    std::string homePath;
    try {
        sftp_ = std::make_unique<SftpClient>(*session_);
        sftp_->open();
        sftpReady_ = true;
        try {
            homePath = sftp_->homeDir();
        } catch (...) {
            homePath = ".";
        }
    } catch (const std::exception& ex) {
        sftpError = ex.what();
        sftp_.reset();
    }

    ready_ = true;
    {
        FsEvent event;
        event.kind = FsEventKind::Ready;
        event.message = sftpError;
        event.path = homePath;
        if (callbacks_.onEvent) callbacks_.onEvent(event);
    }

    auto nextSample = std::chrono::steady_clock::now();
    bool firstSample = true;

    while (running_.load()) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(120),
                         [this]() { return !running_.load() || !tasks_.empty(); });
            if (!running_.load()) break;
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
        }
        if (task) {
            try {
                task();
            } catch (...) {
                // 任务内部已经吞掉异常，这里兜底避免线程退出
            }
            continue;
        }

        // 空闲时做一次采样
        const auto now = std::chrono::steady_clock::now();
        if (sampleInterval_ > 0 && (firstSample || statsPending_ || now >= nextSample)) {
            firstSample = false;
            statsPending_ = false;
            nextSample = now + std::chrono::seconds(sampleInterval_);
            runSample();
        }
    }

    sftp_.reset();
    if (session_) {
        session_->disconnect();
        session_.reset();
    }
    ready_ = false;
    sftpReady_ = false;
}

void RemoteFs::runSample() {
    if (!session_ || !running_.load()) return;

    std::string out;
    int latency = -1;
    const bool ok = execCapture(kSampleScript, out, 4000, &latency);

    RemoteStats stats;
    stats.latencyMs = latency;
    if (!ok) {
        stats.error = "采样失败";
        if (callbacks_.onStats) callbacks_.onStats(stats);
        return;
    }

    enum class Section { None, Host, Os, Up, Load, Cores, Cpu, Cpu2, Mem, Disk };
    Section section = Section::None;
    std::uint64_t idle1 = 0, total1 = 0, idle2 = 0, total2 = 0;

    for (const std::string& raw : splitLines(out)) {
        const std::string line = trim(raw);
        if (line.empty()) continue;
        if (line.rfind("##", 0) == 0) {
            const std::string tag = line.substr(2);
            if (tag == "HOST") section = Section::Host;
            else if (tag == "OS") section = Section::Os;
            else if (tag == "UP") section = Section::Up;
            else if (tag == "LOAD") section = Section::Load;
            else if (tag == "CORES") section = Section::Cores;
            else if (tag == "CPU") section = Section::Cpu;
            else if (tag == "CPU2") section = Section::Cpu2;
            else if (tag == "MEM") section = Section::Mem;
            else if (tag == "DISK") section = Section::Disk;
            else section = Section::None;
            continue;
        }
        switch (section) {
            case Section::Host:
                if (stats.host.empty()) stats.host = line;
                break;
            case Section::Os:
                if (stats.os.empty()) stats.os = line;
                break;
            case Section::Up:
                stats.uptimeSec = std::strtoull(line.c_str(), nullptr, 10);
                break;
            case Section::Load: {
                std::istringstream in(line);
                in >> stats.load1 >> stats.load5 >> stats.load15;
                break;
            }
            case Section::Cores:
                stats.cores = std::atoi(line.c_str());
                break;
            case Section::Cpu:
                parseCpuLine(line, idle1, total1);
                break;
            case Section::Cpu2:
                parseCpuLine(line, idle2, total2);
                break;
            case Section::Mem: {
                const auto colon = line.find(':');
                if (colon == std::string::npos) break;
                const std::string key = line.substr(0, colon);
                const std::uint64_t value = std::strtoull(line.c_str() + colon + 1, nullptr, 10);
                if (key == "MemTotal") stats.memTotalKb = value;
                else if (key == "MemAvailable") stats.memAvailKb = value;
                else if (key == "MemFree" && stats.memAvailKb == 0) stats.memAvailKb = value;
                break;
            }
            case Section::Disk: {
                std::istringstream in(line);
                std::string fsName, blocks, used, avail, capacity;
                if (in >> fsName >> blocks >> used >> avail >> capacity) {
                    stats.diskTotalKb = std::strtoull(blocks.c_str(), nullptr, 10);
                    stats.diskUsedKb = std::strtoull(used.c_str(), nullptr, 10);
                }
                break;
            }
            default:
                break;
        }
    }

    if (total2 > total1 && total1 > 0) {
        const double dTotal = static_cast<double>(total2 - total1);
        const double dIdle = static_cast<double>(idle2 - idle1);
        stats.cpuPercent = std::clamp((dTotal - dIdle) * 100.0 / dTotal, 0.0, 100.0);
    }

    stats.valid = !stats.host.empty() || stats.memTotalKb > 0 || stats.diskTotalKb > 0;
    if (callbacks_.onStats) callbacks_.onStats(stats);
}

bool RemoteFs::execCapture(const std::string& command, std::string& out, int timeoutMs,
                           int* latencyMs, int* exitCode, std::size_t maxOutputBytes,
                           bool* truncated) {
    if (!session_) return false;
    LIBSSH2_SESSION* raw = session_->raw();
    if (!raw) return false;

    if (exitCode) *exitCode = -1;
    if (truncated) *truncated = false;
    if (maxOutputBytes == 0) maxOutputBytes = static_cast<std::size_t>(-1);

    const auto started = std::chrono::steady_clock::now();

    LIBSSH2_CHANNEL* channel = libssh2_channel_open_session(raw);
    if (!channel) return false;
    if (libssh2_channel_exec(channel, command.c_str()) != 0) {
        libssh2_channel_free(channel);
        return false;
    }

    libssh2_session_set_blocking(raw, 0);
    std::vector<char> buf(16 * 1024);
    const auto deadline = started + std::chrono::milliseconds(timeoutMs);
    int spin = 0;
    bool ok = true;

    for (;;) {
        if (!running_.load()) {  // 断开时尽快退出，别把 UI 线程卡在 join 上
            ok = false;
            break;
        }
        bool active = false;
        ssize_t n = libssh2_channel_read(channel, buf.data(), buf.size());
        while (n > 0) {
            // 超出上限的部分照样读掉（否则远端会卡在写满的管道上），只是不保留
            const std::size_t chunk = static_cast<std::size_t>(n);
            const std::size_t room =
                out.size() < maxOutputBytes ? maxOutputBytes - out.size() : 0;
            if (room == 0) {
                if (truncated) *truncated = true;
            } else if (chunk > room) {
                out.append(buf.data(), room);
                if (truncated) *truncated = true;
            } else {
                out.append(buf.data(), chunk);
            }
            active = true;
            n = libssh2_channel_read(channel, buf.data(), buf.size());
        }
        if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
            ok = false;
            break;
        }
        if (libssh2_channel_eof(channel) == 1) break;
        if (std::chrono::steady_clock::now() > deadline) {
            ok = false;
            break;
        }
        if (!active) {
            std::this_thread::sleep_for(std::chrono::milliseconds(++spin % 20 == 0 ? 20 : 5));
        }
    }

    if (exitCode) {
        // 只有正常收到 EOF 时退出码才有意义；超时/中断保持 -1
        if (ok && libssh2_channel_eof(channel) == 1)
            *exitCode = libssh2_channel_get_exit_status(channel);
    }

    if (latencyMs) {
        *latencyMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 started)
                .count());
    }

    // 非阻塞收尾，避免 close 再等一轮网络往返
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    libssh2_session_set_blocking(raw, 1);
    return ok;
}

// ---------------------------------------------------------------- AI 工具的同步接口
//
// 这几个方法会从 AI 工作线程调用，而 libssh2 会话只能由文件线程触碰，
// 所以统一走 runSync。注意：runSync 超时返回后，排队的任务仍可能在文件线程里
// 继续跑，因此结果缓冲区一律用 shared_ptr 持有，不能捕获栈上变量的引用。

RemoteFs::ExecOutcome RemoteFs::execSync(const std::string& command, int timeoutMs,
                                         std::size_t maxOutputBytes) {
    ExecOutcome outcome;
    if (!running_.load()) {
        outcome.error = "远端连接尚未建立";
        return outcome;
    }

    struct State {
        std::string output;
        int exitCode = -1;
        bool truncated = false;
        bool ok = false;
        std::string error;
    };
    auto state = std::make_shared<State>();
    const int waitMs = std::max(timeoutMs, 1000) + 5000;

    const bool finished = runSync(
        [this, state, command, timeoutMs, maxOutputBytes]() {
            try {
                state->ok = execCapture(command, state->output, timeoutMs, nullptr,
                                        &state->exitCode, maxOutputBytes, &state->truncated);
                if (!state->ok) state->error = "命令执行失败或超时";
            } catch (const std::exception& ex) {
                state->ok = false;
                state->error = ex.what();
            }
        },
        waitMs);

    if (!finished) {
        outcome.error = "等待远端响应超时";
        return outcome;
    }

    outcome.ok = state->ok;
    outcome.exitCode = state->exitCode;
    outcome.output = std::move(state->output);
    outcome.truncated = state->truncated;
    outcome.error = std::move(state->error);
    return outcome;
}

bool RemoteFs::readFileSync(const std::string& path, std::size_t maxBytes, std::string& out,
                            bool& truncated, std::string& error) {
    truncated = false;
    error.clear();
    if (!running_.load()) {
        error = "远端连接尚未建立";
        return false;
    }
    if (maxBytes == 0) maxBytes = static_cast<std::size_t>(-1);

    struct State {
        std::string data;
        bool truncated = false;
        bool ok = false;
        std::string error;
    };
    auto state = std::make_shared<State>();

    const bool finished = runSync(
        [this, state, path, maxBytes]() {
            if (!sftp_) {
                state->error = "SFTP 尚未就绪";
                return;
            }
            LIBSSH2_SFTP_HANDLE* handle = nullptr;
            try {
                std::uint64_t size = 0;
                handle = sftp_->openReadHandle(path, size);
                std::vector<char> buffer(32 * 1024);
                for (;;) {
                    if (state->data.size() >= maxBytes) {
                        state->truncated = true;
                        break;
                    }
                    const std::size_t got = sftp_->readHandle(handle, buffer.data(), buffer.size());
                    if (got == 0) break;
                    const std::size_t room = maxBytes - state->data.size();
                    if (got > room) {
                        state->data.append(buffer.data(), room);
                        state->truncated = true;
                        break;
                    }
                    state->data.append(buffer.data(), got);
                }
                sftp_->closeReadHandle(handle);
                handle = nullptr;
                state->ok = true;
            } catch (const std::exception& ex) {
                if (handle) sftp_->closeReadHandle(handle);
                state->error = ex.what();
            }
        },
        60000);

    if (!finished) {
        error = "等待远端响应超时";
        return false;
    }
    if (!state->ok) {
        error = state->error.empty() ? "读取失败" : state->error;
        return false;
    }
    out = std::move(state->data);
    truncated = state->truncated;
    return true;
}

bool RemoteFs::writeFileSync(const std::string& path, const std::string& content,
                             std::string& error) {
    error.clear();
    if (!running_.load()) {
        error = "远端连接尚未建立";
        return false;
    }

    struct State {
        bool ok = false;
        std::string error;
    };
    auto state = std::make_shared<State>();

    const bool finished = runSync(
        [this, state, path, content]() {
            if (!sftp_) {
                state->error = "SFTP 尚未就绪";
                return;
            }
            try {
                sftp_->writeFileContent(path, content);
                state->ok = true;
            } catch (const std::exception& ex) {
                state->error = ex.what();
            }
        },
        60000);

    if (!finished) {
        error = "等待远端响应超时";
        return false;
    }
    if (!state->ok) {
        error = state->error.empty() ? "写入失败" : state->error;
        return false;
    }
    return true;
}

bool RemoteFs::listDirSync(const std::string& path, std::vector<RemoteEntry>& out,
                           std::string& error) {
    error.clear();
    out.clear();
    if (!running_.load()) {
        error = "远端连接尚未建立";
        return false;
    }

    struct State {
        std::vector<RemoteEntry> entries;
        bool ok = false;
        std::string error;
    };
    auto state = std::make_shared<State>();

    const bool finished = runSync(
        [this, state, path]() {
            if (!sftp_) {
                state->error = "SFTP 尚未就绪";
                return;
            }
            try {
                for (const SftpEntry& item : sftp_->listDir(path)) {
                    RemoteEntry entry;
                    entry.name = item.name;
                    entry.path = item.path.empty() ? remoteJoin(path, item.name) : item.path;
                    entry.size = item.size;
                    entry.mode = item.mode;
                    entry.mtime = item.mtime;
                    state->entries.push_back(std::move(entry));
                }
                std::sort(state->entries.begin(), state->entries.end(),
                          [](const RemoteEntry& a, const RemoteEntry& b) {
                              if (a.isDir() != b.isDir()) return a.isDir();
                              return toLower(a.name) < toLower(b.name);
                          });
                state->ok = true;
            } catch (const std::exception& ex) {
                state->error = ex.what();
            }
        },
        30000);

    if (!finished) {
        error = "等待远端响应超时";
        return false;
    }
    if (!state->ok) {
        error = state->error.empty() ? "列目录失败" : state->error;
        return false;
    }
    out = std::move(state->entries);
    return true;
}

}  // namespace rshell
