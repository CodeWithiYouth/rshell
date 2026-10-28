// remote_fs.h - GUI 用的远端文件系统 + 机器状态采样
//
// 为什么单独开一条 SSH 连接：
//   SshShell 的读线程靠 8~20ms 轮询通道，SFTP 的大目录列举/传输会长时间占用
//   libssh2 会话，如果共用一条连接，终端会出现明显的输入/输出卡顿。这里所有
//   libssh2 调用都跑在自己的工作线程上，终端完全不受影响。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <libssh2_sftp.h>

#include "protected_secret.h"
#include "ssh_session.h"

namespace rshell {

class SftpClient;

// 列目录得到的一项
struct RemoteEntry {
    std::string name;
    std::string path;  // 完整路径
    std::uint64_t size = 0;
    unsigned int mode = 0;
    std::int64_t mtime = 0;

    bool isDir() const;
    bool isLink() const;
    bool isFile() const;
};

// 底部状态栏展示的远端指标（Linux 系数据最全，其它系统可能只有部分）
struct RemoteStats {
    bool valid = false;
    std::string host;
    std::string os;
    int cores = 0;
    double load1 = 0.0;
    double load5 = 0.0;
    double load15 = 0.0;
    double cpuPercent = -1.0;  // 负值表示这次算不出来
    std::uint64_t memTotalKb = 0;
    std::uint64_t memAvailKb = 0;
    std::uint64_t diskTotalKb = 0;
    std::uint64_t diskUsedKb = 0;
    std::uint64_t uptimeSec = 0;
    int latencyMs = -1;  // 采样命令的往返耗时
    std::string error;
};

enum class FsEventKind {
    Ready,     // 独立连接已就绪（canWrite 表示 SFTP 可用）
    Failed,    // 连接失败，message 是原因
    Listing,   // 列目录结果
    Started,   // 一项传输开始：path 是远端路径，total 是总字节，message 是文件名
    Progress,  // 传输进度：path 是远端路径，bytes/total 是已传输/总字节
    Done,      // 单个操作成功，message 是给人看的说明
    Cancelled, // 下载被用户取消
    Error,     // 操作失败，message 是原因
};

struct FsEvent {
    FsEventKind kind = FsEventKind::Error;
    std::string path;
    std::string message;
    std::uint64_t bytes = 0;  // Progress：已传输字节
    std::uint64_t total = 0;  // Started/Progress：总字节（0 表示未知）
    // 方向：true 表示上传。上传和下载可能并发（同一远端路径也有可能出现），
    // UI 侧按 path + 方向两项匹配任务，避免进度写到别的条目上。
    bool upload = false;
    // Started：拖动下载这一项的取消开关（资源管理器的复制线程是异步的，
    // 只能让开关跟着事件过来，UI 侧没法按路径回头去找）
    std::shared_ptr<std::atomic<bool>> cancel;
    std::vector<RemoteEntry> entries;
};

struct RemoteFsCallbacks {
    // 都在工作线程触发，GUI 侧自行 PostMessage 回 UI 线程
    std::function<void(const FsEvent&)> onEvent;
    std::function<void(const RemoteStats&)> onStats;
};

class RemoteFs {
public:
    RemoteFs();
    ~RemoteFs();

    RemoteFs(const RemoteFs&) = delete;
    RemoteFs& operator=(const RemoteFs&) = delete;

    void setCallbacks(RemoteFsCallbacks callbacks);
    // 采样周期（秒），0 表示不采样
    void setSampleInterval(int seconds) { sampleInterval_ = seconds; }

    // 建立独立连接；之前若在运行会先停掉
    void start(const ConnectionOptions& options);
    void stop();

    bool running() const { return running_.load(); }
    bool ready() const { return ready_.load(); }
    bool sftpReady() const { return sftpReady_.load(); }

    // 以下都是异步入队，结果通过 onEvent 返回
    void listDir(const std::string& path);
    void mkdir(const std::string& path);
    void remove(const std::string& path, bool isDir);
    void rename(const std::string& from, const std::string& to);
    // cancel 非空时，传输会在每块数据前检查它，置 true 即中断并删掉半截本地文件
    void download(const std::string& remotePath, const std::string& localPath,
                  std::shared_ptr<std::atomic<bool>> cancel = nullptr);
    void upload(const std::string& localPath, const std::string& remotePath,
                std::shared_ptr<std::atomic<bool>> cancel = nullptr);
    void sampleNow();

    // ---- 拖动下载用的按需读取 ----
    // 拖动时不在本地落临时文件：资源管理器在目的目录建好文件后按块向 IStream 要数据，
    // 这里把每次「读一块」调度到文件线程执行（libssh2 会话只能有一个线程碰）。
    class Reader {
    public:
        ~Reader();
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;

        // 顺序读：返回读到的字节数，0 表示读完或出错（出错时 error 非空）
        std::size_t read(char* buffer, std::size_t capacity, std::string* error);
        // 提前关掉远端句柄（取消时用）
        void close();
        std::uint64_t size() const;

    private:
        friend class RemoteFs;
        // 句柄等可变状态单独放在堆上：读取任务排进队列后即使调用方超时返回、
        // Reader 被销毁，排队的任务仍然只碰这块共享状态，不会踩到悬空对象。
        struct State;
        Reader(RemoteFs& fs, std::string path);
        bool open(std::string* error);

        RemoteFs& fs_;
        std::shared_ptr<State> state_;
    };

    // 打开一个远端文件准备按需读；失败返回 nullptr 并写入 error
    std::shared_ptr<Reader> openReader(const std::string& remotePath, std::string* error);

    // ---- AI 工具用的同步接口 ----
    // 全部排进文件线程执行并等待结果；失败时 error 非空。
    // 这些方法可能从 AI 工作线程调用，与面板操作共用同一条连接。
    struct ExecOutcome {
        bool ok = false;  // 命令是否跑完（不代表退出码为 0）
        int exitCode = -1;
        std::string output;
        bool truncated = false;
        std::string error;
    };
    ExecOutcome execSync(const std::string& command, int timeoutMs, std::size_t maxOutputBytes);
    bool readFileSync(const std::string& path, std::size_t maxBytes, std::string& out,
                      bool& truncated, std::string& error);
    bool writeFileSync(const std::string& path, const std::string& content, std::string& error);
    bool listDirSync(const std::string& path, std::vector<RemoteEntry>& out, std::string& error);

private:
    using Task = std::function<void()>;

    void post(Task task);
    // 在文件线程上执行并等待结果（供外部线程同步读取）；超时返回 false
    bool runSync(Task task, int timeoutMs = 20000);
    void workerLoop();
    void runSample();
    // 在自有连接上执行命令并抓取 stdout（不碰进程的 stdout）
    bool execCapture(const std::string& command, std::string& out, int timeoutMs, int* latencyMs,
                     int* exitCode = nullptr, std::size_t maxOutputBytes = 0,
                     bool* truncated = nullptr);

    RemoteFsCallbacks callbacks_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;

    std::unique_ptr<SshSession> session_;
    std::unique_ptr<SftpClient> sftp_;
    ConnectionOptions options_;   // 非敏感字段（host/port/user/keyPath/timeout）
    ProtectedSecret password_;    // 登录密码只以密文形式留在这里
    ProtectedSecret keyPassphrase_;  // 私钥口令同理，建连时才 reveal

    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> sftpReady_{false};
    std::atomic<bool> statsPending_{false};
    int sampleInterval_ = 5;
};

}  // namespace rshell
