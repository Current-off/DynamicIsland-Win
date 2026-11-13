# 一键构建（Windows / MSVC，不需要开发者命令行）
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1
#
# 为什么要有这个脚本：原来的构建入口是 Git Bash 的 build_cpp_msvc.sh，
# 而 Windows 用户默认没有 Git Bash，也不会记得去开 "x64 Native Tools" 命令行。
# 一份能在裸 PowerShell 里直接跑的构建脚本，决定了"clone 下来能不能跑起来"。
#
# 环境从两个**可靠来源**推导，不写死路径：
#   - MSVC      ：vswhere 找到 VS 安装位置，取里面版本号最大的 MSVC
#   - Windows SDK：注册表 HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots
# 手写 C:\Program Files (x86)\Windows Kits\10 是行不通的 —— 实测有些机器
# 把 SDK 装在 D 盘，硬编码路径会让脚本在本机"看起来能跑、在别人机器上找不到头文件"。

param(
    [switch]$SkipTests,   # 只编译、不跑测试
    [switch]$Quiet        # 只输出错误和结果
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Write-Step($msg) { if (-not $Quiet) { Write-Host "=== $msg ===" -ForegroundColor Cyan } }
function Write-Ok($msg) { Write-Host "  OK  $msg" -ForegroundColor Green }
function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- 1. 找工具链
Write-Step "locating MSVC"

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Fail "找不到 vswhere.exe。请安装 Visual Studio 2022 并勾选「使用 C++ 的桌面开发」。"
}
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    Fail "vswhere 没有找到带 C++ 工具链的 Visual Studio。请安装「使用 C++ 的桌面开发」工作负载。"
}

$msvcDir = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
if ($null -eq $msvcDir) { Fail "在 $vsPath 下找不到 MSVC 工具集。" }

$kitRoot = $null
foreach ($key in @('HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
                   'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots')) {
    if (Test-Path $key) {
        $v = (Get-ItemProperty $key).KitsRoot10
        if ($v) { $kitRoot = $v.TrimEnd('\'); break }
    }
}
if (-not $kitRoot) { Fail "找不到 Windows SDK（注册表里没有 KitsRoot10）。" }

$sdkDir = Get-ChildItem (Join-Path $kitRoot 'Include') -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $kitRoot "Lib\$($_.Name)\um\x64") } |
    Sort-Object Name -Descending | Select-Object -First 1
if ($null -eq $sdkDir) { Fail "在 $kitRoot 下找不到可用的 SDK 版本。" }

if (-not $Quiet) {
    Write-Host "  MSVC : $($msvcDir.FullName)"
    Write-Host "  SDK  : $kitRoot\$($sdkDir.Name)"
}

# ---------------------------------------------------------------- 2. 环境
$inc = @(
    (Join-Path $msvcDir.FullName 'include'),
    (Join-Path $kitRoot "Include\$($sdkDir.Name)\ucrt"),
    (Join-Path $kitRoot "Include\$($sdkDir.Name)\shared"),
    (Join-Path $kitRoot "Include\$($sdkDir.Name)\um"),
    (Join-Path $kitRoot "Include\$($sdkDir.Name)\winrt"),
    (Join-Path $kitRoot "Include\$($sdkDir.Name)\cppwinrt")
) -join ';'
$lib = @(
    (Join-Path $msvcDir.FullName 'lib\x64'),
    (Join-Path $kitRoot "Lib\$($sdkDir.Name)\ucrt\x64"),
    (Join-Path $kitRoot "Lib\$($sdkDir.Name)\um\x64")
) -join ';'
$env:INCLUDE = $inc
$env:LIB = $lib
$env:PATH = (Join-Path $msvcDir.FullName 'bin\Hostx64\x64') + ';' + $env:PATH

$cl = Join-Path $msvcDir.FullName 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path $cl)) { Fail "找不到 cl.exe：$cl" }

$outDir = Join-Path $root 'build\cpp'
New-Item -ItemType Directory -Force $outDir | Out-Null

# ---------------------------------------------------------------- 3. 目标表
$flags = @('-nologo', '-std:c++20', '-utf-8', '-EHsc', '-W4', '-DNOMINMAX',
           '-DUNICODE', '-D_UNICODE', '-Icontracts', '-Isrc/core')
$guiLibs = @('user32.lib', 'gdi32.lib', 'gdiplus.lib', 'shlwapi.lib', 'shell32.lib',
             'ole32.lib', 'windowsapp.lib', 'winhttp.lib')

# 顺序有意义：测试先编，编不过就不用再编探针了。
$targets = @(
    @{ name = 'contract_smoke';   src = 'tests/cpp/contract_smoke.cpp';   libs = @() },
    @{ name = 'core_tests';       src = 'tests/cpp/core_tests.cpp';        libs = @() },
    @{ name = 'rules_tests';      src = 'tests/cpp/rules_tests.cpp';       libs = @() },
    @{ name = 'render_tests';     src = 'tests/cpp/render_tests.cpp';      libs = @('gdiplus.lib', 'user32.lib', 'gdi32.lib') },
    @{ name = 'interaction_tests'; src = 'tests/cpp/interaction_tests.cpp'; libs = @() },
    @{ name = 'agent_tests';      src = 'tests/cpp/agent_tests.cpp';       libs = @() },
    @{ name = 'editor_tests';     src = 'tests/cpp/editor_tests.cpp';      libs = @() },
    @{ name = 'browser_tests';    src = 'tests/cpp/browser_tests.cpp';     libs = @() },
    @{ name = 'agent_probe';      src = 'probes/agent_probe.cpp';          libs = $guiLibs },
    @{ name = 'browser_probe';    src = 'probes/browser_probe.cpp';        libs = @('user32.lib', 'shell32.lib', 'ole32.lib') },
    @{ name = 'smtc_probe';       src = 'probes/smtc_probe.cpp';           libs = $guiLibs },
    @{ name = 'settings_save_probe'; src = 'probes/settings_save_probe.cpp'; libs = $guiLibs },
    @{ name = 'toast_probe';      src = 'probes/toast_probe.cpp';          libs = $guiLibs },
    @{ name = 'cover_probe';      src = 'probes/cover_probe.cpp';          libs = $guiLibs },
    @{ name = 'media_inject_probe'; src = 'probes/media_inject_probe.cpp'; libs = $guiLibs },
    @{ name = 'png_probe';        src = 'probes/png_probe.cpp';            libs = $guiLibs },
    @{ name = 'backdrop_probe';   src = 'probes/backdrop_probe.cpp';       libs = $guiLibs },
    @{ name = 'white_bg_probe';   src = 'probes/white_bg_probe.cpp';       libs = $guiLibs },
    @{ name = 'notif_diag_probe'; src = 'probes/notif_diag_probe.cpp';     libs = $guiLibs },
    @{ name = 'notif_probe';      src = 'probes/notif_probe.cpp';          libs = $guiLibs },
    @{ name = 'appicon_probe';    src = 'probes/appicon_probe.cpp';        libs = $guiLibs },
    @{ name = 'lyrics_probe';     src = 'probes/lyrics_probe.cpp';         libs = $guiLibs },
    # 把每一种展开态各渲染一张 PNG：判断「底部太空 / 内容被裁切」只能看，
    # 代码里读不出留白。已经靠它定位过两次（日历、通知/剪贴板/电量）。
    @{ name = 'layout_probe';     src = 'probes/layout_probe.cpp';         libs = $guiLibs },
    @{ name = 'hanzi_probe';      src = 'probes/hanzi_probe.cpp';          libs = $guiLibs },
    @{ name = 'cache_probe';      src = 'probes/cache_probe.cpp';          libs = $guiLibs }
)

$failed = @()
foreach ($t in $targets) {
    if (-not (Test-Path $t.src)) { $failed += "$($t.name) (源文件缺失: $($t.src))"; continue }
    $exe = Join-Path $outDir "$($t.name).exe"
    $obj = Join-Path $outDir "$($t.name).obj"
    # 逐项追加，不写 @('-Fo' + $obj, ...) —— 在数组字面量里做字符串拼接
    # 会被逗号优先级拆开，最终传出去的是几个碎掉的参数（表现为「缺少源文件名」）。
    $clArgs = @()
    $clArgs += $flags
    $clArgs += ('-Fo' + $obj)
    $clArgs += ('-Fe' + $exe)
    $clArgs += $t.src
    $clArgs += '/link'
    $clArgs += '/SUBSYSTEM:CONSOLE'
    foreach ($lib in $t.libs) { $clArgs += $lib }
    & $cl @clArgs 2>&1 | ForEach-Object {
        if ($_ -match 'error|warning') { Write-Host "  $_" -ForegroundColor Yellow }
    }
    if ($LASTEXITCODE -ne 0) { $failed += $t.name }
}

# 主程序（GUI 子系统）
Write-Step "building diw_overlay.exe"
$overlay = Join-Path $outDir 'diw_overlay.exe'
$clArgs = @()
$clArgs += $flags
$clArgs += ('-Fo' + (Join-Path $outDir 'diw_overlay.obj'))
$clArgs += ('-Fe' + $overlay)
$clArgs += 'src/win32/overlay_main.cpp'
$clArgs += '/link'
$clArgs += '/SUBSYSTEM:WINDOWS'
foreach ($lib in $guiLibs) { $clArgs += $lib }
& $cl @clArgs 2>&1 | ForEach-Object {
    if ($_ -match 'error|warning') { Write-Host "  $_" -ForegroundColor Yellow }
}
if ($LASTEXITCODE -ne 0) { $failed += 'diw_overlay' }

if ($failed.Count -gt 0) {
    Fail ("编译失败：" + ($failed -join ', '))
}
Write-Ok "所有目标编译通过"

# ---------------------------------------------------------------- 4. 测试
if ($SkipTests) { Write-Host "已跳过测试（-SkipTests）"; exit 0 }

Write-Step "running tests"
$tests = @('contract_smoke', 'core_tests', 'rules_tests', 'render_tests',
           'interaction_tests', 'agent_tests', 'editor_tests', 'browser_tests')
$bad = @()
foreach ($name in $tests) {
    $exe = Join-Path $outDir "$name.exe"
    if (-not (Test-Path $exe)) { $bad += $name; continue }
    $output = & $exe 2>&1
    if ($LASTEXITCODE -ne 0) {
        $bad += $name
        Write-Host "  FAIL $name" -ForegroundColor Red
        $output | Select-Object -Last 6 | ForEach-Object { Write-Host "    $_" }
    } elseif (-not $Quiet) {
        $line = ($output | Where-Object { $_ -match 'run=|PASS|failed=' } | Select-Object -Last 1)
        Write-Host "  ok   $name  $line"
    }
}

if ($bad.Count -gt 0) { Fail ("测试失败：" + ($bad -join ', ')) }
Write-Ok "全部测试通过"
Write-Host ""
Write-Host ""
Write-Host "可执行文件：$overlay" -ForegroundColor Cyan
Write-Host "运行：     .\build\cpp\diw_overlay.exe" -ForegroundColor Cyan
