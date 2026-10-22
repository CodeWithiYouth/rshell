// main.cpp - 进程入口：全局初始化、错误处理与退出码
#include <csignal>
#include <exception>
#include <iostream>
#include <string>

#include "cli.h"
#include "ssh_session.h"
#include "util.h"

int main(int argc, char** argv) {
#ifndef _WIN32
    // 对端关闭连接时不要让进程直接被 SIGPIPE 杀死
    std::signal(SIGPIPE, SIG_IGN);
#endif

    rshell::initConsole();

    try {
        // libssh2 全局初始化必须覆盖所有会话的生命周期
        rshell::Libssh2Runtime runtime;
        return rshell::runCli(argc, argv);
    } catch (const rshell::Error& ex) {
        std::cerr << rshell::red(std::string("rshell: ") + ex.what()) << "\n";
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << rshell::red(std::string("rshell: 未处理的错误：") + ex.what()) << "\n";
        return 2;
    } catch (...) {
        std::cerr << rshell::red("rshell: 未知错误") << "\n";
        return 2;
    }
}
