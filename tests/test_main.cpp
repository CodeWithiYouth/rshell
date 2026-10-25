// test_main.cpp - 纯函数单元测试（不依赖网络/SSH 服务器）
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "config.h"
#include "credential_store.h"
#include "protected_secret.h"
#include "session_store.h"
#include "ssh_session.h"
#include "terminal.h"
#include "util.h"
#include "vault.h"
#ifdef _WIN32
#include <windows.h>
#include "dnd_virtual_file.h"
#include "win_compat.h"
#endif

using namespace rshell;

namespace {

int g_checks = 0;
int g_failed = 0;

void checkImpl(bool ok, const char* expr, const char* file, int line, const std::string& detail) {
    ++g_checks;
    if (ok) return;
    ++g_failed;
    std::cerr << "FAIL " << file << ":" << line << "  " << expr;
    if (!detail.empty()) std::cerr << "  [" << detail << "]";
    std::cerr << "\n";
}

template <typename A, typename B>
void checkEqImpl(const A& actual, const B& expected, const char* expr, const char* file, int line) {
    std::ostringstream oss;
    oss << "actual=" << actual << " expected=" << expected;
    checkImpl(actual == expected, expr, file, line, oss.str());
}

#define CHECK(cond) checkImpl((cond), #cond, __FILE__, __LINE__, "")
#define CHECK_EQ(actual, expected) checkEqImpl((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

void testStrings() {
    CHECK_EQ(trim("  hi \t\r\n"), std::string("hi"));
    CHECK_EQ(trim(""), std::string(""));
    CHECK_EQ(toLower("AbC"), std::string("abc"));
    CHECK(iequals("HeLLo", "hello"));
    CHECK(!iequals("hello", "hell"));
    CHECK(startsWith("rshell.exe", "rshell"));
    CHECK(endsWith("rshell.exe", ".exe"));
    CHECK(!endsWith("rshell", ".exe"));

    std::vector<std::string> parts = split("a,b,,c", ',');
    CHECK_EQ(parts.size(), static_cast<size_t>(4));
    CHECK_EQ(parts[0], std::string("a"));
    CHECK_EQ(parts[2], std::string(""));
    CHECK_EQ(join(parts, "-"), std::string("a-b--c"));
    CHECK_EQ(replaceAll("a/b\\c", "\\", "/"), std::string("a/b/c"));
}

void testRemotePaths() {
    CHECK_EQ(normalizeRemotePath("/a/./b/../c"), std::string("/a/c"));
    CHECK_EQ(normalizeRemotePath("a//b/"), std::string("a/b"));
    CHECK_EQ(normalizeRemotePath("/"), std::string("/"));
    CHECK_EQ(normalizeRemotePath(""), std::string("."));
    CHECK_EQ(normalizeRemotePath("../a"), std::string("../a"));

    CHECK_EQ(remoteJoin("/etc", "nginx"), std::string("/etc/nginx"));
    CHECK_EQ(remoteJoin("/etc/nginx", "../ssh"), std::string("/etc/ssh"));
    CHECK_EQ(remoteJoin("/", "tmp"), std::string("/tmp"));
    CHECK_EQ(remoteJoin("/etc", "/var/log"), std::string("/var/log"));
    CHECK_EQ(remoteJoin("a", "b"), std::string("a/b"));

    CHECK_EQ(remoteBaseName("/var/log/syslog"), std::string("syslog"));
    CHECK_EQ(remoteBaseName("/var/log/"), std::string("log"));
    CHECK_EQ(remoteBaseName("/"), std::string("/"));
    CHECK_EQ(remoteDirName("/var/log/syslog"), std::string("/var/log"));
    CHECK_EQ(remoteDirName("syslog"), std::string("."));
    CHECK_EQ(remoteDirName("/syslog"), std::string("/"));

    CHECK_EQ(localBaseName("C:\\Users\\me\\file.txt"), std::string("file.txt"));
    CHECK_EQ(localBaseName("/tmp/file.txt"), std::string("file.txt"));
    CHECK_EQ(localBaseName("file.txt"), std::string("file.txt"));

    CHECK(isAbsoluteRemotePath("/tmp"));
    CHECK(!isAbsoluteRemotePath("tmp/a"));
    CHECK(!isAbsoluteRemotePath(""));
}

void testGlob() {
    CHECK(globMatch("*", "anything"));
    CHECK(globMatch("*", ""));
    CHECK(globMatch("*.txt", "notes.txt"));
    CHECK(!globMatch("*.txt", "notes.txt.bak"));
    CHECK(globMatch("file?.log", "file1.log"));
    CHECK(!globMatch("file?.log", "file10.log"));
    CHECK(globMatch("a*b*c", "aXbYc"));
    CHECK(globMatch("exact", "exact"));
    CHECK(!globMatch("exact", "exactly"));
    CHECK(globMatch("\\*literal", "*literal"));
    CHECK(!globMatch("\\*literal", "xliteral"));

    CHECK(hasWildcard("*.log"));
    CHECK(hasWildcard("a?b"));
    CHECK(!hasWildcard("plain.txt"));
    CHECK(!hasWildcard("escaped\\*star"));
}

void testFormatting() {
    CHECK_EQ(formatBytes(0), std::string("0 B"));
    CHECK_EQ(formatBytes(1023), std::string("1023 B"));
    CHECK_EQ(formatBytes(1024), std::string("1.0 KiB"));
    CHECK_EQ(formatBytes(1536), std::string("1.5 KiB"));
    CHECK_EQ(formatBytes(50ull * 1024 * 1024), std::string("50.0 MiB"));

    CHECK_EQ(formatRate(0.0), std::string("--"));
    CHECK_EQ(formatRate(-1.0), std::string("--"));
    CHECK_EQ(formatRate(2048.0), std::string("2.0 KiB/s"));

    CHECK_EQ(formatDuration(0), std::string("00:00"));
    CHECK_EQ(formatDuration(65), std::string("01:05"));
    CHECK_EQ(formatDuration(3661), std::string("1:01:01"));

    CHECK_EQ(formatMode(0644), std::string("-rw-r--r--"));
    CHECK_EQ(formatMode(0040755), std::string("drwxr-xr-x"));
    CHECK_EQ(formatMode(0120777), std::string("lrwxrwxrwx"));
    CHECK_EQ(formatMode(0104755), std::string("-rwsr-xr-x"));
    CHECK_EQ(formatMode(0), std::string("----------"));
}

void testRemoteTarget() {
    RemoteTarget t = parseRemoteTarget("deploy@example.com:/var/log");
    CHECK_EQ(t.user, std::string("deploy"));
    CHECK_EQ(t.host, std::string("example.com"));
    CHECK(t.hasPath);
    CHECK_EQ(t.path, std::string("/var/log"));

    t = parseRemoteTarget("deploy@example.com");
    CHECK_EQ(t.user, std::string("deploy"));
    CHECK_EQ(t.host, std::string("example.com"));
    CHECK(!t.hasPath);

    t = parseRemoteTarget("web:/tmp/a.txt");
    CHECK_EQ(t.host, std::string("web"));
    CHECK(t.hasPath);
    CHECK_EQ(t.path, std::string("/tmp/a.txt"));

    t = parseRemoteTarget("/var/log/syslog");
    CHECK(t.host.empty());
    CHECK(t.hasPath);
    CHECK_EQ(t.path, std::string("/var/log/syslog"));

    t = parseRemoteTarget("webbox");
    CHECK_EQ(t.host, std::string("webbox"));
    CHECK(!t.hasPath);
    CHECK(t.path.empty());

    t = parseRemoteTarget("web:");
    CHECK_EQ(t.host, std::string("web"));
    CHECK(!t.hasPath);

    t = parseRemoteTarget("~");
    CHECK(t.hasPath);
    CHECK_EQ(t.path, std::string("~"));
}

void testModeParsing() {
    CHECK_EQ(parseModeString("644"), 0644u);
    CHECK_EQ(parseModeString("0644"), 0644u);
    CHECK_EQ(parseModeString(" 755 "), 0755u);
    CHECK_EQ(parseModeString("0"), 0u);
    CHECK_EQ(parseModeString("4755"), 04755u);

    bool threw = false;
    try {
        parseModeString("abc");
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        parseModeString("999");
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        parseModeString("");
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
}

std::string writeTempFile(const std::string& name, const std::string& content) {
    fs::path path = fs::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    return pathToUtf8(path);
}

void testSshConfig() {
    const std::string config =
        "# 全局默认\n"
        "User globaluser\n"
        "Port 2200\n"
        "\n"
        "Host web\n"
        "    HostName 192.168.1.10\n"
        "    User deploy\n"
        "    Port 2222\n"
        "    IdentityFile ~/.ssh/id_web\n"
        "\n"
        "Host *.example.com\n"
        "    User alice\n"
        "\n"
        "Host db\n"
        "    HostName 10.0.0.2\n"
        "\n"
        "Match host web\n"
        "    User shouldBeIgnored\n";

    const std::string path = writeTempFile("rshell_test_ssh_config", config);
    SshConfig cfg = SshConfig::load(path);
    CHECK(cfg.loaded());

    HostConfig host;
    CHECK(cfg.lookup("web", host));
    CHECK_EQ(host.hostName, std::string("192.168.1.10"));
    CHECK_EQ(host.user, std::string("deploy"));
    CHECK_EQ(host.port, 2222);
    CHECK(endsWith(host.identityFile, "id_web"));
    CHECK_EQ(host.alias, std::string("web"));

    HostConfig wildcard;
    CHECK(cfg.lookup("api.example.com", wildcard));
    CHECK_EQ(wildcard.hostName, std::string("api.example.com"));
    CHECK_EQ(wildcard.user, std::string("alice"));
    CHECK_EQ(wildcard.port, 2200);  // 来自全局段

    HostConfig db;
    CHECK(cfg.lookup("db", db));
    CHECK_EQ(db.hostName, std::string("10.0.0.2"));
    CHECK_EQ(db.user, std::string("globaluser"));
    CHECK_EQ(db.port, 2200);

    HostConfig unknown;
    CHECK(!cfg.lookup("unknown.host", unknown));
    CHECK_EQ(unknown.hostName, std::string("unknown.host"));

    std::vector<std::string> names = cfg.hostNames();
    CHECK_EQ(names.size(), static_cast<size_t>(2));
    CHECK_EQ(names[0], std::string("web"));
    CHECK_EQ(names[1], std::string("db"));

    fs::remove(utf8ToPath(path));
}

void testRshellSettings() {
    const std::string config =
        "# rshell 配置\n"
        "[default]\n"
        "user = root\n"
        "port = 2222\n"
        "timeout = 45\n"
        "progress = false\n"
        "downloaddir = ~/Downloads\n"
        "\n"
        "[alias web]\n"
        "host = 10.0.0.9\n"
        "user = deploy\n"
        "port = 2022\n";

    const std::string path = writeTempFile("rshell_test_settings", config);
    RshellSettings settings = loadRshellSettings(path);

    CHECK_EQ(settings.defaultUser, std::string("root"));
    CHECK_EQ(settings.defaultPort, 2222);
    CHECK_EQ(settings.timeoutSec, 45);
    CHECK(!settings.progress);
    CHECK(settings.color);
    CHECK(endsWith(settings.downloadDir, "Downloads"));

    auto it = settings.aliases.find("web");
    CHECK(it != settings.aliases.end());
    if (it != settings.aliases.end()) {
        CHECK_EQ(it->second.host, std::string("10.0.0.9"));
        CHECK_EQ(it->second.user, std::string("deploy"));
        CHECK_EQ(it->second.port, 2022);
    }

    // 缺失文件时返回默认值
    RshellSettings fallback = loadRshellSettings(path + ".missing");
    CHECK_EQ(fallback.defaultPort, 0);
    CHECK_EQ(fallback.timeoutSec, 20);

    fs::remove(utf8ToPath(path));
}

// 会话文件只记 savepassword 标记，密码本体存在密码保险库里
void testSessionStorePasswordFlag() {
    const std::string ini =
        "# rshell 会话\n"
        "[session:prod]\n"
        "host = 10.0.0.5\n"
        "user = root\n"
        "port = 2222\n"
        "savepassword=1\n"
        "savekeypassphrase=1\n"
        "\n"
        "[session:plain]\n"
        "host = 10.0.0.6\n";
    const std::string path = writeTempFile("rshell_test_sessions.ini", ini);

    SessionStore store = SessionStore::load(path);
    CHECK_EQ(store.sessions().size(), static_cast<std::size_t>(2));
    const Session* prod = store.find("prod");
    CHECK(prod != nullptr);
    if (prod) {
        CHECK(prod->savePassword);
        CHECK(prod->saveKeyPassphrase);
        CHECK_EQ(prod->port, 2222);
    }
    const Session* plain = store.find("plain");
    CHECK(plain != nullptr);
    if (plain) {
        CHECK(!plain->savePassword);
        CHECK(!plain->saveKeyPassphrase);
    }

    // 写回时同样只有标记，文件里不该出现任何口令
    Session changed;
    changed.name = "plain";
    changed.host = "10.0.0.6";
    changed.savePassword = true;
    changed.saveKeyPassphrase = true;
    store.upsert(changed);
    CHECK(store.save());

    std::ifstream in(utf8ToPath(path), std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    in.close();  // Windows 上文件还开着时删不掉
    const std::string saved = content.str();
    CHECK(saved.find("savepassword=1") != std::string::npos);
    CHECK(saved.find("savekeypassphrase=1") != std::string::npos);
    CHECK(saved.find("savepassword=0") == std::string::npos);

    SessionStore reloaded = SessionStore::load(path);
    const Session* again = reloaded.find("plain");
    CHECK(again != nullptr);
    if (again) {
        CHECK(again->savePassword);
        CHECK(again->saveKeyPassphrase);
    }

    std::error_code ec;
    fs::remove(utf8ToPath(path), ec);
    CHECK(!ec);
}

// 私钥是否带口令：GUI 连接前用它决定是否弹口令框
void testPrivateKeyNeedsPassphrase() {
    const std::string plain = writeTempFile(
        "rshell_key_plain.pem",
        "-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n");
    CHECK(!privateKeyNeedsPassphrase(plain));

    const std::string encrypted = writeTempFile(
        "rshell_key_enc.pem",
        "-----BEGIN RSA PRIVATE KEY-----\n"
        "Proc-Type: 4,ENCRYPTED\n"
        "DEK-Info: AES-128-CBC,00000000000000000000000000000000\n"
        "\nAAAA\n-----END RSA PRIVATE KEY-----\n");
    CHECK(privateKeyNeedsPassphrase(encrypted));

    std::error_code ec;
    fs::remove(utf8ToPath(plain), ec);
    ec.clear();
    fs::remove(utf8ToPath(encrypted), ec);
    CHECK(!ec);
}

// 条目名 + 未接保险库时读写都失败（不再回退到 Windows 凭据管理器）
void testCredentialStore() {
    CHECK_EQ(credentialTarget("prod"), std::string("rshell/prod"));
    CHECK_EQ(credentialTarget(""), std::string("rshell/"));
    CHECK_EQ(keyPassphraseTarget("prod"),
             std::string("rshell/prod/key-passphrase"));

    setCredentialVault(nullptr);
    CHECK(!credentialVaultActive());

    const std::string target = "rshell/no-vault";
    const std::string password = "p@ss 中文 word";
    std::string error;
    CHECK(!saveCredential(target, password, &error));
    CHECK(!error.empty());

    std::string loaded;
    CHECK(!loadCredential(target, loaded, &error));
    CHECK(!hasCredential(target));
    CHECK(!deleteCredential(target, &error));
}

// 保险库接成后端之后：保存/读取/删除都只由保险库承担
void testCredentialVaultRouting() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "rshell_vault_routing";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    Vault vault;
    std::string error;
    CHECK(Vault::create((dir / "routing.rvault").wstring(), L"routing-master", vault, 1000,
                        &error));
    CHECK(vault.unlocked());
    CHECK(!credentialVaultActive());  // 没接上之前不算激活

    setCredentialVault(&vault);
    CHECK(credentialVaultActive());

    const std::string target = "rshell/unit-routing-test";
    const std::string password = "p@ss 中文 routing";
    CHECK(saveCredential(target, password, &error));
    CHECK(vault.has(target));  // 口令真的落在保险库里

    std::string loaded;
    CHECK(loadCredential(target, loaded, &error));
    CHECK_EQ(loaded, password);
    wipeString(loaded);
    CHECK(hasCredential(target));

    // 空密码等于删除：避免下次静默用旧口令
    CHECK(saveCredential(target, "", &error));
    CHECK(!vault.has(target));
    CHECK(!hasCredential(target));
    CHECK(deleteCredential(target, &error));  // 条目不存在也算成功

    // 删除后保险库里不留条目
    CHECK(saveCredential(target, password, &error));
    CHECK(deleteCredential(target, &error));
    CHECK(!vault.has(target));
    CHECK(!hasCredential(target));

    // 断开后端：所有读写都失败，不会偷偷写别的地方
    setCredentialVault(nullptr);
    CHECK(!credentialVaultActive());
    CHECK(!loadCredential(target, loaded, &error));
    CHECK(!saveCredential(target, password, &error));

    // 锁着的保险库即使接上也不生效（Vault::unlocked() 是路由的前提）
    vault.lock();
    setCredentialVault(&vault);
    CHECK(!credentialVaultActive());
    CHECK(!loadCredential(target, loaded, &error));
    CHECK(!saveCredential(target, password, &error));
    setCredentialVault(nullptr);

    fs::remove_all(dir, ec);
}

// 内存口令容器：内部只放密文，reveal 还原原文，clear 之后什么都不剩
void testProtectedSecret() {
    ProtectedSecret secret;
    CHECK(secret.empty());
    CHECK(secret.reveal().empty());
    CHECK(!secret.encrypted());

    const std::wstring password = L"p@ss 中文 word 1234567890";
    secret.assign(password);
    CHECK(!secret.empty());
    CHECK(secret.reveal() == password);
#ifdef _WIN32
    // DPAPI 可用时，内部缓冲里不该再是明文
    CHECK(secret.encrypted());
    CHECK(secret.raw() != password);
    CHECK(secret.rawBytes() % 8 == 0);
    if (!secret.locked()) {
        // 工作集配额被别的进程吃光时 VirtualLock 会失败，不算功能错误
        std::cout << "SKIP VirtualLock 未生效（工作集配额不足）\n";
    }
#endif

    // 8 字节对齐边界：补齐用的 0 不能带进 reveal 的结果
    for (std::size_t len : {1u, 2u, 4u, 5u, 8u, 9u, 17u}) {
        const std::wstring plain(len, L'x');
        secret.assign(plain);
        const std::wstring back = secret.reveal();
        CHECK(back == plain);
        CHECK_EQ(back.size(), len);
    }

    // 重新 assign 覆盖旧口令
    secret.assign(L"second");
    CHECK(secret.reveal() == std::wstring(L"second"));

    // 先用长口令再换短口令：复用的解密工作区不能带出上一次的尾巴
    secret.assign(L"a-much-longer-password-1234567890");
    CHECK(secret.reveal() == std::wstring(L"a-much-longer-password-1234567890"));
    secret.assign(L"short");
    CHECK(secret.reveal() == std::wstring(L"short"));

    // UTF-8 入口：中文密码往返不能损坏
    const std::string utf8Password = "p@ss \xe4\xb8\xad\xe6\x96\x87 word";
    secret.assignUtf8(utf8Password);
    CHECK(secret.revealUtf8() == utf8Password);

    secret.clear();
    CHECK(secret.empty());
    CHECK(secret.reveal().empty());

    // 移动之后密文仍可解
    secret.assign(L"move-me");
    ProtectedSecret moved(std::move(secret));
    CHECK(moved.reveal() == std::wstring(L"move-me"));
    CHECK(secret.empty());
}

// 保险库：文件格式 + 主密码 + 篡改检测 + 改密码
void testVault() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "rshell_vault_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    auto readAll = [](const std::wstring& p) {
        std::ifstream in(fs::path(p), std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                         std::istreambuf_iterator<char>());
    };
    auto writeAll = [](const std::wstring& p, const std::vector<std::uint8_t>& data) {
        std::ofstream out(fs::path(p), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    };

    const std::wstring path = (dir / "main.rvault").wstring();
    const std::wstring password = L"correct horse 中文 battery";
    std::string error;

    // --- 创建：返回时已解锁，文件已落盘
    Vault vault;
    CHECK(Vault::create(path, password, vault, 2000, &error));
    CHECK(error.empty());
    CHECK(vault.unlocked());
    CHECK(vault.names().empty());
    CHECK(fs::exists(fs::path(path)));

    Vault rejected;
    CHECK(!Vault::create(L"", password, rejected, 2000, &error));
    CHECK(!Vault::create((dir / "nopwd.rvault").wstring(), L"", rejected, 2000, &error));
    CHECK(!rejected.unlocked());

    // 默认路径落在 %LOCALAPPDATA%\rshell\vault.dat
    const std::wstring defaultFile = Vault::defaultPath();
    CHECK(defaultFile.find(L"vault.dat") != std::wstring::npos);
    CHECK(defaultFile.find(L"rshell") != std::wstring::npos);

    // --- 写入条目（含中文与空串）后保存
    vault.set("user@host:22", "s3cret!");
    vault.set("中文会话名", "p@ss 中文 word");
    vault.set("empty-secret", "");
    CHECK_EQ(vault.names().size(), static_cast<std::size_t>(3));
    CHECK(vault.has("user@host:22"));
    CHECK(!vault.has("missing"));
    std::string got;
    CHECK(vault.get("user@host:22", got));
    CHECK_EQ(got, std::string("s3cret!"));
    CHECK(vault.get("empty-secret", got));
    CHECK_EQ(got, std::string(""));
    CHECK(!vault.get("missing", got));
    CHECK(vault.save(&error));

    // 文件里不能出现明文口令
    std::vector<std::uint8_t> bytes = readAll(path);
    CHECK(bytes.size() > 32);
    CHECK(bytes.size() < 4096);
    const std::string raw(bytes.begin(), bytes.end());
    CHECK(raw.find("s3cret!") == std::string::npos);
    CHECK(raw.find("p@ss") == std::string::npos);

    // --- 重新打开：条目完整
    Vault reopened;
    error.clear();
    CHECK(Vault::open(path, password, reopened, &error));
    CHECK(reopened.unlocked());
    CHECK_EQ(reopened.names().size(), static_cast<std::size_t>(3));
    CHECK(reopened.get("中文会话名", got));
    CHECK_EQ(got, std::string("p@ss 中文 word"));

    // 同样的主密码、不同的库：随机盐保证两份文件不一样
    Vault second;
    const std::wstring path2 = (dir / "second.rvault").wstring();
    CHECK(Vault::create(path2, password, second, 2000, &error));
    CHECK(readAll(path2) != bytes);

    // 默认迭代次数：文件头要如实记录，且建/开都能走通
    Vault defaultIters;
    const std::wstring path3 = (dir / "default_iters.rvault").wstring();
    const auto started = std::chrono::steady_clock::now();
    CHECK(Vault::create(path3, L"默认迭代口令", defaultIters, kVaultDefaultIterations, &error));
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    std::cout << "vault 默认 KDF（600k 次 PBKDF2）耗时：" << spent << " ms\n";
    const std::vector<std::uint8_t> header = readAll(path3);
    CHECK(header.size() > 16);
    const std::uint32_t storedIters =
        static_cast<std::uint32_t>(header[12]) |
        (static_cast<std::uint32_t>(header[13]) << 8) |
        (static_cast<std::uint32_t>(header[14]) << 16) |
        (static_cast<std::uint32_t>(header[15]) << 24);
    CHECK_EQ(storedIters, kVaultDefaultIterations);
    Vault defaultOpened;
    CHECK(Vault::open(path3, L"默认迭代口令", defaultOpened, &error));
    CHECK(defaultOpened.unlocked());

    // --- 错误口令 / 不存在的文件
    Vault wrong;
    error.clear();
    CHECK(!Vault::open(path, L"wrong password", wrong, &error));
    CHECK(!error.empty());
    CHECK(!wrong.unlocked());
    error.clear();
    CHECK(!Vault::open((dir / "missing.rvault").wstring(), password, wrong, &error));
    CHECK(!error.empty());

    // --- 篡改检测
    auto tampered = bytes;
    tampered[tampered.size() - 20] ^= 0x01;  // 条目区密文
    writeAll(path, tampered);
    CHECK(!Vault::open(path, password, wrong, &error));

    tampered = bytes;
    tampered[16] ^= 0x01;  // salt（会被 slot 的 AAD 覆盖）
    writeAll(path, tampered);
    CHECK(!Vault::open(path, password, wrong, &error));

    tampered = bytes;
    tampered[12] ^= 0x01;  // 迭代次数（同样在 AAD 里）
    writeAll(path, tampered);
    CHECK(!Vault::open(path, password, wrong, &error));

    tampered = bytes;
    tampered[8] = 99;  // version 改高
    writeAll(path, tampered);
    error.clear();
    CHECK(!Vault::open(path, password, wrong, &error));
    CHECK(error.find("更高版本") != std::string::npos);

    tampered = bytes;
    tampered.resize(tampered.size() / 2);  // 截断
    writeAll(path, tampered);
    CHECK(!Vault::open(path, password, wrong, &error));

    tampered = bytes;
    tampered.push_back(0x00);  // 尾部多余数据
    writeAll(path, tampered);
    CHECK(!Vault::open(path, password, wrong, &error));

    writeAll(path, bytes);  // 复原
    CHECK(Vault::open(path, password, reopened, &error));

    // --- 删条目
    CHECK(reopened.remove("empty-secret"));
    CHECK(!reopened.has("empty-secret"));
    CHECK(!reopened.remove("empty-secret"));
    CHECK(reopened.save(&error));

    // --- 换主密码：条目不变，旧口令失效
    CHECK(reopened.changeMasterPassword(L"新主密码 pass", &error));
    Vault oldPwd, newPwd;
    CHECK(!Vault::open(path, password, oldPwd, &error));
    CHECK(Vault::open(path, L"新主密码 pass", newPwd, &error));
    CHECK_EQ(newPwd.names().size(), static_cast<std::size_t>(2));
    CHECK(newPwd.get("user@host:22", got));
    CHECK_EQ(got, std::string("s3cret!"));

    // --- 锁定：DEK 与条目都丢掉，save 被拒绝
    newPwd.lock();
    CHECK(!newPwd.unlocked());
    CHECK(!newPwd.has("user@host:22"));
    CHECK(!newPwd.get("user@host:22", got));
    error.clear();
    CHECK(!newPwd.save(&error));
    CHECK(!error.empty());

    // 锁定后重新打开仍然可用
    Vault relocked;
    CHECK(Vault::open(path, L"新主密码 pass", relocked, &error));
    CHECK(relocked.has("user@host:22"));

    // --- TPM：未启用时 openWithTpm 干净失败（不崩、不误放行）
    Vault viaTpm;
    error.clear();
    CHECK(!Vault::openWithTpm(path, viaTpm, &error));
    CHECK(!error.empty());
    CHECK(!viaTpm.unlocked());

    // 界面在弹出解锁框之前只读文件头，判断要不要给「用 TPM 解锁」按钮
    CHECK(!Vault::tpmSlotPresent(path));
    CHECK(!Vault::tpmSlotPresent((dir / "missing.rvault").wstring()));

    // 真机 TPM 会弹 Windows 安全确认框，只在显式设置环境变量时才跑
    if (std::getenv("RSHELL_TEST_TPM") != nullptr && Vault::tpmAvailable()) {
        // TPM slot 的字节开销固定：type1+flags1+reserved2+len4+nonce12 + 256(OAEP)
        std::error_code sizeEc;
        const std::uintmax_t sizeBefore = fs::file_size(path, sizeEc);
        bool ok = relocked.enableTpm(&error);
        if (!ok) std::cout << "  enableTpm 失败：" << error << "\n";
        CHECK(ok);
        CHECK_EQ(fs::file_size(path, sizeEc), sizeBefore + 276);
        CHECK(relocked.hasTpmSlot());
        CHECK(Vault::tpmSlotPresent(path));
        Vault tpmOpened;
        const auto tpmStart = std::chrono::steady_clock::now();
        ok = Vault::openWithTpm(path, tpmOpened, &error);
        const long long tpmMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - tpmStart).count();
        std::cout << "vault TPM 解锁耗时：" << tpmMs << " ms\n";
        if (!ok) std::cout << "  openWithTpm 失败：" << error << "\n";
        CHECK(ok);
        CHECK(tpmOpened.unlocked());
        CHECK(tpmOpened.get("user@host:22", got));
        CHECK_EQ(got, std::string("s3cret!"));
        // TPM 解锁之外，主密码依然可用
        Vault stillPwd;
        ok = Vault::open(path, L"新主密码 pass", stillPwd, &error);
        if (!ok) std::cout << "  主密码回退失败：" << error << "\n";
        CHECK(ok);

        // TPM 密钥按文件路径派生：换路径后 TPM slot 失效，主密码仍可用，
        // 重新 enableTpm 可以恢复（对应“把 vault.dat 搬到别处”的场景）
        const std::wstring movedPath = (dir / "moved.rvault").wstring();
        std::error_code copyEc;
        fs::copy_file(path, movedPath, fs::copy_options::overwrite_existing, copyEc);
        Vault movedTpm;
        CHECK(!Vault::openWithTpm(movedPath, movedTpm, &error));
        Vault movedPwd;
        ok = Vault::open(movedPath, L"新主密码 pass", movedPwd, &error);
        if (!ok) std::cout << "  换路径后主密码打开失败：" << error << "\n";
        CHECK(ok);
        CHECK(movedPwd.get("user@host:22", got));
        CHECK_EQ(got, std::string("s3cret!"));
        ok = movedPwd.enableTpm(&error);
        if (!ok) std::cout << "  换路径后重新 enableTpm 失败：" << error << "\n";
        CHECK(ok);
        CHECK(Vault::tpmSlotPresent(movedPath));
        CHECK(Vault::openWithTpm(movedPath, movedTpm, &error));
        CHECK(movedTpm.disableTpm(&error));

        ok = tpmOpened.disableTpm(&error);
        if (!ok) std::cout << "  disableTpm 失败：" << error << "\n";
        CHECK(ok);
        CHECK(!tpmOpened.hasTpmSlot());
        CHECK(!Vault::tpmSlotPresent(path));
        Vault noTpm;
        CHECK(!Vault::openWithTpm(path, noTpm, &error));
    } else {
        std::cout << "SKIP TPM 解锁（未设置 RSHELL_TEST_TPM 或本机无 TPM）\n";
    }

    fs::remove_all(dir, ec);
}

void testTerminalBasics() {
    Terminal term(10, 3);
    term.feed("hello");
    CHECK_EQ(term.plainLine(0), std::string("hello"));
    CHECK_EQ(term.cursorCol(), 5);
    CHECK_EQ(term.cursorRow(), 0);

    // CR/LF 与自动滚动
    term.feed("\r\nworld");
    CHECK_EQ(term.plainLine(1), std::string("world"));
    CHECK_EQ(term.cursorRow(), 1);

    // 光标定位（1 基）
    term.feed("\x1b[3;4HX");
    CHECK_EQ(term.cursorRow(), 2);
    CHECK_EQ(term.cursorCol(), 4);
    CHECK_EQ(term.plainLine(2), std::string("   X"));

    // 擦除整屏
    term.feed("\x1b[2J");
    CHECK_EQ(term.plainLine(0), std::string(""));
    CHECK_EQ(term.plainLine(2), std::string(""));

    // 自动换行
    Terminal wrap(10, 3);
    wrap.feed("abcdefghijkl");
    CHECK_EQ(wrap.plainLine(0), std::string("abcdefghij"));
    CHECK_EQ(wrap.plainLine(1), std::string("kl"));
}

void testTerminalColors() {
    Terminal term(20, 3);
    term.feed("\x1b[31mR\x1b[0mN");
    CHECK_EQ(term.lineAt(0)[0].fg, term.theme().palette[1]);
    CHECK_EQ(term.lineAt(0)[1].fg, term.theme().fg);

    term.feed("\x1b[38;5;196mX");
    CHECK_EQ(term.lineAt(0)[2].fg, static_cast<std::uint32_t>(0xFF0000));

    term.feed("\x1b[48;2;1;2;3mY");
    CHECK_EQ(term.lineAt(0)[3].bg, static_cast<std::uint32_t>(0x010203));

    term.feed("\x1b[1;4;7mZ");
    const Cell& cell = term.lineAt(0)[4];
    CHECK((cell.flags & kFlagBold) != 0);
    CHECK((cell.flags & kFlagUnderline) != 0);
    CHECK((cell.flags & kFlagReverse) != 0);

    term.feed("\x1b[0m");
    CHECK_EQ(term.plainLine(0).substr(2), std::string("XYZ"));
}

void testTerminalWideAndScrollback() {
    Terminal term(10, 2);
    term.feed("\xE4\xB8\xAD" "a");  // 中 + a
    CHECK((term.lineAt(0)[0].flags & kFlagWide) != 0);
    CHECK((term.lineAt(0)[1].flags & kFlagWideTail) != 0);
    CHECK_EQ(term.lineAt(0)[2].ch, static_cast<char32_t>(U'a'));

    Terminal scroll(10, 2);
    scroll.feed("a\r\nb\r\nc");
    CHECK_EQ(scroll.scrollbackCount(), static_cast<std::size_t>(1));
    CHECK_EQ(scroll.plainLine(0), std::string("a"));
    CHECK_EQ(scroll.plainLine(1), std::string("b"));
    CHECK_EQ(scroll.plainLine(2), std::string("c"));

    scroll.scrollView(1);
    CHECK_EQ(scroll.scrollOffset(), 1);
    scroll.noteOutput();
    CHECK_EQ(scroll.scrollOffset(), 0);
}

void testTerminalResizeKeepsContent() {
    // 窗口缩小时优先裁掉光标下方的空白行：已有输出不应被整段推进历史
    Terminal term(20, 12);
    term.feed("line1\r\nline2\r\nline3");
    CHECK_EQ(term.plainLine(0), std::string("line1"));
    CHECK_EQ(term.plainLine(2), std::string("line3"));

    term.resize(20, 6);
    CHECK_EQ(term.scrollbackCount(), static_cast<std::size_t>(0));
    CHECK_EQ(term.plainLine(0), std::string("line1"));
    CHECK_EQ(term.plainLine(2), std::string("line3"));

    // 纯空白行不进历史（否则最大化/还原一次就塞进几十行）
    Terminal blank(10, 8);
    blank.resize(10, 4);
    CHECK_EQ(blank.scrollbackCount(), static_cast<std::size_t>(0));
}

void testTerminalAltScreenAndMisc() {
    Terminal term(10, 3);
    term.feed("main");
    term.feed("\x1b[?1049h");
    term.feed("alt");
    CHECK_EQ(term.plainLine(0), std::string("alt"));

    term.feed("\x1b[?1049l");
    CHECK_EQ(term.plainLine(0), std::string("main"));

    // 光标可见性：备用屏（vim / btop 等 TUI）同样要显示光标，
    // 显不显示完全交给应用的 DECSET/DECRST 25，否则 TUI 里永远没有光标
    CHECK(term.cursorVisible());
    term.feed("\x1b[?25l");
    CHECK(!term.cursorVisible());
    term.feed("\x1b[?25h");
    CHECK(term.cursorVisible());
    term.feed("\x1b[?1049h");
    CHECK(term.cursorVisible());  // 进备用屏也要有光标
    term.feed("\x1b[?25l");
    CHECK(!term.cursorVisible());  // 应用点名隐藏时才隐藏
    term.feed("\x1b[?1049l");
    CHECK(term.cursorVisible());  // 回到主屏恢复可见，不被 TUI 遗留的 ?25l 带走

    // ?1049l 还要把光标恢复到进入前的行：TUI 退出后 shell 的提示符才接着原来的
    // 输入行往下走，而不是停在 vim / less 最后待过的屏幕中间盖住已有内容
    Terminal pos(12, 4);
    pos.feed("one\r\n$ ");
    CHECK_EQ(pos.cursorRow(), 1);
    CHECK_EQ(pos.cursorCol(), 2);
    pos.feed("\x1b[?1049h");
    pos.feed("\x1b[3;7Halt");
    CHECK_EQ(pos.cursorRow(), 2);
    pos.feed("\x1b[?1049l");
    CHECK_EQ(pos.cursorRow(), 1);
    CHECK_EQ(pos.cursorCol(), 2);
    CHECK_EQ(pos.plainLine(0), std::string("one"));

    // ?47l / ?1047l 按 xterm 的语义不动光标：沿用备用屏里的位置
    Terminal keep(12, 4);
    keep.feed("one\r\ntwo");
    keep.feed("\x1b[?47h");
    keep.feed("\x1b[2;1Hx");
    keep.feed("\x1b[?47l");
    CHECK_EQ(keep.cursorRow(), 1);
    CHECK_EQ(keep.cursorCol(), 1);
    CHECK_EQ(keep.plainLine(1), std::string("two"));  // 主屏内容照旧

    // 窗口标题
    term.feed("\x1b]0;hello title\x07");
    CHECK_EQ(term.title(), std::string("hello title"));

    // OSC 7：远端 shell 上报的工作目录（file://host/path），GUI 用它让文件面板跟随 cd
    Terminal cwdTerm(20, 4);
    CHECK(cwdTerm.cwd().empty());
    CHECK(!cwdTerm.takeCwdChange());
    cwdTerm.feed("\x1b]7;file://host/home/user/project\x07");
    CHECK_EQ(cwdTerm.cwd(), std::string("/home/user/project"));
    CHECK(cwdTerm.takeCwdChange());
    CHECK(!cwdTerm.takeCwdChange());  // 同一目录反复上报不重复触发
    cwdTerm.feed("\x1b]7;file:///var/log\x1b\\");  // 空 host + ST 结尾
    CHECK_EQ(cwdTerm.cwd(), std::string("/var/log"));
    CHECK(cwdTerm.takeCwdChange());
    cwdTerm.feed("\x1b]7;file://host/home/a%20b\x07");  // 百分号编码
    CHECK_EQ(cwdTerm.cwd(), std::string("/home/a b"));
    // 标题类 OSC 不影响目录
    cwdTerm.feed("\x1b]2;dircase\x07");
    CHECK_EQ(cwdTerm.title(), std::string("dircase"));
    CHECK_EQ(cwdTerm.cwd(), std::string("/home/a b"));
    // 没有路径的报文忽略
    cwdTerm.feed("\x1b]7;file://host\x07");
    CHECK_EQ(cwdTerm.cwd(), std::string("/home/a b"));
    // 全复位（ESC c）清掉目录：换会话后不会误跟随旧连接的位置
    cwdTerm.feed("\x1b" "c");
    CHECK(cwdTerm.cwd().empty());

    // 注入 OSC 7 钩子的三步序列：第 1 步的 `stty -echo` 一定会被远端 tty 回显，
    // 擦几行由本地终端模型数出来（注入起点 → 当前光标），回到起点行首再清到屏尾
    {
        Terminal hook(40, 6);
        hook.feed("user@host:~$ ");
        const long long startLine =
            static_cast<long long>(hook.scrollbackCount()) + hook.cursorRow();
        hook.feed(" stty -echo\r\n");  // 第 1 步：tty 把命令回显在提示符后面并换行
        const long long nowLine =
            static_cast<long long>(hook.scrollbackCount()) + hook.cursorRow();
        const int erase = static_cast<int>(nowLine - startLine);
        CHECK_EQ(erase, 1);
        hook.feed("\x1b[" + std::to_string(erase) + "A\r\x1b[J");  // 第 2 步开头
        hook.feed("\x1b]7;file://host/home/user\x07");
        hook.feed("\r\x1b[2K");
        hook.feed("user@host:~$ ");  // 第 3 步之后 shell 重画提示符
        CHECK(hook.textRange(0, 0, 5, 40).find("stty") == std::string::npos);
        CHECK_EQ(hook.plainLine(0), std::string("user@host:~$"));  // plainLine 去掉行尾空格
    }

    // 远端在注入期间多打了一行（precmd 输出之类）：ESC[<n>A + ESC[J 一次擦两行
    {
        Terminal hook(40, 6);
        hook.feed("user@host:~$ ");
        const long long startLine =
            static_cast<long long>(hook.scrollbackCount()) + hook.cursorRow();
        hook.feed(" stty -echo\r\n");
        hook.feed("motd line\r\n");  // 远端多出来的一行
        const long long nowLine =
            static_cast<long long>(hook.scrollbackCount()) + hook.cursorRow();
        const int erase = static_cast<int>(nowLine - startLine);
        CHECK_EQ(erase, 2);
        hook.feed("\x1b[" + std::to_string(erase) + "A\r\x1b[J");
        hook.feed("user@host:~$ ");
        CHECK(hook.textRange(0, 0, 5, 40).find("stty") == std::string::npos);
        CHECK(hook.textRange(0, 0, 5, 40).find("motd") == std::string::npos);
    }

    // 注入脚本本身：printf 格式串只能有一层反斜杠。写成 \\033 远端 printf 打印的
    // 就是字面的 "\033[1A"，屏幕上的 stty 残留正是这么留下来的
    {
        const std::string script = buildCwdHookScript(2);
        CHECK_EQ(script.substr(0, 26), std::string("printf '\\033[2A\\r\\033[J'; "));
        CHECK(script.find("\\033]7;file://%s%s\\033\\\\") != std::string::npos);
        CHECK(script.find("\\\\033") == std::string::npos);  // 不该出现双层反斜杠
        CHECK_EQ(buildCwdHookScript(0).substr(0, 24), std::string("printf '\\033[1A\\r\\033[J'"));
        // 脚本里不能夹带真实的 CR / ESC 字节：那是发给远端 printf 的转义文本，
        // 混进真字节会把命令行拆开（bash 掉进 PS2 续行，屏幕上多一个 "> "）
        CHECK(script.find('\r') == std::string::npos);
        CHECK(script.find('\x1b') == std::string::npos);
        const std::string restore = buildCwdHookRestoreEcho();
        CHECK_EQ(restore, std::string("stty echo; printf '\\r\\033[2K'"));
        CHECK(restore.find('\r') == std::string::npos);    // 真正的回车由调用方补
        CHECK(restore.find('\x1b') == std::string::npos);
    }

    // 私有集成确认可跨任意分包；恢复初始化画面时保留新目录、标题和尺寸。
    {
        Terminal integration(24,4);
        integration.feed("LOGIN\r\n$ ");
        Terminal snapshot=integration;
        const std::string ack="\x1b]777;rshell-cwd;42:1\x07";
        for (char byte : ack) integration.feed(std::string(1,byte));
        auto acknowledgements=integration.takeShellIntegrationAcks();
        CHECK_EQ(acknowledgements.size(),static_cast<std::size_t>(1));
        CHECK_EQ(acknowledgements.front(),std::string("42:1"));
        CHECK(integration.takeShellIntegrationAcks().empty());
        integration.feed(std::string(240,'x')+"stty -echo\r\n\x1b]7;file://localhost/new-dir\x07\x1b]0;new-title\x07");
        integration.resize(30,5);
        integration.restoreDisplay(std::move(snapshot));
        CHECK_EQ(integration.cols(),30); CHECK_EQ(integration.rows(),5);
        CHECK_EQ(integration.cwd(),std::string("/new-dir"));
        CHECK_EQ(integration.title(),std::string("new-title"));
        CHECK_EQ(integration.plainLine(0),std::string("LOGIN"));
        for (std::size_t row=0;row<integration.lineCount();++row)
            CHECK(integration.plainLine(row).find("stty")==std::string::npos);
        for (int i=0;i<12;++i) integration.feed(ack);
        CHECK_EQ(integration.takeShellIntegrationAcks().size(),static_cast<std::size_t>(8));
        integration.feed(ack); integration.reset();
        CHECK(integration.takeShellIntegrationAcks().empty());
    }

    // 光标位置查询响应
    term.feed("\x1b[2;3H\x1b[6n");
    CHECK_EQ(term.takeResponse(), std::string("\x1b[2;3R"));

    // 插入/删除行
    Terminal lines(8, 3);
    lines.feed("one\r\ntwo\r\nthree");
    lines.feed("\x1b[1;1H\x1b[L");
    CHECK_EQ(lines.plainLine(0), std::string(""));
    CHECK_EQ(lines.plainLine(1), std::string("one"));

    // 选择文本
    CHECK_EQ(lines.textRange(1, 0, 1, 3), std::string("one"));
}

void testTerminalTuiCompat() {
    // DEC Special Graphics：ESC ( 0 后 l/q/k/x 变成制表符
    Terminal dec(12, 3);
    dec.feed("\x1b(0lqqqk\x1b(Babc");
    CHECK_EQ(dec.plainLine(0),
             std::string("\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x90" "abc"));
    dec.feed("\r\nx");
    CHECK_EQ(dec.plainLine(1), std::string("x"));

    // SO/SI 切换 G1/G0
    Terminal g1(10, 2);
    g1.feed("\x1b)0\x0elqk\x0f" "a");
    CHECK_EQ(g1.plainLine(0), std::string("\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90" "a"));

    // REP（CSI b）重复上一个字符
    Terminal rep(10, 2);
    rep.feed("\xE2\x94\x80\x1b[4b");  // ─ 重复 5 次
    CHECK_EQ(rep.plainLine(0), std::string("\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"));

    // 鼠标 / 光标键 / 焦点上报模式
    Terminal modes(10, 2);
    CHECK(modes.mouseMode() == MouseMode::None);
    modes.feed("\x1b[?1000h");
    CHECK(modes.mouseMode() == MouseMode::X10);
    modes.feed("\x1b[?1002h\x1b[?1006h");
    CHECK(modes.mouseMode() == MouseMode::Button);
    CHECK(modes.mouseSgr());
    modes.feed("\x1b[?1003h");
    CHECK(modes.mouseMode() == MouseMode::Any);
    modes.feed("\x1b[?1003l\x1b[?1000l");
    CHECK(modes.mouseMode() == MouseMode::None);
    CHECK(!modes.applicationCursorKeys());
    modes.feed("\x1b[?1h");
    CHECK(modes.applicationCursorKeys());
    modes.feed("\x1b[?1004h");
    CHECK(modes.focusReporting());

    // VS16：把前一个字符升级为双宽，保持列对齐
    Terminal vs(10, 2);
    vs.feed("\xE2\x9A\xA0\xEF\xB8\x8F" "x");  // ⚠️ + x
    CHECK((vs.lineAt(0)[0].flags & kFlagWide) != 0);
    CHECK((vs.lineAt(0)[1].flags & kFlagWideTail) != 0);
    CHECK_EQ(vs.lineAt(0)[2].ch, static_cast<char32_t>(U'x'));

    // 字符宽度：emoji 双宽，制表符/歧义宽度符号单宽
    CHECK_EQ(Terminal::charWidth(0x1F600), 2);
    CHECK_EQ(Terminal::charWidth(0x26A1), 2);
    CHECK_EQ(Terminal::charWidth(0x2500), 1);
    CHECK_EQ(Terminal::charWidth(0x2502), 1);
    CHECK_EQ(Terminal::charWidth(0x25CB), 1);
    CHECK_EQ(Terminal::charWidth(0x4E2D), 2);
    CHECK_EQ(Terminal::charWidth(0xFE0F), 0);

    // CSI > 4 ; 1 m 是 xterm 的 modifyOtherKeys，不是 SGR；
    // 误当作 SGR 4;1 会把之后所有空白画上下划线（屏幕上像一条条横线）
    Terminal keyMode(10, 2);
    keyMode.feed("\x1b[>4;1m\x1b[1;1H\x1b[38;2;255;255;255m\x1b[49m    ");
    CHECK_EQ(keyMode.lineAt(0)[0].flags & (kFlagUnderline | kFlagBold), 0);
    keyMode.feed("\x1b[0m\x1b[4m\x1b[2;1Hu");
    CHECK((keyMode.lineAt(1)[0].flags & kFlagUnderline) != 0);  // 普通 CSI 4 m 仍然有效

    // DECRQM（CSI ? Ps $ p）报告模式状态，程序无需等超时
    Terminal rqm(10, 2);
    rqm.feed("\x1b[?2026$p");
    CHECK_EQ(rqm.takeResponse(), std::string("\x1b[?2026;0$y"));
    rqm.feed("\x1b[?2004h\x1b[?2004$p");
    CHECK_EQ(rqm.takeResponse(), std::string("\x1b[?2004;1$y"));
    rqm.feed("\x1b[?1006h\x1b[?1006$p");
    CHECK_EQ(rqm.takeResponse(), std::string("\x1b[?1006;1$y"));
    rqm.feed("\x1b[?1049h\x1b[?1049$p");
    CHECK_EQ(rqm.takeResponse(), std::string("\x1b[?1049;1$y"));

    // kitty 键盘查询（CSI ? u）要回复“不支持”，避免程序等超时
    rqm.feed("\x1b[?u");
    CHECK_EQ(rqm.takeResponse(), std::string("\x1b[?0u"));
}

#ifdef _WIN32
// 拖出下载的虚拟文件数据对象：不用真的拖动，直接按资源管理器的调用顺序摸一遍
void testVirtualFileDrag() {
    using namespace rshell::dnd;

    // 自由线程封送要靠 COM，测试里自己起一个 STA
    const bool comReady = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    const std::size_t kSize = 300 * 1024 + 123;
    std::string content(kSize, '\0');
    for (std::size_t i = 0; i < kSize; ++i) content[i] = static_cast<char>((i * 31 + 7) & 0xFF);

    VirtualFile file;
    file.key = "/remote/big.bin";
    file.name = L"big.bin";
    file.size = kSize;
    file.cancel = std::make_shared<std::atomic<bool>>(false);
    const auto cancelFlag = file.cancel;

    // 每个流各自一份读位置：预取线程会和调用方线程同时活着
    ReaderFactory factory = [&content, &file](const std::string& key, std::string* error) {
        Reader reader;
        if (key != file.key) {
            if (error) *error = "未知文件";
            return reader;
        }
        auto pos = std::make_shared<std::size_t>(0);
        reader.size = [&content]() { return static_cast<std::uint64_t>(content.size()); };
        reader.read = [&content, pos](char* buffer, std::size_t capacity, std::string*) {
            const std::size_t left = content.size() - *pos;
            const std::size_t n = std::min(capacity, left);
            if (n > 0) memcpy(buffer, content.data() + *pos, n);
            *pos += n;
            return n;
        };
        return reader;
    };

    std::vector<Event> events;
    EventSink sink = [&events](const Event& event) { events.push_back(event); };

    VirtualFileData* data = new VirtualFileData({file}, factory, sink);

    // 1) 必须能枚举出两种格式：EnumFormatEtc 返回 E_NOTIMPL 时资源管理器直接判「不能放」
    IEnumFORMATETC* formats = nullptr;
    CHECK_EQ(data->EnumFormatEtc(DATADIR_GET, &formats), S_OK);
    CHECK(formats != nullptr);
    int formatCount = 0;
    bool sawDescriptor = false;
    bool sawContents = false;
    if (formats) {
        FORMATETC fmt{};
        ULONG fetched = 0;
        while (formats->Next(1, &fmt, &fetched) == S_OK && fetched == 1) {
            ++formatCount;
            if (fmt.cfFormat == static_cast<CLIPFORMAT>(fileDescriptorFormat()) &&
                fmt.tymed == TYMED_HGLOBAL) {
                sawDescriptor = true;
            }
            if (fmt.cfFormat == static_cast<CLIPFORMAT>(fileContentsFormat()) &&
                fmt.tymed == TYMED_ISTREAM) {
                sawContents = true;
            }
        }
        formats->Release();
    }
    CHECK_EQ(formatCount, 2);
    CHECK(sawDescriptor);
    CHECK(sawContents);

    // 2) 查询格式：内容只认合法下标
    FORMATETC descriptor{};
    descriptor.cfFormat = static_cast<CLIPFORMAT>(fileDescriptorFormat());
    descriptor.dwAspect = DVASPECT_CONTENT;
    descriptor.lindex = -1;
    descriptor.tymed = TYMED_HGLOBAL;
    CHECK_EQ(data->QueryGetData(&descriptor), S_OK);

    FORMATETC contents = descriptor;
    contents.cfFormat = static_cast<CLIPFORMAT>(fileContentsFormat());
    contents.tymed = TYMED_ISTREAM;
    contents.lindex = 0;
    CHECK_EQ(data->QueryGetData(&contents), S_OK);
    contents.lindex = 1;
    CHECK_EQ(data->QueryGetData(&contents), static_cast<HRESULT>(DV_E_LINDEX));
    contents.lindex = 0;

    // 3) 文件名 / 大小描述
    STGMEDIUM medium{};
    CHECK_EQ(data->GetData(&descriptor, &medium), S_OK);
    if (medium.hGlobal) {
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
        CHECK(group != nullptr);
        if (group) {
            CHECK_EQ(group->cItems, 1u);
            CHECK(std::wstring(group->fgd[0].cFileName) == std::wstring(L"big.bin"));
            const std::uint64_t size =
                (static_cast<std::uint64_t>(group->fgd[0].nFileSizeHigh) << 32) |
                static_cast<std::uint64_t>(group->fgd[0].nFileSizeLow);
            CHECK_EQ(size, static_cast<std::uint64_t>(kSize));
            CHECK((group->fgd[0].dwFlags & FD_FILESIZE) != 0);
            CHECK((group->fgd[0].dwFlags & FD_UNICODE) != 0);
            // 不带 FD_PROGRESSUI：进度由 rshell 状态栏负责
            CHECK((group->fgd[0].dwFlags & FD_PROGRESSUI) == 0);
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }

    // 4) 内容流：按不规则块读，拼起来要和远端一致
    STGMEDIUM streamMedium{};
    CHECK_EQ(data->GetData(&contents, &streamMedium), S_OK);
    CHECK(streamMedium.pstm != nullptr);
    std::string received;
    if (streamMedium.pstm) {
        STATSTG stat{};
        CHECK_EQ(streamMedium.pstm->Stat(&stat, STATFLAG_NONAME), S_OK);
        CHECK_EQ(static_cast<std::uint64_t>(stat.cbSize.QuadPart),
                 static_cast<std::uint64_t>(kSize));
        // 复制引擎开始前会先问一次「倒回开头」
        ULARGE_INTEGER moved{};
        LARGE_INTEGER zero{};
        zero.QuadPart = 0;
        CHECK_EQ(streamMedium.pstm->Seek(zero, STREAM_SEEK_SET, &moved), S_OK);

        std::vector<char> chunk(7001);
        for (;;) {
            ULONG got = 0;
            CHECK_EQ(streamMedium.pstm->Read(chunk.data(), static_cast<ULONG>(chunk.size()), &got),
                     S_OK);
            if (got == 0) break;
            received.append(chunk.data(), got);
            if (received.size() > kSize + 16) break;  // 防御：不该无限读
        }
        ReleaseStgMedium(&streamMedium);
    }
    CHECK_EQ(received.size(), kSize);
    CHECK(received == content);

    // 5) 事件顺序：Started/Done 各一次，进度不超过总量
    int started = 0;
    int done = 0;
    for (const Event& event : events) {
        if (event.kind == EventKind::Started) {
            ++started;
            CHECK(event.key == file.key);
            CHECK(event.name == file.name);
            CHECK_EQ(event.total, static_cast<std::uint64_t>(kSize));
        }
        if (event.kind == EventKind::Progress) CHECK(event.done <= kSize);
        if (event.kind == EventKind::Done) {
            ++done;
            CHECK_EQ(event.done, static_cast<std::uint64_t>(kSize));
        }
    }
    CHECK_EQ(started, 1);
    CHECK_EQ(done, 1);

    // 6) 取消：置标志后下一次 Read 立刻中断，并抛 Cancelled
    events.clear();
    STGMEDIUM cancelled{};
    CHECK_EQ(data->GetData(&contents, &cancelled), S_OK);
    if (cancelled.pstm) {
        cancelFlag->store(true);
        ULONG got = 0;
        std::vector<char> chunk(4096);
        CHECK_EQ(cancelled.pstm->Read(chunk.data(), 4096, &got), static_cast<HRESULT>(E_ABORT));
        ReleaseStgMedium(&cancelled);
    }
    CHECK_EQ(events.size(), static_cast<std::size_t>(1));
    if (!events.empty()) CHECK(events.front().kind == EventKind::Cancelled);

    // 7) 必须带自由线程封送：跨进程拖放时复制引擎不在创建对象的线程上调 Read。
    //    少了它，调用会被封送回 UI 线程的消息循环，整段下载期间窗口都没响应。
    IUnknown* agile = nullptr;
    if (comReady) {
        CHECK_EQ(data->QueryInterface(IID_IMarshal, reinterpret_cast<void**>(&agile)), S_OK);
        if (agile) agile->Release();
    }

    // 8) 把流封送到另一个线程（MTA）去读：内容一致，说明调用方线程上不需要
    //    任何 UI 消息泵，也就不会拖住窗口。
    cancelFlag->store(false);
    events.clear();
    STGMEDIUM crossMedium{};
    CHECK_EQ(data->GetData(&contents, &crossMedium), S_OK);
    if (crossMedium.pstm) {
        IStream* marshaled = nullptr;
        CHECK_EQ(CoMarshalInterThreadInterfaceInStream(IID_IStream, crossMedium.pstm, &marshaled),
                 S_OK);
        std::string received2;
        ULONG readError = 0;
        std::thread worker([&]() {
            if (!comReady) return;
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
            IStream* other = nullptr;
            if (SUCCEEDED(CoGetInterfaceAndReleaseStream(marshaled, IID_IStream,
                                                         reinterpret_cast<void**>(&other))) &&
                other) {
                std::vector<char> buf(7777);
                for (;;) {
                    ULONG got = 0;
                    const HRESULT hr = other->Read(buf.data(), static_cast<ULONG>(buf.size()), &got);
                    if (FAILED(hr)) {
                        readError = static_cast<ULONG>(hr);
                        break;
                    }
                    if (got == 0) break;
                    received2.append(buf.data(), got);
                    if (received2.size() > kSize + 16) break;
                }
                other->Release();
            }
            CoUninitialize();
        });
        worker.join();
        CHECK_EQ(readError, 0u);
        CHECK_EQ(received2.size(), kSize);
        CHECK(received2 == content);
        ReleaseStgMedium(&crossMedium);
    }

    // 9) Read 不能等整个下载：后台给出 1000 字节后就卡在「网络」上，
    //    第一次 Read 也必须立刻带着这 1000 字节返回。旧实现在这里同步等到底，
    //    UI 线程就是这样被按住整段下载时长的。
    {
        std::mutex gateMutex;
        std::condition_variable gate;
        bool gateOpen = false;
        auto calls = std::make_shared<int>(0);
        ReaderFactory slow = [&gateMutex, &gate, &gateOpen, calls](const std::string&,
                                                                  std::string*) {
            Reader reader;
            reader.size = []() { return static_cast<std::uint64_t>(5000); };
            reader.read = [&gateMutex, &gate, &gateOpen, calls](char* buffer, std::size_t capacity,
                                                               std::string*) {
                const int call = ++*calls;
                if (call == 1) {
                    const std::size_t n = std::min<std::size_t>(1000, capacity);
                    memset(buffer, 'x', n);
                    return n;
                }
                {
                    std::unique_lock<std::mutex> lock(gateMutex);
                    gate.wait(lock, [&gateOpen] { return gateOpen; });
                }
                if (call == 2) {
                    const std::size_t n = std::min<std::size_t>(4000, capacity);
                    memset(buffer, 'y', n);
                    return n;
                }
                return std::size_t{0};
            };
            return reader;
        };

        VirtualFile slowFile = file;
        slowFile.size = 5000;
        VirtualFileData* slowData = new VirtualFileData({slowFile}, slow, sink);
        STGMEDIUM slowMedium{};
        CHECK_EQ(slowData->GetData(&contents, &slowMedium), S_OK);
        if (slowMedium.pstm) {
            std::vector<char> buf(4096);
            ULONG got = 0;
            CHECK_EQ(slowMedium.pstm->Read(buf.data(), 4096, &got), S_OK);
            CHECK_EQ(got, 1000u);  // 没有被后台卡住
            std::size_t total = got;
            {
                std::lock_guard<std::mutex> lock(gateMutex);
                gateOpen = true;
            }
            gate.notify_all();
            for (;;) {
                ULONG n = 0;
                const HRESULT hr = slowMedium.pstm->Read(buf.data(), static_cast<ULONG>(buf.size()), &n);
                if (FAILED(hr) || n == 0) break;
                total += n;
            }
            CHECK_EQ(total, static_cast<std::size_t>(5000));
            ReleaseStgMedium(&slowMedium);
        }
        CHECK_EQ(slowData->Release(), 0u);
    }

    // 10) 必须实现 IDataObjectAsyncCapability：放置目标据此把取数挪到后台线程，
    //     然后立刻从 Drop 返回。缺了它，资源管理器会在 Drop 里同步取完数据，
    //     发起方的 DoDragDrop 就一直不返回——UI 整个下载期间都卡住（就是这里
    //     要修的 bug）。协议细节：DoDragDrop 前后用 InOperation 判断是否还在传。
    if (comReady) {
        IDataObjectAsyncCapability* async = nullptr;
        CHECK_EQ(data->QueryInterface(IID_IDataObjectAsyncCapability,
                                      reinterpret_cast<void**>(&async)),
                 S_OK);
        if (async) {
            BOOL mode = FALSE;
            CHECK_EQ(async->GetAsyncMode(&mode), S_OK);
            CHECK_EQ(mode, TRUE);  // 默认就允许目标后台取数
            CHECK_EQ(async->SetAsyncMode(FALSE), S_OK);
            CHECK_EQ(async->GetAsyncMode(&mode), S_OK);
            CHECK_EQ(mode, FALSE);
            CHECK_EQ(async->SetAsyncMode(TRUE), S_OK);

            BOOL running = TRUE;
            CHECK_EQ(async->InOperation(&running), S_OK);
            CHECK_EQ(running, FALSE);  // 还没开始
            CHECK_EQ(async->StartOperation(nullptr), S_OK);
            CHECK_EQ(async->InOperation(&running), S_OK);
            CHECK_EQ(running, TRUE);  // Drop 已经返回，但后台还在传
            CHECK_EQ(async->EndOperation(S_OK, nullptr, DROPEFFECT_COPY), S_OK);
            CHECK_EQ(async->InOperation(&running), S_OK);
            CHECK_EQ(running, FALSE);  // 传完了
            CHECK_EQ(async->Release(), 1u);  // 只剩 data 自己那一条
        }
    }

    CHECK_EQ(data->Release(), 0u);
    // 流都放掉之后不该还留着：留下的就是没关的远端句柄
    CHECK_EQ(liveStreamCount(), 0);

    if (comReady) CoUninitialize();
}
#endif  // _WIN32


void testPasteNormalize() {
    // Windows 剪贴板是 CRLF，远端 shell 只要一次回车：CRLF 不能变成两个 \r
    CHECK_EQ(normalizePasteText("a\r\nb", false), std::string("a\rb"));
    CHECK_EQ(normalizePasteText("ls -l\r\ncd /tmp\r\n", false), std::string("ls -l\rcd /tmp\r"));
    // 只有 LF（从 Linux 复制）同样归一成 \r
    CHECK_EQ(normalizePasteText("a\nb", false), std::string("a\rb"));
    // 只有 CR 保持原样，空行折叠成单个回车
    CHECK_EQ(normalizePasteText("a\rb", false), std::string("a\rb"));
    CHECK_EQ(normalizePasteText("a\r\n\r\nb", false), std::string("a\r\rb"));
    CHECK_EQ(normalizePasteText("", false), std::string(""));
    // 制表符、多字节 UTF-8 原样保留
    CHECK_EQ(normalizePasteText("a\tb\xE4\xB8\xAD", false), std::string("a\tb\xE4\xB8\xAD"));
    // 普通粘贴保留 ESC（用户可能就想发颜色码）
    CHECK_EQ(normalizePasteText("\x1b[31mred\x1b[0m", false),
             std::string("\x1b[31mred\x1b[0m"));
    // 括号粘贴时剥掉 ESC：否则内容自带 ESC[201~ 就能提前结束粘贴，后面的文本直接被执行
    CHECK_EQ(normalizePasteText("\x1b[201~rm -rf ~", true), std::string("[201~rm -rf ~"));
    CHECK_EQ(normalizePasteText("ok\r\nnext", true), std::string("ok\rnext"));

    // 归一化后喂进终端：CRLF 只换一次行，粘贴多行命令不会多出空行。
    // 终端模型只认 \n（远端的 ICRNL/ONLCR 会把我们发的 \r 变成一次换行），
    // 这里按远端的实际渲染方式把一个 \r 展开成 "\r\n" 再喂。
    Terminal pasteTerm(20, 8);
    pasteTerm.feed("$ ");
    std::string echoed;
    for (char c : normalizePasteText("ls\r\npwd\r\n", false)) {
        if (c == '\r') {
            echoed += "\r\n";
        } else {
            echoed.push_back(c);
        }
    }
    pasteTerm.feed(echoed);
    CHECK_EQ(pasteTerm.cursorRow(), 2);
    CHECK_EQ(pasteTerm.plainLine(0), std::string("$ ls"));
    CHECK_EQ(pasteTerm.plainLine(1), std::string("pwd"));
}

#ifdef _WIN32
UINT WINAPI fakeSystemDpi() { return 144; }
UINT WINAPI fakeWindowDpi(HWND) { return 192; }
UINT WINAPI zeroDpi() { return 0; }
UINT WINAPI zeroWindowDpi(HWND) { return 0; }
int WINAPI fakeMetric(int, UINT dpi) { return static_cast<int>(dpi / 12); }
int WINAPI zeroMetric(int, UINT) { return 0; }
BOOL WINAPI fakeDpiContext(HANDLE context) {
    CHECK(context == reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
    return TRUE;
}

void testWindowsCompatibility() {
    // 空函数表模拟 Win7，避免仅在新系统上测试可用 API 的分支。
    wincompat::Api legacy;
    wincompat::initializeDpi(legacy);
    const UINT fallback = wincompat::systemDpi(legacy);
    CHECK(fallback > 0);
    CHECK_EQ(wincompat::windowDpi(nullptr, legacy), fallback);
    CHECK_EQ(wincompat::systemMetric(SM_CXSIZEFRAME, fallback, legacy), GetSystemMetrics(SM_CXSIZEFRAME));
    CHECK_EQ(wincompat::systemMetric(SM_CXSIZEFRAME, fallback * 2, legacy), GetSystemMetrics(SM_CXSIZEFRAME) * 2);
    CHECK(legacy.setMitigation == nullptr);

    wincompat::Api modern;
    modern.setDpiContext = fakeDpiContext;
    modern.getSystemDpi = fakeSystemDpi;
    modern.getWindowDpi = fakeWindowDpi;
    modern.getMetric = fakeMetric;
    wincompat::initializeDpi(modern);
    CHECK_EQ(wincompat::systemDpi(modern), 144u);
    CHECK_EQ(wincompat::windowDpi(nullptr, modern), 192u);
    CHECK_EQ(wincompat::systemMetric(SM_CXSIZEFRAME, 192, modern), 16);
    modern.getSystemDpi = zeroDpi;
    modern.getWindowDpi = zeroWindowDpi;
    CHECK_EQ(wincompat::systemDpi(modern), fallback);
    CHECK_EQ(wincompat::windowDpi(nullptr, modern), fallback);
    modern.getMetric = zeroMetric;
    CHECK_EQ(wincompat::systemMetric(SM_CXPADDEDBORDER, 96, modern), 0);
    CHECK(wincompat::systemDpi() > 0);
}
#endif

void testTerminalSearch() {
    Terminal term(24, 3, 8);
    CHECK(term.findAll(U"").empty());
    CHECK(term.findAll(U" ").empty());
    term.feed("banana BANANA\r\n中文🙂test\r\nlast");
    const auto overlap = term.findAll(U"ana");
    CHECK_EQ(overlap.size(), static_cast<std::size_t>(2));
    CHECK_EQ(overlap[0].column, 1);
    CHECK_EQ(overlap[1].column, 3);
    CHECK_EQ(overlap[0].width, 3);
    CHECK_EQ(term.findAll(U"BANANA").size(), static_cast<std::size_t>(1));
    const auto wide = term.findAll(U"文🙂t");
    CHECK_EQ(wide.size(), static_cast<std::size_t>(1));
    CHECK_EQ(wide[0].line, static_cast<std::size_t>(1));
    CHECK_EQ(wide[0].column, 2);
    CHECK_EQ(wide[0].width, 5);
    CHECK(term.findAll(U"missing").empty());
    term.feed("\r\nnext\r\nfinal");
    CHECK(term.scrollbackCount() > 0);
    CHECK_EQ(term.findAll(U"banana")[0].line, static_cast<std::size_t>(0));
    CHECK_EQ(term.findAll(U"final").size(), static_cast<std::size_t>(1));
    term.reset();
    CHECK(term.findAll(U"banana").empty());
    term.feed("\x1b[31mred\x1b[0m");
    CHECK_EQ(term.findAll(U"red")[0].column, 0);
    term.feed("\x1b[?1049halt");
    CHECK(term.findAll(U"red").empty());
    CHECK_EQ(term.findAll(U"alt").size(), static_cast<std::size_t>(1));
    term.feed("\x1b[?1049l");
    CHECK_EQ(term.findAll(U"red").size(), static_cast<std::size_t>(1));
    Terminal limited(10, 2, 1);
    limited.feed("old\r\na\r\nb\r\nc");
    CHECK(limited.findAll(U"old").empty());
}

}  // namespace

int main() {
    initConsole();

    testStrings();
    testPasteNormalize();
    testRemotePaths();
    testGlob();
    testFormatting();
    testRemoteTarget();
    testModeParsing();
    testSshConfig();
    testRshellSettings();
    testSessionStorePasswordFlag();
    testPrivateKeyNeedsPassphrase();
    testCredentialStore();
    testCredentialVaultRouting();
    testProtectedSecret();
    testVault();
    testTerminalBasics();
    testTerminalColors();
    testTerminalWideAndScrollback();
    testTerminalResizeKeepsContent();
    testTerminalAltScreenAndMisc();
    testTerminalTuiCompat();
    testTerminalSearch();
#ifdef _WIN32
    testWindowsCompatibility();
    testVirtualFileDrag();
#endif

    std::cout << (g_failed == 0 ? "PASS" : "FAIL") << ": " << (g_checks - g_failed) << "/"
              << g_checks << " 项检查通过\n";
    return g_failed == 0 ? 0 : 1;
}
