// cli.cpp - 命令行解析与子命令实现
#include "cli.h"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "config.h"
#include "sftp_client.h"
#include "sftp_shell.h"
#include "ssh_session.h"
#include "util.h"

namespace rshell {

namespace {

constexpr const char* kVersion = "1.0.0";

bool isKnownCommand(const std::string& s) {
    static const char* kCommands[] = {"connect", "shell", "ssh",  "exec", "run",   "sftp",
                                      "put",     "get",   "ls",   "mkdir", "rm",   "rename",
                                      "chmod",   "help",  "version", "upload", "download"};
    for (const char* cmd : kCommands) {
        if (s == cmd) return true;
    }
    return false;
}

std::string nextValue(int& i, int argc, char** argv, const std::string& name) {
    if (i + 1 >= argc) throw Error("选项 " + name + " 缺少参数");
    return argv[++i];
}

int parsePort(const std::string& text) {
    try {
        size_t used = 0;
        int port = std::stoi(text, &used);
        if (used != text.size() || port <= 0 || port > 65535) throw std::invalid_argument("range");
        return port;
    } catch (...) {
        throw Error("无效的端口：" + text);
    }
}

int parsePositive(const std::string& text, const std::string& what) {
    try {
        size_t used = 0;
        int value = std::stoi(text, &used);
        if (used != text.size() || value <= 0) throw std::invalid_argument("bad");
        return value;
    } catch (...) {
        throw Error("无效的" + what + "：" + text);
    }
}

void parseCommandLine(int argc, char** argv, Options& opt) {
    bool afterDoubleDash = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (afterDoubleDash) {
            opt.args.push_back(arg);
            continue;
        }
        if (arg == "--") {
            afterDoubleDash = true;
            continue;
        }
        if (arg.size() > 1 && arg[0] == '-') {
            if (arg == "-h" || arg == "--help") {
                opt.help = true;
            } else if (arg == "-V" || arg == "--version") {
                opt.version = true;
            } else if (arg == "-v" || arg == "--verbose") {
                opt.verbose = true;
            } else if (arg == "-q" || arg == "--quiet") {
                opt.quiet = true;
            } else if (arg == "-H" || arg == "--host") {
                opt.host = nextValue(i, argc, argv, arg);
            } else if (arg == "-u" || arg == "--user") {
                opt.user = nextValue(i, argc, argv, arg);
            } else if (arg == "-p" || arg == "--port") {
                opt.port = parsePort(nextValue(i, argc, argv, arg));
            } else if (arg == "-i" || arg == "--identity") {
                opt.keyPath = nextValue(i, argc, argv, arg);
            } else if (arg == "--password") {
                opt.password = nextValue(i, argc, argv, arg);
            } else if (arg == "--keepalive") {
                opt.keepAliveSec = parsePositive(nextValue(i, argc, argv, arg), "保活间隔");
            } else if (arg == "--proxy") {
                opt.proxyUrl = nextValue(i, argc, argv, arg);
            } else if (arg == "-F" || arg == "--config") {
                opt.configPath = nextValue(i, argc, argv, arg);
            } else if (arg == "--timeout") {
                opt.timeoutSec = parsePositive(nextValue(i, argc, argv, arg), "超时时间");
            } else if (arg == "--strict-host-key") {
                opt.strictHostKey = true;
            } else if (arg == "--insecure" || arg == "--no-host-key-check") {
                opt.skipHostKeyCheck = true;
            } else if (arg == "-r" || arg == "--recursive") {
                opt.recursive = true;
            } else if (arg == "-f" || arg == "--force" || arg == "--overwrite") {
                opt.overwrite = true;
            } else if (arg == "--resume") {
                opt.resume = true;
            } else if (arg == "--preserve") {
                opt.preserve = true;
            } else if (arg == "--no-progress") {
                opt.noProgress = true;
            } else if (arg == "-l" || arg == "--long") {
                opt.longFormat = true;
            } else if (arg == "-a" || arg == "--all") {
                opt.showAll = true;
            } else if (arg == "-d" || arg == "--dirs") {
                opt.dirsOnly = true;
            } else if (arg == "-1" || arg == "--one-per-line") {
                opt.onePerLine = true;
            } else if (arg == "--parents" || arg == "-P") {
                opt.parents = true;
            } else if (arg == "--mode" || arg == "--chmod") {
                opt.chmodMode = static_cast<int>(parseModeString(nextValue(i, argc, argv, arg)));
            } else if (arg == "--mkdir-mode") {
                opt.mkdirMode = static_cast<int>(parseModeString(nextValue(i, argc, argv, arg)));
            } else if (arg == "-n" || arg == "--dry-run") {
                opt.dryRun = true;
            } else {
                throw Error("未知选项：" + arg + "（用 --help 查看用法）");
            }
            continue;
        }

        if (opt.command.empty() && isKnownCommand(arg)) {
            opt.command = arg;
        } else if (opt.command.empty() && opt.host.empty()) {
            opt.host = arg;  // 形如 rshell myhost "uname -a"
        } else {
            opt.args.push_back(arg);
        }
    }
}

void printUsageText(bool full) {
    std::cout
        << "rshell " << kVersion << " - SSH 远程执行与 SFTP 文件传输\n\n"
        << "用法：\n"
        << "  rshell [选项] <主机> [命令...]        登录交互 shell 或直接执行命令\n"
        << "  rshell [选项] connect [主机]          打开交互 shell\n"
        << "  rshell [选项] exec <命令...>          执行单条命令并返回退出码\n"
        << "  rshell [选项] sftp [主机]             打开交互式 sftp\n"
        << "  rshell [选项] put <本地> [远端]       上传文件/目录\n"
        << "  rshell [选项] get <远端> [本地]       下载文件（支持通配符）\n"
        << "  rshell [选项] ls [-l] [远端...]       列出远端目录\n"
        << "  rshell [选项] mkdir [--parents] <远端...>\n"
        << "  rshell [选项] rm [-r] <远端...>       删除远端文件/目录\n"
        << "  rshell [选项] rename <旧> <新>        重命名\n"
        << "  rshell [选项] chmod <模式> <远端...>  修改权限\n\n"
        << "连接选项：\n"
        << "  -H, --host <主机>        主机名、IP 或 ~/.ssh/config 中的别名\n"
        << "  -u, --user <用户>        登录用户\n"
        << "  -p, --port <端口>        SSH 端口（默认 22）\n"
        << "  -i, --identity <私钥>    私钥文件\n"
        << "      --password <口令>    登录密码（不推荐，会留在历史记录里）\n"
        << "  -F, --config <文件>      指定 ssh_config\n"
        << "      --keepalive <秒>      空闲时发送 SSH 保活消息（0 关闭）\n"
        << "      --proxy <URL>        经代理连接：socks5://[用户:口令@]主机:端口 或 http://...\n"
        << "      --timeout <秒>       连接超时（默认 20）\n"
        << "      --strict-host-key    未知主机直接拒绝\n"
        << "      --insecure           跳过 known_hosts 校验（不安全）\n\n"
        << "传输/列表选项：\n"
        << "  -r, --recursive          递归处理目录\n"
        << "  -f, --force              覆盖已存在的文件\n"
        << "      --resume             断点续传\n"
        << "      --preserve           保留文件权限\n"
        << "      --no-progress        不显示进度条\n"
        << "  -l, --long               ls 显示详细信息\n"
        << "  -d, --dirs               只列出目录\n"
        << "      --parents             mkdir 自动创建父目录\n"
        << "      --mode <八进制>       上传后设置远端权限\n"
        << "  -n, --dry-run            只打印将执行的操作\n\n"
        << "其他：\n"
        << "  -v, --verbose            输出调试信息\n"
        << "  -q, --quiet              只输出错误\n"
        << "  -h, --help               显示帮助\n"
        << "  -V, --version            显示版本\n";

    if (full) {
        std::cout << "\n示例：\n"
                  << "  rshell webbox                      # 使用 ~/.ssh/config 中的别名\n"
                  << "  rshell deploy@10.0.0.5 \"systemctl status nginx\"\n"
                  << "  rshell -p 2222 put ./app.tar.gz /opt/app/\n"
                  << "  rshell get \"/var/log/*.log\" ./logs/\n"
                  << "  rshell ls -l /etc/nginx\n";
    }
}

// ---------------------------------------------------------------- 连接解析

struct ResolvedConnection {
    ConnectionOptions options;
    std::string label;  // user@host:port
};

ResolvedConnection resolveConnection(const Options& opt) {
    RshellSettings settings = loadRshellSettings();
    SshConfig sshConfig = SshConfig::load(opt.configPath);

    RemoteTarget target = parseRemoteTarget(opt.host);

    HostConfig hostConfig;
    bool matched = false;
    if (!target.host.empty()) matched = sshConfig.lookup(target.host, hostConfig);

    ResolvedConnection resolved;
    ConnectionOptions& conn = resolved.options;

    if (opt.keepAliveSec > 0) {
        conn.keepAliveSec = std::clamp(opt.keepAliveSec, 5, 3600);
    }
    if (!opt.proxyUrl.empty()) {
        std::string proxyError;
        if (!parseProxyUrl(opt.proxyUrl, conn.proxy, proxyError))
            throw Error("代理参数无效：" + proxyError);
    }

    // 主机名：ssh_config 的 HostName 优先，否则用用户输入（可能是 IP）
    if (matched && !hostConfig.hostName.empty()) {
        conn.host = hostConfig.hostName;
    } else {
        conn.host = target.host;
    }

    auto aliasIt = settings.aliases.find(target.host);
    if (!matched && aliasIt != settings.aliases.end()) {
        const RshellSettings::Alias& alias = aliasIt->second;
        if (!alias.host.empty()) conn.host = alias.host;
    }

    if (!target.user.empty()) {
        conn.user = target.user;
    } else if (!opt.user.empty()) {
        conn.user = opt.user;
    } else if (matched && !hostConfig.user.empty()) {
        conn.user = hostConfig.user;
    } else if (!matched && aliasIt != settings.aliases.end() && !aliasIt->second.user.empty()) {
        conn.user = aliasIt->second.user;
    } else {
        conn.user = settings.defaultUser;
    }

    if (opt.port > 0) {
        conn.port = opt.port;
    } else if (matched && hostConfig.port > 0) {
        conn.port = hostConfig.port;
    } else if (!matched && aliasIt != settings.aliases.end() && aliasIt->second.port > 0) {
        conn.port = aliasIt->second.port;
    } else if (settings.defaultPort > 0) {
        conn.port = settings.defaultPort;
    }

    if (!opt.keyPath.empty()) {
        conn.keyPath = expandPath(opt.keyPath);
    } else if (matched && !hostConfig.identityFile.empty()) {
        conn.keyPath = hostConfig.identityFile;
    } else if (!matched && aliasIt != settings.aliases.end() && !aliasIt->second.key.empty()) {
        conn.keyPath = aliasIt->second.key;
    } else {
        conn.keyPath = settings.defaultKey;
    }

    conn.password = opt.password;
    conn.verifyHostKey = !opt.skipHostKeyCheck;
    conn.strictHostKey = opt.strictHostKey;
    conn.timeoutSec = opt.timeoutSec > 0 ? opt.timeoutSec : settings.timeoutSec;

    if (conn.host.empty()) {
        throw Error("未指定主机：用 -H <主机> 或 rshell <主机> 形式调用");
    }
    if (conn.user.empty()) {
        conn.user = readLine("用户名 (" + conn.host + ")：");
        if (conn.user.empty()) throw Error("必须指定登录用户");
    }

    std::ostringstream label;
    label << conn.user << "@" << conn.host;
    if (conn.port != 22) label << ":" << conn.port;
    resolved.label = label.str();
    return resolved;
}

// 打开会话并执行 fn；保证析构时断开
int withSession(const ResolvedConnection& resolved, const std::function<int(SshSession&)>& fn) {
    SshSession session(resolved.options);
    session.connect();
    if (log().verbose()) {
        log().info("已连接 " + resolved.label + "（" + session.serverBanner() + "）");
        log().info("主机密钥：" + session.hostKeyAlgorithm() + " " + session.fingerprint());
        log().info("密钥交换：" + session.keyExchange());
    }
    int code = fn(session);
    session.disconnect();
    return code;
}

// ---------------------------------------------------------------- 传输辅助

TransferOptions transferOptions(const Options& opt) {
    TransferOptions t;
    t.overwrite = opt.overwrite;
    t.resume = opt.resume;
    t.preserveMode = opt.preserve;
    t.showProgress = !opt.noProgress && !opt.quiet;
    t.mode = opt.chmodMode;
    return t;
}

void uploadTree(SftpClient& sftp, const fs::path& localDir, const std::string& remoteDir,
                const TransferOptions& options) {
    sftp.mkdir(remoteDir, 0755, true);
    std::error_code ec;
    for (fs::recursive_directory_iterator it(localDir, ec), end; it != end; it.increment(ec)) {
        if (ec) throw Error("遍历本地目录失败：" + ec.message());
        const fs::path& path = it->path();
        fs::path rel = fs::relative(path, localDir, ec);
        if (ec) throw Error("计算相对路径失败：" + ec.message());
        std::string remote = remoteJoin(remoteDir, replaceAll(pathToUtf8(rel), "\\", "/"));
        if (it->is_directory(ec)) {
            sftp.mkdir(remote, 0755, true);
        } else if (it->is_regular_file(ec)) {
            sftp.upload(pathToUtf8(path), remote, options);
        }
    }
}

void downloadTree(SftpClient& sftp, const std::string& remoteDir, const fs::path& localDir,
                  const TransferOptions& options) {
    std::error_code ec;
    fs::create_directories(localDir, ec);
    if (ec) throw Error("无法创建本地目录 " + pathToUtf8(localDir) + "：" + ec.message());

    for (const SftpEntry& entry : sftp.listDir(remoteDir)) {
        fs::path out = localDir / utf8ToPath(entry.name);
        if (entry.isDir()) {
            downloadTree(sftp, entry.path, out, options);
        } else if (entry.isFile()) {
            sftp.download(entry.path, pathToUtf8(out), options);
        } else {
            log().debug("跳过特殊文件：" + entry.path);
        }
    }
}

void removeTree(SftpClient& sftp, const std::string& path) {
    for (const SftpEntry& entry : sftp.listDir(path)) {
        if (entry.isDir()) {
            removeTree(sftp, entry.path);
        } else {
            sftp.removeFile(entry.path);
        }
    }
    sftp.rmdir(path);
}

// ---------------------------------------------------------------- 子命令

int cmdExec(SshSession& session, const Options&, const std::vector<std::string>& args) {
    if (args.empty()) throw Error("用法：rshell exec <命令...>");
    return session.execute(join(args, " "));
}

int cmdShell(SshSession& session, const Options&, const std::vector<std::string>& args) {
    // rshell <主机> <命令...> 的兼容形式
    if (!args.empty()) return session.execute(join(args, " "));
    return session.interactiveShell();
}

int cmdSftp(SshSession& session, const Options&, const std::vector<std::string>&) {
    SftpClient sftp(session);
    sftp.open();
    const ConnectionOptions& conn = session.options();
    return runSftpShell(sftp, conn.user + "@" + conn.host);
}

int cmdPut(SshSession& session, const Options& opt, const std::vector<std::string>& args) {
    if (args.empty()) throw Error("用法：rshell put <本地> [远端]");

    const std::string localSpec = expandPath(args[0]);
    const fs::path localPath = utf8ToPath(localSpec);
    std::error_code ec;
    if (!fs::exists(localPath, ec)) throw Error("本地路径不存在：" + localSpec);

    const std::string remoteSpec = args.size() > 1 ? args[1] : std::string();
    const TransferOptions options = transferOptions(opt);

    SftpClient sftp(session);
    sftp.open();

    if (fs::is_directory(localPath, ec)) {
        if (!opt.recursive) throw Error("" + localSpec + " 是目录，请加 -r/--recursive");
        const std::string localBase = pathToUtf8(localPath.filename());
        std::string remoteBase = remoteSpec.empty() ? localBase : remoteSpec;
        SftpEntry existing;
        if (remoteSpec.empty()) {
            remoteBase = localBase;
        } else if (sftp.stat(remoteBase, existing) && existing.isDir()) {
            remoteBase = remoteJoin(remoteBase, localBase);
        }
        if (opt.dryRun) {
            std::cout << "将上传目录 " << localSpec << " -> " << remoteBase << "\n";
            return 0;
        }
        uploadTree(sftp, localPath, remoteBase, options);
        if (!opt.quiet) std::cerr << green("已上传目录 " + localSpec + " -> " + remoteBase) << "\n";
        return 0;
    }

    const std::string target =
        remoteSpec.empty() ? localBaseName(localSpec) : sftp.resolveRemoteTarget(remoteSpec, localSpec);
    if (opt.dryRun) {
        std::cout << "将上传 " << localSpec << " -> " << target << "\n";
        return 0;
    }
    TransferResult result = sftp.upload(localSpec, target, options);
    if (!opt.quiet) {
        std::cerr << green("已上传 " + localSpec + " -> " + target + "（" + formatBytes(result.bytes) +
                          "，" + formatRate(result.rate) + "）")
                  << "\n";
    }
    return 0;
}

int cmdGet(SshSession& session, const Options& opt, const std::vector<std::string>& args) {
    if (args.empty()) throw Error("用法：rshell get <远端> [本地]");

    const std::string remoteSpec = args[0];
    const TransferOptions options = transferOptions(opt);
    RshellSettings settings = loadRshellSettings();

    std::string localSpec = args.size() > 1 ? args[1] : std::string();
    if (localSpec.empty()) localSpec = settings.downloadDir.empty() ? "." : settings.downloadDir;
    const fs::path localBase = utf8ToPath(expandPath(localSpec));

    SftpClient sftp(session);
    sftp.open();

    std::vector<SftpEntry> entries = sftp.globRemote(remoteSpec);
    if (entries.empty()) throw Error("远端路径不存在：" + remoteSpec);

    std::error_code ec;
    const bool localIsDir = fs::is_directory(localBase, ec);
    const bool multiple = entries.size() > 1;
    if (multiple && !localIsDir && fs::exists(localBase, ec))
        throw Error("匹配到多个远端文件，本地目标必须是目录：" + localSpec);

    for (const SftpEntry& entry : entries) {
        fs::path out = localBase;
        if (localIsDir || multiple) out /= utf8ToPath(entry.name);

        if (entry.isDir()) {
            if (!opt.recursive) throw Error(entry.path + " 是目录，请加 -r/--recursive");
            if (opt.dryRun) {
                std::cout << "将下载目录 " << entry.path << " -> " << pathToUtf8(out) << "\n";
                continue;
            }
            downloadTree(sftp, entry.path, out, options);
            if (!opt.quiet)
                std::cerr << green("已下载目录 " + entry.path + " -> " + pathToUtf8(out)) << "\n";
            continue;
        }

        if (opt.dryRun) {
            std::cout << "将下载 " << entry.path << " -> " << pathToUtf8(out) << "\n";
            continue;
        }
        TransferResult result = sftp.download(entry.path, pathToUtf8(out), options);
        if (!opt.quiet) {
            std::cerr << green("已下载 " + entry.path + " -> " + pathToUtf8(out) + "（" +
                               formatBytes(result.bytes) + "）")
                      << "\n";
        }
    }
    return 0;
}

int cmdLs(SshSession& session, const Options& opt, const std::vector<std::string>& args) {
    SftpClient sftp(session);
    sftp.open();

    std::vector<std::string> paths = args.empty() ? std::vector<std::string>{"."} : args;
    const bool showHeader = paths.size() > 1;
    int code = 0;

    for (const std::string& path : paths) {
        std::vector<SftpEntry> entries;
        try {
            entries = sftp.listTarget(path);
        } catch (const Error& ex) {
            std::cerr << red(ex.what()) << "\n";
            code = 1;
            continue;
        }
        if (showHeader && !entries.empty()) std::cout << path << ":\n";
        for (const SftpEntry& entry : entries) {
            if (opt.dirsOnly && !entry.isDir()) continue;
            if (opt.longFormat) {
                std::cout << formatMode(entry.mode) << " " << formatTimestamp(entry.mtime) << " "
                          << entry.size << "\t" << entry.name;
            } else {
                std::cout << entry.name;
            }
            std::cout << "\n";
        }
    }
    return code;
}

int cmdMkdir(SshSession& session, const Options& opt, const std::vector<std::string>& args) {
    if (args.empty()) throw Error("用法：rshell mkdir <远端...>");
    if (opt.dryRun) {
        for (const std::string& path : args) std::cout << "将创建目录 " << path << "\n";
        return 0;
    }
    SftpClient sftp(session);
    sftp.open();
    for (const std::string& path : args) {
        sftp.mkdir(path, opt.mkdirMode, opt.parents);
        log().debug("已创建目录：" + path);
    }
    return 0;
}

int cmdRm(SshSession& session, const Options& opt, const std::vector<std::string>& args) {
    if (args.empty()) throw Error("用法：rshell rm <远端...>");
    SftpClient sftp(session);
    sftp.open();

    for (const std::string& path : args) {
        std::vector<SftpEntry> entries = sftp.globRemote(path);
        if (entries.empty()) throw Error("远端路径不存在：" + path);
        for (const SftpEntry& entry : entries) {
            if (opt.dryRun) {
                std::cout << "将删除 " << entry.path << "\n";
                continue;
            }
            if (entry.isDir()) {
                if (!opt.recursive) throw Error(entry.path + " 是目录，请加 -r/--recursive");
                removeTree(sftp, entry.path);
            } else {
                sftp.removeFile(entry.path);
            }
            log().debug("已删除：" + entry.path);
        }
    }
    return 0;
}

int cmdRename(SshSession& session, const Options& opt, const std::vector<std::string>& args) {
    if (args.size() < 2) throw Error("用法：rshell rename <旧> <新>");
    if (opt.dryRun) {
        std::cout << "将重命名 " << args[0] << " -> " << args[1] << "\n";
        return 0;
    }
    SftpClient sftp(session);
    sftp.open();
    sftp.rename(args[0], args[1]);
    return 0;
}

int cmdChmod(SshSession& session, const Options& opt, const std::vector<std::string>& args) {
    if (args.size() < 2) throw Error("用法：rshell chmod <模式> <远端...>");
    const unsigned int mode = parseModeString(args[0]);

    if (opt.dryRun) {
        for (size_t i = 1; i < args.size(); ++i)
            std::cout << "将设置 " << args[i] << " -> 0" << std::oct << (mode & 07777) << std::dec
                      << "\n";
        return 0;
    }
    SftpClient sftp(session);
    sftp.open();
    for (size_t i = 1; i < args.size(); ++i) sftp.chmod(args[i], mode);
    return 0;
}

}  // namespace

void printUsage(bool full) { printUsageText(full); }

int runCli(int argc, char** argv) {
    Options opt;
    parseCommandLine(argc, argv, opt);

    if (opt.help || opt.command == "help") {
        printUsage(true);
        return 0;
    }
    if (opt.version || opt.command == "version") {
        std::cout << "rshell " << kVersion << "\n";
        return 0;
    }
    if (argc <= 1) {
        printUsage(false);
        return 1;
    }

    log().setLevel(opt.quiet ? LogLevel::Quiet
                             : (opt.verbose ? LogLevel::Verbose : LogLevel::Normal));

    std::string command = opt.command.empty() ? std::string("shell") : opt.command;
    if (command == "ssh" || command == "connect") command = "shell";
    if (command == "run") command = "exec";
    if (command == "upload") command = "put";
    if (command == "download") command = "get";

    ResolvedConnection resolved = resolveConnection(opt);

    if (command == "exec")
        return withSession(resolved, [&](SshSession& s) { return cmdExec(s, opt, opt.args); });
    if (command == "shell")
        return withSession(resolved, [&](SshSession& s) { return cmdShell(s, opt, opt.args); });
    if (command == "sftp")
        return withSession(resolved, [&](SshSession& s) { return cmdSftp(s, opt, opt.args); });
    if (command == "put")
        return withSession(resolved, [&](SshSession& s) { return cmdPut(s, opt, opt.args); });
    if (command == "get")
        return withSession(resolved, [&](SshSession& s) { return cmdGet(s, opt, opt.args); });
    if (command == "ls")
        return withSession(resolved, [&](SshSession& s) { return cmdLs(s, opt, opt.args); });
    if (command == "mkdir")
        return withSession(resolved, [&](SshSession& s) { return cmdMkdir(s, opt, opt.args); });
    if (command == "rm")
        return withSession(resolved, [&](SshSession& s) { return cmdRm(s, opt, opt.args); });
    if (command == "rename")
        return withSession(resolved, [&](SshSession& s) { return cmdRename(s, opt, opt.args); });
    if (command == "chmod")
        return withSession(resolved, [&](SshSession& s) { return cmdChmod(s, opt, opt.args); });

    throw Error("未知子命令：" + command + "（用 --help 查看用法）");
}

}  // namespace rshell
