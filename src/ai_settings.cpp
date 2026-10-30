// ai_settings.cpp
#include "ai_settings.h"

#include <cstdlib>
#include <fstream>

#include "util.h"
#include "vault.h"

namespace rshell::ai {

namespace {

constexpr const char* kDefaultPathSuffix = "/.rshell/ai.ini";

std::string envOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

int parseInt(const std::string& text, int fallback) {
    try {
        const int value = std::stoi(trim(text));
        return value > 0 ? value : fallback;
    } catch (...) {
        return fallback;
    }
}

}  // namespace

const std::vector<AiProviderPreset>& aiProviderPresets() {
    // 地址与模型列表端点均按各家官方文档（2026-10 核对，均可匿名 GET /models）
    static const std::vector<AiProviderPreset> presets = {
        {"opencode-go", "OpenCode Go", "https://opencode.ai/inference/go/openai/v1",
         "https://opencode.ai/inference/go/v1/models", true},
        {"commandcode", "Command Code", "https://api.commandcode.ai/provider/v1",
         "https://api.commandcode.ai/provider/v1/models", false},
        {"cline", "Cline", "https://api.cline.bot/api/v1",
         "https://api.cline.bot/api/v1/models", false},
    };
    return presets;
}

const AiProviderPreset* findAiProvider(const std::string& id) {
    if (id.empty()) return nullptr;
    for (const AiProviderPreset& preset : aiProviderPresets())
        if (preset.id == id) return &preset;
    return nullptr;
}

std::string aiConfigPath() {    const std::string override = envOrEmpty("RSHELL_AI_CONFIG");
    if (!override.empty()) return override;
    return homeDirectory() + kDefaultPathSuffix;
}

AiSettings loadAiSettings(const std::string& path) {
    AiSettings settings;
    const std::string file = path.empty() ? aiConfigPath() : path;

    std::ifstream in(utf8ToPath(file), std::ios::binary);
    if (!in) return settings;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string text = trim(line);
        if (text.empty() || text.front() == '#' || text.front() == ';') continue;

        const std::size_t eq = text.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = toLower(trim(text.substr(0, eq)));
        const std::string value = trim(text.substr(eq + 1));

        if (key == "provider") {
            settings.provider = value;
        } else if (key == "base_url" || key == "baseurl") {
            if (!value.empty()) settings.baseUrl = value;
        } else if (key == "model") {
            if (!value.empty()) settings.model = value;
        } else if (key == "redact_secrets" || key == "redact") {
            settings.redactSecrets = !(value == "0" || iequals(value, "false") ||
                                       iequals(value, "no"));
        } else if (key == "auto_approve" || key == "autoapprove") {
            settings.autoApprove = !(value == "0" || iequals(value, "false") ||
                                     iequals(value, "no"));
        } else if (key == "max_steps" || key == "maxsteps") {
            settings.maxSteps = parseInt(value, settings.maxSteps);
        } else if (key == "search_provider" || key == "searchprovider") {
            if (value == "tinyfish" || value == "exa") settings.searchProvider = value;
        } else if (key == "save_sessions" || key == "savesessions") {
            // 默认开：只有明确写成 0/false/no 才关
            settings.saveSessions = !(value == "0" || iequals(value, "false") ||
                                      iequals(value, "no"));
        }
    }
    return settings;
}

bool saveAiSettings(const AiSettings& settings, std::string* error, const std::string& path) {
    const std::string file = path.empty() ? aiConfigPath() : path;
    const fs::path target = utf8ToPath(file);

    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        if (error) *error = "无法创建配置目录：" + ec.message();
        return false;
    }

    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "无法写入配置文件：" + file;
        return false;
    }

    // 不写 API Key：它只存在保险库里
    out << "# rshell AI 配置（API Key 保存在主密码保险库，不在此文件）\n";
    out << "provider=" << settings.provider << "\n";
    out << "base_url=" << settings.baseUrl << "\n";
    out << "model=" << settings.model << "\n";
    out << "redact_secrets=" << (settings.redactSecrets ? "1" : "0") << "\n";
    out << "auto_approve=" << (settings.autoApprove ? "1" : "0") << "\n";
    out << "save_sessions=" << (settings.saveSessions ? "1" : "0") << "\n";
    out << "search_provider=" << settings.searchProvider << "\n";
    out << "max_steps=" << settings.maxSteps << "\n";
    if (!out.good()) {
        if (error) *error = "写入配置文件失败：" + file;
        return false;
    }
    return true;
}

std::string aiModelsUrl(const AiSettings& settings) {
    if (const AiProviderPreset* preset = findAiProvider(settings.provider))
        if (!preset->modelsUrl.empty()) return preset->modelsUrl;
    std::string base = settings.baseUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/models";
}

bool loadApiKey(Vault& vault, std::string& out) {
    if (!vault.unlocked()) return false;
    if (!vault.has(kApiKeyEntry)) return false;
    return vault.get(kApiKeyEntry, out) && !out.empty();
}

bool saveApiKey(Vault& vault, const std::string& key, std::string* error) {
    if (!vault.unlocked()) {
        if (error) *error = "保险库未解锁，无法保存 API Key";
        return false;
    }
    if (key.empty()) {
        if (error) *error = "API Key 为空";
        return false;
    }
    vault.set(kApiKeyEntry, key);
    return true;
}

bool clearApiKey(Vault& vault) {
    if (!vault.unlocked()) return false;
    if (!vault.has(kApiKeyEntry)) return false;
    return vault.remove(kApiKeyEntry);
}

// 搜索 Key 与模型 Key 同源：都只放在保险库里，不写 ini；按后端分开存，
// 这样在 TinyFish / Exa 之间来回切不用重新填。
std::string searchKeyEntry(const std::string& provider) {
    return std::string("ai.searchApiKey.") + (provider.empty() ? "none" : provider);
}

bool loadSearchKey(Vault& vault, const std::string& provider, std::string& out) {
    if (provider.empty() || !vault.unlocked()) return false;
    const std::string entry = searchKeyEntry(provider);
    if (!vault.has(entry)) return false;
    return vault.get(entry, out) && !out.empty();
}

bool saveSearchKey(Vault& vault, const std::string& provider, const std::string& key,
                   std::string* error) {
    if (provider.empty()) {
        if (error) *error = "没有选择搜索服务商";
        return false;
    }
    if (!vault.unlocked()) {
        if (error) *error = "保险库未解锁，无法保存搜索 Key";
        return false;
    }
    if (key.empty()) {
        if (error) *error = "搜索 Key 为空";
        return false;
    }
    vault.set(searchKeyEntry(provider), key);
    return true;
}

bool clearSearchKey(Vault& vault, const std::string& provider) {
    if (provider.empty() || !vault.unlocked()) return false;
    const std::string entry = searchKeyEntry(provider);
    if (!vault.has(entry)) return false;
    return vault.remove(entry);
}

}  // namespace ai
