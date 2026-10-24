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
    const bool testQueue = argc > 5 && std::string(argv[5]) == "--queue";

    try {
        shell.start(options, "xterm-256color", 100, 30);
        expect(waitFor("$ ", 5000), "收到 shell 提示符");

        shell.write("echo hello-rshell\r");
        expect(waitFor("hello-rshell\r\n", 5000), "echo 命令回显正确");

        shell.write("uname\r");
        expect(waitFor("Linux rshell-test", 5000), "uname 输出正确");

        shell.resize(120, 40);
        if (testQueue) {
            shell.write("probe-sink 4194304\r");
            expect(waitFor("sink-ready", 5000), "服务器开始背压测试");
            const std::string payload(4 * 1024 * 1024, 'q');
            const auto before = std::chrono::steady_clock::now();
            expect(shell.write(payload), "4 MiB 输入成功入队");
            const auto elapsed = std::chrono::steady_clock::now() - before;
            expect(elapsed < std::chrono::milliseconds(250), "背压时入队无需等待网络发送");
            expect(!shell.write(std::string(9 * 1024 * 1024, 'x')), "超出队列容量时明确拒绝");
            shell.resize(132, 44);
            shell.write("echo queue-after\r");
            expect(waitFor("sink:4194304:ok", 15000), "背压后全部字节完整到达");
            expect(waitFor("queue-after\r\n", 5000), "后续输入保持 FIFO 顺序");
            shell.write("size\r");
            expect(waitFor("132x44\r\n", 5000), "后台 resize 生效");
        }

        shell.write("exit\r");
        expect(waitFor("bye", 5000), "exit 输出正确");
        expect(waitClosed(5000), "通道正常关闭");
        expect(!shell.closedReason().empty(), "提供关闭原因");
        expect(!shell.write("after-close"), "断线后拒绝入队");
        if (testQueue) {
            shell.close();
            {
                std::lock_guard<std::mutex> lock(mutex);
                received.clear();
                closed = false;
            }
            shell.start(options, "xterm-256color", 80, 24);
            expect(waitFor("$ ", 5000), "断线后可重新建立会话");
            shell.write("echo reconnect-ok\r");
            expect(waitFor("reconnect-ok\r\n", 5000), "重连后的发送正常");
            shell.write("probe-sink 4194304\r");
            expect(waitFor("sink-ready", 5000), "开始中途关闭测试");
            shell.write(std::string(4 * 1024 * 1024, 'q'));
            const auto before = std::chrono::steady_clock::now();
            shell.close();
            expect(std::chrono::steady_clock::now() - before < std::chrono::seconds(1),
                   "背压期间关闭可及时退出工作线程");
            {
                std::lock_guard<std::mutex> lock(mutex);
                expect(!closed, "主动关闭不产生迟到的断线回调");
                received.clear();
            }
            shell.start(options, "xterm-256color", 80, 24);
            expect(waitFor("$ ", 5000), "取消发送后可再次连接");
            shell.write("echo fresh-queue\r");
            expect(waitFor("fresh-queue\r\n", 5000), "旧连接队列没有进入新连接");
            shell.write("exit\r");
            expect(waitClosed(5000), "重连会话正常结束");
        }
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
