# 往岛上推一条活动 / 结束一条活动。
#
#   powershell -ExecutionPolicy Bypass -File tools\send-activity.ps1
#   powershell -ExecutionPolicy Bypass -File tools\send-activity.ps1 -Title "构建通过" -Kind bridge.ci
#   powershell -ExecutionPolicy Bypass -File tools\send-activity.ps1 -End -Id "cli:demo"
#
# 协议见 docs/protocol.md：一条连接一条消息，UTF-8 JSON lines，服务端回一行确认。
# 退出码：0 成功；1 连不上（岛没在跑）；2 服务端拒绝（会打印原因）。

param(
    [string]$Id      = 'cli:demo',
    [string]$Kind    = 'bridge.cli',
    [string]$Title   = '来自命令行的活动',
    [string]$Content = '{"body":"pushed via named pipe"}',
    [int]$Score      = 80,
    [int]$StaleMs    = 8000,          # 0 表示不过期（会一直占着主位，慎用）
    [switch]$End,                     # 只结束，不新建
    [string]$Pipe    = 'DynamicIslandWin-v1'
)

$ErrorActionPreference = 'Stop'

$activity = [ordered]@{ id = $Id }
if (-not $End) {
    $activity.kind = $Kind
    $activity.title = $Title
    $activity.relevanceScore = $Score
    $activity.createdAtMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    if ($StaleMs -gt 0) {
        $activity.staleAtMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() + $StaleMs
    }
    $activity.content = $Content | ConvertFrom-Json -AsHashtable
}

$message = [ordered]@{
    protocolVersion = 1
    seq             = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() % 1000000
    type            = if ($End) { 'activity.end' } else { 'activity.upsert' }
    ts              = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    activity        = $activity
}
$json = $message | ConvertTo-Json -Depth 8 -Compress

$client = New-Object System.IO.Pipes.NamedPipeClientStream('.', $Pipe, [System.IO.Pipes.PipeDirection]::InOut)
try {
    $client.Connect(2000)
} catch {
    Write-Host "连不上管道 \\.\pipe\$Pipe —— 岛没在运行？" -ForegroundColor Red
    exit 1
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
$writer = New-Object System.IO.StreamWriter($client, $utf8)
$writer.NewLine = "`n"       # 服务端容忍 CRLF，但发 LF 更明确：一条消息一行
$writer.WriteLine($json)
$writer.Flush()

$reader = New-Object System.IO.StreamReader($client, $utf8)
$response = $reader.ReadToEnd()   # 服务端回完就断开，读到 EOF 即完整
# 服务端在 Respond 之后立刻断开，所以这里再 Dispose 会把缓冲刷到一个已关闭的
# 管道上，抛 "Pipe is broken"。这不是错误，忽略即可 —— 消息早就送达了。
try { $writer.Dispose() } catch { }
try { $reader.Dispose() } catch { }
try { $client.Dispose() } catch { }

$reply = $response.Trim() | ConvertFrom-Json
if ($reply.ok) {
    Write-Host ("已送达：" + $message.type + " " + $Id) -ForegroundColor Green
    exit 0
} else {
    Write-Host ("服务端拒绝：" + $reply.error) -ForegroundColor Red
    exit 2
}
