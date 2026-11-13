# 独立安装器（随 zip 发布，不依赖仓库结构）
#
#   install.cmd            → 装到 %LOCALAPPDATA%\DynamicIsland-Win 并开机自启
#   install.cmd -NoAutostart → 只装，不自启
#   install.cmd -Uninstall   → 卸载（保留配置）
#
# 与仓库里的 tools\install.ps1 的区别：那个要从 build\cpp 拷，必须整仓 clone；
# 这个是给"下载 zip、解压、双击"的用户用的，所有路径都以自己所在目录为基准。

param(
    [switch]$Uninstall,
    [switch]$NoAutostart,
    [switch]$NoLaunch,
    [switch]$KeepConfig = $true
)

$ErrorActionPreference = 'Stop'
$srcDir = $PSScriptRoot
$exeSrc = Join-Path $srcDir 'diw_overlay.exe'
$installDir = Join-Path $env:LOCALAPPDATA 'DynamicIsland-Win'
$exePath = Join-Path $installDir 'diw_overlay.exe'
$configDir = Join-Path $env:APPDATA 'DynamicIsland-Win'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runName = 'DynamicIslandWin'

function Info($msg) { Write-Host $msg }
function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

# 安装目录必须是这个后缀，避免路径算错时误删别的东西。
if (-not $installDir.EndsWith('DynamicIsland-Win')) { Fail "安装目录异常：$installDir" }

if ($Uninstall) {
    Info '正在卸载…'
    Get-Process -Name 'diw_overlay' -ErrorAction SilentlyContinue |
        ForEach-Object { $_ | Stop-Process -Force -ErrorAction SilentlyContinue }

    if (Test-Path $runKey) {
        $existing = (Get-ItemProperty $runKey -ErrorAction SilentlyContinue).$runName
        if ($existing) {
            Remove-ItemProperty -Path $runKey -Name $runName -ErrorAction SilentlyContinue
            Info '  已移除开机自启项'
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

    if ($KeepConfig) {
        if (Test-Path $configDir) { Info "  配置保留在 $configDir（加 -KeepConfig:`$false 可一并删除）" }
    } elseif (Test-Path $configDir) {
        Remove-Item $configDir -Recurse -Force
        Info "  已删除配置 $configDir"
    }

    Info '卸载完成。'
    exit 0
}

if (-not (Test-Path $exeSrc)) {
    Fail "当前目录找不到 diw_overlay.exe。请把整个 zip 解压后再运行 install.cmd（不要只拷 exe）。"
}

Info "安装到 $installDir"
New-Item -ItemType Directory -Force $installDir | Out-Null

# 覆盖前先结束正在跑的实例，否则复制会因文件占用失败。
Get-Process -Name 'diw_overlay' -ErrorAction SilentlyContinue | ForEach-Object {
    $_ | Stop-Process -Force -ErrorAction SilentlyContinue
    Info "  已结束正在运行的实例（PID $($_.Id)）"
    Start-Sleep -Milliseconds 300
}

Copy-Item $exeSrc $exePath -Force
Info '  已复制 diw_overlay.exe'

$lnkPath = Join-Path (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs') 'DynamicIsland-Win.lnk'
$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut($lnkPath)
$lnk.TargetPath = $exePath
$lnk.WorkingDirectory = $installDir
$lnk.Description = 'DynamicIsland-Win'
$lnk.Save()
Info '  已创建开始菜单快捷方式'

if (-not $NoAutostart) {
    Set-ItemProperty -Path $runKey -Name $runName -Value "`"$exePath`"" -Force
    Info '  已设置开机自启（HKCU\...\Run）'
}

Info ''
Info '安装完成。'
Info "  可执行文件：$exePath"
Info "  配置文件：  $(Join-Path $configDir 'rules.json')（首次运行自动生成）"
Info '  退出方式：  托盘图标左键/右键 → 退出；或胶囊右键 → 退出'

if (-not $NoLaunch) {
    Start-Process -FilePath $exePath
    Info '  已启动。'
}
