#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "ai_agent.h"
#include "ai_ssh_backend.h"
#include "operation_cancel.h"
#include "remote_fs.h"
#include "sftp_client.h"
#include "util.h"

using namespace std::chrono_literals;

namespace rshell {
// Exercise the real queue wrapper without requiring a network connection.
struct RemoteFsTestAccess {
    static void arm(RemoteFs& fs) { fs.running_ = true; }
    static bool sync(RemoteFs& fs, std::function<void()> task, int timeout,
                     const CancelCheck& cancel = {}) {
        return fs.runSync(std::move(task), timeout, cancel);
    }
    static void drain(RemoteFs& fs) {
        for (;;) {
            RemoteFs::Task task;
            {
                std::lock_guard<std::mutex> lock(fs.mutex_);
                if (fs.tasks_.empty()) break;
                task = std::move(fs.tasks_.front());
                fs.tasks_.pop_front();
            }
            task();
        }
    }
    static void delay(RemoteFs& fs) { fs.post([] { std::this_thread::sleep_for(150ms); }); }
};
}  // namespace rshell

namespace {
using Clock = std::chrono::steady_clock;
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template <typename Predicate>
void waitFor(Predicate predicate, const char* message) {
    const auto deadline = Clock::now() + 5s;
    while (!predicate() && Clock::now() < deadline) std::this_thread::sleep_for(2ms);
    check(predicate(), message);
}
void localRegression() {
    using namespace rshell;
    check(!operationCancelled(), "initial cancellation scope is empty");
    {
        OperationCancelScope outer([] { return true; });
        { OperationCancelScope inner([] { return false; }); check(operationCancelled(), "nested scope preserves cancellation"); }
        std::atomic<bool> isolated{false};
        std::thread other([&] { isolated = !operationCancelled(); }); other.join();
        check(isolated, "cancellation scopes are local to the invoking thread");
    }
    check(!operationCancelled(), "cancellation scope restores prior state");

    RemoteFs fs;
    RemoteFsTestAccess::arm(fs);
    int effects = 0;
    check(!RemoteFsTestAccess::sync(fs, [&] { ++effects; }, 30), "queued task times out");
    RemoteFsTestAccess::drain(fs);
    check(effects == 0, "expired queued side effects never execute");

    std::atomic<bool> cancel{false};
    std::thread signal([&] { std::this_thread::sleep_for(20ms); cancel = true; });
    const auto cancelledAt = Clock::now();
    const bool done = RemoteFsTestAccess::sync(fs, [&] { ++effects; }, 5000, [&] { return cancel.load(); });
    signal.join();
    check(!done && Clock::now() - cancelledAt < 500ms, "queued cancellation wakes the caller promptly");
    RemoteFsTestAccess::drain(fs);
    check(effects == 0, "cancelled queued side effects never execute");

    cancel = false;
    std::atomic<bool> entered{false}, exited{false};
    std::thread caller([&] {
        RemoteFsTestAccess::sync(fs, [&] {
            entered = true;
            while (!operationCancelled()) std::this_thread::sleep_for(1ms);
            exited = true;
        }, 2000, [&] { return cancel.load(); });
    });
    std::thread worker([&] {
        while (!entered && !exited) { RemoteFsTestAccess::drain(fs); std::this_thread::sleep_for(1ms); }
    });
    waitFor([&] { return entered.load(); }, "worker starts the task");
    cancel = true;
    caller.join(); worker.join();
    check(exited, "running task observes shared cancellation after its caller leaves");

    std::uint64_t lastRequest = 0;
    int callbacks = 0;
    RemoteFsCallbacks cb;
    cb.onEvent = [&](const FsEvent& event) { ++callbacks; lastRequest = event.requestId; };
    fs.setCallbacks(std::move(cb));
    fs.listDir("/old", 41); fs.listDir("/latest", 42);
    RemoteFsTestAccess::drain(fs);
    check(callbacks == 1 && lastRequest == 42, "only the latest queued directory request returns");
    fs.listDir("/stopped", 43); fs.requestStop();
    RemoteFsTestAccess::drain(fs);
    check(callbacks == 1, "stop invalidates pending directory requests");
    std::cout << "PASS: cancellation scopes, queue expiry, queued/running cancellation and navigation coalescing\n";
}

rshell::ConnectionOptions options(char** argv) {
    rshell::ConnectionOptions result;
    result.host = argv[1]; result.port = std::stoi(argv[2]);
    result.user = argv[3]; result.password = argv[4];
    result.verifyHostKey = false; result.allowDefaultKeys = false; result.timeoutSec = 3;
    return result;
}

class CommandChat : public rshell::ai::ChatBackend {
public:
    rshell::ai::ChatResult chat(const std::vector<rshell::ai::Message>&,
                              const std::vector<rshell::ai::ToolSpec>&,
                              rshell::ai::ChatStreamAccumulator::Callbacks,
                              const std::function<bool()>&) override {
        rshell::ai::ChatResult result;
        if (!called_++) {
            rshell::ai::ToolCall call;
            call.id = "cancel-command"; call.name = "run_command";
            call.arguments = "{\"command\":\"RSHELL_TEST_LONG_EXEC\",\"timeout_sec\":60}";
            result.toolCalls.push_back(std::move(call));
        } else result.content = "done";
        return result;
    }
private:
    int called_ = 0;
};

void networkRegression(char** argv) {
    using namespace rshell;
    const auto fixture = utf8ToPath(argv[5]);
    Libssh2Runtime runtime;
    const auto connect = options(argv);
    // Healthy cancellation still permits subsequent SFTP operations.
    {
        SshSession session(connect); session.connect(); SftpClient sftp(session); sftp.open();
        bool cancel = false;
        TransferOptions transfer; transfer.showProgress = false;
        transfer.cancelled = [&] { return cancel; };
        transfer.onProgress = [&](std::uint64_t, std::uint64_t) { cancel = true; };
        bool rejected = false;
        try { sftp.download("/large.bin", pathToUtf8(fixture / "partial.bin"), transfer); }
        catch (const Error&) { rejected = true; }
        check(rejected && !sftp.interrupted(), "healthy cancellation resets the handle without retiring the transport");
        transfer.cancelled = {}; transfer.onProgress = {};
        sftp.download("/small.txt", pathToUtf8(fixture / "after-cancel.txt"), transfer);
    }

    for (const bool upload : {false, true}) {
        SshSession session(connect); session.connect(); SftpClient sftp(session); sftp.open();
        const auto marker = fixture / (upload ? "write-entered" : "read-entered");
        std::atomic<bool> cancel{false};
        std::thread signal([&] { waitFor([&] { return std::filesystem::exists(marker); }, "server entered stalled IO"); cancel = true; });
        TransferOptions transfer; transfer.showProgress = false; transfer.cancelled = [&] { return cancel.load(); };
        const auto started = Clock::now();
        bool rejected = false;
        try {
            if (upload) sftp.upload(pathToUtf8(fixture / "large.bin"), "/slow-write.bin", transfer);
            else sftp.download("/slow-read.bin", pathToUtf8(fixture / "stalled-partial.bin"), transfer);
        } catch (const Error&) { rejected = true; }
        signal.join();
        check(rejected && Clock::now() - started < 2s, "stalled transfer cancellation is bounded");
    }

    // A server which never delivers data also hits the idle timeout without a click.
    {
        auto shortTimeout = connect; shortTimeout.timeoutSec = 1;
        SshSession session(shortTimeout); session.connect(); SftpClient sftp(session); sftp.open();
        TransferOptions transfer; transfer.showProgress = false;
        const auto started = Clock::now();
        bool rejected = false;
        try { sftp.download("/slow-timeout.bin", pathToUtf8(fixture / "timeout.bin"), transfer); }
        catch (const Error&) { rejected = true; }
        check(rejected && Clock::now() - started < 3s, "stalled transfer reaches idle timeout");
    }

    {
        RemoteFs fs; fs.setSampleInterval(0); fs.start(connect);
        waitFor([&] { return fs.sftpReady(); }, "AI file channel is ready");
        std::string readerError;
        auto reader = fs.openReader("/reader.txt", &readerError);
        check(reader != nullptr, "drag reader opens");
        RemoteFsTestAccess::delay(fs);
        const auto closeStarted = Clock::now(); reader->close();
        check(Clock::now() - closeStarted < 100ms, "reader cleanup queues without blocking its caller");
        waitFor([&] { return std::filesystem::exists(fixture / "reader-closed"); }, "queued reader cleanup eventually closes its remote handle");
        ai::SshToolBackend backend(fs); ai::ToolRegistry tools(backend); CommandChat chat;
        ai::Agent agent(chat, tools); agent.addUserMessage("run test command");
        agent.setApproval([](const ai::ApprovalRequest&) { return ai::ApprovalDecision::Allow; });
        std::atomic<bool> cancel{false}; ai::AgentTurn turn;
        std::thread worker([&] { turn = agent.run([&] { return cancel.load(); }); });
        waitFor([&] { return std::filesystem::exists(fixture / "exec-entered"); }, "server started the AI command");
        const auto started = Clock::now(); cancel = true; worker.join();
        check(turn.cancelled && Clock::now() - started < 500ms, "Agent stop reaches a running SSH tool");
        const auto next = fs.execSync("RSHELL_TEST_FAST_EXEC", 2000, 1024);
        check(next.ok && next.output.find("fast-ok") != std::string::npos, "connection remains usable after stopping the command");
        fs.stop();
    }
    {
        RemoteFs fs; fs.setSampleInterval(0);
        std::atomic<bool> failed{false}, cancel{false};
        RemoteFsCallbacks callbacks;
        callbacks.onEvent = [&](const FsEvent& event) { if (event.kind == FsEventKind::Failed) failed = true; };
        fs.setCallbacks(std::move(callbacks)); fs.start(connect);
        waitFor([&] { return fs.sftpReady(); }, "metadata file channel is ready");
        std::thread signal([&] { waitFor([&] { return std::filesystem::exists(fixture / "list-entered"); }, "server entered stalled listing"); cancel = true; });
        std::vector<RemoteEntry> entries; std::string error;
        check(!fs.listDirSync("/slow-list", entries, error, [&] { return cancel.load(); }), "in-flight metadata cancellation fails the request");
        signal.join();
        waitFor([&] { return failed.load(); }, "in-flight cancellation retires the unsafe file transport");
        fs.stop(); fs.start(connect);
        waitFor([&] { return fs.sftpReady(); }, "file channel reconnects after retirement");
        std::string content; bool truncated = false;
        check(fs.readFileSync("/small.txt", 1024, content, truncated, error) && content == "still-connected",
              "retired connection does not poison a subsequent connection");
        fs.stop();
    }
    std::cout << "PASS: healthy/stalled upload and download cancellation, idle timeout and AI command stop\n";
}
}  // namespace

int main(int argc, char** argv) {
    try {
        localRegression();
        if (argc == 6) networkRegression(argv);
        else if (argc != 1) throw std::runtime_error("usage: probe [host port user password fixture-directory]");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
