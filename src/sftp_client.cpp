// sftp_client.cpp
#include "sftp_client.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include "progress.h"
#include "util.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

namespace rshell {

namespace {

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool wouldBlock(int rc) { return rc == LIBSSH2_ERROR_EAGAIN; }

std::string sftpStatusText(unsigned long code) {
    switch (code) {
        case 0: return "成功";
        case 1: return "文件结束";
        case 2: return "文件或目录不存在";
        case 3: return "权限不足";
        case 4: return "操作失败";
        case 5: return "协议消息错误";
        case 6: return "没有连接";
        case 7: return "连接中断";
        case 8: return "服务器不支持该操作";
        default: return "SFTP 状态码 " + std::to_string(code);
    }
}

// 从 std::filesystem 权限位推导 Unix 模式
unsigned int modeFromPerms(fs::perms p) {
    unsigned int mode = 0;
    auto has = [&](fs::perms bit) { return (p & bit) != fs::perms::none; };
    if (has(fs::perms::owner_read)) mode |= 0400;
    if (has(fs::perms::owner_write)) mode |= 0200;
    if (has(fs::perms::owner_exec)) mode |= 0100;
    if (has(fs::perms::group_read)) mode |= 0040;
    if (has(fs::perms::group_write)) mode |= 0020;
    if (has(fs::perms::group_exec)) mode |= 0010;
    if (has(fs::perms::others_read)) mode |= 0004;
    if (has(fs::perms::others_write)) mode |= 0002;
    if (has(fs::perms::others_exec)) mode |= 0001;
    return mode;
}

// 从 Unix 模式位推导 std::filesystem 权限（用于下载后设置本地权限）
fs::perms permsFromMode(unsigned int mode) {
    fs::perms p = fs::perms::none;
    if (mode & 0400) p |= fs::perms::owner_read;
    if (mode & 0200) p |= fs::perms::owner_write;
    if (mode & 0100) p |= fs::perms::owner_exec;
    if (mode & 0040) p |= fs::perms::group_read;
    if (mode & 0020) p |= fs::perms::group_write;
    if (mode & 0010) p |= fs::perms::group_exec;
    if (mode & 0004) p |= fs::perms::others_read;
    if (mode & 0002) p |= fs::perms::others_write;
    if (mode & 0001) p |= fs::perms::others_exec;
    return p;
}

struct SftpHandleGuard {
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    bool keep = false;
    explicit SftpHandleGuard(LIBSSH2_SFTP_HANDLE* h) : handle(h) {}
    ~SftpHandleGuard() {
        if (handle && !keep) libssh2_sftp_close_handle(handle);
    }
    SftpHandleGuard(const SftpHandleGuard&) = delete;
    SftpHandleGuard& operator=(const SftpHandleGuard&) = delete;
};

// 传输期间切到非阻塞（配合 waitSocket），离开作用域时恢复阻塞模式
struct BlockingModeGuard {
    explicit BlockingModeGuard(LIBSSH2_SESSION* session) : session_(session) {
        libssh2_session_set_blocking(session_, 0);
    }
    ~BlockingModeGuard() { libssh2_session_set_blocking(session_, 1); }
    BlockingModeGuard(const BlockingModeGuard&) = delete;
    BlockingModeGuard& operator=(const BlockingModeGuard&) = delete;

private:
    LIBSSH2_SESSION* session_;
};

SftpEntry entryFromAttrs(const std::string& name, const std::string& fullPath,
                         const LIBSSH2_SFTP_ATTRIBUTES& attrs) {
    SftpEntry e;
    e.name = name;
    e.path = fullPath;
    e.size = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? attrs.filesize : 0;
    e.mode = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ? attrs.permissions : 0;
    e.mtime = (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? attrs.mtime : 0;
    e.uid = (attrs.flags & LIBSSH2_SFTP_ATTR_UIDGID) ? attrs.uid : 0;
    e.gid = (attrs.flags & LIBSSH2_SFTP_ATTR_UIDGID) ? attrs.gid : 0;
    return e;
}

}  // namespace

std::string SftpEntry::permissions() const { return formatMode(mode); }

// ---------------------------------------------------------------- 生命周期
SftpClient::SftpClient(SshSession& session) : session_(session) {}

SftpClient::~SftpClient() { close(); }

void SftpClient::open() {
    if (sftp_) return;
    if (!session_.raw()) throw Error("SSH 会话尚未建立，无法启动 SFTP");
    sftp_ = libssh2_sftp_init(session_.raw());
    if (!sftp_) {
        const char* msg = nullptr;
        int len = 0;
        libssh2_session_last_error(session_.raw(), const_cast<char**>(&msg), &len, 0);
        throw Error(std::string("无法启动 SFTP 子系统") +
                    (msg && len > 0 ? std::string("：") + std::string(msg, len) : ""));
    }
    libssh2_session_set_blocking(session_.raw(), 1);
}

void SftpClient::close() {
    if (!sftp_) return;
    libssh2_session_set_blocking(session_.raw(), 1);
    libssh2_sftp_shutdown(sftp_);
    sftp_ = nullptr;
}

void SftpClient::fail(const std::string& what) const {
    std::string msg = what;
    if (sftp_) msg += "：" + sftpStatusText(libssh2_sftp_last_error(sftp_));
    throw Error(msg);
}

void SftpClient::waitSocket(int timeoutMs) const {
    libssh2_socket_t sock = session_.socketFd();
    int dir = libssh2_session_block_directions(session_.raw());
    fd_set readfds;
    fd_set writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) FD_SET(sock, &readfds);
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(sock, &writefds);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
    select(0, &readfds, &writefds, nullptr, &tv);
#else
    select(sock + 1, &readfds, &writefds, nullptr, &tv);
#endif
}

// ---------------------------------------------------------------- 查询
std::string SftpClient::realPath(const std::string& path) {
    if (!sftp_) throw Error("SFTP 未打开");
    char buf[4096];
    int rc = libssh2_sftp_realpath(sftp_, path.empty() ? "." : path.c_str(), buf, sizeof(buf) - 1);
    if (rc < 0) fail("无法解析远端路径 " + path);
    buf[rc] = '\0';
    return std::string(buf);
}

std::string SftpClient::homeDir() {
    try {
        return realPath(".");
    } catch (const Error&) {
        return "/";
    }
}

bool SftpClient::stat(const std::string& path, SftpEntry& out) {
    if (!sftp_) throw Error("SFTP 未打开");
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    if (libssh2_sftp_stat(sftp_, path.c_str(), &attrs) != 0) {
        if (libssh2_sftp_last_error(sftp_) == LIBSSH2_FX_NO_SUCH_FILE) return false;
        // 其他错误（权限等）也视为“拿不到信息”
        return false;
    }
    out = entryFromAttrs(remoteBaseName(path), path, attrs);
    return true;
}

bool SftpClient::exists(const std::string& path) {
    SftpEntry entry;
    return stat(path, entry);
}

// ---------------------------------------------------------------- 目录
std::vector<SftpEntry> SftpClient::listDir(const std::string& path) {
    if (!sftp_) throw Error("SFTP 未打开");
    const std::string dir = path.empty() ? "." : path;
    LIBSSH2_SFTP_HANDLE* handle = libssh2_sftp_opendir(sftp_, dir.c_str());
    if (!handle) fail("无法打开远端目录 " + dir);
    SftpHandleGuard guard(handle);

    std::vector<SftpEntry> entries;
    std::vector<char> nameBuf(512);
    for (;;) {
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        int rc = libssh2_sftp_readdir(handle, nameBuf.data(), nameBuf.size() - 1, &attrs);
        if (rc == 0) break;
        if (rc < 0) fail("读取远端目录 " + dir + " 失败");
        std::string name(nameBuf.data(), static_cast<size_t>(rc));
        if (name == "." || name == "..") continue;
        std::string full = (dir == ".") ? name : remoteJoin(dir, name);
        entries.push_back(entryFromAttrs(name, full, attrs));
    }

    std::sort(entries.begin(), entries.end(), [](const SftpEntry& a, const SftpEntry& b) {
        if (a.isDir() != b.isDir()) return a.isDir();
        return toLower(a.name) < toLower(b.name);
    });
    return entries;
}

std::vector<SftpEntry> SftpClient::globRemote(const std::string& pattern) {
    const std::string base = remoteBaseName(pattern);
    std::string dir = remoteDirName(pattern);
    if (dir.empty()) dir = ".";

    if (!hasWildcard(base)) {
        SftpEntry entry;
        if (stat(pattern, entry)) return {entry};
        return {};
    }

    std::vector<SftpEntry> result;
    for (const SftpEntry& entry : listDir(dir)) {
        // 与 shell 一致：不以 '.' 开头的模式不匹配隐藏文件
        if (!entry.name.empty() && entry.name[0] == '.' && (base.empty() || base[0] != '.'))
            continue;
        if (globMatch(base, entry.name)) result.push_back(entry);
    }
    return result;
}

std::vector<SftpEntry> SftpClient::listTarget(const std::string& path) {
    std::string target = path.empty() ? std::string(".") : path;
    // SFTP 协议本身不认识 ~，这里在客户端展开
    if (target == "~") {
        target = homeDir();
    } else if (startsWith(target, "~/")) {
        target = remoteJoin(homeDir(), target.substr(2));
    }
    if (!hasWildcard(remoteBaseName(target))) {
        SftpEntry entry;
        if (!stat(target, entry)) throw Error("远端路径不存在：" + target);
        if (!entry.isDir()) return {entry};
        return listDir(target);
    }

    std::vector<SftpEntry> matches = globRemote(target);
    if (matches.empty()) throw Error("远端路径不存在：" + target);
    return matches;
}

// ---------------------------------------------------------------- 修改
void SftpClient::mkdir(const std::string& path, int mode, bool parents) {
    if (!sftp_) throw Error("SFTP 未打开");
    if (path.empty()) throw Error("目录名为空");

    if (!parents) {
        if (libssh2_sftp_mkdir(sftp_, path.c_str(), static_cast<long>(mode)) != 0)
            fail("创建目录 " + path + " 失败");
        return;
    }

    const bool absolute = isAbsoluteRemotePath(path);
    std::string current;
    for (const std::string& part : split(path, '/')) {
        if (part.empty() || part == ".") continue;
        current = current.empty() ? (absolute ? "/" + part : part) : current + "/" + part;
        if (libssh2_sftp_mkdir(sftp_, current.c_str(), static_cast<long>(mode)) == 0) continue;
        SftpEntry entry;
        if (stat(current, entry) && entry.isDir()) continue;  // 已存在
        fail("创建目录 " + current + " 失败");
    }
}

void SftpClient::rmdir(const std::string& path) {
    if (!sftp_) throw Error("SFTP 未打开");
    if (libssh2_sftp_rmdir(sftp_, path.c_str()) != 0) fail("删除目录 " + path + " 失败");
}

void SftpClient::removeFile(const std::string& path) {
    if (!sftp_) throw Error("SFTP 未打开");
    if (libssh2_sftp_unlink(sftp_, path.c_str()) != 0) fail("删除文件 " + path + " 失败");
}

void SftpClient::rename(const std::string& from, const std::string& to) {
    if (!sftp_) throw Error("SFTP 未打开");
    if (libssh2_sftp_rename(sftp_, from.c_str(), to.c_str()) != 0)
        fail("重命名 " + from + " -> " + to + " 失败");
}

void SftpClient::chmod(const std::string& path, unsigned int mode) {
    if (!sftp_) throw Error("SFTP 未打开");
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    attrs.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    attrs.permissions = static_cast<unsigned long>(mode & 07777);
    if (libssh2_sftp_setstat(sftp_, path.c_str(), &attrs) != 0)
        fail("修改权限 " + path + " 失败");
}

std::string SftpClient::resolveRemoteTarget(const std::string& remotePath,
                                            const std::string& localPath) {
    SftpEntry entry;
    if (!remotePath.empty() && stat(remotePath, entry) && entry.isDir()) {
        return remoteJoin(remotePath, localBaseName(localPath));
    }
    if (remotePath.empty()) return localBaseName(localPath);
    return remotePath;
}

// ---------------------------------------------------------------- 底层 IO
LIBSSH2_SFTP_HANDLE* SftpClient::openFile(const std::string& path, unsigned long flags,
                                          long mode) {
    LIBSSH2_SFTP_HANDLE* handle = libssh2_sftp_open(sftp_, path.c_str(), flags, mode);
    if (!handle) fail("无法打开远端文件 " + path);
    return handle;
}

void SftpClient::writeAll(LIBSSH2_SFTP_HANDLE* handle, const char* data, std::size_t len,
                          std::uint64_t& written) {
    std::size_t off = 0;
    while (off < len) {
        auto n = libssh2_sftp_write(handle, data + off, len - off);
        if (wouldBlock(static_cast<int>(n))) {
            waitSocket(100);
            continue;
        }
        if (n < 0) fail("写入远端文件失败");
        off += static_cast<std::size_t>(n);
        written += static_cast<std::uint64_t>(n);
    }
}

std::size_t SftpClient::readSome(LIBSSH2_SFTP_HANDLE* handle, char* buf, std::size_t cap) {
    for (;;) {
        auto n = libssh2_sftp_read(handle, buf, cap);
        if (wouldBlock(static_cast<int>(n))) {
            waitSocket(100);
            continue;
        }
        if (n < 0) fail("读取远端文件失败");
        return static_cast<std::size_t>(n);
    }
}

// ---------------------------------------------------------------- 按需顺序读（拖动下载）

LIBSSH2_SFTP_HANDLE* SftpClient::openReadHandle(const std::string& path,
                                                std::uint64_t& sizeOut) {
    if (!sftp_) throw Error("SFTP 未打开");
    SftpEntry entry;
    if (!stat(path, entry)) throw Error("远端文件不存在：" + path);
    if (entry.isDir()) throw Error("远端路径是目录，不能直接下载：" + path);
    sizeOut = entry.size;
    return openFile(path, LIBSSH2_FXF_READ, 0);
}

std::size_t SftpClient::readHandle(LIBSSH2_SFTP_HANDLE* handle, char* buffer,
                                   std::size_t capacity) {
    if (!handle) return 0;
    return readSome(handle, buffer, capacity);
}

void SftpClient::closeReadHandle(LIBSSH2_SFTP_HANDLE* handle) {
    if (handle) libssh2_sftp_close(handle);
}

void SftpClient::writeFileContent(const std::string& path, const std::string& content) {
    LIBSSH2_SFTP_HANDLE* handle = openFile(
        path, LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0644);
    std::uint64_t written = 0;
    try {
        writeAll(handle, content.data(), content.size(), written);
    } catch (...) {
        libssh2_sftp_close(handle);
        throw;
    }
    libssh2_sftp_close(handle);
}

// ---------------------------------------------------------------- 上传
TransferResult SftpClient::upload(const std::string& localPath,
                                  const std::string& remotePath,
                                  const TransferOptions& options) {
    if (!sftp_) throw Error("SFTP 未打开");

    std::error_code ec;
    fs::path local = utf8ToPath(localPath);
    if (!fs::exists(local, ec)) throw Error("本地文件不存在：" + localPath);
    if (fs::is_directory(local, ec)) throw Error("不支持直接上传目录：" + localPath);
    const std::uint64_t total = fs::is_regular_file(local, ec) ? fs::file_size(local, ec) : 0;

    const std::string target = resolveRemoteTarget(remotePath, localPath);

    SftpEntry existing;
    const bool targetExists = stat(target, existing) && existing.isFile();
    if (targetExists && !options.overwrite && !options.resume)
        throw Error("远端文件已存在：" + target + "（加 --force 覆盖）");

    std::uint64_t offset = 0;
    if (options.resume && targetExists && existing.size < total) offset = existing.size;

    std::ifstream in(local, std::ios::binary);
    if (!in) throw Error("无法读取本地文件：" + localPath);
    if (offset > 0) in.seekg(static_cast<std::streamoff>(offset));

    unsigned long flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT;
    if (offset == 0) flags |= LIBSSH2_FXF_TRUNC;
    const long mode = options.mode > 0 ? options.mode : 0644;

    SftpHandleGuard guard(openFile(target, flags, mode));
    if (offset > 0) libssh2_sftp_seek64(guard.handle, static_cast<libssh2_uint64_t>(offset));

    const double start = monotonicSeconds();
    std::uint64_t done = offset;
    double lastProgress = -1.0;
    ProgressBar bar(options.label.empty() ? "上传 " + pathToUtf8(local.filename()) + " -> " + target
                                          : options.label,
                    total, options.showProgress);

    {
        // 传输期间使用非阻塞 + select 等待，close 必须放在作用域之外（恢复阻塞模式后）
        BlockingModeGuard blocking(session_.raw());
        std::vector<char> buf(64 * 1024);
        for (;;) {
            // 取消：UI 点了「取消」就中断，由调用方删掉远端半截文件
            if (options.cancelled && options.cancelled()) throw Error("传输已取消");
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize got = in.gcount();
            if (got <= 0) break;
            writeAll(guard.handle, buf.data(), static_cast<std::size_t>(got), done);
            bar.update(done);
            // 通知 UI：按 ~100ms 限流，避免每 64KB 都跨线程发一次
            if (options.onProgress) {
                const double now = monotonicSeconds();
                if (lastProgress < 0.0 || now - lastProgress >= 0.1 ||
                    (total > 0 && done >= total)) {
                    lastProgress = now;
                    options.onProgress(done, total);
                }
            }
        }

        if (in.bad()) throw Error("读取本地文件失败：" + localPath);
    }

    guard.keep = true;
    int closeRc = libssh2_sftp_close_handle(guard.handle);
    if (closeRc != 0) fail("关闭远端文件 " + target + " 失败");
    bar.finish(done, true);

    if (options.mode > 0) {
        chmod(target, static_cast<unsigned int>(options.mode));
    } else if (options.preserveMode) {
        unsigned int localMode = modeFromPerms(fs::status(local, ec).permissions());
        if (localMode != 0) chmod(target, localMode);
    }

    TransferResult result;
    result.bytes = done - offset;
    result.seconds = monotonicSeconds() - start;
    result.rate = result.seconds > 0 ? static_cast<double>(result.bytes) / result.seconds : 0.0;
    return result;
}

// ---------------------------------------------------------------- 下载
TransferResult SftpClient::download(const std::string& remotePath,
                                    const std::string& localPath,
                                    const TransferOptions& options) {
    if (!sftp_) throw Error("SFTP 未打开");

    SftpEntry entry;
    if (!stat(remotePath, entry)) throw Error("远端文件不存在：" + remotePath);
    if (entry.isDir()) throw Error("远端路径是目录，暂不支持递归下载：" + remotePath);

    std::error_code ec;
    fs::path local = utf8ToPath(localPath);
    if (fs::is_directory(local, ec)) local /= utf8ToPath(remoteBaseName(remotePath));
    const bool localExists = fs::exists(local, ec);
    if (localExists && !options.overwrite && !options.resume)
        throw Error("本地文件已存在：" + pathToUtf8(local) + "（加 --force 覆盖）");

    std::uint64_t offset = 0;
    if (options.resume && localExists && fs::is_regular_file(local, ec)) {
        std::uint64_t have = fs::file_size(local, ec);
        if (have < entry.size) offset = have;
    }

    if (local.has_parent_path()) fs::create_directories(local.parent_path(), ec);

    std::ios::openmode mode = std::ios::binary | std::ios::out;
    mode |= (offset > 0) ? std::ios::app : std::ios::trunc;
    std::ofstream out(local, mode);
    if (!out) throw Error("无法写入本地文件：" + pathToUtf8(local));

    SftpHandleGuard guard(openFile(remotePath, LIBSSH2_FXF_READ, 0));
    if (offset > 0) libssh2_sftp_seek64(guard.handle, static_cast<libssh2_uint64_t>(offset));

    const double start = monotonicSeconds();
    std::uint64_t done = offset;
    double lastProgress = -1.0;
    ProgressBar bar(options.label.empty()
                        ? "下载 " + remotePath + " -> " + pathToUtf8(local)
                        : options.label,
                    entry.size, options.showProgress);

    {
        // 传输期间使用非阻塞 + select 等待，close 必须放在作用域之外（恢复阻塞模式后）
        BlockingModeGuard blocking(session_.raw());
        std::vector<char> buf(64 * 1024);
        for (;;) {
            // 取消：UI 点了「取消」就中断，由调用方决定残留文件怎么处理
            if (options.cancelled && options.cancelled()) throw Error("传输已取消");
            std::size_t got = readSome(guard.handle, buf.data(), buf.size());
            if (got == 0) break;
            out.write(buf.data(), static_cast<std::streamsize>(got));
            if (!out) throw Error("写入本地文件失败：" + pathToUtf8(local));
            done += got;
            bar.update(done);
            // 通知 UI：按 ~100ms 限流，避免每 64KB 都跨线程发一次
            if (options.onProgress) {
                const double now = monotonicSeconds();
                if (lastProgress < 0.0 || now - lastProgress >= 0.1 || done >= entry.size) {
                    lastProgress = now;
                    options.onProgress(done, entry.size);
                }
            }
        }
        out.flush();
        if (!out) throw Error("写入本地文件失败：" + pathToUtf8(local));
    }

    guard.keep = true;
    int closeRc = libssh2_sftp_close_handle(guard.handle);
    if (closeRc != 0) fail("关闭远端文件 " + remotePath + " 失败");
    bar.finish(done, true);

    if (options.mode > 0) {
        fs::permissions(local, permsFromMode(static_cast<unsigned int>(options.mode)),
                        fs::perm_options::replace, ec);
        if (ec) log().warn("设置本地文件权限失败：" + ec.message());
    }

    TransferResult result;
    result.bytes = done - offset;
    result.seconds = monotonicSeconds() - start;
    result.rate = result.seconds > 0 ? static_cast<double>(result.bytes) / result.seconds : 0.0;
    return result;
}

}  // namespace rshell
