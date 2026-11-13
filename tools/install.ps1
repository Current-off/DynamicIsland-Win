# 安装 / 卸载
#
#   安装： powershell -ExecutionPolicy Bypass -File tools\install.ps1
#   自启： powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Autostart
#   卸载： powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Uninstall
#
# 装到 %LOCALAPPDATA%\DynamicIsland-Win —— 不需要管理员权限，
# 也不会往 Program Files 里写需要 UAC 的东西。
#
# 配置（rules.json）在 %APPDATA%\DynamicIsland-Win，和安装目录分开：
# 卸载时默认**保留**它，因为那是用户自己调过的外观和开关，重装应该还在。

param(
    [switch]$Uninstall,
    [switch]$Autostart,
    [switch]$NoLaunch,          # 装完不自动启动
    [switch]$KeepConfig = $true # 卸载时保留配置（默认）
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$installDir = Join-Path $env:LOCALAPPDATA 'DynamicIsland-Win'
$exePath = Join-Path $installDir 'diw_overlay.exe'
$configDir = Join-Path $env:APPDATA 'DynamicIsland-Win'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runName = 'DynamicIslandWin'

function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }
function Info($msg) { Write-Host $msg }

# 防止把安装目录算错之后删掉别的东西 —— 卸载前必须能对上这个后缀。
if (-not $installDir.EndsWith('DynamicIsland-Win')) {
    Fail "安装目录异常，已中止：$installDir"
}

# ---------------------------------------------------------------- 卸载
if ($Uninstall) {
    Info "正在卸载…"

    Get-Process -Name 'diw_overlay' -ErrorAction SilentlyContinue | ForEach-Object {
        $_ | Stop-Process -Force -ErrorAction SilentlyContinue
    }

    if (Test-Path $runKey) {
        $existing = (Get-ItemProperty $runKey -ErrorAction SilentlyContinue).$runName
        if ($existing) {
            Remove-ItemProperty -Path $runKey -Name $runName -ErrorAction SilentlyContinue
            Info "  已移除开机自启项"
        }
    }

    $startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
    foreach ($lnk in @((Join-Path $startMenu 'DynamicIsland-Win.lnk'),
                       (Join-Path ([Environment]::GetFolderPath('Desktop')) 'DynamicIsland-Win.lnk'))) {
        if (Test-Path $lnk) { Remove-Item $lnk -Force; Info "  已删除快捷方式 $(Split-Path -Leaf $lnk)" }
    }

    if (Test-Path $installDir) {
        Remove-Item $installDir -Recurse -Force
        Info "  已删除 $installDir"
    }

    if (-not $KeepConfig -and (Test-Path $configDir)) {
        Remove-Item $configDir -Recurse -Force
        Info "  已删除配置 $configDir"
    } elseif (Test-Path $configDir) {
        Info "  配置保留在 $configDir（加 -KeepConfig:$false 可一并删除）"
    }

    Info "卸载完成。"
    exit 0
}

# ---------------------------------------------------------------- 安装
$built = Join-Path $root 'build\cpp\diw_overlay.exe'
if (-not (Test-Path $built)) {
    Fail "找不到 $built。请先运行 tools\build.ps1 编译。"
}

Info "安装到 $installDir"
New-Item -ItemType Directory -Force $installDir | Out-Null

# 覆盖前先结束正在跑的实例，否则复制会因文件占用失败。
Get-Process -Name 'diw_overlay' -ErrorAction SilentlyContinue | ForEach-Object {
    $_ | Stop-Process -Force -ErrorAction SilentlyContinue
    Info "  已结束正在运行的实例（PID $($_.Id)）"
    Start-Sleep -Milliseconds 300
}

Copy-Item $built $exePath -Force
Info "  已复制 diw_overlay.exe"

# 开始菜单快捷方式（卸载/系统搜索都能用到）
$lnkPath = Join-Path (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs') 'DynamicIsland-Win.lnk'
$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut($lnkPath)
$lnk.TargetPath = $exePath
$lnk.WorkingDirectory = $installDir
$lnk.Description = 'DynamicIsland-Win'
$lnk.Save()
Info "  已创建开始菜单快捷方式"

if ($Autostart) {
    Set-ItemProperty -Path $runKey -Name $runName -Value "`"$exePath`"" -Force
    Info "  已设置开机自启（HKCU\...\Run\$runName）"
}

Info ""
Info "安装完成。"
Info "  可执行文件：$exePath"
Info "  配置文件：  $(Join-Path $configDir 'rules.json')（首次运行自动生成）"

if (-not $NoLaunch) {
    Start-Process -FilePath $exePath
    Info "  已启动。托盘/右键菜单里可以打开设置。"
}
