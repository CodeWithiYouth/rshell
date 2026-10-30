// ai_search.cpp
#include "ai_search.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>

#include "ai_json.h"

namespace rshell::ai {

namespace {

constexpr wchar_t kUserAgent[] = L"rshell-gui/1.2";

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

std::string trimCopy(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r' ||
                             text.back() == '\t'))
        text.pop_back();
    std::size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\n' || text[begin] == '\r' ||
            text[begin] == '\t'))
        ++begin;
    return text.substr(begin);
}

// https://a.b/c?d → a.b
std::string hostOf(const std::string& url) {
    std::size_t begin = url.find("://");
    begin = begin == std::string::npos ? 0 : begin + 3;
    const std::size_t end = url.find('/', begin);
    std::string host = url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    if (host.rfind("www.", 0) == 0) host = host.substr(4);
    return host;
}

// 一次 HTTP 请求：GET 时 body 为空，POST 时带 JSON
bool httpRequest(const wchar_t* host, const wchar_t* method, const std::string& path,
                 const std::wstring& extraHeaders, const std::string& body, int timeoutMs,
                 std::string& response, DWORD& status, std::string& error) {
    HandleCloser session;
    session.handle = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle) {
        // Win7 没有 AUTOMATIC_PROXY：退回系统默认代理
        session.handle = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.handle) {
        error = "无法初始化 HTTP 会话";
        return false;
    }
    WinHttpSetTimeouts(session.handle, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HandleCloser connect;
    connect.handle = WinHttpConnect(session.handle, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect.handle) {
        error = "无法连接搜索服务";
        return false;
    }

    const std::wstring widePath(path.begin(), path.end());
    HandleCloser request;
    request.handle = WinHttpOpenRequest(connect.handle, method, widePath.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!request.handle) {
        error = "无法创建 HTTP 请求";
        return false;
    }

    std::wstring headers = L"Accept: application/json\r\n";
    headers += extraHeaders;
    void* payload = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    const DWORD payloadSize = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.handle, headers.c_str(), static_cast<DWORD>(-1L), payload,
                            payloadSize, payloadSize, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        error = "搜索请求失败（网络或代理）";
        return false;
    }

    status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        error = "无法读取 HTTP 状态码";
        return false;
    }
    response = readAll(request.handle, 512 * 1024);
    return true;
}

void collectTinyfish(const Json& root, std::vector<SearchHit>& out) {
    const Json* results = root.find("results");
    if (!results || !results->isArray()) return;
    for (const Json& item : results->items()) {
        if (!item.isObject()) continue;
        SearchHit hit;
        if (const Json* value = item.find("title"); value && value->isString())
            hit.title = trimCopy(value->asString());
        if (const Json* value = item.find("url"); value && value->isString())
            hit.url = trimCopy(value->asString());
        if (const Json* value = item.find("snippet"); value && value->isString())
            hit.snippet = trimCopy(value->asString());
        if (const Json* value = item.find("site_name"); value && value->isString())
            hit.site = trimCopy(value->asString());
        if (const Json* value = item.find("date"); value && value->isString())
            hit.date = trimCopy(value->asString());
        if (hit.url.empty()) continue;  // 没有链接的结果对模型没用
        out.push_back(std::move(hit));
    }
}

}  // namespace

// ---------------------------------------------------------------- 后端选择

SearchProvider searchProviderFromId(const std::string& id) {
    if (id == "tinyfish") return SearchProvider::TinyFish;
    if (id == "exa") return SearchProvider::Exa;
    return SearchProvider::None;
}

const char* searchProviderId(SearchProvider provider) {
    switch (provider) {
        case SearchProvider::TinyFish: return "tinyfish";
        case SearchProvider::Exa: return "exa";
        default: return "";
    }
}

const wchar_t* searchProviderName(SearchProvider provider) {
    switch (provider) {
        case SearchProvider::TinyFish: return L"TinyFish";
        case SearchProvider::Exa: return L"Exa";
        default: return L"关闭";
    }
}

// ---------------------------------------------------------------- TinyFish

std::string tinyfishPath(const std::string& query) {
    // RFC 3986：只保留 unreserved 字符，其余按 UTF-8 字节百分号编码
    std::string encoded;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char ch : query) {
        const bool unreserved = std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(ch));
        } else if (ch == ' ') {
            encoded.push_back('+');
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[ch >> 4]);
            encoded.push_back(hex[ch & 0xF]);
        }
    }
    return "/?query=" + encoded;
}

std::string tinyfishUrl(const std::string& query) {
    return std::string("https://api.search.tinyfish.ai") + tinyfishPath(query);
}

bool parseTinyfishResponse(const std::string& json, std::vector<SearchHit>& out,
                           std::string& error) {
    Json root;
    std::string parseError;
    if (!Json::parse(json, root, &parseError) || !root.isObject()) {
        error = parseError.empty() ? "返回内容不是合法 JSON" : parseError;
        return false;
    }
    if (!root.find("results")) {
        error = "返回里没有 results 数组";
        return false;
    }
    std::vector<SearchHit> hits;
    collectTinyfish(root, hits);
    out = std::move(hits);
    return true;
}

bool tinyfishSearch(const std::string& apiKey, const std::string& query, int maxResults,
                    std::vector<SearchHit>& out, std::string& error, int timeoutMs) {
    out.clear();
    if (apiKey.empty()) {
        error = "还没有配置搜索 API Key";
        return false;
    }

    const std::wstring key(apiKey.begin(), apiKey.end());
    std::string body;
    DWORD status = 0;
    if (!httpRequest(L"api.search.tinyfish.ai", L"GET", tinyfishPath(query),
                     L"X-API-Key: " + key + L"\r\n", {}, timeoutMs, body, status, error))
        return false;

    if (status == 401) {
        error = "TinyFish Key 无效（HTTP 401），到设置里重新填一次";
        return false;
    }
    if (status == 402) {
        error = "TinyFish 今日免费额度已用完（HTTP 402）";
        return false;
    }
    if (status == 429) {
        error = "TinyFish 限流（HTTP 429），稍后再试";
        return false;
    }
    if (status != 200) {
        error = "TinyFish 返回 HTTP " + std::to_string(status);
        return false;
    }

    std::vector<SearchHit> hits;
    if (!parseTinyfishResponse(body, hits, error)) return false;
    if (maxResults > 0 && hits.size() > static_cast<std::size_t>(maxResults))
        hits.resize(static_cast<std::size_t>(maxResults));
    out = std::move(hits);
    return true;
}

// ---------------------------------------------------------------- Exa

std::string exaRequestBody(const std::string& query, int maxResults) {
    Json root = Json::object();
    root["query"] = Json::string(query);
    root["numResults"] = Json::number(maxResults > 0 ? maxResults : 8);
    root["type"] = Json::string("auto");
    // highlights 只回几段相关摘要，比整页正文省 token
    Json contents = Json::object();
    contents["highlights"] = Json::boolean(true);
    root["contents"] = std::move(contents);
    return root.dump();
}

bool parseExaResponse(const std::string& json, std::vector<SearchHit>& out, std::string& error) {
    Json root;
    std::string parseError;
    if (!Json::parse(json, root, &parseError) || !root.isObject()) {
        error = parseError.empty() ? "返回内容不是合法 JSON" : parseError;
        return false;
    }
    const Json* results = root.find("results");
    if (!results || !results->isArray()) {
        error = "返回里没有 results 数组";
        return false;
    }
    std::vector<SearchHit> hits;
    for (const Json& item : results->items()) {
        if (!item.isObject()) continue;
        SearchHit hit;
        if (const Json* value = item.find("title"); value && value->isString())
            hit.title = trimCopy(value->asString());
        if (const Json* value = item.find("url"); value && value->isString())
            hit.url = trimCopy(value->asString());
        if (const Json* value = item.find("publishedDate"); value && value->isString())
            hit.date = trimCopy(value->asString());
        // 摘要优先用 highlights（几段相关句），没有就退回 text 开头
        if (const Json* value = item.find("highlights"); value && value->isArray()) {
            for (const Json& line : value->items()) {
                if (!line.isString() || line.asString().empty()) continue;
                if (!hit.snippet.empty()) hit.snippet += " ";
                hit.snippet += trimCopy(line.asString());
            }
        }
        if (hit.snippet.empty()) {
            if (const Json* value = item.find("text"); value && value->isString())
                hit.snippet = trimCopy(value->asString());
        }
        if (hit.url.empty()) continue;
        hit.site = hostOf(hit.url);
        hits.push_back(std::move(hit));
    }
    out = std::move(hits);
    return true;
}

bool exaSearch(const std::string& apiKey, const std::string& query, int maxResults,
               std::vector<SearchHit>& out, std::string& error, int timeoutMs) {
    out.clear();
    if (apiKey.empty()) {
        error = "还没有配置搜索 API Key";
        return false;
    }

    const std::wstring key(apiKey.begin(), apiKey.end());
    std::string body;
    DWORD status = 0;
    if (!httpRequest(L"api.exa.ai", L"POST", "/search",
                     L"Content-Type: application/json\r\nx-api-key: " + key + L"\r\n",
                     exaRequestBody(query, maxResults), timeoutMs, body, status, error))
        return false;

    if (status == 401 || status == 403) {
        error = "Exa Key 无效或没有权限（HTTP " + std::to_string(status) + "）";
        return false;
    }
    if (status == 402) {
        error = "Exa 额度不足（HTTP 402），去控制台看看余额";
        return false;
    }
    if (status == 429) {
        error = "Exa 限流（HTTP 429），稍后再试";
        return false;
    }
    if (status != 200) {
        error = "Exa 返回 HTTP " + std::to_string(status);
        return false;
    }

    std::vector<SearchHit> hits;
    if (!parseExaResponse(body, hits, error)) return false;
    if (maxResults > 0 && hits.size() > static_cast<std::size_t>(maxResults))
        hits.resize(static_cast<std::size_t>(maxResults));
    out = std::move(hits);
    return true;
}

// ---------------------------------------------------------------- 分发

bool runSearch(SearchProvider provider, const std::string& apiKey, const std::string& query,
               int maxResults, std::vector<SearchHit>& out, std::string& error) {
    switch (provider) {
        case SearchProvider::TinyFish:
            return tinyfishSearch(apiKey, query, maxResults, out, error);
        case SearchProvider::Exa:
            return exaSearch(apiKey, query, maxResults, out, error);
        default:
            error = "没有启用联网搜索";
            return false;
    }
}

// ---------------------------------------------------------------- 格式化

std::string formatSearchHits(const std::vector<SearchHit>& hits, std::size_t maxBytes) {
    if (hits.empty()) return "（没有结果）";
    std::string text;
    for (std::size_t i = 0; i < hits.size(); ++i) {
        const SearchHit& hit = hits[i];
        if (!text.empty()) text += "\n\n";
        text += std::to_string(i + 1) + ". " + hit.title + "\n";
        text += hit.url + "\n";
        std::string snippet = hit.snippet;
        // 单条摘要太长会挤掉其它结果
        if (snippet.size() > 360) {
            std::size_t cut = 360;
            while (cut > 0 && (static_cast<unsigned char>(snippet[cut]) & 0xC0) == 0x80) --cut;
            snippet = snippet.substr(0, cut) + "…";
        }
        if (!snippet.empty()) text += snippet + "\n";
        std::string meta;
        if (!hit.site.empty()) meta = hit.site;
        if (!hit.date.empty()) meta += (meta.empty() ? "" : " · ") + hit.date;
        if (!meta.empty()) text += "(" + meta + ")\n";
        if (text.size() >= maxBytes) break;
    }
    if (text.size() > maxBytes) {
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text = text.substr(0, cut) + "…";
    }
    return text;
}

}  // namespace rshell::ai
