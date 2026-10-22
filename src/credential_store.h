// credential_store.h - 用系统提供的凭据存储保存 SSH 密码
//
// Windows 10/11 上对外暴露的“保存密码”能力主要有两条路：
//
//   1) 凭据管理器（Credential Manager）：CredWriteW / CredReadW / CredDeleteW
//      头文件 wincred.h，库 advapi32.lib。
//      密码交给系统，按当前用户用 DPAPI 密钥加密后存进用户的凭据库；
//      用户能在“控制面板 → 凭据管理器 → Windows 凭据”里看到条目、改或删。
//
//   2) DPAPI：CryptProtectData / CryptUnprotectData
//      头文件 dpapi.h，库 crypt32.lib。
//      它只负责把一段字节加密，密文放哪里、怎么清理、怎么迁移都得应用自己管。
//
// 这里用第 1 条：rshell 不需要自己维护密文文件，用户也能自己管理/删除条目。
// sessions.ini 里只写 savepassword=1 这个标记，密码本身绝不落盘。
// 注意凭据按“当前 Windows 用户 + 当前机器”解密，换用户或换机器都读不出来。
//
// 默认后端是凭据管理器；GUI 解锁密码保险库之后会换成保险库（见文件末尾的
// setCredentialVault），此时同一口令只存在保险库里，凭据管理器里那份会被删掉。
#pragma once

#include <string>

namespace rshell {

class Vault;  // 前置声明：只有 GUI 会把保险库接成后端

// 会话密码在凭据管理器中的条目名：rshell/<sessionName>
std::string credentialTarget(const std::string& sessionName);

// 保存（或覆盖）一条通用凭据。userLabel 只用于在凭据管理器里显示（如 user@host:port）。
// 失败时 error（可空）返回系统错误文本；非 Windows 平台始终失败。
bool saveCredential(const std::string& target, const std::string& userLabel,
                    const std::string& password, std::string* error = nullptr);

// 读取密码。条目不存在时返回 false，并把原因写进 error（可空）。
bool loadCredential(const std::string& target, std::string& password,
                    std::string* error = nullptr);

// 删除条目。条目本来就不存在也算成功。
bool deleteCredential(const std::string& target, std::string* error = nullptr);

bool hasCredential(const std::string& target);

// ---------------------------------------------------------------- 后端切换
//
// 默认后端是 Windows 凭据管理器（上面这些函数原来的语义）。GUI 解锁密码
// 保险库后调用 setCredentialVault(&vault) 换成保险库，传 nullptr 断开：
//
//   - 保险库已解锁：save 写进保险库并立刻落盘，load 先查保险库；
//   - 保险库里没有的条目仍回退到凭据管理器，迁移期两种来源能共存；
//   - 解锁时把口令搬进保险库之后再删掉凭据管理器里那份，同一口令不留两处。
//
// CLI 没有主密码入口，不会接入保险库，行为与以前完全一致（只认凭据管理器）。
void setCredentialVault(Vault* vault);

// 当前后端是不是「已解锁的保险库」。界面用它决定提示文字说
// “保存到密码保险库”还是“保存到 Windows 凭据管理器”。
bool credentialVaultActive();

// 只删凭据管理器里的条目，不看保险库。
// 迁移用：口令已经搬进保险库之后清掉旧副本。
bool deleteManagedCredential(const std::string& target, std::string* error = nullptr);

// 把字符串内容清零（密码用完别留在堆里等下次分配捡到）
void wipeString(std::string& text);

}  // namespace rshell
