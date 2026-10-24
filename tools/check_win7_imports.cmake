# 这是常见版本门槛的回归检查，不能代替 Windows 7 实机测试。
if(NOT EXISTS "${EXECUTABLE}" OR NOT EXISTS "${DUMPBIN}")
    message(FATAL_ERROR "Executable and dumpbin are required for the Win7 import check")
endif()
execute_process(COMMAND "${DUMPBIN}" /imports "${EXECUTABLE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE imports ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot inspect imports: ${error}")
endif()
set(unsupported
    CreateFile2 GetDpiForSystem GetDpiForWindow GetSystemMetricsForDpi
    SetProcessDpiAwarenessContext SetThreadDpiAwarenessContext SetProcessMitigationPolicy
    GetSystemTimePreciseAsFileTime WaitOnAddress WakeByAddressSingle WakeByAddressAll
    GetTempPath2A GetTempPath2W SetThreadDescription GetThreadDescription
    GetCurrentThreadStackLimits GetOverlappedResultEx DiscardVirtualMemory
    IsWow64Process2 VirtualAlloc2)
foreach(symbol IN LISTS unsupported)
    if(imports MATCHES "[ \t]${symbol}[\r\n]")
        message(FATAL_ERROR "${EXECUTABLE} imports ${symbol}, which is unavailable on Windows 7")
    endif()
endforeach()
message(STATUS "Win7 import regression check passed: ${EXECUTABLE}")
