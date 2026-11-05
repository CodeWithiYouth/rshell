#include <windows.h>
#include <iterator>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!n || n >= std::size(path)) return 1;
    const std::wstring marker = std::wstring(path, n) + L".started";
    HANDLE file = CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 2;
    CloseHandle(file);
    return 0;
}
