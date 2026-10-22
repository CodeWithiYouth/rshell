// vault.cpp - 主密码 + 可选 TPM 的密码保险库（Windows CNG 实现）
#include "vault.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>
#endif

namespace rshell {

namespace {

constexpr std::uint8_t kSlotPassword = 1;
constexpr std::uint8_t kSlotTpm = 2;

constexpr std::size_t kDekBytes = 32;
constexpr std::size_t kNonceBytes = 12;
constexpr std::size_t kTagBytes = 16;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kPasswordSlotBytes = kDekBytes + kTagBytes;  // 32 + 16
constexpr std::size_t kMaxNameBytes = 4096;
constexpr std::size_t kMaxSecretBytes = 64 * 1024;
constexpr std::size_t kMaxSlots = 4;

const char kMagic[8] = {'R', 'S', 'V', 'A', 'U', 'L', 'T', '1'};

void wipeBytes(void* data, std::size_t bytes) {
    if (!data || bytes == 0) return;
#ifdef _WIN32
    SecureZeroMemory(data, bytes);
#else
    std::memset(data, 0, bytes);
#endif
}

std::string utf8Of(const std::wstring& text) {
#ifdef _WIN32
    if (text.empty()) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
#else
    return std::string(text.begin(), text.end());
#endif
}

#ifdef _WIN32
std::string ntError(const char* what, LONG status) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s（NTSTATUS 0x%08lX）", what,
                  static_cast<unsigned long>(status));
    return buf;
}
#else
std::string notSupported() { return "密码保险库目前只支持 Windows（需要 CNG）"; }
#endif

// ---------------------------------------------------------------- 加密原语

#ifdef _WIN32

bool randomBytes(std::uint8_t* out, std::size_t len) {
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

class AlgHandle {
public:
    AlgHandle() = default;
    ~AlgHandle() {
        if (handle_) BCryptCloseAlgorithmProvider(handle_, 0);
    }
    AlgHandle(const AlgHandle&) = delete;
    AlgHandle& operator=(const AlgHandle&) = delete;

    bool open(LPCWSTR name, ULONG flags = 0) {
        return BCryptOpenAlgorithmProvider(&handle_, name, nullptr, flags) >= 0;
    }
    BCRYPT_ALG_HANDLE get() const { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class KeyHandle {
public:
    KeyHandle() = default;
    ~KeyHandle() {
        if (key_) BCryptDestroyKey(key_);
    }
    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;

    bool generate(BCRYPT_ALG_HANDLE alg, const std::uint8_t* key, std::size_t len) {
        return BCryptGenerateSymmetricKey(alg, &key_, nullptr, 0,
                                          const_cast<PUCHAR>(key), static_cast<ULONG>(len),
                                          0) >= 0;
    }
    BCRYPT_KEY_HANDLE get() const { return key_; }

private:
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

bool aesGcmSeal(const std::uint8_t* key, const std::uint8_t* nonce, const std::uint8_t* aad,
                std::size_t aadLen, const std::uint8_t* plain, std::size_t plainLen,
                std::uint8_t* out, std::uint8_t* tag) {
    AlgHandle aes;
    if (!aes.open(BCRYPT_AES_ALGORITHM)) return false;
    const wchar_t* mode = BCRYPT_CHAIN_MODE_GCM;
    if (BCryptSetProperty(aes.get(), BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)),
                          static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0) < 0)
        return false;

    KeyHandle keyHandle;
    if (!keyHandle.generate(aes.get(), key, kDekBytes)) return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = static_cast<ULONG>(kNonceBytes);
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aadLen);
    info.pbTag = tag;
    info.cbTag = static_cast<ULONG>(kTagBytes);

    ULONG done = 0;
    return BCryptEncrypt(keyHandle.get(), const_cast<PUCHAR>(plain),
                         static_cast<ULONG>(plainLen), &info, nullptr, 0, out,
                         static_cast<ULONG>(plainLen), &done, 0) >= 0 &&
           done == plainLen;
}

bool aesGcmOpen(const std::uint8_t* key, const std::uint8_t* nonce, const std::uint8_t* aad,
                std::size_t aadLen, const std::uint8_t* cipher, std::size_t cipherLen,
                const std::uint8_t* tag, std::uint8_t* out) {
    AlgHandle aes;
    if (!aes.open(BCRYPT_AES_ALGORITHM)) return false;
    const wchar_t* mode = BCRYPT_CHAIN_MODE_GCM;
    if (BCryptSetProperty(aes.get(), BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)),
                          static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0) < 0)
        return false;

    KeyHandle keyHandle;
    if (!keyHandle.generate(aes.get(), key, kDekBytes)) return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = static_cast<ULONG>(kNonceBytes);
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aadLen);
    info.pbTag = const_cast<PUCHAR>(tag);
    info.cbTag = static_cast<ULONG>(kTagBytes);

    ULONG done = 0;
    return BCryptDecrypt(keyHandle.get(), const_cast<PUCHAR>(cipher),
                         static_cast<ULONG>(cipherLen), &info, nullptr, 0, out,
                         static_cast<ULONG>(cipherLen), &done, 0) >= 0 &&
           done == cipherLen;
}

bool deriveKey(const std::wstring& password, const std::uint8_t* salt, std::size_t saltLen,
               std::uint32_t iterations, std::uint8_t* out) {
    std::string utf8 = utf8Of(password);
    AlgHandle hmac;
    bool ok = hmac.open(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (ok) {
        ok = BCryptDeriveKeyPBKDF2(hmac.get(), reinterpret_cast<PUCHAR>(utf8.data()),
                                   static_cast<ULONG>(utf8.size()), const_cast<PUCHAR>(salt),
                                   static_cast<ULONG>(saltLen), iterations, out,
                                   static_cast<ULONG>(kDekBytes), 0) >= 0;
    }
    wipeBytes(utf8.data(), utf8.size());
    return ok;
}

#else  // 非 Windows：没有 CNG，加密原语一律失败（上层会返回明确错误）

bool randomBytes(std::uint8_t*, std::size_t) { return false; }

bool deriveKey(const std::wstring&, const std::uint8_t*, std::size_t, std::uint32_t,
               std::uint8_t*) {
    return false;
}

bool aesGcmSeal(const std::uint8_t*, const std::uint8_t*, const std::uint8_t*, std::size_t,
                const std::uint8_t*, std::size_t, std::uint8_t*, std::uint8_t*) {
    return false;
}

bool aesGcmOpen(const std::uint8_t*, const std::uint8_t*, const std::uint8_t*, std::size_t,
                const std::uint8_t*, std::size_t, const std::uint8_t*, std::uint8_t*) {
    return false;
}

#endif  // _WIN32

// ---------------------------------------------------------------- 序列化

class Writer {
public:
    void u8(std::uint8_t value) { buf_.push_back(value); }

    void u16(std::uint16_t value) {
        buf_.push_back(static_cast<std::uint8_t>(value & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    }

    void u32(std::uint32_t value) {
        for (int i = 0; i < 4; ++i)
            buf_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }

    void bytes(const void* data, std::size_t len) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        buf_.insert(buf_.end(), p, p + len);
    }

    void magic() { bytes(kMagic, sizeof(kMagic)); }

    const std::vector<std::uint8_t>& data() const { return buf_; }

private:
    std::vector<std::uint8_t> buf_;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t len) : data_(data), len_(len) {}

    bool ok() const { return ok_; }
    std::size_t remaining() const { return ok_ ? len_ - off_ : 0; }

    std::uint8_t u8() {
        const std::uint8_t* p = take(1);
        return p ? *p : 0;
    }

    std::uint16_t u16() {
        const std::uint8_t* p = take(2);
        return p ? static_cast<std::uint16_t>(p[0] | (p[1] << 8)) : 0;
    }

    std::uint32_t u32() {
        const std::uint8_t* p = take(4);
        if (!p) return 0;
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
    }

    bool bytes(void* out, std::size_t count) {
        const std::uint8_t* p = take(count);
        if (!p) return false;
        std::memcpy(out, p, count);
        return true;
    }

    bool skip(std::size_t count) { return take(count) != nullptr; }

private:
    const std::uint8_t* take(std::size_t count) {
        if (!ok_ || count > len_ - off_) {
            ok_ = false;
            return nullptr;
        }
        const std::uint8_t* p = data_ + off_;
        off_ += count;
        return p;
    }

    const std::uint8_t* data_;
    std::size_t len_;
    std::size_t off_ = 0;
    bool ok_ = true;
};

struct RawVault {
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint32_t iterations = 0;
    std::array<std::uint8_t, kSaltBytes> salt{};
    bool hasTpm = false;
    std::array<std::uint8_t, kPasswordSlotBytes> passwordSlot{};
    std::array<std::uint8_t, kNonceBytes> passwordSlotNonce{};
    std::vector<std::uint8_t> tpmBlob;
    std::array<std::uint8_t, kNonceBytes> tpmNonce{};
    std::array<std::uint8_t, kNonceBytes> payloadNonce{};
    std::vector<std::uint8_t> payload;
    std::array<std::uint8_t, kTagBytes> payloadTag{};
};

std::vector<std::uint8_t> slotAad(std::uint8_t type, std::uint32_t iterations,
                                  const std::uint8_t* salt) {
    Writer w;
    w.magic();
    w.u16(kVaultFormatVersion);
    w.u8(type);
    w.u32(iterations);
    w.bytes(salt, kSaltBytes);
    return w.data();
}

std::vector<std::uint8_t> payloadAad() {
    Writer w;
    w.magic();
    w.u16(kVaultFormatVersion);
    w.u8(0x50);  // 'P'：条目区专用标签
    return w.data();
}

std::vector<std::uint8_t> encodeRaw(const RawVault& raw) {
    Writer w;
    w.magic();
    w.u16(raw.version);
    w.u16(raw.flags);
    w.u32(raw.iterations);
    w.bytes(raw.salt.data(), raw.salt.size());

    w.u8(static_cast<std::uint8_t>(raw.hasTpm ? 2u : 1u));
    w.u8(kSlotPassword);
    w.u8(0);
    w.u16(0);
    w.u32(static_cast<std::uint32_t>(kPasswordSlotBytes));
    w.bytes(raw.passwordSlotNonce.data(), raw.passwordSlotNonce.size());
    w.bytes(raw.passwordSlot.data(), raw.passwordSlot.size());
    if (raw.hasTpm) {
        w.u8(kSlotTpm);
        w.u8(0);
        w.u16(0);
        w.u32(static_cast<std::uint32_t>(raw.tpmBlob.size()));
        w.bytes(raw.tpmNonce.data(), raw.tpmNonce.size());
        w.bytes(raw.tpmBlob.data(), raw.tpmBlob.size());
    }

    w.bytes(raw.payloadNonce.data(), raw.payloadNonce.size());
    w.u32(static_cast<std::uint32_t>(raw.payload.size()));
    w.bytes(raw.payload.data(), raw.payload.size());
    w.bytes(raw.payloadTag.data(), raw.payloadTag.size());
    return w.data();
}

bool decodeRaw(const std::vector<std::uint8_t>& file, RawVault& raw, std::string* error) {
    Reader r(file.data(), file.size());
    char magic[8] = {};
    if (!r.bytes(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(magic)) != 0) {
        if (error) *error = "文件不是 rshell 密码保险库（magic 不匹配）";
        return false;
    }
    raw.version = r.u16();
    raw.flags = r.u16();
    raw.iterations = r.u32();
    r.bytes(raw.salt.data(), raw.salt.size());
    if (!r.ok()) {
        if (error) *error = "保险库文件头被截断";
        return false;
    }
    if (raw.version != kVaultFormatVersion) {
        if (error) {
            *error = raw.version > kVaultFormatVersion
                         ? "保险库由更高版本的 rshell 创建，请升级后再打开"
                         : "保险库格式版本过旧，无法读取";
        }
        return false;
    }
    if (raw.iterations == 0 || raw.iterations > 100000000u) {
        if (error) *error = "保险库的 KDF 参数异常";
        return false;
    }

    const std::uint8_t slotCount = r.u8();
    if (!r.ok() || slotCount == 0 || slotCount > kMaxSlots) {
        if (error) *error = "保险库 slot 数量异常";
        return false;
    }

    bool havePassword = false;
    for (std::uint8_t i = 0; i < slotCount; ++i) {
        const std::uint8_t type = r.u8();
        const std::uint8_t slotFlags = r.u8();
        r.u16();  // reserved
        const std::uint32_t len = r.u32();
        if (!r.ok() || slotFlags != 0 || len > 8192) {
            if (error) *error = "保险库 slot 描述符损坏";
            return false;
        }
        if (type == kSlotPassword) {
            if (len != kPasswordSlotBytes || havePassword) {
                if (error) *error = "保险库主密码 slot 损坏";
                return false;
            }
            if (!r.bytes(raw.passwordSlotNonce.data(), raw.passwordSlotNonce.size()) ||
                !r.bytes(raw.passwordSlot.data(), raw.passwordSlot.size())) {
                if (error) *error = "保险库主密码 slot 被截断";
                return false;
            }
            havePassword = true;
        } else if (type == kSlotTpm) {
            if (len == 0 || raw.hasTpm) {
                if (error) *error = "保险库 TPM slot 损坏";
                return false;
            }
            raw.tpmBlob.resize(len);
            if (!r.bytes(raw.tpmNonce.data(), raw.tpmNonce.size()) ||
                !r.bytes(raw.tpmBlob.data(), len)) {
                if (error) *error = "保险库 TPM slot 被截断";
                return false;
            }
            raw.hasTpm = true;
        } else if (!r.skip(len)) {
            // 未知 slot：跳过，保证旧版本能读新版本多出来的解锁方式
            if (error) *error = "保险库 slot 被截断";
            return false;
        }
    }
    if (!havePassword) {
        if (error) *error = "保险库缺少主密码 slot";
        return false;
    }

    r.bytes(raw.payloadNonce.data(), raw.payloadNonce.size());
    const std::uint32_t payloadLen = r.u32();
    if (!r.ok() || payloadLen == 0 || payloadLen > 16u * 1024u * 1024u) {
        if (error) *error = "保险库条目区长度异常";
        return false;
    }
    raw.payload.resize(payloadLen);
    if (!r.bytes(raw.payload.data(), payloadLen) ||
        !r.bytes(raw.payloadTag.data(), raw.payloadTag.size())) {
        if (error) *error = "保险库条目区被截断";
        return false;
    }
    if (r.remaining() != 0) {
        if (error) *error = "保险库文件尾部有多余数据";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 条目表

std::vector<std::uint8_t> encodeEntries(const std::map<std::string, ProtectedSecret>& entries) {
    Writer w;
    w.u32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& item : entries) {
        const std::string secret = item.second.revealUtf8();
        w.u16(static_cast<std::uint16_t>(item.first.size()));
        w.bytes(item.first.data(), item.first.size());
        w.u32(static_cast<std::uint32_t>(secret.size()));
        w.bytes(secret.data(), secret.size());
    }
    return w.data();
}

bool decodeEntries(const std::uint8_t* data, std::size_t len,
                   std::map<std::string, ProtectedSecret>& out, std::string* error) {
    Reader r(data, len);
    const std::uint32_t count = r.u32();
    if (!r.ok() || count > 100000) {
        if (error) *error = "保险库条目数量异常";
        return false;
    }
    std::map<std::string, ProtectedSecret> parsed;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint16_t nameLen = r.u16();
        if (!r.ok() || nameLen == 0 || nameLen > kMaxNameBytes) {
            if (error) *error = "保险库条目名长度异常";
            return false;
        }
        std::string name(nameLen, '\0');
        if (!r.bytes(name.data(), nameLen)) {
            if (error) *error = "保险库条目名被截断";
            return false;
        }
        const std::uint32_t secretLen = r.u32();
        if (!r.ok() || secretLen > kMaxSecretBytes) {
            if (error) *error = "保险库条目口令长度异常";
            return false;
        }
        std::string secret(secretLen, '\0');
        if (!r.bytes(secret.data(), secretLen)) {
            if (error) *error = "保险库条目口令被截断";
            return false;
        }
        const bool duplicate = parsed.find(name) != parsed.end();
        if (!duplicate) {
            ProtectedSecret holder;
            if (!secret.empty()) holder.assignUtf8(secret);
            parsed.emplace(name, std::move(holder));
        }
        wipeBytes(secret.data(), secret.size());
        if (duplicate) {
            if (error) *error = "保险库里有重名条目";
            return false;
        }
    }
    if (r.remaining() != 0) {
        if (error) *error = "保险库条目区尾部有多余数据";
        return false;
    }
    out = std::move(parsed);
    return true;
}

// ---------------------------------------------------------------- 文件读写

bool readFile(const std::wstring& path, std::vector<std::uint8_t>& out, std::string* error) {
    const std::filesystem::path file(path);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        if (error) *error = "保险库文件不存在";
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "无法读取保险库文件（权限不足或文件被占用）";
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size < 0 || size > 32 * 1024 * 1024) {
        if (error) *error = "保险库文件大小异常";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), size)) {
        if (error) *error = "读取保险库文件失败";
        return false;
    }
    return true;
}

bool writeFileAtomic(const std::wstring& path, const std::vector<std::uint8_t>& data,
                     std::string* error) {
    const std::filesystem::path target(path);
    std::filesystem::path temp = target;
    temp += L".tmp";

    std::error_code ec;
    if (!target.parent_path().empty()) std::filesystem::create_directories(target.parent_path(), ec);

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "无法写入保险库文件（目录不可写？）";
            return false;
        }
        if (!data.empty()) out.write(reinterpret_cast<const char*>(data.data()),
                                     static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(temp, ec);
            if (error) *error = "写入保险库文件失败（磁盘已满？）";
            return false;
        }
    }

#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::filesystem::remove(temp, ec);
        if (error) {
            *error = "替换保险库文件失败：" + std::system_category().message(static_cast<int>(code));
        }
        return false;
    }
#else
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        if (error) *error = "替换保险库文件失败：" + ec.message();
        return false;
    }
#endif
    return true;
}

// ---------------------------------------------------------------- TPM

#ifdef _WIN32

struct TpmHandle {
    NCRYPT_PROV_HANDLE prov = 0;
    NCRYPT_KEY_HANDLE key = 0;
    ~TpmHandle() {
        if (key) NCryptFreeObject(key);
        if (prov) NCryptFreeObject(prov);
    }
};

// TPM 里每个保险库文件用一把独立的持久密钥（名字取路径哈希）
std::wstring tpmKeyName(const std::wstring& path) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const wchar_t ch : path) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    wchar_t buf[40] = {};
    std::swprintf(buf, 40, L"rshell-vault-%016llx",
                  static_cast<unsigned long long>(hash));
    return std::wstring(buf);
}

bool tpmPresent() {
    NCRYPT_PROV_HANDLE prov = 0;
    if (NCryptOpenStorageProvider(&prov, MS_PLATFORM_KEY_STORAGE_PROVIDER, 0) < 0) return false;
    DWORD impl = 0;
    DWORD size = 0;
    const LONG status = NCryptGetProperty(prov, NCRYPT_IMPL_TYPE_PROPERTY,
                                         reinterpret_cast<PBYTE>(&impl), sizeof(impl), &size, 0);
    NCryptFreeObject(prov);
    return status >= 0 && (impl & NCRYPT_IMPL_HARDWARE_FLAG) != 0;
}

// 打开（必要时创建）持久密钥。创建时声明 UI policy：使用该密钥需要用户在场。
bool tpmEnsureKey(TpmHandle& handle, const std::wstring& name, bool allowCreate,
                  std::string* error) {
    if (NCryptOpenStorageProvider(&handle.prov, MS_PLATFORM_KEY_STORAGE_PROVIDER, 0) < 0) {
        if (error) *error = "本机没有可用的 TPM（打不开 Platform Crypto Provider）";
        return false;
    }
    LONG status = NCryptOpenKey(handle.prov, &handle.key, name.c_str(), 0, 0);
    if (status >= 0) return true;

    if (!allowCreate) {
        if (error) *error = "本机 TPM 中没有该保险库的密钥（重装系统或换了机器）";
        return false;
    }

    status = NCryptCreatePersistedKey(handle.prov, &handle.key, NCRYPT_RSA_ALGORITHM,
                                      name.c_str(), 0, 0);
    if (status < 0) {
        if (error) *error = ntError("无法在 TPM 中创建密钥", status);
        return false;
    }

    DWORD bits = 2048;
    NCryptSetProperty(handle.key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&bits),
                      sizeof(bits), 0);

    NCRYPT_UI_POLICY policy{};
    policy.dwVersion = 1;
    policy.pszCreationTitle = const_cast<LPWSTR>(L"rshell 密码保险库");
    policy.pszFriendlyName = const_cast<LPWSTR>(L"rshell 密码保险库主密钥");
    policy.pszDescription = const_cast<LPWSTR>(L"用于解锁 rshell 保存的 SSH 密码");
    policy.dwFlags = NCRYPT_UI_PROTECT_KEY_FLAG;
    NCryptSetProperty(handle.key, NCRYPT_UI_POLICY_PROPERTY,
                      reinterpret_cast<PBYTE>(&policy), sizeof(policy), 0);

    status = NCryptFinalizeKey(handle.key, 0);
    if (status < 0) {
        if (error) *error = ntError("TPM 密钥创建失败", status);
        return false;
    }
    return true;
}

bool tpmWrap(const std::wstring& path, const std::uint8_t* dek, std::vector<std::uint8_t>& out,
             std::string* error) {
    TpmHandle handle;
    if (!tpmEnsureKey(handle, tpmKeyName(path), true, error)) return false;

    // 关键：Platform Crypto Provider 不接受“默认” OAEP 参数（传 nullptr 会返回
    // NTE_INVALID_PARAMETER 0x80090027），必须显式给出 OAEP 用的哈希算法。
    // TPM 2.0 强制支持 SHA-256；解包端必须用同一个哈希（这里写死在代码里）。
    BCRYPT_OAEP_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM, nullptr, 0};

    DWORD needed = 0;
    LONG status = NCryptEncrypt(handle.key, const_cast<PBYTE>(dek),
                                static_cast<DWORD>(kDekBytes), &padding, nullptr, 0, &needed,
                                NCRYPT_PAD_OAEP_FLAG);
    if (status < 0 || needed == 0) {
        if (error) {
            *error = ntError("TPM 加密失败", status);
            if (static_cast<unsigned long>(status) == 0x80090027ul) {
                *error += "（该 TPM 不支持 RSA-OAEP-SHA256，需要 TPM 2.0）";
            }
        }
        return false;
    }
    out.assign(needed, 0);
    DWORD written = 0;
    status = NCryptEncrypt(handle.key, const_cast<PBYTE>(dek),
                           static_cast<DWORD>(kDekBytes), &padding, out.data(), needed, &written,
                           NCRYPT_PAD_OAEP_FLAG);
    if (status < 0 || written != needed) {
        out.clear();
        if (error) *error = ntError("TPM 加密失败", status);
        return false;
    }
    return true;
}

bool tpmUnwrap(const std::wstring& path, const std::vector<std::uint8_t>& blob,
               std::uint8_t* dek, std::string* error) {
    TpmHandle handle;
    if (!tpmEnsureKey(handle, tpmKeyName(path), false, error)) return false;

    BCRYPT_OAEP_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM, nullptr, 0};

    // 注意：PCP 对 NCryptDecrypt 的“先问长度”调用只回 SUCCESS 而不填长度
    // （needed 保持 0），所以这里直接用 kDekBytes 的缓冲一次解出来。
    DWORD written = 0;
    LONG status = NCryptDecrypt(handle.key, const_cast<PBYTE>(blob.data()),
                                static_cast<DWORD>(blob.size()), &padding, dek,
                                static_cast<DWORD>(kDekBytes), &written, NCRYPT_PAD_OAEP_FLAG);
    if (status < 0 || written != kDekBytes) {
        if (error) {
            *error = ntError("TPM 解密失败（密钥与本文件不匹配，或用户取消了确认）", status);
        }
        return false;
    }
    return true;
}

// 删掉该保险库在 TPM 里的持久密钥（去掉 TPM slot 后用不到了）。
// 残留密钥本身不含秘密，只白占一个 TPM 密钥槽，所以删除失败不报错。
void tpmDeleteKey(const std::wstring& path) {
    NCRYPT_PROV_HANDLE prov = 0;
    if (NCryptOpenStorageProvider(&prov, MS_PLATFORM_KEY_STORAGE_PROVIDER, 0) < 0) return;
    NCRYPT_KEY_HANDLE key = 0;
    if (NCryptOpenKey(prov, &key, tpmKeyName(path).c_str(), 0, 0) >= 0) {
        // 成功时 NCryptDeleteKey 会自己释放句柄；失败时句柄仍然有效
        if (NCryptDeleteKey(key, 0) < 0) NCryptFreeObject(key);
    }
    NCryptFreeObject(prov);
}

#else

bool tpmPresent() { return false; }

bool tpmWrap(const std::wstring&, const std::uint8_t*, std::vector<std::uint8_t>&,
             std::string* error) {
    if (error) *error = notSupported();
    return false;
}

bool tpmUnwrap(const std::wstring&, const std::vector<std::uint8_t>&, std::uint8_t*,
               std::string* error) {
    if (error) *error = notSupported();
    return false;
}

void tpmDeleteKey(const std::wstring&) {}

#endif  // _WIN32

}  // namespace

namespace {

bool lockBytes(const void* data, std::size_t bytes) {
#ifdef _WIN32
    if (!data || bytes == 0) return false;
    return VirtualLock(const_cast<void*>(data), bytes) != FALSE;
#else
    (void)data;
    (void)bytes;
    return false;
#endif
}

void unlockBytes(const void* data, std::size_t bytes) {
#ifdef _WIN32
    if (!data || bytes == 0) return;
    VirtualUnlock(const_cast<void*>(data), bytes);
#else
    (void)data;
    (void)bytes;
#endif
}

}  // namespace

// 把已解出的 DEK + 原始文件内容组装成可用的 Vault（主密码和 TPM 两条路共用）
struct VaultLoader {
    static bool adopt(const std::wstring& path, RawVault& raw,
                      const std::array<std::uint8_t, kDekBytes>& dek, Vault& out,
                      std::string* error) {
        Vault vault;
        vault.path_ = path;
        vault.iterations_ = raw.iterations;
        vault.salt_ = raw.salt;
        vault.dek_ = dek;
        vault.lockDek();
        vault.passwordSlot_ = raw.passwordSlot;
        vault.passwordSlotNonce_ = raw.passwordSlotNonce;
        vault.hasTpmSlot_ = raw.hasTpm;
        vault.tpmBlob_ = std::move(raw.tpmBlob);
        vault.tpmNonce_ = raw.tpmNonce;

        std::vector<std::uint8_t> plain(raw.payload.size());
        const std::vector<std::uint8_t> aad = payloadAad();
        if (!aesGcmOpen(vault.dek_.data(), raw.payloadNonce.data(), aad.data(), aad.size(),
                        raw.payload.data(), raw.payload.size(), raw.payloadTag.data(),
                        plain.data())) {
            if (error) *error = "保险库条目区解密失败（文件已损坏或被篡改）";
            vault.lock();
            return false;
        }
        const bool parsed = decodeEntries(plain.data(), plain.size(), vault.entries_, error);
        wipeBytes(plain.data(), plain.size());
        if (!parsed) {
            vault.lock();
            return false;
        }
        vault.unlocked_ = true;
        out = std::move(vault);
        return true;
    }
};

Vault::Vault() = default;

Vault::~Vault() {
    entries_.clear();
    wipeDek();
    wipeTpmBlob();
    wipeBytes(passwordSlot_.data(), passwordSlot_.size());
    wipeBytes(salt_.data(), salt_.size());
}

Vault::Vault(Vault&& other) noexcept { *this = std::move(other); }

Vault& Vault::operator=(Vault&& other) noexcept {
    if (this == &other) return *this;

    entries_.clear();
    wipeDek();
    wipeTpmBlob();

    path_ = std::move(other.path_);
    iterations_ = other.iterations_;
    salt_ = other.salt_;
    dek_ = other.dek_;
    passwordSlotNonce_ = other.passwordSlotNonce_;
    passwordSlot_ = other.passwordSlot_;
    tpmBlob_ = std::move(other.tpmBlob_);
    tpmNonce_ = other.tpmNonce_;
    unlocked_ = other.unlocked_;
    hasTpmSlot_ = other.hasTpmSlot_;
    entries_ = std::move(other.entries_);
    dirty_ = other.dirty_;
    dekLocked_ = false;
    if (unlocked_) lockDek();

    // 源头立刻抹掉，避免同一份密钥在堆上留两份
    other.wipeDek();
    other.wipeTpmBlob();
    wipeBytes(other.passwordSlot_.data(), other.passwordSlot_.size());
    wipeBytes(other.salt_.data(), other.salt_.size());
    other.unlocked_ = false;
    other.hasTpmSlot_ = false;
    other.dirty_ = false;
    return *this;
}

void Vault::lockDek() { dekLocked_ = lockBytes(dek_.data(), dek_.size()); }

void Vault::wipeDek() {
    wipeBytes(dek_.data(), dek_.size());
    if (dekLocked_) {
        unlockBytes(dek_.data(), dek_.size());
        dekLocked_ = false;
    }
}

void Vault::wipeTpmBlob() {
    if (!tpmBlob_.empty()) {
        wipeBytes(tpmBlob_.data(), tpmBlob_.size());
        tpmBlob_.clear();
        tpmBlob_.shrink_to_fit();
    }
}

void Vault::lock() {
    entries_.clear();
    wipeDek();
    wipeTpmBlob();
    wipeBytes(passwordSlot_.data(), passwordSlot_.size());
    wipeBytes(passwordSlotNonce_.data(), passwordSlotNonce_.size());
    wipeBytes(salt_.data(), salt_.size());
    unlocked_ = false;
    hasTpmSlot_ = false;
    dirty_ = false;
}

bool Vault::openWithTpm(const std::wstring& path, Vault& out, std::string* error) {
    std::vector<std::uint8_t> file;
    if (!readFile(path, file, error)) return false;

    RawVault raw;
    if (!decodeRaw(file, raw, error)) return false;
    if (!raw.hasTpm) {
        if (error) *error = "该保险库没有启用 TPM 解锁";
        return false;
    }

    std::array<std::uint8_t, kDekBytes> dek{};
    if (!tpmUnwrap(path, raw.tpmBlob, dek.data(), error)) {
        wipeBytes(dek.data(), dek.size());
        return false;
    }
    Vault vault;
    const bool adopted = VaultLoader::adopt(path, raw, dek, vault, error);
    wipeBytes(dek.data(), dek.size());
    if (!adopted) return false;
    out = std::move(vault);
    return true;
}

bool Vault::save(std::string* error) {
    if (!unlocked_) {
        if (error) *error = "保险库处于锁定状态";
        return false;
    }

    std::vector<std::uint8_t> plain = encodeEntries(entries_);

    RawVault raw;
    raw.version = kVaultFormatVersion;
    raw.flags = static_cast<std::uint16_t>(hasTpmSlot_ ? 1 : 0);
    raw.iterations = iterations_;
    raw.salt = salt_;
    raw.passwordSlot = passwordSlot_;
    raw.passwordSlotNonce = passwordSlotNonce_;
    raw.hasTpm = hasTpmSlot_;
    raw.tpmBlob = tpmBlob_;
    raw.tpmNonce = tpmNonce_;

    bool sealed = randomBytes(raw.payloadNonce.data(), raw.payloadNonce.size());
    if (sealed) {
        raw.payload.resize(plain.size());
        const std::vector<std::uint8_t> aad = payloadAad();
        sealed = aesGcmSeal(dek_.data(), raw.payloadNonce.data(), aad.data(), aad.size(),
                            plain.data(), plain.size(), raw.payload.data(),
                            raw.payloadTag.data());
    }
    wipeBytes(plain.data(), plain.size());
    if (!sealed) {
        if (error) *error = "条目加密失败";
        return false;
    }

    if (!writeFileAtomic(path_, encodeRaw(raw), error)) return false;
    dirty_ = false;
    return true;
}

bool Vault::changeMasterPassword(const std::wstring& newPassword, std::string* error) {
    if (!unlocked_) {
        if (error) *error = "保险库处于锁定状态";
        return false;
    }
    if (newPassword.empty()) {
        if (error) *error = "主密码不能为空";
        return false;
    }

    std::array<std::uint8_t, kSaltBytes> salt{};
    std::array<std::uint8_t, kNonceBytes> nonce{};
    if (!randomBytes(salt.data(), salt.size()) || !randomBytes(nonce.data(), nonce.size())) {
        if (error) *error = "无法获取随机数（CNG 不可用）";
        return false;
    }

    std::array<std::uint8_t, kDekBytes> kek{};
    if (!deriveKey(newPassword, salt.data(), salt.size(), iterations_, kek.data())) {
        if (error) *error = "主密码派生失败（CNG 不可用）";
        return false;
    }
    const std::vector<std::uint8_t> aad = slotAad(kSlotPassword, iterations_, salt.data());
    std::array<std::uint8_t, kDekBytes> cipher{};
    std::array<std::uint8_t, kTagBytes> tag{};
    const bool sealed = aesGcmSeal(kek.data(), nonce.data(), aad.data(), aad.size(),
                                   dek_.data(), kDekBytes, cipher.data(), tag.data());
    wipeBytes(kek.data(), kek.size());
    if (!sealed) {
        if (error) *error = "主密钥加密失败";
        return false;
    }

    // 换 salt/口令只影响 DEK 的包裹方式，条目密文原样保留
    const auto oldSalt = salt_;
    const auto oldNonce = passwordSlotNonce_;
    const auto oldSlot = passwordSlot_;
    salt_ = salt;
    passwordSlotNonce_ = nonce;
    std::memcpy(passwordSlot_.data(), cipher.data(), cipher.size());
    std::memcpy(passwordSlot_.data() + cipher.size(), tag.data(), tag.size());

    if (!save(error)) {
        salt_ = oldSalt;
        passwordSlotNonce_ = oldNonce;
        passwordSlot_ = oldSlot;
        return false;
    }
    return true;
}

bool Vault::enableTpm(std::string* error) {
    if (!unlocked_) {
        if (error) *error = "保险库处于锁定状态";
        return false;
    }
    std::vector<std::uint8_t> blob;
    if (!tpmWrap(path_, dek_.data(), blob, error)) return false;

    std::vector<std::uint8_t> oldBlob = std::move(tpmBlob_);
    const bool oldHasTpm = hasTpmSlot_;
    tpmBlob_ = std::move(blob);
    hasTpmSlot_ = true;
    if (!save(error)) {
        tpmBlob_ = std::move(oldBlob);
        hasTpmSlot_ = oldHasTpm;
        return false;
    }
    return true;
}

bool Vault::disableTpm(std::string* error) {
    if (!unlocked_) {
        if (error) *error = "保险库处于锁定状态";
        return false;
    }
    if (!hasTpmSlot_) return true;

    std::vector<std::uint8_t> oldBlob = std::move(tpmBlob_);
    hasTpmSlot_ = false;
    if (!save(error)) {
        tpmBlob_ = std::move(oldBlob);
        hasTpmSlot_ = true;
        return false;
    }
    wipeBytes(oldBlob.data(), oldBlob.size());  // 文件里已删掉，内存副本也抹掉
    tpmDeleteKey(path_);                        // TPM 里的持久密钥也一并清掉
    return true;
}

bool Vault::tpmAvailable() { return tpmPresent(); }

bool Vault::tpmSlotPresent(const std::wstring& path) {
    // 只解析文件头/slot 描述，不做任何派生或解密：调用方（GUI）在对话框
    // 弹出前就能知道要不要给「用 TPM 解锁」按钮。失败原因这里不关心，
    // 文件不存在和文件损坏对界面是同一件事——都退回主密码。
    std::vector<std::uint8_t> file;
    if (!readFile(path, file, nullptr)) return false;
    RawVault raw;
    if (!decodeRaw(file, raw, nullptr)) return false;
    return raw.hasTpm;
}

std::wstring Vault::defaultPath() {
#ifdef _WIN32
    std::wstring base;
    const DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (needed > 1) {
        base.resize(needed);
        const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), needed);
        base.resize(written);
    }
    if (base.empty()) {
        // 环境变量缺失（极简服务账户）：退到 exe 同级的 data 目录
        wchar_t module[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, module, MAX_PATH) > 0) {
            base = (std::filesystem::path(module).parent_path() / L"data").wstring();
        }
    }
    return (std::filesystem::path(base) / L"rshell" / L"vault.dat").wstring();
#else
    return L"vault.dat";
#endif
}

// ---------------------------------------------------------------- 条目

bool Vault::has(const std::string& name) const { return entries_.find(name) != entries_.end(); }

bool Vault::get(const std::string& name, std::string& secret) const {
    const auto it = entries_.find(name);
    if (it == entries_.end()) return false;
    secret = it->second.revealUtf8();
    return true;
}

void Vault::set(const std::string& name, const std::string& secret) {
    if (!unlocked_ || name.empty() || name.size() > kMaxNameBytes ||
        secret.size() > kMaxSecretBytes) {
        return;
    }
    ProtectedSecret holder;
    if (!secret.empty()) holder.assignUtf8(secret);
    entries_[name] = std::move(holder);
    dirty_ = true;
}

bool Vault::remove(const std::string& name) {
    const auto it = entries_.find(name);
    if (it == entries_.end()) return false;
    entries_.erase(it);
    dirty_ = true;
    return true;
}

std::vector<std::string> Vault::names() const {
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto& item : entries_) result.push_back(item.first);
    return result;
}

// ---------------------------------------------------------------- 创建 / 打开

bool Vault::create(const std::wstring& path, const std::wstring& masterPassword, Vault& out,
                   std::uint32_t iterations, std::string* error) {
    if (path.empty()) {
        if (error) *error = "保险库路径为空";
        return false;
    }
    if (masterPassword.empty()) {
        if (error) *error = "主密码不能为空";
        return false;
    }
    if (iterations < 1000) iterations = 1000;
    if (iterations > 100000000u) iterations = 100000000u;

    Vault vault;
    vault.path_ = path;
    vault.iterations_ = iterations;
    if (!randomBytes(vault.salt_.data(), vault.salt_.size()) ||
        !randomBytes(vault.dek_.data(), vault.dek_.size()) ||
        !randomBytes(vault.passwordSlotNonce_.data(), vault.passwordSlotNonce_.size())) {
        if (error) *error = "无法获取随机数（CNG 不可用）";
        return false;
    }
    vault.lockDek();

    std::array<std::uint8_t, kDekBytes> kek{};
    if (!deriveKey(masterPassword, vault.salt_.data(), vault.salt_.size(), iterations, kek.data())) {
        if (error) *error = "主密码派生失败（CNG 不可用）";
        return false;
    }
    const std::vector<std::uint8_t> aad =
        slotAad(kSlotPassword, iterations, vault.salt_.data());
    std::array<std::uint8_t, kDekBytes> cipher{};
    std::array<std::uint8_t, kTagBytes> tag{};
    const bool sealed = aesGcmSeal(kek.data(), vault.passwordSlotNonce_.data(), aad.data(),
                                   aad.size(), vault.dek_.data(), kDekBytes, cipher.data(),
                                   tag.data());
    wipeBytes(kek.data(), kek.size());
    if (!sealed) {
        if (error) *error = "主密钥加密失败";
        return false;
    }
    std::memcpy(vault.passwordSlot_.data(), cipher.data(), cipher.size());
    std::memcpy(vault.passwordSlot_.data() + cipher.size(), tag.data(), tag.size());

    vault.unlocked_ = true;
    if (!vault.save(error)) {
        vault.lock();
        return false;
    }
    out = std::move(vault);
    return true;
}

bool Vault::open(const std::wstring& path, const std::wstring& masterPassword, Vault& out,
                 std::string* error) {
    std::vector<std::uint8_t> file;
    if (!readFile(path, file, error)) return false;

    RawVault raw;
    if (!decodeRaw(file, raw, error)) return false;

    std::array<std::uint8_t, kDekBytes> kek{};
    if (!deriveKey(masterPassword, raw.salt.data(), raw.salt.size(), raw.iterations,
                   kek.data())) {
        if (error) *error = "主密码派生失败（CNG 不可用）";
        return false;
    }
    const std::vector<std::uint8_t> aad =
        slotAad(kSlotPassword, raw.iterations, raw.salt.data());
    std::array<std::uint8_t, kDekBytes> dek{};
    const bool opened =
        aesGcmOpen(kek.data(), raw.passwordSlotNonce.data(), aad.data(), aad.size(),
                   raw.passwordSlot.data(), kDekBytes, raw.passwordSlot.data() + kDekBytes,
                   dek.data());
    wipeBytes(kek.data(), kek.size());
    if (!opened) {
        wipeBytes(dek.data(), dek.size());
        if (error) *error = "主密码不正确，或保险库文件已被篡改";
        return false;
    }

    Vault vault;
    const bool adopted = VaultLoader::adopt(path, raw, dek, vault, error);
    wipeBytes(dek.data(), dek.size());
    if (!adopted) return false;
    out = std::move(vault);
    return true;
}




}  // namespace rshell

