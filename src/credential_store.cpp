// credential_store.cpp - 凭据存储：Windows 凭据管理器 + 可选的密码保险库后端
#include "credential_store.h"

#include <algorithm>

#include "vault.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>
#endif

namespace rshell {

std::string credentialTarget(const std::string& sessionName) {
    // "应用/资源" 是微软对 TargetName 的推荐写法，便于在凭据管理器里辨认
    return "rshell/" + sessionName;
}

void wipeString(std::string& text) {
    if (!text.empty()) std::fill(text.begin(), text.end(), '\0');
    text.clear();
}

namespace {

// 路由层也要报错，但 setError 在各平台分支里各有一份，这里单独放一个
void setErrorText(std::string* error, const std::string& text) {
    if (error) *error = text;
}

// 非空表示 GUI 把密码保险库接成了后端（见 setCredentialVault）
Vault* g_vault = nullptr;

bool vaultActive() { return g_vault != nullptr && g_vault->unlocked(); }

}  // namespace

#ifdef _WIN32

namespace {

// 显式做 UTF-8 <-> UTF-16 转换：凭据管理器里存 UTF-16，凭据管理器 UI 才能
// 正常显示和编辑（换成 UTF-8 在界面上会变成乱码）
std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                        nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
}

void secureWipe(std::wstring& text) {
    if (!text.empty()) SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
    text.clear();
}

std::string win32Error(DWORD code) {
    wchar_t* buffer = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text;
    if (buffer && len > 0) {
        while (len > 0 && (buffer[len - 1] == L'\r' || buffer[len - 1] == L'\n' ||
                           buffer[len - 1] == L' ')) {
            buffer[--len] = L'\0';
        }
        text = narrow(std::wstring(buffer, len));
    }
    if (buffer) LocalFree(buffer);
    if (text.empty()) text = "Win32 error " + std::to_string(code);
    return text;
}

void setError(std::string* error, const std::string& text) {
    if (error) *error = text;
}

bool isValidTarget(const std::string& target, std::string* error) {
    if (!target.empty()) return true;
    setError(error, "凭据条目名为空");
    return false;
}

}  // namespace

namespace {

// 下面这几个只在 Windows 凭据管理器上工作，由文件末尾的公共函数按后端路由。
// 返回值语义与公共 API 一致，只是不经过保险库。

bool managerDelete(const std::string& target, std::string* error);

bool managerSave(const std::string& target, const std::string& userLabel,
                 const std::string& password, std::string* error) {
    if (!isValidTarget(target, error)) return false;
    if (password.empty()) {
        // 空密码没有保存意义，顺手把旧条目删掉，免得下次静默用旧密码
        return managerDelete(target, error);
    }

    std::wstring wtarget = widen(target);
    std::wstring wuser = widen(userLabel);
    std::wstring wpassword = widen(password);
    if (wpassword.size() * sizeof(wchar_t) > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        secureWipe(wpassword);
        setError(error, "密码太长，超出凭据管理器单条 2560 字节的上限");
        return false;
    }

    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = wtarget.data();
    cred.UserName = wuser.empty() ? nullptr : wuser.data();
    cred.Comment = const_cast<LPWSTR>(L"rshell 保存的 SSH 登录密码");
    cred.CredentialBlobSize = static_cast<DWORD>(wpassword.size() * sizeof(wchar_t));
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(wpassword.data());
    // LOCAL_MACHINE：客户端后续登录会话都能用；不用 ENTERPRISE，避免密码跟着
    // 域账号的漫游配置同步到其它机器
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    const BOOL ok = CredWriteW(&cred, 0);  // 同名条目会被覆盖
    const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    secureWipe(wpassword);
    if (!ok) {
        setError(error, "写入凭据管理器失败：" + win32Error(code));
        return false;
    }
    return true;
}

bool managerLoad(const std::string& target, std::string& password, std::string* error) {
    if (!isValidTarget(target, error)) return false;
    std::wstring wtarget = widen(target);

    PCREDENTIALW cred = nullptr;
    if (!CredReadW(wtarget.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
        const DWORD code = GetLastError();
        if (code == ERROR_NOT_FOUND) {
            setError(error, "凭据管理器里没有这条密码");
        } else {
            setError(error, "读取凭据管理器失败：" + win32Error(code));
        }
        return false;
    }

    bool ok = true;
    if (!cred->CredentialBlob || cred->CredentialBlobSize == 0) {
        setError(error, "凭据条目里没有密码内容");
        ok = false;
    } else if (cred->CredentialBlobSize % sizeof(wchar_t) != 0) {
        // 不是 UTF-16（可能是别的工具写的），原样当字节串返回
        password.assign(reinterpret_cast<const char*>(cred->CredentialBlob),
                        cred->CredentialBlobSize);
    } else {
        std::wstring wide(reinterpret_cast<const wchar_t*>(cred->CredentialBlob),
                          cred->CredentialBlobSize / sizeof(wchar_t));
        while (!wide.empty() && wide.back() == L'\0') wide.pop_back();
        password = narrow(wide);
        secureWipe(wide);
    }
    // CredReadW 把解密后的副本放在进程堆上，CredFree 之前先抹掉
    if (cred->CredentialBlob && cred->CredentialBlobSize) {
        SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    }
    CredFree(cred);
    return ok;
}

bool managerDelete(const std::string& target, std::string* error) {
    if (!isValidTarget(target, error)) return false;
    std::wstring wtarget = widen(target);
    if (!CredDeleteW(wtarget.c_str(), CRED_TYPE_GENERIC, 0)) {
        const DWORD code = GetLastError();
        if (code != ERROR_NOT_FOUND) {  // 本来就没有 = 已经是想要的状态
            setError(error, "删除凭据失败：" + win32Error(code));
            return false;
        }
    }
    return true;
}

}  // namespace

#else  // 非 Windows：GUI 只在 Windows 构建，这里只是让核心库能编过

namespace {
void setError(std::string* error, const std::string& text) {
    if (error) *error = text;
}
constexpr char kUnsupported[] = "当前平台没有 Windows 凭据管理器，密码不会保存";
}  // namespace

namespace {

bool managerSave(const std::string&, const std::string&, const std::string&,
                 std::string* error) {
    setError(error, kUnsupported);
    return false;
}

bool managerLoad(const std::string&, std::string&, std::string* error) {
    setError(error, kUnsupported);
    return false;
}

bool managerDelete(const std::string&, std::string* error) {
    setError(error, kUnsupported);
    return false;
}

}  // namespace

#endif

// ---------------------------------------------------------------- 后端路由
//
// 保险库接上并且已解锁时优先走保险库；否则（没接、已锁定）退回凭据管理器。
// load 在读不到时回退，是为了让「迁移前存进凭据管理器的旧口令」仍然可用。

void setCredentialVault(Vault* vault) { g_vault = vault; }

bool credentialVaultActive() { return vaultActive(); }

bool deleteManagedCredential(const std::string& target, std::string* error) {
    return managerDelete(target, error);
}

bool saveCredential(const std::string& target, const std::string& userLabel,
                    const std::string& password, std::string* error) {
    if (target.empty()) {
        setErrorText(error, "凭据条目名为空");
        return false;
    }
    if (password.empty()) {
        // 空密码没有保存意义，顺手把旧条目删掉，免得下次静默用旧密码
        return deleteCredential(target, error);
    }
    if (vaultActive()) {
        g_vault->set(target, password);  // Vault 内部存 DPAPI 密文，这里不留明文
        if (!g_vault->save(error)) return false;
        // 口令已经进了保险库：凭据管理器里那份多余，删掉免得同一口令留两处
        managerDelete(target, nullptr);
        return true;
    }
    return managerSave(target, userLabel, password, error);
}

bool loadCredential(const std::string& target, std::string& password, std::string* error) {
    if (target.empty()) {
        setErrorText(error, "凭据条目名为空");
        return false;
    }
    if (vaultActive() && g_vault->has(target)) {
        if (g_vault->get(target, password)) return true;
        setErrorText(error, "密码保险库里的条目读取失败");
        return false;
    }
    return managerLoad(target, password, error);
}

bool deleteCredential(const std::string& target, std::string* error) {
    if (target.empty()) {
        setErrorText(error, "凭据条目名为空");
        return false;
    }
    bool ok = true;
    if (vaultActive() && g_vault->has(target)) {
        g_vault->remove(target);
        ok = g_vault->save(error);
    }
    if (!managerDelete(target, ok ? error : nullptr)) ok = false;
    return ok;
}

bool hasCredential(const std::string& target) {
    std::string password;
    std::string error;
    const bool ok = loadCredential(target, password, &error);
    wipeString(password);
    return ok;
}

}  // namespace rshell
