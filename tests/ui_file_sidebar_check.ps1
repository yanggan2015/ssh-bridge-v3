# ui_file_sidebar_check.ps1 — 左侧“文件”页：串口共享目录浏览 / SSH 转 SFTP
param(
    [string]$Exe = 'C:\code\ssh-bridge-windows3\output\build\ssh_bridge3.exe',
    [string]$TemplateConfig = 'C:\code\ssh-bridge-windows3\scripts\dist-config.json',
    [string]$WorkDir = 'C:\code\ssh-bridge-windows3\output\dist\file-sidebar-test',
    [int]$Port = 18086
)
$ErrorActionPreference = 'Stop'
$base = "http://127.0.0.1:$Port"
$fail = 0
$total = 0
$app = $null

function Check([string]$name, [bool]$ok, [string]$detail = '') {
    $script:total++
    if ($ok) { Write-Host ("PASS  {0}  {1}" -f $name, $detail) }
    else { $script:fail++; Write-Host ("FAIL  {0}  {1}" -f $name, $detail) -ForegroundColor Red }
}

function Act([string]$id, [string]$action, [string]$text = '') {
    $body = @{ id = $id; action = $action }
    if ($text) { $body.text = $text }
    Invoke-RestMethod "$base/action" -Method Post -ContentType 'application/json' `
        -Body ($body | ConvertTo-Json -Compress) -TimeoutSec 10
}

function FindControl([string]$id) {
    (Invoke-RestMethod "$base/ui-tree" -TimeoutSec 10).controls | Where-Object { $_.id -eq $id } | Select-Object -First 1
}

function Wait-Until([scriptblock]$probe, [int]$timeoutMs = 8000) {
    $until = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $until) {
        if (& $probe) { return $true }
        Start-Sleep -Milliseconds 150
    }
    return $false
}

try {
    try {
        Invoke-RestMethod "$base/ping" -TimeoutSec 2 | Out-Null
        throw "端口 $Port 已被占用；请先退出正在运行的 ssh_bridge3"
    } catch {
        if ($_.Exception.Message -match '已被占用') { throw }
    }

    if (-not (Test-Path -LiteralPath $Exe)) { throw "缺少 exe：$Exe" }
    if (Test-Path -LiteralPath $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
    New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
    Copy-Item -LiteralPath $Exe -Destination (Join-Path $WorkDir 'ssh_bridge3.exe') -Force
    $share = Join-Path $WorkDir 'work\share'
    New-Item -ItemType Directory -Force -Path (Join-Path $share 'common') | Out-Null
    Set-Content -LiteralPath (Join-Path $share 'probe.txt') -Value 'FILESIDEBAR_PROBE' -Encoding utf8NoBOM
    Set-Content -LiteralPath (Join-Path $share '00-first.txt') -Value 'FIRST' -Encoding utf8NoBOM

    $c = Get-Content -LiteralPath $TemplateConfig -Raw | ConvertFrom-Json
    $serial = $c.boards[0] | ConvertTo-Json -Depth 10 | ConvertFrom-Json
    $serial.name = 'filechan'; $serial.serial_device = 'loop:ai'; $serial.auto_open = $false
    $serial.buttons = @()
    $c.boards = @($serial)
    $s = $c.ssh[0] | ConvertTo-Json -Depth 10 | ConvertFrom-Json
    $s.name = 'side-ssh'; $s.host = '192.0.2.99'; $s.port = 22
    $s.username = 'demo'; $s.password = 'CHANGE_ME'
    $c.ssh = @($s)
    $c.tftp.root = ''
    $c.ui.split = 1; $c.ui.active_pane = 0; $c.ui.pane_channels = @('filechan')
    $c | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $WorkDir 'ssh_bridge_config.json') -Encoding utf8NoBOM

    $app = Start-Process -FilePath (Join-Path $WorkDir 'ssh_bridge3.exe') -WorkingDirectory $WorkDir -PassThru
    if (-not (Wait-Until { try { Invoke-RestMethod "$base/ping" -TimeoutSec 2 | Out-Null; $true } catch { $false } } 15000)) {
        throw '测试实例未在 15 秒内启动桥接'
    }

    Act 'tab.left' 'select' '文件' | Out-Null
    if (-not (Wait-Until { (FindControl 'list.file').visible } 4000)) { throw '文件页未显示列表' }

    $list = FindControl 'list.file'
    $path = FindControl 'label.file.path'
    $note = FindControl 'label.file.note'
    $up = FindControl 'btn.file.up'
    $items = @($list.items) -join '|'
    Check 'F1 文件页显示共享目录路径' `
        ([string]$path.text -match 'work\\share$') "path=$($path.text)"
    Check 'F2 共享目录列出子目录与文件' `
        ($list.item_count -eq 3 -and $items -match 'common' -and $items -match 'probe\.txt' -and $items -match '00-first\.txt') `
        "count=$($list.item_count) items=$items"
    Check 'F3 共享目录计数提示' `
        ([string]$note.text -match '3 项') "note=$($note.text)"
    Check 'F4 根部禁用上级按钮' `
        ($up.enabled -eq $false) "enabled=$($up.enabled)"

    Act 'tree.channels' 'select' 'SSH (1) / side-ssh  ·  离线' | Out-Null
    Start-Sleep -Milliseconds 500
    $sftp = FindControl 'btn.file.sftp'
    $list2 = FindControl 'list.file'
    $note2 = FindControl 'label.file.note'
    Check 'F5 SSH 通道显示 SFTP 入口' `
        ($sftp.visible -eq $true -and $sftp.enabled -eq $true) "visible=$($sftp.visible) enabled=$($sftp.enabled)"
    Check 'F6 SSH 通道隐藏本地列表' `
        ($list2.visible -eq $false -and ([string]$note2.text -match 'SFTP')) `
        "listVisible=$($list2.visible) note=$($note2.text)"
}
catch {
    Check 'EXCEPTION' $false $_.Exception.Message
}
finally {
    if ($app -and -not $app.HasExited) { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
if ($fail -eq 0) {
    Write-Host "RESULT: ALL PASS ($total 项)" -ForegroundColor Green
    exit 0
}
Write-Host "RESULT: FAIL ($fail/$total)" -ForegroundColor Red
exit 1
