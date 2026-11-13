#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include "remote_fs.h"
#include "util.h"

using namespace std::chrono_literals;

namespace rshell {
struct RemoteFsTestAccess {
    static bool tightSocket(RemoteFs& fs) {
        const int bytes = 1024;
        return fs.session_ && setsockopt(fs.session_->socketFd(), SOL_SOCKET, SO_SNDBUF,
                                        reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
    }
};
}  // namespace rshell

namespace {
using Clock = std::chrono::steady_clock;
using Token = std::shared_ptr<std::atomic<bool>>;

void check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

template <typename Predicate>
void waitFor(Predicate predicate, const std::string& message, int timeoutMs = 6000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate() && Clock::now() < deadline) std::this_thread::sleep_for(2ms);
    check(predicate(), message);
}

struct TransferState {
    bool started = false;
    bool finished = false;
    bool directory = false;
    rshell::FsEventKind result = rshell::FsEventKind::Error;
    std::uint64_t bytes = 0;
    std::string message;
};

class Events {
public:
    std::atomic<int> samples{0};
    std::atomic<bool> disconnected{false};
    std::atomic<bool> fifoViolation{false};

    void expectAfter(const Token& later, const Token& earlier) {
        std::lock_guard<std::mutex> lock(mutex_);
        predecessors_[later.get()] = earlier.get();
    }

    void receive(const rshell::FsEvent& event) {
        if (event.kind == rshell::FsEventKind::Failed) {
            std::lock_guard<std::mutex> lock(mutex_);
            disconnectMessage_ = event.message;
            disconnected = true;
        }
        if (!event.cancel) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = transfers_[event.cancel.get()];
        state.directory = state.directory || event.directory;
        if (event.kind == rshell::FsEventKind::Started && !event.queued) {
            state.started = true;
            const auto predecessor = predecessors_.find(event.cancel.get());
            if (predecessor != predecessors_.end() && !transfers_[predecessor->second].finished)
                fifoViolation = true;
        }
        if (event.kind == rshell::FsEventKind::Progress) state.bytes = event.bytes;
        if (event.kind == rshell::FsEventKind::Done || event.kind == rshell::FsEventKind::Error ||
            event.kind == rshell::FsEventKind::Cancelled) {
            state.finished = true;
            state.result = event.kind;
            state.message = event.message;
        }
    }

    TransferState state(const Token& token) {
        std::lock_guard<std::mutex> lock(mutex_);
        return transfers_[token.get()];
    }

    void active(const Token& token, const std::string& context) {
        const auto current = state(token);
        check(current.started && (current.directory || current.bytes >= 65536) && !current.finished,
              context + " must finish while the transfer is still active: " + current.message);
    }

    void waitStarted(const Token& token) {
        waitFor([&] {
            const auto current = state(token);
            return (current.started && current.directory) || current.bytes >= 65536 || current.finished;
        },
                "large transfer starts sending full blocks");
        active(token, "initial progress");
    }

    void waitDone(const Token& token, rshell::FsEventKind expected = rshell::FsEventKind::Done) {
        waitFor([&] { return state(token).finished; }, "transfer finishes", 12000);
        const auto current = state(token);
        check(current.result == expected, "unexpected transfer result: " + current.message);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check(!disconnected, "cooperative interactions keep the same connection alive: " +
                  disconnectMessage_ + " (last transfer: " + current.message + ")");
        }
        check(!fifoViolation, "transfers remain FIFO at every cooperative yield");
    }

private:
    std::mutex mutex_;
    std::map<const std::atomic<bool>*, TransferState> transfers_;
    std::map<const std::atomic<bool>*, const std::atomic<bool>*> predecessors_;
    std::string disconnectMessage_;
};

Token token() { return std::make_shared<std::atomic<bool>>(false); }

void interactions(rshell::RemoteFs& fs, Events& events, const Token& transfer,
                  const Token& queued, const std::string& label) {
    events.active(transfer, label + " before interaction");
    const auto started = Clock::now();
    std::vector<rshell::RemoteEntry> entries;
    std::string error, text;
    bool truncated = false;
    check(fs.listDirSync("/", entries, error) && !entries.empty(), label + " listDirSync: " + error);
    events.active(transfer, label + " directory listing");
    check(fs.readFileSync("/small.txt", 1024, text, truncated, error) &&
          !truncated && text == "interaction-ok", label + " readFileSync: " + error);
    events.active(transfer, label + " small file read");
    text = "stale";
    truncated = true;
    check(fs.readFileSync("/empty.txt", 1024, text, truncated, error) &&
          text.empty() && !truncated, label + " empty file EOF: " + error);
    check(fs.readFileSync("/small-tail.bin", 70000, text, truncated, error) &&
          text.size() == 65537 && !truncated, label + " non-aligned file EOF: " + error);
    bool tailIntact = text.size() == 65537 && text.back() == 'Z';
    for (std::size_t i = 0; i < 65536 && tailIntact; ++i)
        tailIntact = static_cast<unsigned char>(text[i]) == i % 256;
    check(tailIntact, label + " final one-byte tail is preserved");
    check(fs.readFileSync("/small-tail.bin", 65536, text, truncated, error) &&
          text.size() == 65536 && truncated, label + " bounded read reports truncation: " + error);
    events.active(transfer, label + " empty/short/tail/truncated reads");
    const auto command = fs.execSync("RSHELL_TRANSFER_FAST", 1500, 1024);
    check(command.ok && command.exitCode == 0 && command.output == "command-ok",
          label + " execSync: " + command.error);
    events.active(transfer, label + " command execution");
    const int samples = events.samples.load();
    fs.sampleNow();
    waitFor([&] { return events.samples.load() > samples; }, label + " sample responds", 1500);
    events.active(transfer, label + " status sample");
    check(Clock::now() - started < 2s, label + " interactions stay within two seconds");
    if (queued) check(!events.state(queued).started, "queued transfer cannot start recursively at a yield");
    std::cout << "PASS: " << label << " listing/read/command/sample while transfer active ("
              << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()
              << " ms)\n";
}

void run(char** argv, bool tightSocket) {
    using namespace rshell;
    const auto fixture = utf8ToPath(argv[5]);
    const auto remote = fixture / "remote";
    const auto local = fixture / "client";
    Libssh2Runtime runtime;
    ConnectionOptions options;
    options.host = argv[1]; options.port = std::stoi(argv[2]);
    options.user = argv[3]; options.password = argv[4];
    options.verifyHostKey = false; options.allowDefaultKeys = false; options.timeoutSec = 4;
    Events events;
    RemoteFs fs;
    fs.setSampleInterval(3600);
    RemoteFsCallbacks callbacks;
    callbacks.onEvent = [&](const FsEvent& event) { events.receive(event); };
    callbacks.onStats = [&](const RemoteStats& stats) { if (stats.valid) ++events.samples; };
    fs.setCallbacks(std::move(callbacks));
    fs.start(options);
    waitFor([&] { return fs.sftpReady() && events.samples.load() > 0; }, "file channel and first sample are ready");
    if (tightSocket)
        check(RemoteFsTestAccess::tightSocket(fs), "tight-window pressure sets a 1 KiB client send buffer");

    const auto upload = token(), queuedUpload = token();
    fs.upload(pathToUtf8(local / "source.bin"), "/uploaded.bin", upload);
    events.waitStarted(upload);
    events.expectAfter(queuedUpload, upload);
    fs.upload(pathToUtf8(local / "queue.txt"), "/queued-upload.txt", queuedUpload);
    interactions(fs, events, upload, queuedUpload, "upload");

    // Cancel a running interactive command while the outer transfer remains healthy.
    std::atomic<bool> cancelInteraction{false};
    auto cancelledCommand = std::async(std::launch::async, [&] {
        return fs.execSync("RSHELL_TRANSFER_WAIT", 2000, 1024,
                           [&] { return cancelInteraction.load(); });
    });
    waitFor([&] { return std::filesystem::exists(remote / "interaction-entered"); },
            "interactive command starts inside the active transfer", 1500);
    events.active(upload, "interactive cancellation");
    const auto cancelledAt = Clock::now();
    cancelInteraction = true;
    const auto cancelled = cancelledCommand.get();
    check(!cancelled.ok && Clock::now() - cancelledAt < 700ms, "interactive cancellation returns promptly");
    events.active(upload, "after interactive cancellation");
    events.waitDone(upload);
    events.waitDone(queuedUpload);
    check(std::filesystem::file_size(remote / "uploaded.bin") == std::filesystem::file_size(local / "source.bin"),
          "upload preserves the entire byte count after interactive cancellation");

    const auto download = token(), queuedDownload = token();
    fs.download("/source.bin", pathToUtf8(local / "downloaded.bin"), download);
    events.waitStarted(download);
    events.expectAfter(queuedDownload, download);
    fs.download("/small.txt", pathToUtf8(local / "queued-download.txt"), queuedDownload);
    interactions(fs, events, download, queuedDownload, "download");
    events.waitDone(download);
    events.waitDone(queuedDownload);

    // Cancelling the suspended transfer must not cancel the interactive scope.
    const auto partial = token();
    fs.download("/source.bin", pathToUtf8(local / "resumed.bin"), partial);
    events.waitStarted(partial);
    auto cancelTransfer = std::async(std::launch::async, [&] {
        waitFor([&] { return std::filesystem::exists(remote / "transfer-command-entered"); },
                "command starts before transfer cancellation", 1500);
        partial->store(true);
    });
    const auto duringCancel = fs.execSync("RSHELL_TRANSFER_CANCEL_DURING_EXEC", 2000, 1024);
    cancelTransfer.get();
    check(duringCancel.ok && duringCancel.output == "command-ok",
          "transfer cancellation does not leak into the yielded interactive command: " + duringCancel.error);
    events.waitDone(partial, FsEventKind::Cancelled);
    const auto partialBytes = std::filesystem::file_size(local / "resumed.bin");
    check(partialBytes > 0 && partialBytes < std::filesystem::file_size(remote / "source.bin"),
          "cancel preserves a partial file for explicit resume");
    const auto resumed = token();
    fs.download("/source.bin", pathToUtf8(local / "resumed.bin"), resumed, true);
    events.waitStarted(resumed);
    interactions(fs, events, resumed, {}, "resumed download");
    events.waitDone(resumed);

    const auto partialUpload = token();
    fs.upload(pathToUtf8(local / "source.bin"), "/resumed-upload.bin", partialUpload);
    events.waitStarted(partialUpload);
    auto cancelUpload = std::async(std::launch::async, [&] {
        waitFor([&] { return std::filesystem::exists(remote / "upload-command-entered"); },
                "command starts before upload cancellation", 1500);
        partialUpload->store(true);
    });
    const auto duringUploadCancel = fs.execSync("RSHELL_TRANSFER_CANCEL_UPLOAD_DURING_EXEC", 2000, 1024);
    cancelUpload.get();
    check(duringUploadCancel.ok && duringUploadCancel.output == "command-ok",
          "upload cancellation does not leak into the yielded interactive command: " + duringUploadCancel.error);
    events.waitDone(partialUpload, FsEventKind::Cancelled);
    const auto uploadedBytes = std::filesystem::file_size(remote / "resumed-upload.bin");
    check(uploadedBytes > 0 && uploadedBytes < std::filesystem::file_size(local / "source.bin"),
          "cancelled upload preserves the remote prefix for explicit resume");
    const auto resumedUpload = token();
    fs.upload(pathToUtf8(local / "source.bin"), "/resumed-upload.bin", resumedUpload, true);
    events.waitStarted(resumedUpload);
    interactions(fs, events, resumedUpload, {}, "resumed upload");
    events.waitDone(resumedUpload);

    const auto treeUpload = token();
    fs.uploadDirectory(pathToUtf8(local / "tree"), "/uploaded-tree", treeUpload, false);
    events.waitStarted(treeUpload);
    interactions(fs, events, treeUpload, {}, "directory upload");
    events.waitDone(treeUpload);
    const auto treeDownload = token();
    fs.downloadDirectory("/uploaded-tree", pathToUtf8(local / "downloaded-tree"), treeDownload, false);
    events.waitStarted(treeDownload);
    interactions(fs, events, treeDownload, {}, "directory download");
    events.waitDone(treeDownload);
    fs.stop();
    std::cout << "PASS: transfer FIFO, cancellation isolation, partial resume and recursive transfers\n";
}
}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 6 || (argc == 7 && std::string(argv[6]) == "--tight-socket"),
              "usage: probe host port user password fixture-directory [--tight-socket]");
        run(argv, argc == 7);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
