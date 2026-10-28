// ai_tools_probe.cpp - AI 工具链对真实 SSH/SFTP 的端到端冒烟测试
//
// 用法:
//   python tests/sftp_test_server.py --root build_tmp/airoot --port 2299 ^
//       --user tester --password testpass
//   build\modern\rshell_ai_probe.exe 127.0.0.1 2299 tester testpass
//
// 覆盖：命令执行与退出码、写文件、读回、列目录、读路径黑名单、输出脱敏。
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "ai_ssh_backend.h"
#include "ai_tools.h"
#include "remote_fs.h"
#include "ssh_session.h"
#include "util.h"

using namespace rshell;

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "用法: ai_tools_probe <host> <port> <user> <password>\n";
        return 2;
    }

    ConnectionOptions options;
    options.host = argv[1];
    options.port = std::atoi(argv[2]);
    options.user = argv[3];
    options.password = argv[4];
    options.timeoutSec = 10;
    options.verifyHostKey = false;  // 只用于本地测试服务器

    Libssh2Runtime runtime;
    RemoteFs fs;
    fs.setSampleInterval(0);  // 不要后台采样，避免和命令计时互相干扰
    fs.setCallbacks(RemoteFsCallbacks{
        [](const FsEvent& event) {
            if (event.kind == FsEventKind::Failed)
                std::cerr << "文件连接失败：" << event.message << "\n";
        },
        [](const RemoteStats&) {}});

    int failures = 0;
    auto expect = [&failures](bool ok, const char* what) {
        std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
        if (!ok) ++failures;
    };

    fs.start(options);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while ((!fs.ready() || !fs.sftpReady()) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (!fs.ready()) {
        std::cerr << "连接未就绪，放弃\n";
        fs.stop();
        return 1;
    }
    expect(fs.sftpReady(), "SFTP 子系统就绪");

    ai::SshToolBackend backend(fs);
    ai::ToolRegistry registry(backend);

    // 1) 命令执行
    const ai::ToolResult echo = registry.execute("run_command", R"({"command":"echo hello-ai"})");
    expect(echo.ok, "echo 命令退出码为 0");
    expect(echo.json.find("hello-ai") != std::string::npos, "命令输出回传");

    // 2) 非零退出码要能带回来
    const ai::ToolResult failed = registry.execute("run_command", R"({"command":"exit 3"})");
    expect(!failed.ok, "非零退出码判定为失败");
    expect(failed.json.find("\"exit_code\":3") != std::string::npos, "退出码原样回传");

    // 3) 输出脱敏
    const ai::ToolResult secret =
        registry.execute("run_command", R"({"command":"echo AKIAABCDEFGHIJKLMNOP"})");
    expect(secret.json.find("AKIAABCDEFGHIJKLMNOP") == std::string::npos, "输出中的凭据被隐藏");
    expect(secret.json.find("redacted") != std::string::npos, "脱敏标记存在");

    // 4) 写文件
    const ai::ToolResult wrote = registry.execute(
        "write_file", R"({"path":"/ai_probe.txt","content":"alpha\nbeta\n"})");
    expect(wrote.ok, "写入远端文件");

    // 5) 读回
    const ai::ToolResult read = registry.execute("read_file", R"({"path":"/ai_probe.txt"})");
    expect(read.ok, "读回远端文件");
    expect(read.json.find("beta") != std::string::npos, "读回的内容正确");

    // 6) 列目录
    const ai::ToolResult listed = registry.execute("list_dir", R"({"path":"/"})");
    expect(listed.ok, "列目录成功");
    expect(listed.json.find("ai_probe.txt") != std::string::npos, "目录里能看到刚写的文件");

    // 7) 读取被策略拒绝，且不访问远端
    const ai::ToolResult denied = registry.execute("read_file", R"({"path":"~/.ssh/id_rsa"})");
    expect(!denied.ok && denied.denied, "私钥路径被安全策略拒绝");

    // 8) 不存在的文件给出明确错误
    const ai::ToolResult missing =
        registry.execute("read_file", R"({"path":"/ai_probe_missing.txt"})");
    expect(!missing.ok && !missing.denied, "不存在的文件报错而不是静默成功");

    fs.stop();
    std::cout << (failures == 0 ? "PASS" : "FAIL") << "\n";
    return failures == 0 ? 0 : 1;
}
