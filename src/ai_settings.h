// ai_settings.h - AI 功能的配置与密钥
//
// 分工（按产品决策）：
//   - baseURL / 模型名等非敏感项：~/.rshell/ai.ini（RSHELL_AI_CONFIG 可覆盖）
//   - API Key：主密码保险库里的 "ai.apiKey" 条目，绝不明文落盘
// 配置读取失败一律回退默认值，不抛异常——AI 面板打不开不应该影响 SSH 主流程。
#pragma once

#include <string>

namespace rshell {

class Vault;

namespace ai {

struct AiSettings {
    std::string baseUrl = "https://api.openai.com/v1";
    std::string model = "gpt-4o-mini";
    bool redactSecrets = true;   // 回灌模型前隐藏疑似凭据
    int maxSteps = 16;           // 单轮工具调用上限
};

// AI 配置文件路径：~/.rshell/ai.ini（可用 RSHELL_AI_CONFIG 覆盖）
std::string aiConfigPath();

// 读取配置；文件不存在或字段缺失时保留默认值
AiSettings loadAiSettings(const std::string& path = std::string());

// 写回配置；失败返回 false 并写入 error
bool saveAiSettings(const AiSettings& settings, std::string* error = nullptr,
                    const std::string& path = std::string());

// 保险库里的条目名
constexpr const char* kApiKeyEntry = "ai.apiKey";

// 保险库未解锁或没有该条目时返回 false
bool loadApiKey(Vault& vault, std::string& out);
// 保险库必须已解锁；写入后调用方负责 save()
bool saveApiKey(Vault& vault, const std::string& key, std::string* error = nullptr);
bool clearApiKey(Vault& vault);

}  // namespace ai
}  // namespace rshell
