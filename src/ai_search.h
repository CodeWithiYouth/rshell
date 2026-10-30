// ai_search.h - 联网搜索（TinyFish Search API）
//
// 为什么用 API 而不是抓 DuckDuckGo 页面：搜索结果页有反爬（202 挑战 / 429），
// HTML 结构也会变；TinyFish 返回稳定 JSON，免费额度 12000 次/天，够个人用。
//
// 这一层只依赖 WinHTTP（和 AI 请求、更新检查同一套网络栈），
// 解析与格式化是纯函数，可以在单元测试里直接跑。
#pragma once

#include <string>
#include <vector>

namespace rshell::ai {

struct SearchHit {
    std::string title;
    std::string url;
    std::string snippet;
    std::string site;   // 域名
    std::string date;   // 发布时间，可能为空
};

// 可选的搜索后端。id 存在 ai.ini 的 search_provider 里，Key 按后端分别存进保险库。
enum class SearchProvider { None, TinyFish, Exa };

SearchProvider searchProviderFromId(const std::string& id);
const char* searchProviderId(SearchProvider provider);
const wchar_t* searchProviderName(SearchProvider provider);

// 拼请求路径与完整地址：query 走 percent-encoding。
// （TinyFish 没有 count 参数，默认一次 10 条，我们自己截断。）
std::string tinyfishPath(const std::string& query);
std::string tinyfishUrl(const std::string& query);

// 解析 TinyFish 的响应；失败时 error 带可读原因（含 401/402/429 的处理在调用方）
bool parseTinyfishResponse(const std::string& json, std::vector<SearchHit>& out,
                           std::string& error);

// Exa（POST https://api.exa.ai/search）：标题/URL/发布日期 + highlights 摘要
bool parseExaResponse(const std::string& json, std::vector<SearchHit>& out, std::string& error);
std::string exaRequestBody(const std::string& query, int maxResults);

// 给模型看的一段文本：编号 + 标题 + URL + 摘要，超过 maxBytes 就截断
std::string formatSearchHits(const std::vector<SearchHit>& hits, std::size_t maxBytes);

// 发一次搜索。同步阻塞，调用方放在工作线程里跑。
bool tinyfishSearch(const std::string& apiKey, const std::string& query, int maxResults,
                    std::vector<SearchHit>& out, std::string& error, int timeoutMs = 15000);
bool exaSearch(const std::string& apiKey, const std::string& query, int maxResults,
               std::vector<SearchHit>& out, std::string& error, int timeoutMs = 15000);

// 按所选后端分发；provider == None 直接失败（模型那边也就看不到工具）
bool runSearch(SearchProvider provider, const std::string& apiKey, const std::string& query,
               int maxResults, std::vector<SearchHit>& out, std::string& error);

}  // namespace rshell::ai
