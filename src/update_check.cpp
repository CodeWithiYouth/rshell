// update_check.cpp
#include "update_check.h"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <cctype>
#include <cstdlib>
#include <vector>

#include "ai_json.h"
#include "version_compare.h"

#ifndef RSHELL_VERSION
#define RSHELL_VERSION "0.0.0"
#endif

namespace rshell {

namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kApiPath[] = L"/repos/CodeWithiYouth/rshell/releases/latest";

struct HandleCloser {
    HINTERNET handle = nullptr;
    ~HandleCloser() {
        if (handle) WinHttpCloseHandle(handle);
    }
};

std::string readAll(HINTERNET request, std::size_t limit) {
    std::string out;
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &read) || read == 0) break;
        if (out.size() < limit) out.append(buffer, read);
        if (out.size() >= limit) break;
    }
    return out;
}

}  // namespace

const char* appVersion() { return RSHELL_VERSION; }

ReleaseInfo fetchLatestRelease(const std::string& currentVersion, int timeoutMs) {
    ReleaseInfo info;
    HandleCloser session;
    session.handle = WinHttpOpen(L"rshell-gui/" L"" RSHELL_VERSION,
                                 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle) {
        // Win7 没有 AUTOMATIC_PROXY：退回系统默认代理设置
        session.handle = WinHttpOpen(L"rshell-gui/" L"" RSHELL_VERSION,
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.handle) {
        info.error = "无法初始化 HTTP 会话";
        return info;
    }
    WinHttpSetTimeouts(session.handle, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HandleCloser connect;
    connect.handle = WinHttpConnect(session.handle, kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect.handle) {
        info.error = "无法连接 api.github.com";
        return info;
    }

    HandleCloser request;
    request.handle = WinHttpOpenRequest(connect.handle, L"GET", kApiPath, nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!request.handle) {
        info.error = "无法创建 HTTP 请求";
        return info;
    }
    // GitHub API 要求带 User-Agent；Accept 固定版本，避免以后默认格式变化
    const wchar_t* headers = L"User-Agent: rshell-gui\r\nAccept: application/vnd.github+json\r\n";
    if (!WinHttpSendRequest(request.handle, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        info.error = "请求 GitHub 失败（网络或代理）";
        return info;
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        info.error = "无法读取 HTTP 状态码";
        return info;
    }
    if (status == 404) {
        info.error = "仓库还没有发布任何版本";
        return info;
    }
    if (status != 200) {
        info.error = "GitHub 返回 HTTP " + std::to_string(status);
        return info;
    }

    const std::string body = readAll(request.handle, 256 * 1024);
    ai::Json json;
    std::string parseError;
    if (!ai::Json::parse(body, json, &parseError) || !json.isObject()) {
        info.error = "返回内容不是合法 JSON";
        return info;
    }
    const auto field = [&json](const char* key) {
        const ai::Json* value = json.find(key);
        return value && value->isString() ? value->asString() : std::string();
    };
    info.tag = field("tag_name");
    info.url = field("html_url");
    info.name = field("name");
    info.body = field("body");
    if (info.body.size() > 4096) info.body = info.body.substr(0, 4096) + "\n…";
    if (info.tag.empty()) {
        info.error = "返回里没有版本号";
        return info;
    }
    info.ok = true;
    info.newer = versionNewer(info.tag, currentVersion);
    return info;
}

void openInBrowser(const std::string& url) {
    if (url.empty()) return;
    const std::wstring wide(url.begin(), url.end());
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace rshell
