// shell_probe.cpp - 无 GUI 的交互式 shell 冒烟测试
// 用法: shell_probe <host> <port> <user> <password>
// 通常配合 tests/sftp_test_server.py（已内置最小 shell）使用。
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

#include "ssh_shell.h"
#include "util.h"

using namespace rshell;

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "用法: shell_probe <host> <port> <user> <password>\n";
        return 2;
    }

    ConnectionOptions options;
    options.host = argv[1];
    options.port = std::atoi(argv[2]);
    options.user = argv[3];
    options.password = argv[4];
    options.timeoutSec = 10;
    // 只用于本地测试服务器，避免污染 known_hosts
    options.verifyHostKey = false;

    Libssh2Runtime runtime;
    SshShell shell;
    std::mutex mutex;
    std::condition_variable cv;
    std::string received;
    bool closed = false;

    shell.setCallbacks(ShellCallbacks{
        [&](std::string data) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                received += data;
            }
            cv.notify_all();
        },
        [&]() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                closed = true;
            }
            cv.notify_all();
        }});

    auto waitFor = [&](const std::string& needle, int timeoutMs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::unique_lock<std::mutex> lock(mutex);
        while (received.find(needle) == std::string::npos) {
            if (cv.wait_until(lock, deadline) == std::cv_status::timeout) return false;
        }
        return true;
    };
    auto waitClosed = [&](int timeoutMs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::unique_lock<std::mutex> lock(mutex);
        while (!closed) {
            if (cv.wait_until(lock, deadline) == std::cv_status::timeout) return false;
        }
        return true;
    };

    int failures = 0;
    auto expect = [&](bool ok, const char* what) {
        std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
        if (!ok) ++failures;
    };

    try {
        shell.start(options, "xterm-256color", 100, 30);
        expect(waitFor("$ ", 5000), "收到 shell 提示符");

        shell.write("echo hello-rshell\r");
        expect(waitFor("hello-rshell\r\n", 5000), "echo 命令回显正确");

        shell.write("uname\r");
        expect(waitFor("Linux rshell-test", 5000), "uname 输出正确");

        shell.resize(120, 40);

        shell.write("exit\r");
        expect(waitFor("bye", 5000), "exit 输出正确");
        expect(waitClosed(5000), "通道正常关闭");
    } catch (const std::exception& ex) {
        std::cerr << "错误：" << ex.what() << "\n";
        shell.close();
        return 1;
    }

    std::cout << "----- 远端输出 -----\n" << received << "\n-------------------\n";
    shell.close();
    std::cout << (failures == 0 ? "PASS" : "FAIL") << "\n";
    return failures == 0 ? 0 : 1;
}
