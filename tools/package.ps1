# 打包成「下载即用」的 zip。
#
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 -SkipBuild -Version 0.1.0
#
# 为什么需要它：仓库要求 clone + 编译，那一步就筛掉了绝大多数人。
# 打出来的 zip 解压后双击 install.cmd 即可，不需要构建环境。
#
# 产物：build\dist\DynamicIsland-Win-<版本>.zip
#   diw_overlay.exe   主程序（单文件，只依赖系统库）
#   install.cmd       双击安装（含开机自启）
#   uninstall.cmd     双击卸载（保留配置）
#   install.ps1       实际逻辑
#   README.txt        给最终用户看：含首次运行被 SmartScreen 拦截的说明
#   LICENSE

param(
    [string]$Version = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if ([string]::IsNullOrWhiteSpace($Version)) {
    $sha = (& git rev-parse --short HEAD 2>$null)
    if ([string]::IsNullOrWhiteSpace($sha)) { $sha = 'nogit' }
    $Version = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd'), $sha.Trim()
}

$exe = 'build\cpp\diw_overlay.exe'
if (-not $SkipBuild) {
    & "$PSScriptRoot\build.ps1"
    if ($LASTEXITCODE -ne 0) { throw 'build.ps1 失败，先修构建再打包' }
}
if (-not (Test-Path $exe)) { throw "找不到 $exe" }

$stageRoot = Join-Path 'build\dist' ("DynamicIsland-Win-$Version")
$zipPath = Join-Path 'build\dist' ("DynamicIsland-Win-$Version.zip")
if (Test-Path $stageRoot) { Remove-Item $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Force $stageRoot | Out-Null

function Copy-TextWithBom([string]$src, [string]$dst) {
    # 带 BOM 写出：Windows PowerShell 5.1 会按 ANSI 读无 BOM 的 UTF-8，
    # 中文注释里的引号会把脚本截断成语法错误（tools\build.ps1 踩过一次）。
    # 路径必须先转绝对：Set-Location 只改 PowerShell 的位置，
    # [System.IO.File] 用的是进程当前目录，两者可能不同。
    $text = Get-Content -Raw -Encoding UTF8 $src
    $utf8Bom = New-Object System.Text.UTF8Encoding($true)
    $full = Join-Path (Get-Location).Path $dst
    [System.IO.File]::WriteAllText($full, $text, $utf8Bom)
}

Copy-Item $exe (Join-Path $stageRoot 'diw_overlay.exe') -Force
Copy-TextWithBom 'pack\install.ps1' (Join-Path $stageRoot 'install.ps1')
Copy-Item 'pack\install.cmd'   $stageRoot -Force
Copy-Item 'pack\uninstall.cmd' $stageRoot -Force
# README.txt / LICENSE 由记事本或编辑器直接打开，UTF-8 无 BOM 即可。
Copy-Item 'pack\README.txt'    $stageRoot -Force
Copy-Item 'LICENSE'            $stageRoot -Force

# 自检：缺一个文件用户就会卡住，且报错信息通常没指向真正原因。
foreach ($need in @('diw_overlay.exe', 'install.cmd', 'uninstall.cmd', 'install.ps1',
                    'README.txt', 'LICENSE')) {
    if (-not (Test-Path (Join-Path $stageRoot $need))) { throw "打包缺少 $need" }
}
$exeSize = (Get-Item (Join-Path $stageRoot 'diw_overlay.exe')).Length
if ($exeSize -lt 200KB) { throw "exe 体积异常（$exeSize 字节），像是构建没完成" }

if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $stageRoot '*') -DestinationPath $zipPath -CompressionLevel Optimal
$zipSize = [math]::Round((Get-Item $zipPath).Length / 1MB, 2)

Write-Host ''
Write-Host ("打包完成：{0}（{1} MB，exe {2:N0} 字节）" -f $zipPath, $zipSize, $exeSize) -ForegroundColor Green
Write-Host '解压后双击 install.cmd 即可，无需构建环境。'
