// vault.h - 主密码加密的密码保险库（可选 TPM 解锁）
//
// 设计目标：把“记住的密码”从“系统按账户身份保护”换成“只有知道主密码
// 的人能解开”。凭据管理器那种方案，同账户下任何程序调一次 CredReadW 就能
// 拿到明文；这里文件对所有人可读，但没有主密码就只是一段密文。
//
// 文件格式（小端，全部字段紧密排列）：
//
//   magic       8   "RSVAULT1"
//   version     2   当前 1
//   flags       2   bit0 = 含 TPM slot
//   iterations  4   PBKDF2-HMAC-SHA256 迭代次数
//   salt       16   口令派生用盐（随机，每次改主密码重新生成）
//   slot_count  1
//   slot[]          type(1) + flags(1) + reserved(2) + len(4) + nonce(12) + data
//                   type 1 = 主密码包住的 DEK（AES-256-GCM，len=48）
//                   type 2 = TPM 包住的 DEK（RSA-OAEP-SHA256，len=256；密钥按路径派生）
//   payload         nonce(12) + len(4) + AES-256-GCM(条目表) + tag(16)
//
// 关键点：条目本身由随机数据密钥（DEK）加密，DEK 再由每个 slot 各包一份。
// 因此改主密码只需要重包 DEK，不用把所有条目解密再加密一遍；加 TPM 也只是
// 多一个 slot。
//
// 明文的条目表：count(4) + [name_len(2) + name + secret_len(4) + secret]...
// name/secret 都是 UTF-8。
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "protected_secret.h"

namespace rshell {

// 文件格式版本；不兼容改动时 +1，open() 会拒绝更高版本
constexpr std::uint16_t kVaultFormatVersion = 1;

// PBKDF2-HMAC-SHA256 迭代次数。600k 在现代桌面 CPU 上约 0.1~0.4 秒，
// 测试可以传更小的值加速（create 的参数）。
constexpr std::uint32_t kVaultDefaultIterations = 600000;

class Vault {
public:
    Vault();
    ~Vault();

    Vault(Vault&& other) noexcept;
    Vault& operator=(Vault&& other) noexcept;
    Vault(const Vault&) = delete;
    Vault& operator=(const Vault&) = delete;

    // 新建空保险库并立刻落盘，返回时 out 已解锁。iterations 仅供测试使用。
    static bool create(const std::wstring& path, const std::wstring& masterPassword, Vault& out,
                       std::uint32_t iterations = kVaultDefaultIterations,
                       std::string* error = nullptr);

    // 用主密码解锁。文件不存在 / 口令不对 / 格式损坏都返回 false。
    static bool open(const std::wstring& path, const std::wstring& masterPassword, Vault& out,
                     std::string* error = nullptr);

    // 用 TPM 解锁：要求 vault 里有 TPM slot，且本机 TPM 中有对应密钥。
    static bool openWithTpm(const std::wstring& path, Vault& out, std::string* error = nullptr);

    bool unlocked() const { return unlocked_; }
    const std::wstring& path() const { return path_; }
    bool hasTpmSlot() const { return hasTpmSlot_; }
    bool dirty() const { return dirty_; }  // 有未保存的改动

    // 本机是否有可用的 TPM（Platform Crypto Provider + 能建密钥）
    static bool tpmAvailable();

    // 只读文件头：该路径上的保险库是否带 TPM slot。不用解锁就能判断，
    // 界面据此决定要不要显示「用 TPM 解锁」；文件不存在/损坏一律 false。
    static bool tpmSlotPresent(const std::wstring& path);

    // 默认存放位置：%LOCALAPPDATA%\rshell\vault.dat
    static std::wstring defaultPath();

    // 丢掉 DEK 与内存中的条目，回到锁定态
    void lock();

    // 条目操作：只改内存，save() 才落盘。name 是会话名（不是秘密），
    // secret 是口令（内部以 DPAPI 密文保存）。
    bool has(const std::string& name) const;
    bool get(const std::string& name, std::string& secret) const;
    void set(const std::string& name, const std::string& secret);
    bool remove(const std::string& name);
    std::vector<std::string> names() const;

    // 原子写入（temp + MoveFileEx）。未解锁时失败。
    bool save(std::string* error = nullptr);

    // 换主密码：重新生成 salt 并重包 DEK，条目密文不变。
    bool changeMasterPassword(const std::wstring& newPassword, std::string* error = nullptr);

    // 加/去掉 TPM 解锁。enableTpm 需要本机有 TPM，失败不影响主密码解锁。
    bool enableTpm(std::string* error = nullptr);
    bool disableTpm(std::string* error = nullptr);

private:
    std::wstring path_;
    std::uint32_t iterations_ = kVaultDefaultIterations;
    std::array<std::uint8_t, 16> salt_{};
    std::array<std::uint8_t, 32> dek_{};
    std::array<std::uint8_t, 12> passwordSlotNonce_{};
    std::array<std::uint8_t, 48> passwordSlot_{};  // AES-GCM 包住的 DEK（32+16）
    std::vector<std::uint8_t> tpmBlob_;
    std::array<std::uint8_t, 12> tpmNonce_{};
    bool dekLocked_ = false;   // VirtualLock 是否成功（失败不影响功能）
    bool unlocked_ = false;
    bool hasTpmSlot_ = false;
    std::map<std::string, ProtectedSecret> entries_;
    bool dirty_ = false;

    void lockDek();
    void wipeDek();
    void wipeTpmBlob();
    friend struct VaultLoader;
};

}  // namespace rshell
