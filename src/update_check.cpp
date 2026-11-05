// update_check.cpp
#include "update_check.h"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
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
constexpr std::size_t kMaxAsset = 64u * 1024u * 1024u;

bool validDigest(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool validTag(const std::string& tag) {
    return !tag.empty() && tag.size() <= 80 &&
        std::all_of(tag.begin(), tag.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-' || c == '_';
        });
}

std::string digestFromNotes(const std::string& body) {
    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t end = body.find('\n', pos);
        const std::string line = body.substr(pos, end == std::string::npos ? end : end - pos);
        if (line.size() >= 65 && validDigest(line.substr(0, 64))) {
            std::size_t name = line.find_first_not_of(" \t*", 64);
            if (name != std::string::npos && line.substr(name).find("rshell-gui.exe") == 0) {
                std::string suffix = line.substr(name + 14);
                if (suffix.find_first_not_of(" \t\r") == std::string::npos)
                    return lowercase(line.substr(0, 64));
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return {};
}

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

ReleaseInfo parseReleaseInfo(const std::string& body, const std::string& currentVersion) {
    ReleaseInfo info;
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
    const std::string fullNotes = field("body");
    info.body = fullNotes.size() > 4096 ? fullNotes.substr(0, 4096) + "\n…" : fullNotes;
    if (!validTag(info.tag)) { info.error = "返回的版本号无效"; return info; }
    const std::string officialPage = "https://github.com/CodeWithiYouth/rshell/releases/tag/" + info.tag;
    if (info.url != officialPage) info.url = officialPage;
    const std::string expected = "https://github.com/CodeWithiYouth/rshell/releases/download/" +
                                 info.tag + "/rshell-gui.exe";
    const ai::Json* assets = json.find("assets");
    if (assets && assets->isArray()) {
        for (const auto& asset : assets->items()) {
            const ai::Json* name = asset.find("name");
            const ai::Json* url = asset.find("browser_download_url");
            if (!name || name->asString() != "rshell-gui.exe" || !url || url->asString() != expected)
                continue;
            const ai::Json* size = asset.find("size");
            const double number = size ? size->asNumber() : 0;
            if (number < 1024 || number > static_cast<double>(kMaxAsset) ||
                number != static_cast<unsigned long long>(number)) continue;
            info.downloadUrl = expected;
            info.assetSize = static_cast<unsigned long long>(number);
            const ai::Json* digest = asset.find("digest");
            if (digest && digest->isString() && digest->asString().rfind("sha256:", 0) == 0)
                info.sha256 = lowercase(digest->asString().substr(7));
            if (!validDigest(info.sha256)) info.sha256 = digestFromNotes(fullNotes);
            break;
        }
    }
    info.ok = true;
    info.newer = versionNewer(info.tag, currentVersion);
    return info;
}

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
    return parseReleaseInfo(body, currentVersion);
}

namespace {
bool hashMatches(const std::vector<unsigned char>& data, const std::string& expected) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, resultSize = 0;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                          sizeof(objectSize), &resultSize, 0) >= 0) {
        std::vector<unsigned char> object(objectSize), digest(32);
        if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) >= 0 &&
            BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0) {
            static const char hex[] = "0123456789abcdef";
            std::string actual;
            for (unsigned char byte : digest) { actual += hex[byte >> 4]; actual += hex[byte & 15]; }
            ok = actual == lowercase(expected);
        }
        if (hash) BCryptDestroyHash(hash);
    }
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool isWindowsX64Exe(const std::vector<unsigned char>& data) {
    if (data.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, data.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return false;
    const std::size_t offset = static_cast<std::size_t>(dos.e_lfanew);
    if (offset > data.size() || data.size() - offset < sizeof(IMAGE_NT_HEADERS64)) return false;
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, data.data() + offset, sizeof(nt));
    return nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
           (nt.FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) &&
           !(nt.FileHeader.Characteristics & IMAGE_FILE_DLL) &&
           nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
           nt.OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI;
}

std::wstring parentPath(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring quoted(const std::wstring& value) { return L"\"" + value + L"\""; }
}  // namespace

UpdateDownload downloadRelease(const ReleaseInfo& release, const std::wstring& executablePath) {
    UpdateDownload result;
#if defined(_WIN32_WINNT) && _WIN32_WINNT <= 0x0601
    result.error = "GitHub 当前发布版面向 Windows 10/11，请使用适合 Windows 7 的构建";
    return result;
#endif
    const std::string expected = "https://github.com/CodeWithiYouth/rshell/releases/download/" +
                                 release.tag + "/rshell-gui.exe";
    if (!release.ok || !release.newer || !validTag(release.tag) || release.downloadUrl != expected ||
        !validDigest(release.sha256) || release.assetSize < 1024 || release.assetSize > kMaxAsset) {
        result.error = "发布版本缺少可验证的桌面版附件或 SHA-256，请使用 GitHub 页面下载";
        return result;
    }
    HandleCloser session;
    session.handle = WinHttpOpen(L"rshell-gui/" L"" RSHELL_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle)
        session.handle = WinHttpOpen(L"rshell-gui/" L"" RSHELL_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle) { result.error = "无法初始化下载"; return result; }
    WinHttpSetTimeouts(session.handle, 15000, 15000, 30000, 30000);
    HandleCloser connection;
    connection.handle = WinHttpConnect(session.handle, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection.handle) { result.error = "无法连接 GitHub"; return result; }
    const std::string path = "/CodeWithiYouth/rshell/releases/download/" + release.tag + "/rshell-gui.exe";
    const std::wstring widePath(path.begin(), path.end());
    HandleCloser request;
    request.handle = WinHttpOpenRequest(connection.handle, L"GET", widePath.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request.handle) { result.error = "无法创建下载请求"; return result; }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)) ||
        !WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        result.error = "下载失败，请检查网络或代理"; return result;
    }
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        result.error = "下载服务器返回 HTTP " + std::to_string(status); return result;
    }
    std::vector<unsigned char> bytes;
    bytes.reserve(static_cast<std::size_t>(release.assetSize));
    unsigned char buffer[32768];
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.handle, buffer, sizeof(buffer), &got)) {
            result.error = "下载中断"; return result;
        }
        if (!got) break;
        if (bytes.size() + got > release.assetSize) { result.error = "下载文件超出预期大小"; return result; }
        bytes.insert(bytes.end(), buffer, buffer + got);
    }
    if (bytes.size() != release.assetSize || !hashMatches(bytes, release.sha256) || !isWindowsX64Exe(bytes)) {
        result.error = "更新文件大小、SHA-256 或程序格式校验失败"; return result;
    }
    const std::wstring directory = parentPath(executablePath);
    wchar_t temporary[MAX_PATH]{};
    if (directory.empty() || !GetTempFileNameW(directory.c_str(), L"rsu", 0, temporary)) {
        result.error = "无法在程序目录创建临时文件，请检查写入权限"; return result;
    }
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { DeleteFileW(temporary); result.error = "无法写入更新文件"; return result; }
    DWORD written = 0;
    const bool writtenOk = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                           written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!writtenOk) { DeleteFileW(temporary); result.error = "写入更新文件失败"; return result; }
    result.stagedPath = temporary;
    result.ok = true;
    return result;
}

bool launchUpdateHelper(const std::wstring& executablePath, const std::wstring& stagedPath,
                        std::string& error) {
    wchar_t directory[MAX_PATH]{}, helper[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"rsh", 0, helper) ||
        !CopyFileW(executablePath.c_str(), helper, FALSE)) {
        error = "无法创建更新助手";
        if (helper[0]) DeleteFileW(helper);
        return false;
    }
    std::wstring command = quoted(helper) + L" --rshell-update-apply " +
                           std::to_wstring(GetCurrentProcessId()) + L" " +
                           quoted(executablePath) + L" " + quoted(stagedPath);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        DeleteFileW(helper); error = "无法启动更新助手"; return false;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return true;
}

int runUpdateHelper(int argc, wchar_t** argv) {
    if (argc != 5) return 2;
    wchar_t* end = nullptr;
    const unsigned long pid = wcstoul(argv[2], &end, 10);
    if (!pid || !end || *end) return 2;
    const std::wstring target = argv[3], staged = argv[4];
    if (parentPath(target).empty() || _wcsicmp(parentPath(target).c_str(), parentPath(staged).c_str()) != 0)
        return 2;
    HANDLE oldProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (oldProcess) {
        const DWORD wait = WaitForSingleObject(oldProcess, 120000);
        CloseHandle(oldProcess);
        if (wait != WAIT_OBJECT_0) { MessageBoxW(nullptr, L"原程序未退出，更新已取消。", L"rshell 更新", MB_OK | MB_ICONWARNING); return 3; }
    } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
        MessageBoxW(nullptr, L"无法确认原程序已退出，更新已取消。", L"rshell 更新", MB_OK | MB_ICONERROR);
        return 3;
    }
    wchar_t backup[MAX_PATH]{};
    if (!GetTempFileNameW(parentPath(target).c_str(), L"rsb", 0, backup) || !DeleteFileW(backup)) {
        MessageBoxW(nullptr, L"无法创建备份，原程序没有改变。", L"rshell 更新", MB_OK | MB_ICONERROR);
        return 4;
    }
    if (!ReplaceFileW(target.c_str(), staged.c_str(), backup, 0, nullptr, nullptr)) {
        const bool backupExists = GetFileAttributesW(backup) != INVALID_FILE_ATTRIBUTES;
        if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES && backupExists)
            MoveFileExW(backup, target.c_str(), MOVEFILE_WRITE_THROUGH);
        const bool targetExists = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
        const std::wstring message = targetExists
            ? L"无法替换程序，原程序仍在。请检查文件权限或其他运行中的实例。"
            : L"无法替换或恢复程序，请检查备份文件：\n" + std::wstring(backup);
        MessageBoxW(nullptr, message.c_str(), L"rshell 更新", MB_OK | MB_ICONERROR);
        return 4;
    }
    std::wstring command = quoted(target);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        if (!ReplaceFileW(target.c_str(), backup, nullptr, 0, nullptr, nullptr)) {
            const std::wstring message = L"新版本无法启动，自动恢复也失败。旧版本保存在：\n" + std::wstring(backup);
            MessageBoxW(nullptr, message.c_str(), L"rshell 更新", MB_OK | MB_ICONERROR);
            return 5;
        }
        MessageBoxW(nullptr, L"新版本无法启动，已恢复原程序。", L"rshell 更新", MB_OK | MB_ICONERROR);
        return 5;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    DeleteFileW(backup);
    return 0;
}

void openInBrowser(const std::string& url) {
    if (url.empty()) return;
    const std::wstring wide(url.begin(), url.end());
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace rshell
