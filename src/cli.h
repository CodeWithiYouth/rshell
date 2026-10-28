// cli.h - 命令行解析与子命令分发
#pragma once

#include <string>
#include <vector>

#include "config.h"

namespace rshell {

// 所有子命令共享的选项
struct Options {
    // 连接
    std::string host;          // 别名或 IP；可被位置参数覆盖
    std::string user;
    int port = 0;
    std::string keyPath;
    std::string password;
    std::string configPath;    // 指定 ssh config 路径
    std::string proxyUrl;
    int keepAliveSec = 0;     // 0 = 关闭      // socks5://host:port 或 http://host:port
    int timeoutSec = 0;
    bool strictHostKey = false;
    bool skipHostKeyCheck = false;

    // 行为
    bool quiet = false;
    bool verbose = false;
    bool help = false;
    bool version = false;

    // 传输
    bool recursive = false;
    bool overwrite = false;
    bool resume = false;
    bool preserve = false;
    bool noProgress = false;
    int chmodMode = -1;

    // 列表
    bool longFormat = false;
    bool showAll = false;
    bool dirsOnly = false;
    bool onePerLine = false;

    // mkdir
    bool parents = false;
    int mkdirMode = 0755;

    // 其他
    bool dryRun = false;
    std::string command;                 // 子命令名（argv[0] 之后的第一个非选项）
    std::vector<std::string> args;       // 子命令位置参数
};

// 返回进程退出码；main() 负责捕获 Error
int runCli(int argc, char** argv);

// 打印用法
void printUsage(bool full);

}  // namespace rshell
