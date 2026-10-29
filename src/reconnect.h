// reconnect.h - 断线自动重连的退避策略
//
// 只放策略（要不要重试、隔多久、哪些错误不该重试），不碰界面和网络，
// 这样能被单元测试直接覆盖。
#pragma once

#include <string>

namespace rshell {

struct ReconnectPolicy {
    // 最多尝试几次（含第一次），总共 3 次：1 秒、2 秒、4 秒后
    static constexpr int maxAttempts = 3;
    static constexpr int baseDelayMs = 1000;

    // attempt 从 0 开始：1000 / 2000 / 4000
    static int delayMs(int attempt) {
        if (attempt < 0) return baseDelayMs;
        if (attempt >= 20) return baseDelayMs << 10;  // 防御性上限，正常用不到
        return baseDelayMs << attempt;
    }

    static bool shouldRetry(int failedAttempts) { return failedAttempts < maxAttempts; }

    // 这些失败重试也没意义，而且继续撞只会更糟：直接停手交给用户。
    //   - 主机指纹/未知主机：可能是服务器被换掉，必须人确认
    //   - 认证失败：密码错/密钥不被接受，重试只会触发 fail2ban
    static bool retryableError(const std::string& error) {
        if (error.empty()) return true;
        static const char* const fatal[] = {
            "主机指纹", "未知主机", "指纹", "host key", "Host key",
            "认证失败", "authentication", "Authentication",
            "Permission denied", "权限", "私钥", "publickey",
        };
        for (const char* needle : fatal) {
            if (error.find(needle) != std::string::npos) return false;
        }
        return true;
    }
};

}  // namespace rshell
