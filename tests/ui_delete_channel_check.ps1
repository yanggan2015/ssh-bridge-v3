# ui_delete_channel_check.ps1 — 真实删除通道闭环（串口 / SSH / 最后一个通道）
# 启动一个隔离测试实例，不读取也不修改用户当前配置。
# 用法：pwsh -NoProfile -File tests\ui_delete_channel_check.ps1
param(
    [string]$Exe = 'C:\code\ssh-bridge-windows3\output\build\ssh_bridge3.exe',
    [string]$TemplateConfig = 'C:\code\ssh-bridge-windows3\scripts\dist-config.json',
    [string]$WorkDir = 'C:\code\ssh-bridge-windows3\output\dist\delete-channel-test',
    [int]$Port = 18096
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

function State { (Invoke-RestMethod "$base/state" -TimeoutSec 10).state }

function FindControl([string]$id) {
    (Invoke-RestMethod "$base/ui-tree" -TimeoutSec 10).controls | Where-Object { $_.id -eq $id }
}

function Wait-Until([scriptblock]$probe, [int]$timeoutMs = 8000) {
    $until = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $until) {
        if (& $probe) { return $true }
        Start-Sleep -Milliseconds 150
    }
    return $false
}

function DeleteCurrent {
    Act 'menu.file.deleteChannel' 'click' | Out-Null
    if (-not (Wait-Until { (FindControl 'dlg.channel.delete') } 4000)) { throw '删除对话框未出现' }
    $btn = FindControl 'dlg.channel.delete'
    Act 'dlg.channel.delete' 'click' | Out-Null
    Start-Sleep -Milliseconds 250
    $armed = FindControl 'dlg.channel.delete'
    if (-not $armed -or $armed.text -ne '确认删除') { throw '删除按钮未进入二次确认态' }
    Act 'dlg.channel.delete' 'click' | Out-Null
    if (-not (Wait-Until { -not (State).ui.channel_dialog_open } 10000)) { throw '删除对话框未关闭' }
}

try {
    # 端口必须空闲，避免误操作真实实例
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
    New-Item -ItemType Directory -Force -Path (Join-Path $WorkDir 'work\logs') | Out-Null

    $c = Get-Content -LiteralPath $TemplateConfig -Raw | ConvertFrom-Json
    $del = $c.boards[0] | ConvertTo-Json -Depth 10 | ConvertFrom-Json
    $del.name = 'delme'; $del.serial_device = 'loop:ai'; $del.auto_open = $true; $del.buttons = @()
    $keep = $c.boards[0] | ConvertTo-Json -Depth 10 | ConvertFrom-Json
    $keep.name = 'keepme'; $keep.serial_device = 'loop:ai'; $keep.auto_open = $true; $keep.buttons = @()
    $c.boards = @($del, $keep)
    $s = $c.ssh[0] | ConvertTo-Json -Depth 10 | ConvertFrom-Json
    $s.name = 'delme-ssh'; $s.host = '192.0.2.99'; $s.port = 22
    $s.username = 'demo'; $s.password = 'CHANGE_ME'
    $c.ssh = @($s)
    $c.ui.split = 1; $c.ui.active_pane = 0; $c.ui.pane_channels = @('delme')
    $c | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $WorkDir 'ssh_bridge_config.json') -Encoding utf8NoBOM

    $app = Start-Process -FilePath (Join-Path $WorkDir 'ssh_bridge3.exe') -WorkingDirectory $WorkDir -PassThru
    if (-not (Wait-Until { try { Invoke-RestMethod "$base/ping" -TimeoutSec 2 | Out-Null; $true } catch { $false } } 15000)) {
        throw '测试实例未在 15 秒内启动桥接'
    }

    $st = State
    Check 'D1 初始通道与终端' `
        ((@($st.channels).name -join ',') -eq 'delme,keepme,delme-ssh' -and $st.terminal.panes[0].channel -eq 'delme') `
        "channels=$(@($st.channels).name -join ',') pane=$($st.terminal.panes[0].channel)"

    DeleteCurrent
    $st = State
    $cfg = Get-Content -LiteralPath (Join-Path $WorkDir 'ssh_bridge_config.json') -Raw | ConvertFrom-Json
    Check 'D2 删除串口通道并落盘' `
        ((@($st.channels).name -join ',') -eq 'keepme,delme-ssh' -and (@($cfg.boards).name -join ',') -eq 'keepme') `
        "channels=$(@($st.channels).name -join ',') configBoards=$(@($cfg.boards).name -join ',')"
    Check 'D3 删除当前通道后切到剩余通道' `
        ($st.terminal.panes[0].channel -eq 'keepme' -and $st.terminal.panes[0].active) `
        "pane=$($st.terminal.panes[0].channel) active=$($st.terminal.panes[0].active)"

    Act 'tree.channels' 'select' 'SSH (1) / delme-ssh  ·  离线' | Out-Null
    Start-Sleep -Milliseconds 400
    $st = State
    Check 'D4 选中 SSH 通道' ($st.terminal.panes[0].channel -eq 'delme-ssh') "pane=$($st.terminal.panes[0].channel)"

    DeleteCurrent
    $st = State
    $cfg = Get-Content -LiteralPath (Join-Path $WorkDir 'ssh_bridge_config.json') -Raw | ConvertFrom-Json
    Check 'D5 删除 SSH 通道并落盘' `
        ((@($st.channels).name -join ',') -eq 'keepme' -and @($cfg.ssh).Count -eq 0) `
        "channels=$(@($st.channels).name -join ',') configSsh=$(@($cfg.ssh).Count)"
    Check 'D6 SSH 删除后回落剩余通道' ($st.terminal.panes[0].channel -eq 'keepme') "pane=$($st.terminal.panes[0].channel)"

    DeleteCurrent
    $st = State
    $cfg = Get-Content -LiteralPath (Join-Path $WorkDir 'ssh_bridge_config.json') -Raw | ConvertFrom-Json
    Check 'D7 删除最后一个通道' `
        ((@($st.channels).Count -eq 0) -and @($cfg.boards).Count -eq 0 -and @($cfg.ssh).Count -eq 0) `
        "channels=$(@($st.channels).Count) config=空"
    # 产品语义：终端工作区始终至少保留一个画框；通道删空后该画框为空，
    # 配置只保存一个空槽位，不伪造通道。
    $panes = @($st.terminal.panes)
    $paneCfg = @($cfg.ui.pane_channels | Where-Object { $_ })
    Check 'D7b 删空后保留一个空画框' `
        ($st.terminal.split -eq 1 -and $panes.Count -eq 1 -and
         [string]$panes[0].channel -eq '' -and $paneCfg.Count -eq 0) `
        "split=$($st.terminal.split) panes=$($panes.Count) pane=$([string]$panes[0].channel) paneCfg=$($paneCfg.Count)"
    Check 'D8 删空后进程仍存活' `
        ((Invoke-RestMethod "$base/ping" -TimeoutSec 3).ok -eq $true) 'ping=ok'
    Check 'D8b 删空后空画框提示新建通道' `
        ([string]$st.terminal.panes[0].empty_hint -eq '没有通道，点“新建”创建一条') `
        "hint=$([string]$st.terminal.panes[0].empty_hint)"

    # 删空不是终点：新建通道后必须能重新绑定到当前空画框。
    Act 'menu.file.newSerial' 'click' | Out-Null
    if (-not (Wait-Until { (FindControl 'dlg.channel.save') } 4000)) { throw '新建通道对话框未出现' }
    Act 'dlg.channel.name' 'set_text' 'reborn' | Out-Null
    Act 'dlg.channel.device' 'set_text' 'loop:ai' | Out-Null
    Act 'dlg.channel.save' 'click' | Out-Null
    if (-not (Wait-Until { -not (State).ui.channel_dialog_open } 10000)) { throw '新建通道对话框未关闭' }
    $st = State
    $cfg = Get-Content -LiteralPath (Join-Path $WorkDir 'ssh_bridge_config.json') -Raw | ConvertFrom-Json
    Check 'D9 删空后新建通道恢复终端' `
        ((@($st.channels).name -join ',') -eq 'reborn' -and
         $st.terminal.panes[0].channel -eq 'reborn' -and $st.terminal.panes[0].active -and
         (@($cfg.boards).name -join ',') -eq 'reborn') `
        "channels=$(@($st.channels).name -join ',') pane=$($st.terminal.panes[0].channel) configBoards=$(@($cfg.boards).name -join ',')"
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
