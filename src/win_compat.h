// win_compat.h - 新版 Windows API 按需解析；Windows 7 保留系统 DPI 与基本功能。
#pragma once

#ifdef _WIN32
#include <windows.h>

namespace rshell::wincompat {

struct Api {
    using SetDpiContext = BOOL(WINAPI*)(HANDLE);
    using GetSystemDpi = UINT(WINAPI*)();
    using GetWindowDpi = UINT(WINAPI*)(HWND);
    using GetMetric = int(WINAPI*)(int, UINT);
    using SetMitigation = BOOL(WINAPI*)(PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
    SetDpiContext setDpiContext = nullptr;
    GetSystemDpi getSystemDpi = nullptr;
    GetWindowDpi getWindowDpi = nullptr;
    GetMetric getMetric = nullptr;
    SetMitigation setMitigation = nullptr;
};

inline const Api& api() {
    static const Api functions = [] {
        Api result;
        const HMODULE user = GetModuleHandleW(L"user32.dll");
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (user) {
            result.setDpiContext = reinterpret_cast<Api::SetDpiContext>(GetProcAddress(user, "SetProcessDpiAwarenessContext"));
            result.getSystemDpi = reinterpret_cast<Api::GetSystemDpi>(GetProcAddress(user, "GetDpiForSystem"));
            result.getWindowDpi = reinterpret_cast<Api::GetWindowDpi>(GetProcAddress(user, "GetDpiForWindow"));
            result.getMetric = reinterpret_cast<Api::GetMetric>(GetProcAddress(user, "GetSystemMetricsForDpi"));
        }
        if (kernel)
            result.setMitigation = reinterpret_cast<Api::SetMitigation>(GetProcAddress(kernel, "SetProcessMitigationPolicy"));
        return result;
    }();
    return functions;
}

inline void initializeDpi(const Api& functions = api()) {
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 的 ABI 值为 -4。
    if (functions.setDpiContext && functions.setDpiContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))))
        return;
    SetProcessDPIAware();  // Vista+；老系统采用系统 DPI，不自动随监视器切换。
}

inline UINT systemDpi(const Api& functions = api()) {
    if (functions.getSystemDpi) {
        const UINT value = functions.getSystemDpi();
        if (value) return value;
    }
    HDC dc = GetDC(nullptr);
    const int value = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 0;
    if (dc) ReleaseDC(nullptr, dc);
    return value > 0 ? static_cast<UINT>(value) : 96;
}

inline UINT windowDpi(HWND window, const Api& functions = api()) {
    if (functions.getWindowDpi) {
        const UINT value = functions.getWindowDpi(window);
        if (value) return value;
    }
    return systemDpi(functions);
}

inline int systemMetric(int index, UINT dpi, const Api& functions = api()) {
    if (functions.getMetric) return functions.getMetric(index, dpi);
    const UINT baseDpi = systemDpi(functions);
    return MulDiv(GetSystemMetrics(index), static_cast<int>(dpi ? dpi : baseDpi), static_cast<int>(baseDpi));
}

}  // namespace rshell::wincompat
#endif
