cmake_minimum_required(VERSION 3.20)
project(rshell VERSION 1.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RSHELL_BUILD_TESTS "Build rshell unit tests" ON)
option(RSHELL_WINDOWS7_COMPAT "Target Windows 7 SP1 / Server 2008 R2 SP1 (requires MSVC v143 or older)" ON)

# WinCNG 的 ECDH/ECDSA 依赖 BCryptDeriveKey 的 RAW_SECRET（Windows 10+）。
# 只开放 curve25519/ECDH 的服务器必须打开它；Win7 兼容构建不能用。
if(RSHELL_WINDOWS7_COMPAT)
    set(RSHELL_WINCNG_ECDSA_DEFAULT OFF)
else()
    set(RSHELL_WINCNG_ECDSA_DEFAULT ON)
endif()
option(RSHELL_WINCNG_ECDSA
    "WinCNG ECDH/ECDSA support (Windows 10+; needed for servers that only offer ECDH, incompatible with Windows 7)" ${RSHELL_WINCNG_ECDSA_DEFAULT})

if(WIN32 AND RSHELL_WINCNG_ECDSA AND RSHELL_WINDOWS7_COMPAT)
    message(FATAL_ERROR
        "RSHELL_WINCNG_ECDSA 需要 Windows 10+ 的 CNG 接口，不能与 RSHELL_WINDOWS7_COMPAT=ON 同时开启。"
        "Win7 构建请加 -DRSHELL_WINCNG_ECDSA=OFF；要连接只开放 curve25519/ECDH 的服务器，"
        "请用 -DRSHELL_WINDOWS7_COMPAT=OFF 构建（默认启用 CNG ECDH）。")
endif()

if(WIN32 AND RSHELL_WINDOWS7_COMPAT)
    if(MSVC AND MSVC_VERSION GREATER_EQUAL 1950)
        message(FATAL_ERROR
            "Windows 7 compatibility requires MSVC v143 (14.44) or older; MSVC 14.50+ removed legacy Windows support. "
            "Install the v143 toolset and configure a fresh build directory with -T v143,version=14.44. "
            "Use -DRSHELL_WINDOWS7_COMPAT=OFF only for a modern-Windows build.")
    endif()
    add_compile_definitions(WINVER=0x0601 _WIN32_WINNT=0x0601 NTDDI_VERSION=0x06010000)
endif()

# 图标资源（src/rshell.rc -> src/res/rshell.ico）需要 RC 语言；
# 图标由 tools/make_icon.py 生成，改了设计重跑一次脚本即可。
if(WIN32)
    enable_language(RC)
endif()

# ---------------------------------------------------------------------------
# MSVC 运行库：默认 /MT 静态链接
#   exe 自带 CRT，分发时目标机不需要安装 VC++ 2015-2022 运行库，拷一个 exe 就能跑。
#   代价：rshell-gui.exe 约 +300 KB（747 KB -> 1.06 MB），但仍小于"exe + 3 个 CRT DLL"
#   的 1.6 MB。需要 /MD（例如目标机已统一装好运行库、想减小下载体积）时：
#       cmake -S . -B build_cmake -DRSHELL_STATIC_CRT=OFF
# ---------------------------------------------------------------------------
option(RSHELL_STATIC_CRT "静态链接 MSVC 运行库（/MT），分发无需 VC++ 运行库" ON)

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    # vcpkg 的 *-static 三元组使用 /MT 运行库，必须保持一致
    if(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "static")
        set(RSHELL_STATIC_CRT ON)
    endif()
    if(RSHELL_STATIC_CRT)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        message(STATUS "MSVC 运行库: /MT (静态链接，分发时不需要 VC++ 运行库)")
    else()
        message(STATUS "MSVC 运行库: /MD (目标机需要安装 VC++ 2015-2022 运行库)")
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# 依赖：libssh2 (BSD-3-Clause) + 系统加密后端
#   Windows 默认使用 WinCNG（BCrypt，无第三方依赖）
#   如需 ed25519 支持，用 -DRSHELL_CRYPTO_BACKEND=OpenSSL 并让 CMake 找到 OpenSSL
#   Linux/macOS 优先使用系统 libssh2（通常已带 OpenSSL）
# ---------------------------------------------------------------------------
set(RSHELL_CRYPTO_BACKEND "" CACHE STRING
    "libssh2 crypto backend when building the vendored copy (WinCNG/OpenSSL/mbedTLS)")

find_package(PkgConfig QUIET)
set(RSHELL_HAVE_SYSTEM_LIBSSH2 FALSE)
if(NOT WIN32)
    find_package(libssh2 CONFIG QUIET)
    if(TARGET libssh2::libssh2)
        set(RSHELL_LIBSSH2_TARGET libssh2::libssh2)
        set(RSHELL_HAVE_SYSTEM_LIBSSH2 TRUE)
    elseif(PkgConfig_FOUND)
        pkg_check_modules(LIBSSH2 IMPORTED_TARGET libssh2)
        if(TARGET PkgConfig::LIBSSH2)
            set(RSHELL_LIBSSH2_TARGET PkgConfig::LIBSSH2)
            set(RSHELL_HAVE_SYSTEM_LIBSSH2 TRUE)
        endif()
    endif()
endif()

if(NOT RSHELL_HAVE_SYSTEM_LIBSSH2)
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING_SAVED ${BUILD_TESTING})
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_ZLIB_COMPRESSION OFF CACHE BOOL "" FORCE)
    set(CLEAR_MEMORY ON CACHE BOOL "" FORCE)
    if(WIN32 AND (NOT RSHELL_CRYPTO_BACKEND OR RSHELL_CRYPTO_BACKEND STREQUAL "WinCNG"))
        # 把 rshell 的选项转给 vendored libssh2；OpenSSL 后端自带 ECDH，不需要这个开关。
        set(ENABLE_ECDSA_WINCNG ${RSHELL_WINCNG_ECDSA} CACHE BOOL "WinCNG ECDSA support" FORCE)
    endif()
    if(RSHELL_CRYPTO_BACKEND)
        set(CRYPTO_BACKEND ${RSHELL_CRYPTO_BACKEND} CACHE STRING "" FORCE)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libssh2/CMakeLists.txt)
        message(STATUS "Using vendored libssh2: third_party/libssh2")
        add_subdirectory(third_party/libssh2 EXCLUDE_FROM_ALL)
    else()
        FetchContent_Declare(libssh2
            GIT_REPOSITORY https://github.com/libssh2/libssh2.git
            GIT_TAG libssh2-1.11.1
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(libssh2)
    endif()
    set(RSHELL_LIBSSH2_TARGET libssh2)

    # 内嵌 Monocypher 提供 Ed25519/X25519，补上 WinCNG 后端缺失的密钥登录算法。
    if(WIN32 AND (NOT RSHELL_CRYPTO_BACKEND OR RSHELL_CRYPTO_BACKEND STREQUAL "WinCNG"))
        set(RSHELL_MONOCYPHER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher")
        # libssh2 是别名，真正的静态库目标名是 libssh2_static。
        target_compile_definitions(libssh2_static PRIVATE LIBSSH2_ED25519_MONOCYPHER)
        target_sources(libssh2_static PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libssh2/src/wincng_ed25519.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libssh2/src/wincng_keys.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libssh2/src/wincng_ppk.c"
            "${RSHELL_MONOCYPHER_DIR}/monocypher.c"
            "${RSHELL_MONOCYPHER_DIR}/monocypher-ed25519.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/argon2.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/core.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/encoding.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/ref.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/thread.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/blake2/blake2b.c")
        target_include_directories(libssh2_static PRIVATE
            "${RSHELL_MONOCYPHER_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/blake2")
        target_compile_definitions(libssh2_static PRIVATE
            LIBSSH2_ED25519_MONOCYPHER ARGON2_NO_THREADS)
        set_source_files_properties(
            "${RSHELL_MONOCYPHER_DIR}/monocypher.c"
            "${RSHELL_MONOCYPHER_DIR}/monocypher-ed25519.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/argon2.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/core.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/encoding.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/ref.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/thread.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/blake2/blake2b.c"
            PROPERTIES C_STANDARD 99)
    endif()
endif()

message(STATUS "libssh2 target: ${RSHELL_LIBSSH2_TARGET}")


# ---------------------------------------------------------------------------
# 核心库（可复用于 GUI / 单元测试）
# ---------------------------------------------------------------------------
add_library(rshell_core STATIC
    src/util.cpp
    src/config.cpp
    src/progress.cpp
    src/credential_store.cpp
    src/protected_secret.cpp
    src/vault.cpp
    src/ssh_session.cpp
    src/sftp_client.cpp
)

if(WIN32)
    # 拖出下载用的虚拟文件数据对象（OLE）
    target_sources(rshell_core PRIVATE src/dnd_virtual_file.cpp)
    target_link_libraries(rshell_core PUBLIC ole32 shell32)
endif()

target_include_directories(rshell_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rshell_core PUBLIC ${RSHELL_LIBSSH2_TARGET})


if(WIN32)
    target_link_libraries(rshell_core PUBLIC ws2_32 crypt32 shell32 bcrypt ncrypt)
endif()

# ---------------------------------------------------------------------------
# CLI 可执行文件
# ---------------------------------------------------------------------------
add_executable(rshell
    src/main.cpp
    src/cli.cpp
    src/sftp_shell.cpp
)
if(WIN32)
    # exe 图标（资源管理器里显示的那个）
    target_sources(rshell PRIVATE src/rshell.rc)
endif()
target_link_libraries(rshell PRIVATE rshell_core)

if(WIN32)
    set_target_properties(rshell PROPERTIES OUTPUT_NAME "rshell")
endif()

# ---------------------------------------------------------------------------
# GUI 可执行文件（Win32 原生窗口 + 内置终端）
# ---------------------------------------------------------------------------
if(WIN32)
    add_executable(rshell_gui WIN32
        src/gui_main.cpp
        src/terminal.cpp
        src/ssh_shell.cpp
        src/session_store.cpp
        src/remote_fs.cpp
        src/rshell.rc
    )
    target_link_libraries(rshell_gui PRIVATE rshell_core comdlg32 user32 gdi32 gdiplus dwmapi ole32 uuid imm32)
    set_target_properties(rshell_gui PROPERTIES OUTPUT_NAME "rshell-gui")
endif()

# 检查标准库/第三方静态库是否意外引入 Win8+ API，不能只检查我们自己的源码。
if(WIN32 AND MSVC AND RSHELL_WINDOWS7_COMPAT)
    get_filename_component(RSHELL_LINKER_DIRECTORY "${CMAKE_LINKER}" DIRECTORY)
    find_program(RSHELL_DUMPBIN dumpbin HINTS "${RSHELL_LINKER_DIRECTORY}" REQUIRED)
    foreach(target rshell rshell_gui)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DEXECUTABLE=$<TARGET_FILE:${target}>" "-DDUMPBIN=${RSHELL_DUMPBIN}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_win7_imports.cmake"
            VERBATIM)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 单元测试（不依赖网络，不需要 SSH 服务器）
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 端到端测试（需要 Python 3 + paramiko；本地拉起临时 SFTP 服务器）
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(RSHELL_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_custom_target(rshell_e2e
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_test.py
                --exe $<TARGET_FILE:rshell>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        DEPENDS rshell
        COMMENT "运行 rshell 端到端测试（put/get/mkdir/rm/rename/chmod/exec/密钥认证）"
        USES_TERMINAL)
endif()

if(RSHELL_BUILD_TESTS)
    enable_testing()
    add_executable(rshell_tests tests/test_main.cpp src/terminal.cpp src/session_store.cpp)
    target_link_libraries(rshell_tests PRIVATE rshell_core)
    add_test(NAME rshell_tests COMMAND rshell_tests)

    if(WIN32)
        # 隐藏窗口回归：直接调用 GUI 的标签、事件分发及连接清理路径。
        add_executable(rshell_gui_tabs_probe tests/gui_tabs_probe.cpp
            src/terminal.cpp src/ssh_shell.cpp src/session_store.cpp src/remote_fs.cpp)
        target_link_libraries(rshell_gui_tabs_probe PRIVATE rshell_core
            comdlg32 user32 gdi32 gdiplus dwmapi ole32 uuid imm32)
        add_test(NAME rshell_gui_tabs COMMAND rshell_gui_tabs_probe)
    endif()

    # 无 GUI 的交互式 shell 冒烟测试（配合 tests/sftp_test_server.py 使用）
    add_executable(rshell_shell_probe tests/shell_probe.cpp src/ssh_shell.cpp)
    target_link_libraries(rshell_shell_probe PRIVATE rshell_core)

    # 离屏回放：把抓到的原始输出喂给终端模拟器并打印文本网格（排查 TUI 渲染问题）
    add_executable(rshell_term_replay tests/replay.cpp src/terminal.cpp)
    target_link_libraries(rshell_term_replay PRIVATE rshell_core)
endif()
