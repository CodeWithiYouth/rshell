// ssh_session.cpp
#include "ssh_session.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include "credential_store.h"
#include "util.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace rshell {

namespace {

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void closeSocket(libssh2_socket_t s) {
    if (s == LIBSSH2_INVALID_SOCKET) return;
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

void setNonBlocking(libssh2_socket_t s, bool nonBlocking) {
#ifdef _WIN32
    u_long mode = nonBlocking ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return;
    if (nonBlocking)
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
    else
        fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
#endif
}

void setSocketTimeout(libssh2_socket_t s, int seconds) {
#ifdef _WIN32
    DWORD ms = static_cast<DWORD>(seconds) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv{};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool wouldBlock() {
#ifdef _WIN32
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEINVAL;
#else
    return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

std::string lastSocketError() {
#ifdef _WIN32
    int err = WSAGetLastError();
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(err), 0, reinterpret_cast<char*>(&msg), 0,
                   nullptr);
    std::string out = msg ? trim(msg) : ("socket error " + std::to_string(err));
    if (msg) LocalFree(msg);
    return out;
#else
    return std::strerror(errno);
#endif
}

// 把 hostkey 类型映射到 known_hosts 的 key 类型掩码
int knownHostKeyType(int hostKeyType) {
    switch (hostKeyType) {
        case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
        case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
        case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
        default: return 0;
    }
}

const char* hostKeyTypeName(int hostKeyType) {
    switch (hostKeyType) {
        case LIBSSH2_HOSTKEY_TYPE_RSA: return "RSA";
        case LIBSSH2_HOSTKEY_TYPE_DSS: return "DSA";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return "ECDSA-256";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return "ECDSA-384";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ECDSA-521";
        case LIBSSH2_HOSTKEY_TYPE_ED25519: return "Ed25519";
        default: return "unknown";
    }
}

// 打开一个通道并保证异常安全地释放
struct ChannelGuard {
    LIBSSH2_CHANNEL* channel = nullptr;
    explicit ChannelGuard(LIBSSH2_CHANNEL* ch) : channel(ch) {}
    ~ChannelGuard() {
        if (channel) libssh2_channel_free(channel);
    }
    ChannelGuard(const ChannelGuard&) = delete;
    ChannelGuard& operator=(const ChannelGuard&) = delete;
};

}  // namespace

// ---------------------------------------------------------------- runtime
Libssh2Runtime::Libssh2Runtime() {
#ifdef _WIN32
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    if (libssh2_init(0) != 0) throw Error("libssh2 初始化失败");
}

Libssh2Runtime::~Libssh2Runtime() {
    libssh2_exit();
#ifdef _WIN32
    WSACleanup();
#endif
}

// ---------------------------------------------------------------- 构造/析构
SshSession::SshSession(ConnectionOptions options) : options_(std::move(options)) {}

void ConnectionOptions::wipeSecrets() {
    wipeString(password);
    wipeString(keyPassphrase);
}

SshSession::~SshSession() { disconnect(); }

void SshSession::disconnect() {
    if (session_) {
        libssh2_session_disconnect(session_, "rshell 主动断开");
        libssh2_session_free(session_);
        session_ = nullptr;
    }
    if (sock_ != LIBSSH2_INVALID_SOCKET) {
        closeSocket(sock_);
        sock_ = LIBSSH2_INVALID_SOCKET;
    }
    authenticated_ = false;
}

std::string SshSession::sshErrorText(int code) const {
    const char* msg = nullptr;
    if (session_) {
        int len = 0;
        libssh2_session_last_error(session_, const_cast<char**>(&msg), &len, 0);
        if (msg && len > 0) return std::string(msg, static_cast<size_t>(len));
    }
    switch (code) {
        case LIBSSH2_ERROR_AUTHENTICATION_FAILED: return "认证失败";
        case LIBSSH2_ERROR_BAD_USE: return "接口调用顺序错误";
        case LIBSSH2_ERROR_CHANNEL_FAILURE: return "通道请求被拒绝";
        case LIBSSH2_ERROR_HOSTKEY_INIT: return "主机密钥初始化失败";
        case LIBSSH2_ERROR_SOCKET_TIMEOUT: return "网络超时";
        case LIBSSH2_ERROR_SOCKET_DISCONNECT: return "连接被断开";
        case LIBSSH2_ERROR_TIMEOUT: return "操作超时";
        case LIBSSH2_ERROR_FILE: return "文件/密钥读取失败";
        default: return "libssh2 错误码 " + std::to_string(code);
    }
}

std::string SshSession::lastErrorText() const { return sshErrorText(0); }

void SshSession::fail(const std::string& what) const {
    std::string msg = what;
    std::string detail = lastErrorText();
    if (!detail.empty()) msg += "：" + detail;
    throw Error(msg);
}

// ---------------------------------------------------------------- TCP
void SshSession::openTcp() {
    // 配了代理就先经代理拿到一个已连接的 socket，后面握手逻辑完全不变
    if (options_.proxy.enabled()) {
        std::string error;
        const std::intptr_t raw = connectThroughProxy(options_.proxy, options_.host, options_.port,
                                                      options_.timeoutSec, error);
        if (raw < 0) {
            throw Error("通过代理连接 " + options_.host + ":" + std::to_string(options_.port) +
                        " 失败：" + error);
        }
        sock_ = static_cast<libssh2_socket_t>(raw);
        log().debug("经代理 " + proxyToUrl(options_.proxy) + " 连接 " + options_.host + ":" +
                    std::to_string(options_.port));
        return;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string portStr = std::to_string(options_.port);
    addrinfo* result = nullptr;
    int rc = getaddrinfo(options_.host.c_str(), portStr.c_str(), &hints, &result);
    if (rc != 0) {
        throw Error("无法解析主机名 " + options_.host + "：" +
#ifdef _WIN32
                    std::to_string(rc)
#else
                    std::string(gai_strerror(rc))
#endif
        );
    }

    std::string lastError = "没有可用地址";
    libssh2_socket_t connected = LIBSSH2_INVALID_SOCKET;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        libssh2_socket_t s = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == LIBSSH2_INVALID_SOCKET) {
            lastError = lastSocketError();
            continue;
        }
        setNonBlocking(s, true);
        int cr = ::connect(s, it->ai_addr, static_cast<int>(it->ai_addrlen));
        if (cr != 0 && !wouldBlock()) {
            lastError = lastSocketError();
            closeSocket(s);
            continue;
        }
        if (cr != 0) {
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(s, &writeSet);
            fd_set errSet;
            FD_ZERO(&errSet);
            FD_SET(s, &errSet);
            timeval tv{};
            tv.tv_sec = options_.timeoutSec;
            tv.tv_usec = 0;
#ifdef _WIN32
            int sel = select(0, nullptr, &writeSet, &errSet, &tv);
#else
            int sel = select(s + 1, nullptr, &writeSet, &errSet, &tv);
#endif
            if (sel <= 0) {
                lastError = (sel == 0) ? "连接超时" : lastSocketError();
                closeSocket(s);
                continue;
            }
            int soError = 0;
#ifdef _WIN32
            int soLen = sizeof(soError);
#else
            socklen_t soLen = sizeof(soError);
#endif
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soLen);
            if (soError != 0) {
                lastError = std::string("连接失败：") + std::strerror(soError);
                closeSocket(s);
                continue;
            }
        }
        setNonBlocking(s, false);
        setSocketTimeout(s, options_.timeoutSec);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        connected = s;
        break;
    }
    freeaddrinfo(result);

    if (connected == LIBSSH2_INVALID_SOCKET) {
        throw Error("连接 " + options_.host + ":" + portStr + " 失败：" + lastError);
    }
    sock_ = connected;
    log().debug("TCP 已连接 " + options_.host + ":" + portStr);
}

// ---------------------------------------------------------------- 握手
void SshSession::connect() {
    if (session_) throw Error("会话已连接，请勿重复 connect()");
    guardEmptyHost();
    openTcp();

    session_ = libssh2_session_init();
    if (!session_) throw Error("无法创建 libssh2 会话（内存不足？）");
    libssh2_session_set_blocking(session_, 1);
    libssh2_session_set_timeout(session_, options_.timeoutSec * 1000);

    int rc = libssh2_session_handshake(session_, sock_);
    if (rc != 0) {
        std::string message = "SSH 握手失败：" + sshErrorText(rc);
        if (rc == LIBSSH2_ERROR_KEX_FAILURE) {
            // 算法没有交集时 libssh2 只给一句 "Unable to exchange encryption keys"，
            // 不知道的人会误以为是私钥或密码问题。
            message +=
                "\n服务器与客户端没有共同的密钥交换/加密/主机密钥算法。WinCNG 的 Win7 兼容构建只支持 "
                "diffie-hellman-*，如果服务器只开放 curve25519/ECDH（新版 OpenSSH 加固配置常见），需要：\n"
                "  - Windows 10/11：用 -DRSHELL_WINDOWS7_COMPAT=OFF 重新构建（默认启用 CNG ECDH）；\n"
                "  - 或让服务器在 sshd_config 的 KexAlgorithms 中放开 diffie-hellman-group-exchange-sha256。";
        }
        throw Error(message);
    }

    if (const char* banner = libssh2_session_banner_get(session_)) banner_ = trim(banner);
    if (const char* kex = libssh2_session_methods(session_, LIBSSH2_METHOD_KEX)) kex_ = kex;
    if (const char* hk = libssh2_session_methods(session_, LIBSSH2_METHOD_HOSTKEY)) hostKeyAlgo_ = hk;

    if (const char* hash =
            libssh2_hostkey_hash(session_, LIBSSH2_HOSTKEY_HASH_SHA256)) {
        std::string b64 = base64Encode(reinterpret_cast<const unsigned char*>(hash), 32);
        while (!b64.empty() && b64.back() == '=') b64.pop_back();
        fingerprint_ = "SHA256:" + b64;
    }

    log().debug("协商完成：kex=" + kex_ + " hostkey=" + hostKeyAlgo_);
    if (!fingerprint_.empty()) log().debug("主机指纹 " + fingerprint_);

    verifyHostKey();
    authenticate();
}

void SshSession::guardEmptyHost() const {
    if (options_.host.empty()) throw Error("主机地址为空");
    if (options_.port <= 0 || options_.port > 65535) throw Error("端口号无效");
}

// ---------------------------------------------------------------- known_hosts
void SshSession::verifyHostKey() {
    if (!options_.verifyHostKey) {
        log().warn("已禁用主机密钥校验（--no-hostkey-check），连接可能被中间人劫持");
        return;
    }

    size_t keyLen = 0;
    int keyType = 0;
    const char* key = libssh2_session_hostkey(session_, &keyLen, &keyType);
    if (!key || keyLen == 0) fail("无法获取服务器主机密钥");

    const int khType = knownHostKeyType(keyType);
    if (khType == 0) {
        log().warn("无法识别的主机密钥类型，跳过 known_hosts 校验");
        return;
    }

    const std::string hostField = options_.port == 22
                                      ? options_.host
                                      : "[" + options_.host + "]:" + std::to_string(options_.port);

    LIBSSH2_KNOWNHOSTS* knownHosts = libssh2_knownhost_init(session_);
    if (!knownHosts) fail("无法初始化 known_hosts 集合");

    const std::string khPath = homeDirectory() + "/.ssh/known_hosts";
    std::error_code ec;
    fs::create_directories(utf8ToPath(homeDirectory() + "/.ssh"), ec);
    int readRc = libssh2_knownhost_readfile(knownHosts, khPath.c_str(),
                                            LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    if (readRc < 0 && readRc != LIBSSH2_ERROR_FILE) {
        log().debug("读取 known_hosts 失败：" + sshErrorText(readRc));
    }

    struct libssh2_knownhost* entry = nullptr;
    int check = libssh2_knownhost_checkp(
        knownHosts, options_.host.c_str(), options_.port, key, keyLen,
        LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | khType, &entry);

    if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
        log().debug("主机密钥已匹配 known_hosts");
        libssh2_knownhost_free(knownHosts);
        return;
    }

    if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
        libssh2_knownhost_free(knownHosts);
        throw Error(
            "主机密钥与 known_hosts 中的记录不一致！\n"
            "  主机: " + hostField + "\n"
            "  指纹: " + (fingerprint_.empty() ? "未知" : fingerprint_) + "\n"
            "可能正在遭受中间人攻击。若确认服务器密钥已更换，请先删除 known_hosts 中对应条目。");
    }

    if (check == LIBSSH2_KNOWNHOST_CHECK_FAILURE) {
        libssh2_knownhost_free(knownHosts);
        log().warn("known_hosts 校验出错，无法确认服务器身份");
        return;
    }

    // NOTFOUND：默认拒绝，交互式确认后才写入 known_hosts
    log().warn("该主机尚未记录在 known_hosts 中");
    log().warn("  " + hostField + " 指纹 " + fingerprint_ + "（" +
               hostKeyTypeName(keyType) + "）");
    if (options_.strictHostKey) {
        libssh2_knownhost_free(knownHosts);
        throw Error("未知主机密钥被拒绝（严格模式）。"
                    "可先用 ssh 登录一次以信任该主机，或在校验指纹后加到 known_hosts。");
    }

    bool trusted = false;
    if (options_.confirmHostKey) {
        trusted = options_.confirmHostKey(hostField, fingerprint_, hostKeyTypeName(keyType));
    } else if (stdinIsTty()) {
        std::string answer =
            toLower(trim(readLine("是否信任该主机并写入 known_hosts? (yes/no) ")));
        trusted = (answer == "yes" || answer == "y");
    }
    if (!trusted) {
        libssh2_knownhost_free(knownHosts);
        throw Error("未信任的主机密钥，连接已取消。"
                    "可先用 ssh 登录一次以信任该主机，或在校验指纹后加到 known_hosts。");
    }

    int addRc = libssh2_knownhost_addc(knownHosts, hostField.c_str(), nullptr, key, keyLen,
                                      nullptr, 0,
                                      LIBSSH2_KNOWNHOST_TYPE_PLAIN |
                                          LIBSSH2_KNOWNHOST_KEYENC_RAW | khType,
                                      nullptr);
    if (addRc != 0) {
        std::string detail = sshErrorText(addRc);
        libssh2_knownhost_free(knownHosts);
        log().warn("写入 known_hosts 内存表失败：" + detail);
        return;
    }
    int writeRc = libssh2_knownhost_writefile(knownHosts, khPath.c_str(),
                                              LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    if (writeRc != 0) {
        std::string detail = sshErrorText(writeRc);
        libssh2_knownhost_free(knownHosts);
        log().warn("保存 known_hosts 失败：" + detail);
        return;
    }
    libssh2_knownhost_free(knownHosts);
    log().info("已记录主机密钥到 " + khPath);
}

// ---------------------------------------------------------------- 认证
namespace {

// 私钥算法提示：WinCNG 后端（Windows 默认）不支持 Ed25519，
// 否则用户只会看到含糊的“私钥认证失败”。
// 判断是否为 OpenSSH 新格式私钥（PEM 头部不同，部分 libssh2 后端无法解析）
bool isOpenSshNewFormat(const std::string& keyPath) {
    std::ifstream in(utf8ToPath(keyPath), std::ios::binary);
    if (!in) return false;
    std::string line;
    std::getline(in, line);
    return line.find("BEGIN OPENSSH PRIVATE KEY") != std::string::npos;
}

std::string keyAlgorithmHint(const std::string& keyPath) {
    std::string algorithm;
    std::ifstream pub(utf8ToPath(keyPath + ".pub"), std::ios::binary);
    if (pub) {
        std::string line;
        std::getline(pub, line);
        line = trim(line);
        size_t space = line.find(' ');
        algorithm = (space == std::string::npos) ? line : line.substr(0, space);
    }
    if (algorithm.empty() &&
        toLower(remoteBaseName(keyPath)).find("ed25519") != std::string::npos) {
        algorithm = "ssh-ed25519";
    }
    if (toLower(algorithm).find("ed25519") != std::string::npos) {
        return "私钥为 Ed25519 算法：当前 Windows 构建已支持。若认证失败，请确认私钥与服务器 "
               "authorized_keys 匹配；带口令时连接前会提示输入。";
    }
    std::ifstream keyFile(utf8ToPath(keyPath), std::ios::binary);
    std::string firstLine;
    if (keyFile) std::getline(keyFile, firstLine);
    if (firstLine.find("BEGIN EC PRIVATE KEY") != std::string::npos) {
        return "ECDSA 私钥为 SEC1 PEM 格式：modern（Windows 10+）构建支持；"
               "Win7 兼容构建不支持 ECDSA。";
    }
    if (isOpenSshNewFormat(keyPath)) {
        return "私钥为 OpenSSH 新格式：当前构建支持 RSA/ECDSA/Ed25519。若认证失败，"
               "请检查是否带口令（GUI 无口令输入框），以及 ECDSA 是否在 Win7 兼容构建上使用。";
    }
    return std::string();
}

// 加密私钥在没有口令时，libssh2 返回的是 PUBLICKEY_UNVERIFIED
//（"Callback returned error"）而不是 DECRYPT，所以要先从文件头判断，
// 交互式连接才能在正确的位置提示输入口令。
// OpenSSH 新格式的加密标记在 base64 内的 ciphername 里，需要解出来看。
int base64CharValue(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<unsigned char> decodeBase64Text(const std::string& text) {
    std::vector<unsigned char> out;
    int buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        const int value = base64CharValue(c);
        if (value < 0) return {};
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

bool isEncryptedPrivateKey(const std::string& keyPath) {
    std::ifstream in(utf8ToPath(keyPath), std::ios::binary);
    if (!in) return false;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    if (text.find("Proc-Type: 4,ENCRYPTED") != std::string::npos) return true;
    if (text.find("BEGIN ENCRYPTED PRIVATE KEY") != std::string::npos) return true;
    if (text.find("PuTTY-User-Key-File-") != std::string::npos &&
        text.find("Encryption: aes256-cbc") != std::string::npos)
        return true;

    const std::string begin = "-----BEGIN OPENSSH PRIVATE KEY-----";
    const std::string end = "-----END OPENSSH PRIVATE KEY-----";
    const size_t start = text.find(begin);
    if (start == std::string::npos) return false;
    const size_t bodyStart = start + begin.size();
    const size_t bodyEnd = text.find(end, bodyStart);
    if (bodyEnd == std::string::npos) return false;
    const std::vector<unsigned char> blob =
        decodeBase64Text(text.substr(bodyStart, bodyEnd - bodyStart));

    static const char kMagic[] = "openssh-key-v1";
    const size_t magicLen = sizeof(kMagic) - 1;
    if (blob.size() < magicLen + 1 + 4) return false;
    if (std::memcmp(blob.data(), kMagic, magicLen) != 0) return false;
    size_t offset = magicLen + 1;  // 跳过结尾的 '\0'
    const size_t cipherLen = (static_cast<size_t>(blob[offset]) << 24) |
                             (static_cast<size_t>(blob[offset + 1]) << 16) |
                             (static_cast<size_t>(blob[offset + 2]) << 8) |
                             static_cast<size_t>(blob[offset + 3]);
    offset += 4;
    if (cipherLen == 0 || offset + cipherLen > blob.size()) return false;
    return !(cipherLen == 4 && std::memcmp(blob.data() + offset, "none", 4) == 0);
}

}  // namespace

bool privateKeyNeedsPassphrase(const std::string& keyPath) {
    if (keyPath.empty()) return false;
    return isEncryptedPrivateKey(expandPath(keyPath));
}

void SshSession::authenticate() {
    const std::string& user = options_.user;
    if (user.empty()) throw Error("用户名为空");

    auto tryPassword = [&](const std::string& password) -> bool {
        if (password.empty()) return false;
        int rc = libssh2_userauth_password_ex(session_, user.c_str(),
                                              static_cast<unsigned int>(user.size()),
                                              password.c_str(),
                                              static_cast<unsigned int>(password.size()),
                                              nullptr);
        if (rc == 0) return true;
        log().debug("密码认证失败：" + sshErrorText(rc));
        return false;
    };

    // 私钥格式/算法提示：认证全失败时一并抛给界面，GUI 里也能看到原因。
    std::string keyHint;

    auto tryKey = [&](const std::string& keyPath, const std::string& passphrase) -> int {
        if (keyPath.empty()) return LIBSSH2_ERROR_BAD_USE;
        std::error_code ec;
        if (!fs::exists(utf8ToPath(keyPath), ec)) {
            log().debug("私钥不存在：" + keyPath);
            return LIBSSH2_ERROR_FILE;
        }
        std::string pubPath = keyPath + ".pub";
        const char* pub = fs::exists(utf8ToPath(pubPath), ec) ? pubPath.c_str() : nullptr;
        int rc = libssh2_userauth_publickey_fromfile_ex(
            session_, user.c_str(), static_cast<unsigned int>(user.size()), pub,
            keyPath.c_str(), passphrase.empty() ? nullptr : passphrase.c_str());
        if (rc != 0) {
            log().debug("私钥认证失败 (" + keyPath + ")：" + sshErrorText(rc));
            std::string hint = keyAlgorithmHint(keyPath);
            if (isEncryptedPrivateKey(keyPath)) {
                const std::string passphraseHint =
                    "私钥带口令（passphrase）：请确认连接时输入的口令正确；"
                    "也可以在 GUI 会话里勾选“记住私钥口令”保存到密码保险库。";
                hint = hint.empty() ? passphraseHint : passphraseHint + "\n" + hint;
            }
            if (!hint.empty()) {
                log().debug(hint);
                if (keyHint.empty()) keyHint = hint;
            }
        }
        return rc;
    };

    // 1) 指定私钥
    if (!options_.keyPath.empty()) {
        std::string path = expandPath(options_.keyPath);
        const bool encrypted = isEncryptedPrivateKey(path);
        const bool canPrompt = stdinIsTty() && options_.password.empty();
        if (encrypted && canPrompt && options_.keyPassphrase.empty()) {
            // 最多试 3 次口令；直接回车表示放弃私钥，继续走密码认证。
            for (int i = 0; i < 3 && !authenticated_; ++i) {
                std::string entered = readPassword("私钥口令 (" + path + "): ");
                if (entered.empty()) break;
                const bool keyOk = tryKey(path, entered) == 0;
                if (keyOk) {
                    options_.keyPassphrase = entered;
                    authenticated_ = true;
                }
                wipeString(entered);
            }
            if (!authenticated_) log().warn("使用指定私钥认证失败：" + path);
        } else {
            if (tryKey(path, options_.keyPassphrase) == 0) {
                authenticated_ = true;
            } else {
                log().warn("使用指定私钥认证失败：" + path);
            }
        }
    }

    // 2) 显式密码
    if (!authenticated_ && !options_.password.empty()) {
        if (tryPassword(options_.password)) {
            authenticated_ = true;
        } else {
            log().warn("密码认证失败");
        }
    }

    // 3) 默认私钥
    if (!authenticated_ && options_.allowDefaultKeys && options_.keyPath.empty()) {
        const std::string sshDir = homeDirectory() + "/.ssh";
        for (const char* name : {"id_ed25519", "id_ecdsa", "id_rsa"}) {
            if (tryKey(sshDir + "/" + name, options_.keyPassphrase) == 0) {
                log().debug(std::string("使用默认私钥认证成功：") + name);
                authenticated_ = true;
                break;
            }
        }
    }

    // 4) 交互式密码
    for (int i = 0; !authenticated_ && i < 3 && stdinIsTty(); ++i) {
        std::string pwd = readPassword(user + "@" + options_.host + " 的密码: ");
        if (pwd.empty()) break;
        const bool passwordOk = tryPassword(pwd);
        if (passwordOk) authenticated_ = true;
        wipeString(pwd);
    }

    if (!authenticated_) {
        char* methods = libssh2_userauth_list(session_, user.c_str(),
                                              static_cast<unsigned int>(user.size()));
        std::string hint = methods ? std::string("服务器允许的方式：") + methods : std::string();
        std::string message = "SSH 认证失败（用户 " + user + "）" +
                              (hint.empty() ? "" : "，" + hint);
        if (!keyHint.empty()) message += "\n" + keyHint;
        throw Error(message);
    }
    log().debug("SSH 认证成功");
    // 凭据已经交给 libssh2 会话，SshSession 自己不再需要明文口令
    wipeString(options_.password);
    wipeString(options_.keyPassphrase);
}

// ---------------------------------------------------------------- 执行命令
namespace {

enum class StdinState { Data, NoData, Eof };

// 非阻塞地探测本地 stdin 是否可读；Windows 下交互式控制台一律不转发
StdinState pollStdin(int timeoutMs) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return StdinState::Eof;
    if (GetFileType(h) == FILE_TYPE_CHAR) return StdinState::NoData;
    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) return StdinState::Eof;
        return StdinState::NoData;
    }
    if (avail > 0) return StdinState::Data;
    WaitForSingleObject(h, static_cast<DWORD>(timeoutMs));
    return StdinState::NoData;
#else
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int rc = select(STDIN_FILENO + 1, &set, nullptr, nullptr, &tv);
    return rc > 0 ? StdinState::Data : StdinState::NoData;
#endif
}

}  // namespace

int SshSession::execute(const std::string& command) {
    if (!authenticated_ || !session_) throw Error("尚未连接，无法执行命令");

    ChannelGuard guard(libssh2_channel_open_session(session_));
    if (!guard.channel) fail("无法打开 SSH 会话通道");
    if (libssh2_channel_exec(guard.channel, command.c_str()) != 0)
        fail("远端拒绝执行命令");

    const bool feedStdin = !stdinIsTty();
    bool stdinOpen = feedStdin;
    bool eofSent = false;
    std::vector<char> buf(32 * 1024);

    libssh2_session_set_blocking(session_, 0);
    uint64_t spin = 0;
    for (;;) {
        bool active = false;
        auto n = libssh2_channel_read(guard.channel, buf.data(), buf.size());
        while (n > 0) {
            std::fwrite(buf.data(), 1, static_cast<size_t>(n), stdout);
            std::fflush(stdout);
            active = true;
            n = libssh2_channel_read(guard.channel, buf.data(), buf.size());
        }
        if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
            libssh2_session_set_blocking(session_, 1);
            fail("读取远端标准输出失败");
        }

        n = libssh2_channel_read_stderr(guard.channel, buf.data(), buf.size());
        while (n > 0) {
            std::fwrite(buf.data(), 1, static_cast<size_t>(n), stderr);
            std::fflush(stderr);
            active = true;
            n = libssh2_channel_read_stderr(guard.channel, buf.data(), buf.size());
        }
        if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
            libssh2_session_set_blocking(session_, 1);
            fail("读取远端标准错误失败");
        }

        if (stdinOpen && !eofSent) {
            StdinState state = pollStdin(20);
            if (state == StdinState::Data) {
                size_t got = std::fread(buf.data(), 1, buf.size(), stdin);
                if (got > 0) {
                    size_t off = 0;
                    while (off < got) {
                        auto written = libssh2_channel_write(guard.channel, buf.data() + off,
                                                             got - off);
                        if (written == LIBSSH2_ERROR_EAGAIN) {
                            sleepMs(5);
                            continue;
                        }
                        if (written < 0) {
                            libssh2_session_set_blocking(session_, 1);
                            fail("向远端写入标准输入失败");
                        }
                        off += static_cast<size_t>(written);
                        active = true;
                    }
                } else if (std::feof(stdin)) {
                    state = StdinState::Eof;
                }
            }
            if (state == StdinState::Eof) {
                libssh2_channel_send_eof(guard.channel);
                eofSent = true;
                stdinOpen = false;
            }
        }

        if (libssh2_channel_eof(guard.channel) == 1) break;
        if (!active) sleepMs(++spin % 20 == 0 ? 20 : 5);
    }

    // 通道 EOF 之后可能还有残留数据
    auto n2 = libssh2_channel_read(guard.channel, buf.data(), buf.size());
    while (n2 > 0) {
        std::fwrite(buf.data(), 1, static_cast<size_t>(n2), stdout);
        n2 = libssh2_channel_read(guard.channel, buf.data(), buf.size());
    }
    n2 = libssh2_channel_read_stderr(guard.channel, buf.data(), buf.size());
    while (n2 > 0) {
        std::fwrite(buf.data(), 1, static_cast<size_t>(n2), stderr);
        n2 = libssh2_channel_read_stderr(guard.channel, buf.data(), buf.size());
    }
    std::fflush(stdout);
    std::fflush(stderr);

    libssh2_session_set_blocking(session_, 1);
    if (libssh2_channel_close(guard.channel) != 0) fail("关闭远端通道失败");
    int exitStatus = libssh2_channel_get_exit_status(guard.channel);
    return exitStatus;
}

// ---------------------------------------------------------------- 交互式 shell
namespace {

#ifdef _WIN32
volatile LONG g_shellActive = 0;

BOOL WINAPI shellCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        // shell 模式下把 Ctrl+C 交给远端处理（作为输入字节转发）
        if (InterlockedCompareExchange(&g_shellActive, 1, 1) == 1) return TRUE;
    }
    return FALSE;
}

class RawTerminal {
public:
    RawTerminal() {
        in_ = GetStdHandle(STD_INPUT_HANDLE);
        out_ = GetStdHandle(STD_OUTPUT_HANDLE);
        if (GetConsoleMode(in_, &oldIn_)) {
            DWORD mode = oldIn_;
            mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
            mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
            if (SetConsoleMode(in_, mode)) inOk_ = true;
        }
        if (GetConsoleMode(out_, &oldOut_)) {
            if (SetConsoleMode(out_, oldOut_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) outOk_ = true;
        }
        SetConsoleCtrlHandler(shellCtrlHandler, TRUE);
        InterlockedExchange(&g_shellActive, 1);
    }
    ~RawTerminal() {
        InterlockedExchange(&g_shellActive, 0);
        SetConsoleCtrlHandler(shellCtrlHandler, FALSE);
        if (inOk_) SetConsoleMode(in_, oldIn_);
        if (outOk_) SetConsoleMode(out_, oldOut_);
        std::fflush(stdout);
    }
    RawTerminal(const RawTerminal&) = delete;
    RawTerminal& operator=(const RawTerminal&) = delete;

private:
    HANDLE in_ = INVALID_HANDLE_VALUE;
    HANDLE out_ = INVALID_HANDLE_VALUE;
    DWORD oldIn_ = 0;
    DWORD oldOut_ = 0;
    bool inOk_ = false;
    bool outOk_ = false;
};

bool consoleSize(int& cols, int& rows) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return false;
    cols = info.srWindow.Right - info.srWindow.Left + 1;
    rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    return cols > 0 && rows > 0;
}

// >0 读取到的字节数，0 表示 EOF，-1 表示暂无数据
int readLocalInput(char* buf, size_t cap, int timeoutMs) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return 0;
    DWORD wait = WaitForSingleObject(h, static_cast<DWORD>(timeoutMs));
    if (wait != WAIT_OBJECT_0) return -1;
    DWORD got = 0;
    if (!ReadConsoleA(h, buf, static_cast<DWORD>(cap), &got, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) return 0;
        return -1;
    }
    return static_cast<int>(got);
}

#else  // POSIX

class RawTerminal {
public:
    RawTerminal() {
        if (!isatty(STDIN_FILENO)) return;
        if (tcgetattr(STDIN_FILENO, &old_) != 0) return;
        termios raw = old_;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) ok_ = true;
    }
    ~RawTerminal() {
        if (ok_) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_);
    }
    RawTerminal(const RawTerminal&) = delete;
    RawTerminal& operator=(const RawTerminal&) = delete;

private:
    termios old_{};
    bool ok_ = false;
};

bool consoleSize(int& cols, int& rows) {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 && ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0)
        return false;
    if (ws.ws_col == 0 || ws.ws_row == 0) return false;
    cols = ws.ws_col;
    rows = ws.ws_row;
    return true;
}

int readLocalInput(char* buf, size_t cap, int timeoutMs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (select(STDIN_FILENO + 1, &set, nullptr, nullptr, &tv) <= 0) return -1;
    ssize_t n = ::read(STDIN_FILENO, buf, cap);
    return n <= 0 ? -1 : static_cast<int>(n);
}

#endif

}  // namespace

void SshSession::terminalSize(int& cols, int& rows) {
    cols = 80;
    rows = 24;
    int c = 0, r = 0;
    if (consoleSize(c, r)) {
        cols = c;
        rows = r;
    }
}

int SshSession::interactiveShell(const std::string& term) {
    if (!authenticated_ || !session_) throw Error("尚未连接，无法打开 shell");

    ChannelGuard guard(libssh2_channel_open_session(session_));
    if (!guard.channel) fail("无法打开 SSH 会话通道");

    int cols = 80, rows = 24;
    terminalSize(cols, rows);
    if (libssh2_channel_request_pty_ex(guard.channel, term.c_str(),
                                       static_cast<unsigned int>(term.size()), nullptr, 0, cols,
                                       rows, 0, 0) != 0)
        fail("无法申请伪终端");
    if (libssh2_channel_shell(guard.channel) != 0) fail("远端拒绝启动 shell");

    libssh2_session_set_blocking(session_, 0);
    {
        RawTerminal raw;
        std::vector<char> buf(32 * 1024);
        uint64_t spin = 0;
        int lastCols = cols, lastRows = rows;
        bool eofSent = false;
        for (;;) {
            bool active = false;
            auto n = libssh2_channel_read(guard.channel, buf.data(), buf.size());
            while (n > 0) {
                std::fwrite(buf.data(), 1, static_cast<size_t>(n), stdout);
                std::fflush(stdout);
                active = true;
                n = libssh2_channel_read(guard.channel, buf.data(), buf.size());
            }
            if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) break;

            n = libssh2_channel_read_stderr(guard.channel, buf.data(), buf.size());
            while (n > 0) {
                std::fwrite(buf.data(), 1, static_cast<size_t>(n), stderr);
                std::fflush(stderr);
                active = true;
                n = libssh2_channel_read_stderr(guard.channel, buf.data(), buf.size());
            }

            int got = readLocalInput(buf.data(), buf.size(), active ? 0 : 10);
            if (got == 0 && !eofSent) {
                libssh2_channel_send_eof(guard.channel);
                eofSent = true;
            } else if (got > 0) {
                size_t off = 0;
                while (off < static_cast<size_t>(got)) {
                    auto written = libssh2_channel_write(guard.channel, buf.data() + off,
                                                         static_cast<size_t>(got) - off);
                    if (written == LIBSSH2_ERROR_EAGAIN) {
                        sleepMs(2);
                        continue;
                    }
                    if (written < 0) break;
                    off += static_cast<size_t>(written);
                }
            }

            if (libssh2_channel_eof(guard.channel) == 1) break;

            if (++spin % 50 == 0) {
                int c = 80, r = 24;
                terminalSize(c, r);
                if (c != lastCols || r != lastRows) {
                    libssh2_channel_request_pty_size(guard.channel, c, r);
                    lastCols = c;
                    lastRows = r;
                }
            }
            if (!active && got < 0) sleepMs(5);
        }
    }

    libssh2_session_set_blocking(session_, 1);
    libssh2_channel_close(guard.channel);
    return libssh2_channel_get_exit_status(guard.channel);
}

}  // namespace rshell
