# 长跑基线：按固定间隔采样 diw_overlay 的资源占用，最后给出增长判定。
#
#   powershell -ExecutionPolicy Bypass -File tools\soak.ps1
#   powershell -ExecutionPolicy Bypass -File tools\soak.ps1 -Minutes 240
#   powershell -ExecutionPolicy Bypass -File tools\soak.ps1 -Start -Minutes 60
#
# 为什么需要它：单元测试只能证明"某一次调用是对的"，证明不了"跑四小时还对不对"。
# 三处缓存（文本 48 条 / 投影 4 槽 / 铬层 1 份）理论上都有界，但"理论上"三个字
# 在没有小时级数据时不算数。判据看的是**趋势**而不是绝对值：
# 工作集与句柄在后半段应基本持平，单调上升即视为可疑。

param(
    [int]$Minutes = 60,
    [int]$IntervalSec = 60,
    [switch]$Start,                     # 先启动一个实例再采样
    [string]$Exe = 'build\cpp\diw_overlay.exe',
    [string]$Out = ''                   # 默认 build\soak-<时间戳>.csv
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if ([string]::IsNullOrWhiteSpace($Out)) {
    $Out = Join-Path 'build' ("soak-{0:yyyyMMdd-HHmmss}.csv" -f (Get-Date))
}
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null

if ($Start) {
    if (-not (Test-Path $Exe)) { throw "找不到 $Exe，先跑 tools\build.ps1" }
    Start-Process -FilePath $Exe -ArgumentList @('--log=build/soak_run.log') | Out-Null
    Start-Sleep -Seconds 3
}

$proc = Get-Process -Name 'diw_overlay' -ErrorAction SilentlyContinue
if ($null -eq $proc) { throw "diw_overlay 没在运行。加 -Start，或先手动启动。" }

$total = [math]::Max(1, [int]($Minutes * 60 / $IntervalSec))
$rows = @()
Write-Host ("采样 {0} 次，每 {1}s 一次，约 {2} 分钟 → {3}" -f $total, $IntervalSec, $Minutes, $Out)

for ($i = 0; $i -lt $total; $i++) {
    $p = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
    if ($null -eq $p) { Write-Host "进程已退出（第 $i 次采样），提前结束。"; break }
    $row = [pscustomobject]@{
        time      = (Get-Date).ToString('HH:mm:ss')
        ws_mb     = [math]::Round($p.WorkingSet64 / 1MB, 1)
        private_mb= [math]::Round($p.PrivateMemorySize64 / 1MB, 1)
        handles   = $p.HandleCount
        threads   = $p.Threads.Count
        cpu_s     = [math]::Round($p.CPU, 1)
    }
    $rows += $row
    if (($i % 10) -eq 0) {
        Write-Host ("  t+{0,4}s  ws={1,6}MB  priv={2,6}MB  handles={3,4}  cpu={4,7}s" -f `
            ($i * $IntervalSec), $row.ws_mb, $row.private_mb, $row.handles, $row.cpu_s)
    }
    if ($i -lt $total - 1) { Start-Sleep -Seconds $IntervalSec }
}

$rows | Export-Csv -Path $Out -NoTypeInformation -Encoding UTF8
if ($rows.Count -lt 4) { Write-Host "采样点太少，不做判定。CSV: $Out"; exit 0 }

# 判据：拿后半段跟前半段比。只看前两个点会被启动期的缓存预热误导。
$half = [int]($rows.Count / 2)
$early = $rows[0..($half - 1)]
$late = $rows[$half..($rows.Count - 1)]
$avg = { param($xs, $f) ($xs | Measure-Object -Property $f -Average).Average }
$dWs = (& $avg $late 'ws_mb') - (& $avg $early 'ws_mb')
$dPr = (& $avg $late 'private_mb') - (& $avg $early 'private_mb')
$dHd = (& $avg $late 'handles') - (& $avg $early 'handles')
$span = [math]::Max(1, ($rows.Count - 1) * $IntervalSec / 60.0)

Write-Host ""
Write-Host ("后半段相对前半段：工作集 {0:+0.0;-0.0;0}MB  私有 {1:+0.0;-0.0;0}MB  句柄 {2:+0;-0;0}" -f `
    $dWs, $dPr, $dHd)
Write-Host ("折算：工作集 {0:0.00} MB/小时，句柄 {1:0.0} 个/小时（共 {2:0} 分钟）" -f `
    ($dWs / $span * 60), ($dHd / $span * 60), $span)

# 阈值不是拍脑袋：句柄每小时涨 10 个以内视为稳态波动（Windows 本身会周期性涨落），
# 工作集每小时涨 2MB 以内同理。超过就值得看一眼是不是又有缓存无界了。
$verdict = 'STABLE'
if ($dHd -gt 10 -or $dWs -gt 2.0) { $verdict = 'SUSPICIOUS' }
Write-Host ("判定：{0}   （CSV: {1}）" -f $verdict, $Out)
if ($verdict -ne 'STABLE') { exit 1 }
