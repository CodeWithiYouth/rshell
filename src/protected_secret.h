// protected_secret.h - 在内存里以密文形式保存口令
//
// 长期持有的密码（例如整个 SSH 会话期间用于重连文件通道的那份）不必一直是
// 明文：Windows 上可以用 CryptProtectMemory 原地加密，只有本进程能解开。
// 需要用时才 reveal() 出一份临时副本，调用方用完立刻抹掉。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace rshell {

// 宽字符串清零（Windows 用 SecureZeroMemory，防编译器优化掉）
void wipeWideString(std::wstring& text);

// 口令容器：内部只存密文，assign/reveal 是唯一出入口，禁止拷贝。
// 密文缓冲和解密工作区都会尝试 VirtualLock，尽量不换出到 pagefile。
class ProtectedSecret {
public:
    ProtectedSecret() = default;
    ~ProtectedSecret();

    ProtectedSecret(const ProtectedSecret&) = delete;
    ProtectedSecret& operator=(const ProtectedSecret&) = delete;
    ProtectedSecret(ProtectedSecret&& other) noexcept;
    ProtectedSecret& operator=(ProtectedSecret&& other) noexcept;

    // 换成新口令（会先清掉旧内容）
    void assign(const std::wstring& plain);
    void assignUtf8(const std::string& utf8);

    // 解密出一份明文副本；调用方用完请 wipeWideString()/wipeString()。
    std::wstring reveal() const;
    std::string revealUtf8() const;

    bool empty() const { return data_.empty(); }
    void clear();  // 清零并丢弃

    // 诊断/测试用：是否真的存成了密文（没有 DPAPI 时退化为明文）
    bool encrypted() const { return encrypted_; }
    // 密文缓冲是否锁在工作集里（工作集配额不足时 VirtualLock 会失败）
    bool locked() const { return dataLocked_; }
    // 密文副本（诊断/测试用；长度已补齐到 8 字节的倍数）
    std::wstring raw() const { return std::wstring(data_.begin(), data_.end()); }
    std::size_t rawBytes() const { return data_.size() * sizeof(wchar_t); }

private:
    void releaseScratch() const;

    std::vector<wchar_t> data_;              // 密文；堆分配（不受 SSO 影响），尽量锁页
    mutable std::vector<wchar_t> scratch_;   // 解密工作区，同样尽量锁页
    bool encrypted_ = false;
    bool dataLocked_ = false;
    mutable bool scratchLocked_ = false;
};

}  // namespace rshell

