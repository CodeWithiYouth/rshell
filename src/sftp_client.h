// sftp_client.h - 基于 libssh2_sftp 的远端文件操作
#pragma once

#include <libssh2_sftp.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ssh_session.h"

namespace rshell {

// 远端目录项
struct SftpEntry {
    std::string name;
    std::string path;  // 完整路径（由 listDir 填充）
    std::uint64_t size = 0;
    unsigned int mode = 0;
    std::int64_t mtime = 0;
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;

    bool isDir() const { return (mode & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR; }
    bool isFile() const { return (mode & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFREG; }
    bool isSymlink() const { return (mode & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFLNK; }
    std::string permissions() const;  // -rw-r--r--
};

struct TransferOptions {
    bool overwrite = true;    // 目标已存在时是否覆盖
    bool resume = false;      // 断点续传（按字节偏移追加）
    bool preserveMode = false;  // 上传后把本地权限位应用到远端文件
    bool showProgress = true;
    int mode = -1;            // 上传后强制 chmod（八进制），-1 表示不改
    std::string label;        // 进度条左侧文字，空则自动生成
    // 进度回调：done 是本次已传输的累计字节（续传时含起始偏移），total 是文件总字节。
    // 在传输线程上调用，实现方自行切换到 UI 线程；内部已按 ~100ms 限流。
    std::function<void(std::uint64_t done, std::uint64_t total)> onProgress;
    // 取消询问：每个数据块前调用一次，返回 true 就抛 Error("传输已取消") 中断传输。
    // 在传输线程上调用，实现方只能用原子变量之类的无锁状态。
    std::function<bool()> cancelled;
};

struct TransferResult {
    std::uint64_t bytes = 0;
    double seconds = 0.0;
    double rate = 0.0;  // 字节/秒
};

// 一个 SshSession 上只能有有限个 SFTP 子系统；本类自行管理生命周期
class SftpClient {
public:
    explicit SftpClient(SshSession& session);
    ~SftpClient();

    SftpClient(const SftpClient&) = delete;
    SftpClient& operator=(const SftpClient&) = delete;

    void open();
    void close();
    bool isOpen() const { return sftp_ != nullptr; }

    // 远端 home 目录（realpath(".")）
    std::string homeDir();
    // 列表；path 为空时用 "."
    std::vector<SftpEntry> listDir(const std::string& path);
    // 读取单个条目；不存在返回 false
    bool stat(const std::string& path, SftpEntry& out);
    bool exists(const std::string& path);
    std::string realPath(const std::string& path);

    void mkdir(const std::string& path, int mode = 0755, bool parents = false);
    void rmdir(const std::string& path);
    void removeFile(const std::string& path);
    void rename(const std::string& from, const std::string& to);
    void chmod(const std::string& path, unsigned int mode);

    // 以内存内容整体覆盖远端文件（不存在则创建）。给 AI 的 write_file 工具用，
    // 不经过本地临时文件，也不做断点续传。
    void writeFileContent(const std::string& path, const std::string& content);

    TransferResult upload(const std::string& localPath, const std::string& remotePath,
                          const TransferOptions& options = TransferOptions());
    TransferResult download(const std::string& remotePath, const std::string& localPath,
                            const TransferOptions& options = TransferOptions());

    // ---- 按需顺序读（拖动下载用）----
    // 拖动时不在本地落临时文件：资源管理器在目标目录建好文件后，由它按块向 IStream 要数据。
    // 下面三个函数把「打开/读/关」拆开，句柄归调用方持有，调用方必须保证同一时刻只有一个线程
    // 使用它（RemoteFs::Reader 会把所有调用调度到自己的工作线程上）。
    LIBSSH2_SFTP_HANDLE* openReadHandle(const std::string& path, std::uint64_t& sizeOut);
    std::size_t readHandle(LIBSSH2_SFTP_HANDLE* handle, char* buffer, std::size_t capacity);
    void closeReadHandle(LIBSSH2_SFTP_HANDLE* handle);

    // 如果 path 是目录，返回其中的条目（递归展开由调用方决定）
    std::vector<SftpEntry> globRemote(const std::string& pattern);

    // 列目录语义：目录返回其内容，普通文件返回自身；不存在返回空
    std::vector<SftpEntry> listTarget(const std::string& path);

    // 简单判断：远端路径是目录，则在其中拼接本地文件名
    std::string resolveRemoteTarget(const std::string& remotePath, const std::string& localPath);

    LIBSSH2_SFTP* raw() const { return sftp_; }

private:
    void waitSocket(int timeoutMs) const;
    [[noreturn]] void fail(const std::string& what) const;
    LIBSSH2_SFTP_HANDLE* openFile(const std::string& path, unsigned long flags, long mode);
    void writeAll(LIBSSH2_SFTP_HANDLE* handle, const char* data, std::size_t len,
                  std::uint64_t& written);
    std::size_t readSome(LIBSSH2_SFTP_HANDLE* handle, char* buf, std::size_t cap);

    SshSession& session_;
    LIBSSH2_SFTP* sftp_ = nullptr;
};

}  // namespace rshell
