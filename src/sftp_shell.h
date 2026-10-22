// sftp_shell.h - 交互式 sftp 命令循环
#pragma once

#include <string>

namespace rshell {

class SftpClient;

// 在已打开的 SftpClient 上运行交互式 shell。
// hostLabel 仅用于提示符显示，例如 "deploy@example.com"。
int runSftpShell(SftpClient& sftp, const std::string& hostLabel);

}  // namespace rshell
