// credential_store.h - 密码保险库作为唯一的口令持久化后端
//
// rshell 不使用 Windows 凭据管理器：凭据管理器按「当前登录用户」解密，
// 同一账户下运行的任何程序调一次 CredReadW 都能读到口令。这里改为只认
// 密码保险库（见 vault.h）——没有主密码（或 TPM）就拿不到条目。
//
// sessions.ini 里只写 savepassword=1 这个标记，口令本身只存在于 vault.dat。
// GUI 解锁保险库之后调用 setCredentialVault(&vault)；未解锁时所有读写都失败，
// 由调用方（GUI）引导用户新建或解锁保险库。
#pragma once

#include <string>

namespace rshell {

class Vault;  // 前置声明：只有 GUI 会把保险库接成后端

// 会话密码在保险库中的条目名：rshell/<sessionName>
std::string credentialTarget(const std::string& sessionName);

// 会话私钥口令的条目名：rshell/<sessionName>/key-passphrase
std::string keyPassphraseTarget(const std::string& sessionName);

// 代理密码的条目名：rshell/<sessionName>/proxy-password
std::string proxyPasswordTarget(const std::string& sessionName);

// 保存（或覆盖）一条口令。保险库未解锁时返回 false，error（可空）给出原因。
bool saveCredential(const std::string& target, const std::string& password,
                    std::string* error = nullptr);

// 读取密码。保险库未解锁或条目不存在时返回 false，并把原因写进 error（可空）。
bool loadCredential(const std::string& target, std::string& password,
                    std::string* error = nullptr);

// 删除条目。条目本来就不存在也算成功；保险库未解锁时返回 false。
bool deleteCredential(const std::string& target, std::string* error = nullptr);

bool hasCredential(const std::string& target);

// ---------------------------------------------------------------- 后端接入
//
// GUI 新建/解锁保险库成功后调用 setCredentialVault(&g_app.vault)，
// 锁定或退出时传 nullptr 断开。只有「已解锁」的保险库才会接管读写，
// 锁着的保险库即使接上也不生效（Vault::unlocked() 是路由的前提）。

void setCredentialVault(Vault* vault);

// 当前是否有已解锁的保险库接管口令读写。
bool credentialVaultActive();

// 把字符串内容清零（密码用完别留在堆里等下次分配捡到）
void wipeString(std::string& text);

}  // namespace rshell
