# 发布到 GitHub
#
#   powershell -ExecutionPolicy Bypass -File tools\publish.ps1 -RepoUrl https://github.com/<你>/<仓库>.git
#
# 做三件事：设 remote（已存在则改成新的）、推送 main、打印仓库地址。
# 不替你建仓库 —— 建仓库要网页或 gh CLI 的授权，脚本拿不到也不该假装拿到。

param(
    [Parameter(Mandatory = $true)]
    [string]$RepoUrl,
    [string]$Branch = 'main'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path (Join-Path $root '.git'))) {
    Write-Host "ERROR: 当前目录不是 git 仓库。" -ForegroundColor Red
    exit 1
}

# 允许传 "owner/repo" 这种简写，省得每次写全 URL。
if ($RepoUrl -notmatch '^https?://|^git@') {
    $RepoUrl = "https://github.com/$RepoUrl.git"
    if ($RepoUrl -notmatch '\.git$') { $RepoUrl += '.git' }
}

$existing = git remote 2>$null
if ($existing -contains 'origin') {
    git remote set-url origin $RepoUrl
    Write-Host "已更新 origin -> $RepoUrl"
} else {
    git remote add origin $RepoUrl
    Write-Host "已添加 origin -> $RepoUrl"
}

# 确认工作区是干净的，避免推上去一份半成品。
$dirty = git status --porcelain
if ($dirty) {
    Write-Host ""
    Write-Host "工作区还有未提交的改动：" -ForegroundColor Yellow
    $dirty | Select-Object -First 10 | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
    Write-Host "先提交再推送： git add -A; git commit -m `"...`"" -ForegroundColor Yellow
    exit 1
}

Write-Host "推送 $Branch …"
git push -u origin $Branch
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "推送失败。常见原因：" -ForegroundColor Red
    Write-Host "  1) 仓库还没建 —— 先去 https://github.com/new 建一个空仓库（不要勾 README）"
    Write-Host "  2) 没有凭证 —— 用 Git Credential Manager 登录一次，或改用 SSH 地址"
    exit 1
}

Write-Host ""
Write-Host "完成：$($RepoUrl -replace '\.git$', '')" -ForegroundColor Green
