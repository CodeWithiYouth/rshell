<#
.SYNOPSIS
    一键构建 rshell。默认生成 Windows 10/11 版本，也可以用 -Win7 生成 Windows 7 兼容版。

.DESCRIPTION
    统一用 NMake 单配置构建，产物直接落在构建目录里：

      .\build.ps1          -> build\modern\rshell.exe / rshell-gui.exe
      .\build.ps1 -Win7    -> build\win7\rshell.exe / rshell-gui.exe

    默认（modern）用 Visual Studio 当前默认的 MSVC，关闭 RSHELL_WINDOWS7_COMPAT，
    并启用 WinCNG ECDH/ECDSA（可连只开放 ECDH 的服务器）。

    -Win7：兼容构建，要求 MSVC 14.44 或更早：优先用仓库里 build_tmp\msvc143 的工具集，
    否则用 Visual Studio 已安装的 v143 14.44。构建后自动执行导入表检查。

.EXAMPLE
    .\build.ps1
    构建 Windows 10/11 版（Release），产物在 build\modern。

.EXAMPLE
    .\build.ps1 -Win7
    构建 Win7 兼容版（Release），产物在 build\win7。

.EXAMPLE
    .\build.ps1 -Test
    构建后运行 ctest 单元测试。

.EXAMPLE
    .\build.ps1 -Clean -Test
    删除构建目录后干净重建并测试。
#>
[CmdletBinding()]
param(
    # 构建配置：Release / Debug。
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    # 构建目录，默认 build\modern 或 build\win7。
    [string]$BuildDir,

    # 构建 Windows 7 SP1 兼容版（默认是 Windows 10/11 版）。
    [switch]$Win7,

    # 显式选择 Windows 10/11 版（默认就是它，保留这个开关兼容旧命令）。
    [switch]$Modern,

    # 构建前删除构建目录。
    [switch]$Clean,

    # 构建后运行 ctest。
    [switch]$Test,

    # 兼容模式下强制使用 Visual Studio 安装的 v143 工具集，忽略 build_tmp\msvc143。
    [switch]$PreferInstalledToolset
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host "==> $Text" -ForegroundColor Cyan
}

function Get-ProgramFilesX86 {
    $value = ${env:ProgramFiles(x86)}
    if ($value) { return $value }
    return $env:ProgramFiles
}

function Find-CMake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $vswhere = Join-Path (Get-ProgramFilesX86) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * -property installationPath |
            Select-Object -First 1
        if ($install) {
            $candidate = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $candidate) { return $candidate }
        }
    }
    throw '找不到 cmake.exe：请安装 CMake 3.20+，或安装 Visual Studio 的 CMake 组件。'
}

function Get-VisualStudioInstalls {
    $vswhere = Join-Path (Get-ProgramFilesX86) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return @() }
    return @(& $vswhere -products * -property installationPath)
}

# 兼容版只接受 14.44 及更早的 MSVC 工具集。
function Get-LocalToolset {
    $tools = Join-Path $root 'build_tmp\msvc143\VC\Tools\MSVC'
    if (-not (Test-Path $tools)) { return $null }
    $candidates = Get-ChildItem $tools -Directory -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -match '^14\.(3[0-9]|4[0-4])\.' -and
            (Test-Path (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe'))
        } |
        Sort-Object Name -Descending
    if (-not $candidates) { return $null }
    return [pscustomobject]@{
        Path    = $candidates[0].FullName
        Name    = $candidates[0].Name
        Version = [version]$candidates[0].Name
        Source  = 'build_tmp\msvc143'
    }
}

function Get-InstalledToolsets {
    $result = @()
    foreach ($install in Get-VisualStudioInstalls) {
        $tools = Join-Path $install 'VC\Tools\MSVC'
        if (-not (Test-Path $tools)) { continue }
        foreach ($directory in Get-ChildItem $tools -Directory -ErrorAction SilentlyContinue) {
            $version = $null
            if (-not [version]::TryParse($directory.Name, [ref]$version)) { continue }
            if (-not (Test-Path (Join-Path $directory.FullName 'bin\Hostx64\x64\cl.exe'))) { continue }
            $result += [pscustomobject]@{
                Path    = $directory.FullName
                Name    = $directory.Name
                Version = $version
                Source  = 'Visual Studio'
            }
        }
    }
    return @($result | Sort-Object Version -Descending)
}

function Get-WindowsSdk {
    $kitsRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' `
            -Name KitsRoot10 -ErrorAction SilentlyContinue).KitsRoot10
    if (-not $kitsRoot) {
        throw '找不到 Windows SDK：注册表里没有 KitsRoot10，请安装 Windows 10/11 SDK。'
    }
    $kitsRoot = $kitsRoot.TrimEnd('\')
    $includeRoot = Join-Path $kitsRoot 'Include'
    $versions = Get-ChildItem $includeRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') } |
        Sort-Object { $_.Name -as [version] } -Descending
    foreach ($version in $versions) {
        $bin = Join-Path $kitsRoot "bin\$($version.Name)\x64"
        if (Test-Path (Join-Path $bin 'rc.exe')) {
            return [pscustomobject]@{ Root = $kitsRoot; Version = $version.Name; Bin = $bin }
        }
    }
    throw "Windows SDK 缺少 x64 工具（rc.exe）：$includeRoot"
}

# ---------------------------------------------------------------- 选择工具链
$localToolset = Get-LocalToolset
$installedToolsets = Get-InstalledToolsets

if ($Win7 -and $Modern) {
    throw '-Win7 和 -Modern 不能同时使用：默认就是 modern，Win7 兼容版只需加 -Win7。'
}

$mode = if ($Win7) { 'win7' } else { 'modern' }

if ($mode -eq 'modern') {
    $target = 'Windows 10/11'
    $toolset = $installedToolsets | Select-Object -First 1
    if (-not $toolset) {
        $toolset = $localToolset
        if ($toolset) {
            Write-Host '警告：没有找到 Visual Studio 的 MSVC 工具集，改用 build_tmp\msvc143。' -ForegroundColor Yellow
        }
    }
    if (-not $toolset) {
        Write-Host '没有可用的 MSVC 工具集：请安装 Visual Studio 的 C++ 生成工具。' -ForegroundColor Red
        throw '没有可用的 MSVC 工具集。'
    }
    if (-not $BuildDir) { $BuildDir = 'build\modern' }
} else {
    $target = 'Windows 7 SP1 x64 兼容'
    $toolset = $null
    if (-not $PreferInstalledToolset) { $toolset = $localToolset }
    if (-not $toolset) {
        $toolset = $installedToolsets |
            Where-Object { $_.Version -le [version]'14.44.99999' } |
            Select-Object -First 1
    }
    if (-not $toolset) {
        Write-Host '没有可用的 MSVC 14.44 工具集，无法构建 Windows 7 兼容版：' -ForegroundColor Red
        Write-Host '  - 在 Visual Studio Installer 里安装「MSVC v143 - VS 2022 C++ x64/x86 生成工具 (v14.44)」；'
        Write-Host '  - 或者把 14.44 工具集放到 build_tmp\msvc143（目录结构 VC\Tools\MSVC\14.44.*）；'
        Write-Host '  - 如果只需要 Windows 10/11，直接运行：.\build.ps1'
        throw '没有可用的 MSVC 14.44 工具集（说明见上）。'
    }
    if (-not $BuildDir) { $BuildDir = 'build\win7' }
}

if ([IO.Path]::IsPathRooted($BuildDir)) {
    $buildPath = [IO.Path]::GetFullPath($BuildDir)
} else {
    $buildPath = [IO.Path]::GetFullPath((Join-Path $root $BuildDir))
}

if ($Clean) {
    $rootPrefix = [IO.Path]::GetFullPath($root).TrimEnd('\') + '\'
    if (-not $buildPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "-Clean 只允许删除仓库内的构建目录：$buildPath"
    }
    if (Test-Path -LiteralPath $buildPath) {
        Write-Step "清理构建目录 $buildPath"
        Remove-Item -LiteralPath $buildPath -Recurse -Force
    }
}
New-Item -ItemType Directory -Path $buildPath -Force | Out-Null

# ---------------------------------------------------------------- 准备工具链环境
$toolsetRoot = $toolset.Path
$compilerBin = Join-Path $toolsetRoot 'bin\Hostx64\x64'
if (-not (Test-Path (Join-Path $compilerBin 'cl.exe'))) {
    throw "工具集不完整，缺少 cl.exe：$compilerBin"
}
$sdk = Get-WindowsSdk

$env:PATH = "$compilerBin;$($sdk.Bin);$env:PATH"
$env:INCLUDE = (@(
        (Join-Path $toolsetRoot 'include')
        (Join-Path $sdk.Root "Include\$($sdk.Version)\ucrt")
        (Join-Path $sdk.Root "Include\$($sdk.Version)\shared")
        (Join-Path $sdk.Root "Include\$($sdk.Version)\um")
        (Join-Path $sdk.Root "Include\$($sdk.Version)\winrt")
    ) -join ';')
$env:LIB = (@(
        (Join-Path $toolsetRoot 'lib\x64')
        (Join-Path $sdk.Root "Lib\$($sdk.Version)\ucrt\x64")
        (Join-Path $sdk.Root "Lib\$($sdk.Version)\um\x64")
    ) -join ';')

if ($Configuration -eq 'Debug') {
    $runtimeLibrary = 'MultiThreadedDebug'
} else {
    $runtimeLibrary = 'MultiThreaded'
}

$compilerPath = $compilerBin.Replace('\', '/')
$sdkBinPath = $sdk.Bin.Replace('\', '/')
if ($mode -eq 'modern') {
    $compatValue = 'OFF'
    $ecdhValue = 'ON'
} else {
    $compatValue = 'ON'
    $ecdhValue = 'OFF'
}

$commonArgs = @(
    '-S', $root, '-B', $buildPath
    '-G', 'NMake Makefiles'
    "-DCMAKE_MAKE_PROGRAM=$compilerPath/nmake.exe"
    "-DCMAKE_C_COMPILER=$compilerPath/cl.exe"
    "-DCMAKE_CXX_COMPILER=$compilerPath/cl.exe"
    "-DCMAKE_LINKER=$compilerPath/link.exe"
    "-DCMAKE_RC_COMPILER=$sdkBinPath/rc.exe"
    "-DCMAKE_MT=$sdkBinPath/mt.exe"
    "-DCMAKE_BUILD_TYPE=$Configuration"
    "-DCMAKE_TRY_COMPILE_CONFIGURATION=$Configuration"
    # 本地 14.44 工具集只有静态 CRT 库，必须在编译器检查前就定成 /MT。
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=$runtimeLibrary"
    "-DRSHELL_WINDOWS7_COMPAT=$compatValue"
    "-DRSHELL_WINCNG_ECDSA=$ecdhValue"
    "-DRSHELL_CRYPTO_BACKEND=WinCNG"
)

$cmake = Find-CMake
Write-Step "构建 $target（$Configuration，MSVC $($toolset.Name)，$($toolset.Source)）"
Write-Step "配置 $buildPath"
& $cmake @commonArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake 配置失败（退出码 $LASTEXITCODE）。如果换了工具链或构建目录，请加 -Clean 重试。"
}

Write-Step '编译'
& $cmake --build $buildPath
if ($LASTEXITCODE -ne 0) { throw "编译失败（退出码 $LASTEXITCODE）。" }

if ($Test) {
    Write-Step '运行单元测试'
    $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
    if (-not (Test-Path $ctest)) { $ctest = 'ctest' }
    & $ctest --test-dir $buildPath --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "单元测试失败（退出码 $LASTEXITCODE）。" }
}

Write-Host ''
Write-Host "构建完成，产物目录：$buildPath" -ForegroundColor Green
foreach ($name in @('rshell.exe', 'rshell-gui.exe', 'rshell_tests.exe')) {
    $path = Join-Path $buildPath $name
    if (Test-Path $path) {
        $size = [math]::Round((Get-Item $path).Length / 1KB)
        Write-Host ("  {0}  ({1} KB)" -f $path, $size)
    }
}
