// ai_ssh_backend.cpp
#include "ai_ssh_backend.h"

#include <algorithm>

#include "remote_fs.h"

namespace rshell::ai {

namespace {
// 单条命令最多缓冲这么多输出：再多也没意义（工具层还会按策略再截断一次），
// 但留足余量让脱敏能覆盖整段敏感内容。
constexpr std::size_t kMaxCommandOutput = 256u * 1024u;
}  // namespace

ToolBackend::CommandResult SshToolBackend::runCommand(const std::string& command, int timeoutSec,
                                                          const std::string& host) {
    (void)host;  // 绑定在单条连接上
    const int timeoutMs = std::clamp(timeoutSec, 1, 300) * 1000;
    const RemoteFs::ExecOutcome outcome = fs_.execSync(command, timeoutMs, kMaxCommandOutput);

    CommandResult result;
    result.truncated = outcome.truncated;
    if (!outcome.ok) {
        result.exitCode = -1;
        result.output = outcome.error.empty() ? "命令未能执行" : outcome.error;
        return result;
    }
    result.exitCode = outcome.exitCode;
    result.output = outcome.output;
    return result;
}

bool SshToolBackend::readFile(const std::string& path, std::size_t maxBytes, std::string& out,
                              bool& truncated, std::string& error, const std::string& host) {
    (void)host;
    return fs_.readFileSync(path, maxBytes, out, truncated, error);
}

bool SshToolBackend::writeFile(const std::string& path, const std::string& content,
                               std::string& error, const std::string& host) {
    (void)host;
    return fs_.writeFileSync(path, content, error);
}

bool SshToolBackend::listDir(const std::string& path, std::vector<DirEntryView>& out,
                             std::string& error, const std::string& host) {
    (void)host;
    std::vector<RemoteEntry> entries;
    if (!fs_.listDirSync(path, entries, error)) return false;

    out.clear();
    out.reserve(entries.size());
    for (const RemoteEntry& entry : entries) {
        DirEntryView view;
        view.name = entry.name;
        view.size = entry.size;
        view.isDir = entry.isDir();
        out.push_back(std::move(view));
    }
    return true;
}

}  // namespace rshell::ai
