// ai_ssh_backend.h - 把 GUI 的 RemoteFs（第二条 SSH 连接）适配成 AI 工具后端
//
// 复用同一条 SFTP 连接而不是新开会话：服务器 MaxSessions 往往只给 2~3 条，
// 终端已经占一条，AI 再开一条很容易连不上。
#pragma once

#include <string>
#include <vector>

#include "ai_tools.h"

namespace rshell {

class RemoteFs;

namespace ai {

class SshToolBackend : public ToolBackend {
public:
    explicit SshToolBackend(RemoteFs& fs) : fs_(fs) {}

    CommandResult runCommand(const std::string& command, int timeoutSec) override;
    bool readFile(const std::string& path, std::size_t maxBytes, std::string& out,
                  bool& truncated, std::string& error) override;
    bool writeFile(const std::string& path, const std::string& content,
                   std::string& error) override;
    bool listDir(const std::string& path, std::vector<DirEntryView>& out,
                 std::string& error) override;

private:
    RemoteFs& fs_;
};

}  // namespace ai
}  // namespace rshell
