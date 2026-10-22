// protected_secret.cpp - CryptProtectMemory + VirtualLock 封装（非 Windows 退化为明文）
#include "protected_secret.h"

#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dpapi.h>
#endif

namespace rshell {

namespace {

constexpr std::size_t kBlock = 8;  // CRYPTPROTECTMEMORY_BLOCK_SIZE

std::size_t paddedBytes(std::size_t bytes) { return (bytes + kBlock - 1) / kBlock * kBlock; }

bool protect(wchar_t* data, std::size_t bytes) {
#ifdef _WIN32
    return CryptProtectMemory(data, static_cast<DWORD>(bytes),
                              CRYPTPROTECTMEMORY_SAME_PROCESS) != FALSE;
#else
    (void)data;
    (void)bytes;
    return false;  // 没有等价 API：保持明文，但 clear() 仍会清零
#endif
}

bool unprotect(wchar_t* data, std::size_t bytes) {
#ifdef _WIN32
    return CryptUnprotectMemory(data, static_cast<DWORD>(bytes),
                                CRYPTPROTECTMEMORY_SAME_PROCESS) != FALSE;
#else
    (void)data;
    (void)bytes;
    return false;
#endif
}

// 把一段字节清零（长度按字节）
void wipeBytes(wchar_t* data, std::size_t bytes) {
    if (!data || bytes == 0) return;
#ifdef _WIN32
    SecureZeroMemory(data, bytes);
#else
    std::fill_n(data, bytes / sizeof(wchar_t), L'\0');
#endif
}

// 锁进工作集：被锁的页不会被写进 pagefile，也不会被换出
bool lockPages(const wchar_t* data, std::size_t bytes) {
#ifdef _WIN32
    if (!data || bytes == 0) return false;
    return VirtualLock(const_cast<wchar_t*>(data), bytes) != FALSE;
#else
    (void)data;
    (void)bytes;
    return false;
#endif
}

void unlockPages(const wchar_t* data, std::size_t bytes) {
#ifdef _WIN32
    if (!data || bytes == 0) return;
    VirtualUnlock(const_cast<wchar_t*>(data), bytes);
#else
    (void)data;
    (void)bytes;
#endif
}

// UTF-8 <-> UTF-16LE；非 Windows 上按字节直通（GUI 只在 Windows 构建）
std::wstring widen(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

std::string narrow(const std::wstring& wide) {
#ifdef _WIN32
    if (wide.empty()) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
#else
    return std::string(wide.begin(), wide.end());
#endif
}

}  // namespace

void wipeWideString(std::wstring& text) {
    if (text.empty()) return;
#ifdef _WIN32
    SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
#else
    std::fill(text.begin(), text.end(), L'\0');
#endif
    text.clear();
}

ProtectedSecret::~ProtectedSecret() { clear(); }

ProtectedSecret::ProtectedSecret(ProtectedSecret&& other) noexcept
    : data_(std::move(other.data_)),
      encrypted_(other.encrypted_),
      dataLocked_(other.dataLocked_) {
    // 缓冲随 vector 一起换了主人（同一块内存），锁依然有效
    other.data_.clear();
    other.encrypted_ = false;
    other.dataLocked_ = false;
}

ProtectedSecret& ProtectedSecret::operator=(ProtectedSecret&& other) noexcept {
    if (this != &other) {
        clear();
        data_ = std::move(other.data_);
        encrypted_ = other.encrypted_;
        dataLocked_ = other.dataLocked_;
        other.data_.clear();
        other.encrypted_ = false;
        other.dataLocked_ = false;
    }
    return *this;
}

void ProtectedSecret::releaseScratch() const {
    if (!scratch_.empty()) {
        const std::size_t bytes = scratch_.size() * sizeof(wchar_t);
        wipeBytes(scratch_.data(), bytes);
        if (scratchLocked_) unlockPages(scratch_.data(), bytes);
    }
    scratch_.clear();
    scratch_.shrink_to_fit();
    scratchLocked_ = false;
}

void ProtectedSecret::assign(const std::wstring& plain) {
    clear();
    if (plain.empty()) return;

    // 用 vector 而不是 wstring：短口令不会落进 SSO 的内联缓冲，
    // 保证数据一定在可以 VirtualLock 的堆页上
    std::vector<wchar_t> buffer(plain.begin(), plain.end());
    const std::size_t bytes = buffer.size() * sizeof(wchar_t);
    const std::size_t padded = paddedBytes(bytes);
    buffer.resize(padded / sizeof(wchar_t), L'\0');

    encrypted_ = protect(buffer.data(), padded);
    // 加密失败（理论上只在没有 DPAPI 的环境）就退化为明文保存；
    // reveal() 靠 encrypted_ 判断要不要解密，语义仍然正确
    data_ = std::move(buffer);
    dataLocked_ = lockPages(data_.data(), data_.size() * sizeof(wchar_t));
}

std::wstring ProtectedSecret::reveal() const {
    if (data_.empty()) return std::wstring();

    // 解密在锁定的 scratch 里做：中间明文不落到可换出的页上
    if (scratch_.size() != data_.size()) {
        releaseScratch();
        scratch_.assign(data_.size(), L'\0');
        scratchLocked_ =
            lockPages(scratch_.data(), scratch_.size() * sizeof(wchar_t));
    }
    std::copy(data_.begin(), data_.end(), scratch_.begin());
    const std::size_t bytes = scratch_.size() * sizeof(wchar_t);

    if (encrypted_ && !unprotect(scratch_.data(), bytes)) {
        wipeBytes(scratch_.data(), bytes);
        return std::wstring();
    }
    // 去掉 assign 时为对齐补的 0
    std::size_t len = scratch_.size();
    while (len > 0 && scratch_[len - 1] == L'\0') --len;

    std::wstring plain(scratch_.data(), len);
    wipeBytes(scratch_.data(), bytes);  // 工作区用完立刻清零
    return plain;
}

void ProtectedSecret::assignUtf8(const std::string& utf8) {
    std::wstring plain = widen(utf8);
    assign(plain);
    wipeWideString(plain);
}

std::string ProtectedSecret::revealUtf8() const {
    std::wstring plain = reveal();
    std::string utf8 = narrow(plain);
    wipeWideString(plain);
    return utf8;
}

void ProtectedSecret::clear() {
    if (!data_.empty()) {
        const std::size_t bytes = data_.size() * sizeof(wchar_t);
        wipeBytes(data_.data(), bytes);
        if (dataLocked_) unlockPages(data_.data(), bytes);
    }
    data_.clear();
    data_.shrink_to_fit();
    encrypted_ = false;
    dataLocked_ = false;
    releaseScratch();
}

}  // namespace rshell
