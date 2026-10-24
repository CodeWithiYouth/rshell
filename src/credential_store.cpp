// credential_store.cpp - 口令持久化：只走密码保险库
#include "credential_store.h"

#include <algorithm>

#include "vault.h"

namespace rshell {

std::string credentialTarget(const std::string& sessionName) {
    // 保持和旧条目名一致：迁移过保险库的用户不用改任何东西
    return "rshell/" + sessionName;
}

std::string keyPassphraseTarget(const std::string& sessionName) {
    // 和登录密码分开存，避免同一会话在密码/私钥认证之间切换时互相覆盖
    return credentialTarget(sessionName) + "/key-passphrase";
}

void wipeString(std::string& text) {
    if (!text.empty()) std::fill(text.begin(), text.end(), '\0');
    text.clear();
}

namespace {

void setErrorText(std::string* error, const std::string& text) {
    if (error) *error = text;
}

// 非空表示 GUI 把密码保险库接成了后端（见 setCredentialVault）
Vault* g_vault = nullptr;

bool vaultActive() { return g_vault != nullptr && g_vault->unlocked(); }

}  // namespace

void setCredentialVault(Vault* vault) { g_vault = vault; }

bool credentialVaultActive() { return vaultActive(); }

bool saveCredential(const std::string& target, const std::string& password,
                    std::string* error) {
    if (target.empty()) {
        setErrorText(error, "凭据条目名为空");
        return false;
    }
    if (password.empty()) {
        // 空密码没有保存意义，顺手把旧条目删掉，免得下次静默用旧密码
        return deleteCredential(target, error);
    }
    if (!vaultActive()) {
        setErrorText(error, "密码保险库未解锁，无法保存密码");
        return false;
    }
    g_vault->set(target, password);  // Vault 内部存 DPAPI 密文，这里不留明文
    return g_vault->save(error);
}

bool loadCredential(const std::string& target, std::string& password, std::string* error) {
    if (target.empty()) {
        setErrorText(error, "凭据条目名为空");
        return false;
    }
    if (!vaultActive()) {
        setErrorText(error, "密码保险库未解锁");
        return false;
    }
    if (!g_vault->has(target)) {
        setErrorText(error, "密码保险库里没有这条密码");
        return false;
    }
    if (!g_vault->get(target, password)) {
        setErrorText(error, "密码保险库里的条目读取失败");
        return false;
    }
    return true;
}

bool deleteCredential(const std::string& target, std::string* error) {
    if (target.empty()) {
        setErrorText(error, "凭据条目名为空");
        return false;
    }
    if (!vaultActive()) {
        setErrorText(error, "密码保险库未解锁，无法删除密码");
        return false;
    }
    if (!g_vault->has(target)) return true;  // 本来就没有 = 已经是想要的状态
    g_vault->remove(target);
    return g_vault->save(error);
}

bool hasCredential(const std::string& target) {
    std::string password;
    std::string error;
    const bool ok = loadCredential(target, password, &error);
    wipeString(password);
    return ok;
}

}  // namespace rshell
