// sftp_shell.cpp - 交互式 sftp 命令循环
#include "sftp_shell.h"

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "sftp_client.h"
#include "config.h"
#include "util.h"

namespace rshell {

namespace {

// sftp 层没有 chdir，这里用一个逻辑 cwd + 路径拼接来模拟
std::string resolveRemote(const std::string& cwd, const std::string& path) {
    if (path.empty()) return cwd;
    std::string p = path;
    if (p == "~") {
        p = ".";
    } else if (p.rfind("~/", 0) == 0) {
        p = p.substr(2);
    }
    if (isAbsoluteRemotePath(p)) return normalizeRemotePath(p);
    if (cwd.empty() || cwd == ".") return normalizeRemotePath(p);
    return normalizeRemotePath(remoteJoin(cwd, p));
}

// 本地路径支持 ~ 与相对当前本地目录
std::string resolveLocal(const std::string& cwd, const std::string& path) {
    if (path.empty()) return cwd;
    fs::path p = utf8ToPath(expandPath(path));
    if (p.is_absolute()) return pathToUtf8(p.lexically_normal());
    return pathToUtf8((utf8ToPath(cwd) / p).lexically_normal());
}

void printEntry(const SftpEntry& entry, bool longFormat) {
    if (longFormat) {
        std::cout << formatMode(entry.mode) << " " << formatTimestamp(entry.mtime) << " "
                  << entry.size << "\t" << entry.name << "\n";
    } else {
        std::cout << entry.name << "\n";
    }
}

void printHelp() {
    std::cout
        << "可用命令：\n"
           "  ls [-l] [路径]          列出远端目录\n"
           "  cd <路径>               切换远端目录（支持 ~ 和 ..）\n"
           "  pwd                     显示远端当前目录\n"
           "  mkdir <路径>            创建远端目录（自动创建父目录）\n"
           "  rmdir <路径>            删除远端空目录\n"
           "  rm <路径>               删除远端文件（支持通配符）\n"
           "  rename <旧> <新>        重命名\n"
           "  chmod <模式> <路径>     修改权限，例如 chmod 644 a.txt\n"
           "  put <本地> [远端]       上传（远端为目录时自动拼接文件名）\n"
           "  get <远端> [本地]       下载（支持通配符）\n"
           "  lls [路径]              列出本地目录\n"
           "  lcd <路径>              切换本地目录\n"
           "  lpwd                    显示本地当前目录\n"
           "  help                    显示本帮助\n"
           "  exit / quit             退出\n";
}

// 按空白切分，支持双引号包裹带空格的参数
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string current;
    bool inQuote = false;
    for (char c : line) {
        if (c == '"') {
            inQuote = !inQuote;
            continue;
        }
        if (!inQuote && std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

}  // namespace

int runSftpShell(SftpClient& sftp, const std::string& hostLabel) {
    const std::string home = sftp.homeDir();
    std::string remoteCwd = home.empty() ? "/" : home;
    std::string localCwd = pathToUtf8(fs::current_path());

    std::cout << "rshell sftp - " << hostLabel << "\n"
              << "远端目录：" << remoteCwd << "\n"
              << "输入 help 查看命令，exit 退出。\n";

    for (;;) {
        std::string line;
        try {
            line = readLine("sftp:" + remoteCwd + "> ");
        } catch (const Error&) {
            break;  // stdin 已关闭
        }
        if (line.empty()) continue;
        std::vector<std::string> tokens = tokenize(line);
        if (tokens.empty()) continue;
        const std::string cmd = toLower(tokens[0]);
        const std::vector<std::string> args(tokens.begin() + 1, tokens.end());

        try {
            if (cmd == "exit" || cmd == "quit" || cmd == "bye") {
                break;
            } else if (cmd == "help" || cmd == "?") {
                printHelp();
            } else if (cmd == "pwd") {
                std::cout << remoteCwd << "\n";
            } else if (cmd == "lpwd") {
                std::cout << localCwd << "\n";
            } else if (cmd == "cd") {
                if (args.empty()) {
                    remoteCwd = home.empty() ? "/" : home;
                } else {
                    std::string target = resolveRemote(remoteCwd, args[0]);
                    SftpEntry entry;
                    if (!sftp.stat(target, entry)) throw Error("目录不存在：" + target);
                    if (!entry.isDir()) throw Error("不是目录：" + target);
                    remoteCwd = target;
                }
            } else if (cmd == "lcd") {
                if (args.empty()) {
                    localCwd = expandPath("~");
                } else {
                    std::string target = resolveLocal(localCwd, args[0]);
                    std::error_code ec;
                    if (!fs::is_directory(utf8ToPath(target), ec))
                        throw Error("本地目录不存在：" + target);
                    localCwd = target;
                }
            } else if (cmd == "lls") {
                std::string dir = args.empty() ? localCwd : resolveLocal(localCwd, args[0]);
                std::error_code ec;
                fs::directory_iterator it(utf8ToPath(dir), ec);
                if (ec) throw Error("无法读取本地目录：" + dir + "（" + ec.message() + "）");
                for (const auto& item : it) std::cout << pathToUtf8(item.path().filename()) << "\n";
            } else if (cmd == "ls") {
                bool longFormat = false;
                std::vector<std::string> paths;
                for (const std::string& arg : args) {
                    if (arg == "-l" || arg == "-al" || arg == "-la") {
                        longFormat = true;
                    } else if (arg == "-a") {
                        // listDir 默认就返回隐藏文件
                    } else {
                        paths.push_back(arg);
                    }
                }
                if (paths.empty()) paths.emplace_back();
                for (const std::string& raw : paths) {
                    std::string target = resolveRemote(remoteCwd, raw);
                    std::vector<SftpEntry> entries = sftp.listTarget(target);
                    if (paths.size() > 1) std::cout << target << ":\n";
                    for (const SftpEntry& entry : entries) printEntry(entry, longFormat);
                }
            } else if (cmd == "mkdir") {
                if (args.empty()) throw Error("用法：mkdir <路径>");
                sftp.mkdir(resolveRemote(remoteCwd, args[0]), 0755, true);
            } else if (cmd == "rmdir") {
                if (args.empty()) throw Error("用法：rmdir <路径>");
                sftp.rmdir(resolveRemote(remoteCwd, args[0]));
            } else if (cmd == "rm") {
                if (args.empty()) throw Error("用法：rm <路径>");
                for (const std::string& arg : args) {
                    std::vector<SftpEntry> entries = sftp.globRemote(resolveRemote(remoteCwd, arg));
                    if (entries.empty()) throw Error("未找到：" + arg);
                    for (const SftpEntry& entry : entries) {
                        if (entry.isDir()) sftp.rmdir(entry.path);
                        else sftp.removeFile(entry.path);
                    }
                }
            } else if (cmd == "rename" || cmd == "mv") {
                if (args.size() < 2) throw Error("用法：rename <旧> <新>");
                sftp.rename(resolveRemote(remoteCwd, args[0]), resolveRemote(remoteCwd, args[1]));
            } else if (cmd == "chmod") {
                if (args.size() < 2) throw Error("用法：chmod <模式> <路径>");
                unsigned int mode = parseModeString(args[0]);
                for (size_t i = 1; i < args.size(); ++i)
                    sftp.chmod(resolveRemote(remoteCwd, args[i]), mode);
            } else if (cmd == "put") {
                if (args.empty()) throw Error("用法：put <本地> [远端]");
                std::string local = resolveLocal(localCwd, args[0]);
                std::string remote =
                    resolveRemote(remoteCwd, args.size() > 1 ? args[1] : localBaseName(local));
                TransferOptions options;
                sftp.upload(local, sftp.resolveRemoteTarget(remote, local), options);
            } else if (cmd == "get") {
                if (args.empty()) throw Error("用法：get <远端> [本地]");
                std::vector<SftpEntry> entries =
                    sftp.globRemote(resolveRemote(remoteCwd, args[0]));
                if (entries.empty()) throw Error("未找到：" + args[0]);
                std::string localTarget =
                    args.size() > 1 ? resolveLocal(localCwd, args[1]) : localCwd;
                const bool multiple = entries.size() > 1;
                for (const SftpEntry& entry : entries) {
                    if (entry.isDir()) {
                        std::cerr << yellow("跳过目录：" + entry.path) << "\n";
                        continue;
                    }
                    std::error_code ec;
                    fs::path out = utf8ToPath(localTarget);
                    if (fs::is_directory(out, ec) || multiple) out /= utf8ToPath(entry.name);
                    TransferOptions options;
                    sftp.download(entry.path, pathToUtf8(out), options);
                }
            } else {
                std::cerr << red("未知命令：" + tokens[0]) << "\n";
                printHelp();
            }
        } catch (const Error& ex) {
            std::cerr << red(std::string("错误：") + ex.what()) << "\n";
        }
    }
    return 0;
}

}  // namespace rshell
