# step14_check.ps1 — 串口自愈 / 通道右键 / 配置拆分专项验收
# 用法：pwsh -NoProfile -File tests\step14_check.ps1 [-SkipBuild]
# 退出码：0 = 全绿；1 = 有 FAIL
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'output\build\ssh_bridge3.exe'
$runDir = Join-Path $root 'output\step14\check'
$base = 'http://127.0.0.1:18096'
$script:rows = @()
$script:hold = $null
$script:proc = $null

function Check([string]$id, [string]$desc, [bool]$ok, [string]$detail = '') {
    $script:rows += [pscustomobject]@{
        id = $id; item = $desc; result = $(if ($ok) { 'PASS' } else { 'FAIL' }); detail = $detail
    }
    Write-Host ("{0} {1} {2} {3}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $id, $desc, $detail)
    return $ok
}

function Kill-App {
    Get-Process ssh_bridge3 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}

function Get-Channel {
    $s = Invoke-RestMethod -Uri "$base/state" -TimeoutSec 6
    return @($s.state.channels | Where-Object { $_.name -eq 'com4test' })[0]
}

function Invoke-Command([int]$id) {
    $body = @{ id = 'tree.channels'; action = 'command'; value = $id } | ConvertTo-Json -Compress
    Invoke-RestMethod -Uri "$base/action" -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 8 |
        Out-Null
}

function Wait-Channel([scriptblock]$pred, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    do {
        try { $c = Get-Channel } catch { Start-Sleep -Milliseconds 250; continue }
        if (& $pred $c) { return $c }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    return $c
}

function Release-App {
    try { Invoke-Command 40001 } catch {}
    for ($i = 0; $i -lt 10; $i++) {
        Start-Sleep -Milliseconds 500
        if (-not (Get-Process ssh_bridge3 -ErrorAction SilentlyContinue)) { return }
    }
    Get-Process ssh_bridge3 -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

New-Item -ItemType Directory -Force -Path $runDir | Out-Null
if (-not $SkipBuild) {
    $env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
    $out = (& 'C:\msys64\mingw64\bin\mingw32-make.exe' -C $root -j4 2>&1 | Out-String)
    Check 'F01' '编译 0 error / 0 warning' ($LASTEXITCODE -eq 0 -and $out -notmatch 'warning:|error:') 'make -j4'
}

Kill-App
$cfg = Join-Path $runDir 'ssh_bridge_config.json'
$ui = Join-Path $runDir 'ssh_bridge_ui.json'
$configText = @'
{
  "inbound_ssh": { "enable": false, "port": 2222 },
  "agent": { "enable": true, "port": 18096, "token": "" },
  "web": { "enable": false, "port": 18081, "token": "" },
  "default": {
    "username": "root", "password": "", "buffer_size": 1048576,
    "log_enabled": false, "work_dir": "", "log_dir": "", "auto_discover_serial": false
  },
  "boards": [
    {
      "name": "com4test", "serial_device": "COM4", "baudrate": 9600,
      "databits": 8, "parity": "N", "stopbits_x10": 10, "flowcontrol": "none",
      "dtr": false, "rts": false, "line_ending": "lf", "encoding": "utf-8",
      "auto_open": true, "timestamp": false, "buffer_size": 0,
      "log_enabled": false, "log_dir": "", "password": "", "username": "root", "buttons": []
    }
  ],
  "ssh": [],
  "tftp": { "enable": false, "port": 69, "bind_ip": "", "root": "work/share", "allow_write": true },
  "share": { "enable": false, "port": 18082, "root": "" },
  "nfs": { "enable": false, "alias": "/share", "uid": 0, "gid": 0, "root": "" },
  "relay_devices": []
}
'@
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($cfg, $configText, $utf8)
[System.IO.File]::WriteAllText($ui, @'
{
  "schema": "ssh-bridge-ui/1",
  "ui": { "close_to_tray": true, "start_minimized": false, "scrollback_lines": 5000,
          "font_size": 0, "split": 1, "active_pane": 0, "show_statusbar": true,
          "pane_channels": ["com4test"] },
  "window": { "rect": "120,80,1400,900" }
}
'@, $utf8)

try {
    $script:hold = New-Object System.IO.Ports.SerialPort 'COM4', 9600, 'None', 8, 'One'
    $script:hold.Open()
    Check 'D00' '测试占用 COM4 成功' $true 'SerialPort open'

    $script:proc = Start-Process $exe -WorkingDirectory $runDir -PassThru
    Start-Sleep -Seconds 4
    $c = Wait-Channel { param($x) $x.state -eq 'offline' -and $x.retry_attempt -ge 1 } 8
    Check 'D01' '端口占用时状态 offline 且有错误' ($c.state -eq 'offline' -and $c.last_error -match 'failed|busy') `
        "state=$($c.state) error=$($c.last_error)"
    Check 'D02' '失败后进入退避重试' ($c.retry_attempt -ge 1) "attempt=$($c.retry_attempt)"

    Invoke-Command 40010
    Start-Sleep -Seconds 4
    $c = Get-Channel
    $visible = (Invoke-RestMethod -Uri "$base/state" -TimeoutSec 6).state.visible
    Check 'D02b' '托盘隐藏时重试继续' ((-not $visible) -and $c.retry_attempt -ge 2) `
        "visible=$visible attempt=$($c.retry_attempt)"

    $script:hold.Close()
    $script:hold = $null
    $c = Wait-Channel { param($x) $x.state -eq 'ready' } 20
    Check 'D03' '释放端口后自动恢复 ready' ($c.state -eq 'ready') "state=$($c.state)"

    Invoke-Command 40020
    Start-Sleep -Seconds 1
    $visible = (Invoke-RestMethod -Uri "$base/state" -TimeoutSec 6).state.visible
    Check 'D03b' '托盘显示恢复窗口' $visible "visible=$visible"

    Invoke-Command 40047
    Start-Sleep -Seconds 1
    $c = Get-Channel
    Check 'D04' '手动断开后 want_open=false' (($c.state -eq 'idle') -and (-not $c.want_open)) `
        "state=$($c.state) want_open=$($c.want_open)"
    Start-Sleep -Seconds 10
    $c = Get-Channel
    Check 'D04b' '两个重试周期后仍未自动重开' (($c.state -eq 'idle') -and (-not $c.want_open)) `
        "state=$($c.state) want_open=$($c.want_open)"

    Invoke-Command 40012
    Start-Sleep -Seconds 1
    $c = Get-Channel
    Check 'D05' '手动重连立即恢复 ready' (($c.state -eq 'ready') -and $c.want_open) `
        "state=$($c.state) want_open=$($c.want_open)"

    $offText = $configText -replace '"auto_open": true', '"auto_open": false'
    [System.IO.File]::WriteAllText($cfg, $offText, $utf8)
    Invoke-Command 40044
    Start-Sleep -Seconds 2
    $c = Get-Channel
    Check 'D08' '重载 auto_open=false 后 want_open=false' (-not $c.want_open) `
        "state=$($c.state) want_open=$($c.want_open)"
    [System.IO.File]::WriteAllText($cfg, $configText, $utf8)
    Invoke-Command 40044
    Start-Sleep -Seconds 2
    $c = Get-Channel
    Check 'D08b' '重载 auto_open=true 后恢复守护' ($c.want_open -and $c.state -eq 'ready') `
        "state=$($c.state) want_open=$($c.want_open)"

    $src = Get-Content -LiteralPath (Join-Path $root 'src\ui\shell\shell.cpp') -Raw
    Check 'E01' '通道树注册 NM_RCLICK' ($src -match 'NM_RCLICK') 'source'
    Check 'E02' '命中节点先选中再弹菜单' ($src -match 'TVM_HITTEST' -and $src -match 'show_tree_context_menu') 'source'
    Check 'E03' '通道菜单含打开/重连' ($src -match 'online \? L"重连" : L"打开"') 'source'
    Check 'E04' '通道菜单含断开/编辑/删除' ($src -match 'kIdmDisconnect' -and $src -match 'kIdmChanEdit') 'source'
    Check 'E05' '分组菜单含新建串口/SSH/重载' ($src -match '新建串口通道' -and $src -match '重载配置') 'source'
    Check 'E06' '右键菜单实测截图存在' (Test-Path (Join-Path $root 'output\step14\tree-channel-menu2.png')) 'output/step14'

    Release-App
    $script:proc = $null
    Start-Sleep -Seconds 2
    Check 'D07' '退出后无残留进程' (-not (Get-Process ssh_bridge3 -ErrorAction SilentlyContinue)) 'none'
    $probe = New-Object System.IO.Ports.SerialPort 'COM4', 9600, 'None', 8, 'One'
    $probe.Open()
    $probe.Close()
    Check 'D07b' '退出后 COM4 句柄已释放' $true 'SerialPort reopen'
}
finally {
    if ($script:hold) { try { $script:hold.Close() } catch {} }
    if (Get-Process ssh_bridge3 -ErrorAction SilentlyContinue) { Release-App }
}

$fail = @($script:rows | Where-Object { $_.result -eq 'FAIL' }).Count
$script:rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
$resultPath = Join-Path $root 'output\step14\result.json'
$script:rows | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $resultPath -Encoding UTF8
Write-Host "RESULT: $(if ($fail -eq 0) { 'ALL PASS' } else { "FAIL ($fail)" })"
exit $(if ($fail -eq 0) { 0 } else { 1 })
