// update_check.h - 检查 GitHub 上有没有更新的 Release
//
// 只做两件事：拉一次 /releases/latest，比较版本号。界面怎么提示由宿主决定。
// 走系统 winhttp.dll，不额外引依赖（和 AI 面板同一套网络栈）。
#pragma once

#include <string>

namespace rshell {

struct ReleaseInfo {
    bool ok = false;          // 请求成功（HTTP 200 且解析出 tag）
    std::string tag;          // 例如 v1.2.6
    std::string url;          // html_url
    std::string name;         // release 标题
    std::string body;         // 更新说明（可能为空，截断到 4 KB）
    std::string error;        // 失败原因（网络错误、非 200 等）
    bool newer = false;       // tag 比当前版本新
    std::string downloadUrl;  // 官方桌面版附件地址
    std::string sha256;       // 官方 Release 提供的 SHA-256
    unsigned long long assetSize = 0;
};

struct UpdateDownload {
    bool ok = false;
    std::wstring stagedPath;
    std::string error;
};

// 当前编译进去的版本号（CMake 的 PROJECT_VERSION）
const char* appVersion();

// 版本号比较在 version_compare.h（纯头文件，单元测试直接包含）

// 同步请求，调用方放在工作线程里跑；timeoutMs 建议 5~10 秒
ReleaseInfo fetchLatestRelease(const std::string& currentVersion, int timeoutMs = 8000);
ReleaseInfo parseReleaseInfo(const std::string& json, const std::string& currentVersion);
UpdateDownload downloadRelease(const ReleaseInfo& release, const std::wstring& executablePath);
bool launchUpdateHelper(const std::wstring& executablePath, const std::wstring& stagedPath,
                        std::string& error);
int runUpdateHelper(int argc, wchar_t** argv);

// 用默认浏览器打开链接
void openInBrowser(const std::string& url);

}  // namespace rshell
