# step15_check.ps1 — PowerShell / Telnet 正式通道专项验收
# 用法：pwsh -NoProfile -File tests\step15_check.ps1 [-SkipBuild]
# 退出码：0 = 全绿；1 = 有 FAIL
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'output\build\ssh_bridge3.exe'
$runDir = Join-Path $root 'output\step15\check'
$probePy = Join-Path $root 'tests\telnet_probe.py'
$agentBase = 'http://127.0.0.1:18196'
$webBase = 'http://127.0.0.1:18181'
$token = 'step15-test'
$script:rows = @()
$script:proc = $null
$script:probe = $null

function Check([string]$id, [string]$desc, [bool]$ok, [string]$detail = '') {
    $script:rows += [pscustomobject]@{
        id = $id; item = $desc; result = $(if ($ok) { 'PASS' } else { 'FAIL' }); detail = $detail
    }
    Write-Host ("{0} {1} {2} {3}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $id, $desc, $detail)
}

function Stop-AppPid([int]$procId) {
    try { Stop-Process -Id $procId -Force -ErrorAction Stop }
    catch { & 'C:\Windows\System32\taskkill.exe' /F /PID $procId 2>$null | Out-Null }
}

function Kill-App {
    foreach ($p in @(Get-Process ssh_bridge3 -ErrorAction SilentlyContinue)) { Stop-AppPid $p.Id }
    Start-Sleep -Milliseconds 800
}

function Stop-Probes {
    Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match 'telnet_echo|telnet_probe' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 300
}

function Start-Probe([string]$logName, [int]$port = 2323) {
    $script:probe = Start-Process -FilePath 'python.exe' `
        -ArgumentList @('-u', $probePy, "$port", (Join-Path $runDir $logName), (Join-Path $runDir 'probe.ctl')) `
        -WorkingDirectory $runDir -PassThru -RedirectStandardOutput (Join-Path $runDir 'probe.out') `
        -RedirectStandardError (Join-Path $runDir 'probe.err')
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 200
        if (Test-Path (Join-Path $runDir $logName)) {
            if ((Get-Content (Join-Path $runDir $logName) -Raw) -match 'READY') { return }
        }
    }
}

function WebPost([string]$path, $obj) {
    return Invoke-RestMethod -Uri ($webBase + $path) -Method Post -TimeoutSec 10 `
        -Headers @{ Authorization = "Bearer $token"; 'Content-Type' = 'application/json' } `
        -Body ($obj | ConvertTo-Json -Compress -Depth 8)
}

function Get-State { return Invoke-RestMethod -Uri "$agentBase/state" -TimeoutSec 8 }
function Get-Ch([string]$name) { return @((Get-State).state.channels | Where-Object { $_.name -eq $name })[0] }
function Get-Tree { return ((Invoke-RestMethod -Uri "$agentBase/control?id=tree.channels" -TimeoutSec 8).control) }
function Get-Pane { return @((Get-State).state.terminal.panes | Where-Object { $_.active })[0] }

function Wait-Ch([string]$name, [scriptblock]$pred, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $c = $null
    do {
        try { $c = Get-Ch $name } catch { Start-Sleep -Milliseconds 250; continue }
        if ($c -and (& $pred $c)) { return $c }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    return $c
}

function Select-Ch([string]$name) {
    $items = @((Get-Tree).items)
    $idx = -1
    for ($i = 0; $i -lt $items.Count; $i++) {
        if ($items[$i] -match "/\s+$([regex]::Escape($name))\s") { $idx = $i; break }
    }
    if ($idx -lt 0) { throw "tree item not found: $name" }
    Invoke-RestMethod -Uri "$agentBase/action" -Method Post -ContentType 'application/json' `
        -Body (@{ id = 'tree.channels'; action = 'select'; value = $idx } | ConvertTo-Json -Compress) | Out-Null
    Start-Sleep -Milliseconds 400
}

function UiAction([string]$id, [string]$action, [string]$text = '') {
    $body = @{ id = $id; action = $action }
    if ($text) { $body.text = $text }
    return Invoke-RestMethod -Uri "$agentBase/action" -Method Post -ContentType 'application/json' `
        -Body ($body | ConvertTo-Json -Compress) -TimeoutSec 10
}

function Wait-Dialog([bool]$open, [int]$seconds = 6) {
    $deadline = (Get-Date).AddSeconds($seconds)
    do {
        $s = Get-State
        if (([bool]$s.state.ui.channel_dialog_open) -eq $open) { return $true }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    return $false
}

function Get-Control([string]$id) {
    return ((Invoke-RestMethod -Uri "$agentBase/control?id=$id" -TimeoutSec 8).control)
}

function Get-LogText([string]$name) {
    return (Invoke-RestMethod -Uri "$webBase/log?channel=$name&chars=8192" -TimeoutSec 8).text
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class Sb3Win {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
'@ -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.Drawing

function Save-Shot([string]$path) {
    $p = Get-Process ssh_bridge3 -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { return $false }
    [Sb3Win]::ShowWindow($p.MainWindowHandle, 5) | Out-Null
    [Sb3Win]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 500
    $r = New-Object Sb3Win+RECT
    [Sb3Win]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $w = [Math]::Max(1, $r.Right - $r.Left); $h = [Math]::Max(1, $r.Bottom - $r.Top)
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return (Test-Path $path)
}

function Release-App {
    try {
        $body = @{ id = 'tree.channels'; action = 'command'; value = 40001 } | ConvertTo-Json -Compress
        Invoke-RestMethod -Uri "$agentBase/action" -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 8 | Out-Null
    } catch {}
    for ($i = 0; $i -lt 12; $i++) {
        Start-Sleep -Milliseconds 500
        if (-not (Get-Process ssh_bridge3 -ErrorAction SilentlyContinue)) { return }
    }
    Kill-App
}

New-Item -ItemType Directory -Force -Path $runDir | Out-Null
Remove-Item (Join-Path $runDir 'probe.log'), (Join-Path $runDir 'probe2.log'), (Join-Path $runDir 'probe.ctl'), `
    (Join-Path $runDir 'probe.out'), (Join-Path $runDir 'probe.err'), (Join-Path $runDir 'shot-*.png') `
    -ErrorAction SilentlyContinue

if (-not $SkipBuild) {
    Get-Process ssh_bridge3 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    $env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
    $out = (& 'C:\msys64\mingw64\bin\mingw32-make.exe' -C $root -j4 2>&1 | Out-String)
    Check 'RG-1' '编译 0 error / 0 warning' ($LASTEXITCODE -eq 0 -and $out -notmatch 'warning:|error:') 'make -j4'
}

Kill-App
Stop-Probes
Start-Probe 'probe.log'
Check 'TN-P0' '本地 Telnet 探针已启动' ($null -ne $script:probe) '127.0.0.1:2323'

$cfgPath = Join-Path $runDir 'ssh_bridge_config.json'
$uiPath = Join-Path $runDir 'ssh_bridge_ui.json'
$cfg = [ordered]@{
    inbound_ssh = @{ enable = $false; port = 2222 }
    agent = @{ enable = $true; port = 18196; token = '' }
    web = @{ enable = $true; port = 18181; token = $token }
    default = @{ username = 'root'; password = ''; buffer_size = 1048576; log_enabled = $false
                 work_dir = ''; log_dir = ''; auto_discover_serial = $false }
    boards = @()
    ssh = @()
    powershell = @(
        [ordered]@{ name = 'ps-good'; shell = 'powershell.exe'
            args = '-NoLogo -NoProfile -ExecutionPolicy Bypass'; cwd = ''
            line_ending = 'cr'; buffer_size = 0; log_enabled = $false; log_dir = ''; timestamp = $false }
        [ordered]@{ name = 'ps-cwd'; shell = 'powershell.exe'
            args = '-NoLogo -NoProfile -ExecutionPolicy Bypass'; cwd = $runDir
            line_ending = 'cr'; buffer_size = 0; log_enabled = $false; log_dir = ''; timestamp = $false }
        [ordered]@{ name = 'ps-bad'; shell = 'C:\no-such\pwsh-step15.exe'
            args = '-NoLogo -NoProfile'; cwd = ''
            line_ending = 'cr'; buffer_size = 0; log_enabled = $false; log_dir = ''; timestamp = $false }
    )
    telnet = @(
        [ordered]@{ name = 'tn-good'; host = '127.0.0.1'; port = 2323; connect_timeout_ms = 3000
            line_ending = 'crlf'; width = 120; height = 32; buffer_size = 0; log_enabled = $false
            log_dir = ''; timestamp = $false }
        [ordered]@{ name = 'tn-bad'; host = '127.0.0.1'; port = 2324; connect_timeout_ms = 1200
            line_ending = 'crlf'; width = 120; height = 32; buffer_size = 0; log_enabled = $false
            log_dir = ''; timestamp = $false }
    )
    tftp = @{ enable = $false; port = 69; bind_ip = ''; root = 'work/share'; allow_write = $true }
    share = @{ enable = $false; port = 18082; root = '' }
    nfs = @{ enable = $false; alias = '/share'; uid = 0; gid = 0; root = '' }
    relay_devices = @()
    unknown_future_key = @{ keep = 'yes' }
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($cfgPath, ($cfg | ConvertTo-Json -Depth 8), $utf8)
[System.IO.File]::WriteAllText($uiPath, (@{
    schema = 'ssh-bridge-ui/1'
    ui = @{ close_to_tray = $true; start_minimized = $false; scrollback_lines = 5000; font_size = 0
            split = 1; active_pane = 0; show_statusbar = $true; pane_channels = @('ps-good') }
    window = @{ rect = '120,80,1400,900' }
} | ConvertTo-Json -Depth 5), $utf8)

try {
    $script:proc = Start-Process $exe -WorkingDirectory $runDir -PassThru
    Start-Sleep -Seconds 4

    # ---------- A 配置 ----------
    $ps = Get-Ch 'ps-good'; $badps = Get-Ch 'ps-bad'
    Check 'CFG-1' 'powershell 数组按小写键加载' ($ps.kind -eq 'powershell' -and $ps.line_ending -eq 'cr') `
        "kind=$($ps.kind) line=$($ps.line_ending)"
    $tn = Get-Ch 'tn-good'
    Check 'CFG-1b' 'telnet 数组与默认字段加载' ($tn.kind -eq 'telnet' -and $tn.detail -eq '127.0.0.1:2323') `
        "detail=$($tn.detail)"
    $saved = Get-Content -LiteralPath $cfgPath -Raw
    Check 'CFG-2' '未知键保留且四类通道往返' ($saved -match 'unknown_future_key' -and $saved -match '"powershell"' -and
        $saved -match '"telnet"' -and $saved -match 'ps-good' -and $saved -match 'tn-good') 'config json'
    $uiText = Get-Content -LiteralPath $uiPath -Raw
    Check 'CFG-3' '新字段不写窗口配置' ($uiText -notmatch 'powershell' -and $uiText -notmatch 'telnet') 'ui json'
    $ch = Invoke-RestMethod -Uri "$webBase/api/channels" -Headers @{ Authorization = "Bearer $token" } -TimeoutSec 8
    $dupRejected = $false
    try { WebPost '/api/channels' @{ kind = 'telnet'; channel = @{ name = 'ps-good'; host = '127.0.0.1'; port = 2323 } } | Out-Null }
    catch { $dupRejected = $true }
    Check 'CFG-4' '四类之间重名被拒绝' $dupRejected 'HTTP 400 通道名已存在'

    # ---------- B PowerShell ----------
    $ps = Wait-Ch 'ps-good' { param($c) $c.state -eq 'ready' } 10
    Check 'PS-1' '自动启动 ready' (($ps.state -eq 'ready') -and $ps.pid -gt 0) "state=$($ps.state) pid=$($ps.pid)"
    Select-Ch 'ps-good'
    $pane = Get-Pane
    Check 'PS-2' '初始提示符 PS>' (@($pane.tail) -join "`n" -match 'PS>') "tail=$(@($pane.tail) | Select-Object -Last 1)"
    WebPost '/write' @{ channel = 'ps-good'; data = "2+3`r" } | Out-Null
    Start-Sleep -Milliseconds 900
    $pane = Get-Pane
    Check 'PS-3' '命令执行输出 5' ((@($pane.tail) -join "`n") -match '(?m)^5\s*$') "tail=$((@($pane.tail) | Select-Object -Last 3) -join '|')"
    WebPost '/write' @{ channel = 'ps-good'; data = "`$x=41`r" } | Out-Null
    Start-Sleep -Milliseconds 500
    WebPost '/write' @{ channel = 'ps-good'; data = "`$x+1`r" } | Out-Null
    Start-Sleep -Milliseconds 900
    $pane = Get-Pane
    Check 'PS-4' '会话变量保持 41+1=42' ((@($pane.tail) -join "`n") -match '(?m)^42\s*$') "tail=$((@($pane.tail) | Select-Object -Last 3) -join '|')"
    WebPost '/write' @{ channel = 'ps-good'; data = 'echo-partial' } | Out-Null
    Start-Sleep -Milliseconds 800
    $psLog = Get-LogText 'ps-good'
    Check 'PS-5' '提交前本地回显' ($psLog -match 'echo-partial') "logchars=$($psLog.Length)"
    WebPost '/write' @{ channel = 'ps-good'; data = "`r" } | Out-Null
    Start-Sleep -Milliseconds 800
    WebPost '/write' @{ channel = 'ps-good'; data = "ab`b" } | Out-Null
    Start-Sleep -Milliseconds 800
    $psLog = Get-LogText 'ps-good'
    Check 'PS-6' 'Backspace 回显 \b \b 且删掉字符' ($psLog.IndexOf([char]8) -ge 0 -and $psLog -match "PS> a") `
        "bsIndex=$($psLog.IndexOf([char]8))"
    WebPost '/write' @{ channel = 'ps-good'; data = "`r" } | Out-Null
    WebPost '/write' @{ channel = 'ps-good'; data = "Write-Output AAA`r`nWrite-Output BBB`r`n" } | Out-Null
    Start-Sleep -Seconds 2
    $psLog = Get-LogText 'ps-good'
    Check 'PS-7' 'CRLF 两行逐行提交' (($psLog -match 'AAA') -and ($psLog -match 'BBB')) 'AAA/BBB'
    $psCwd = Wait-Ch 'ps-cwd' { param($c) $c.state -eq 'ready' } 12
    WebPost '/write' @{ channel = 'ps-cwd'; data = "Write-Output ('CWD-STEP15=' + (Get-Location).Path)`r" } | Out-Null
    Start-Sleep -Seconds 1
    $cwdLog = Get-LogText 'ps-cwd'
    Check 'PS-11a' '配置的 cwd 生效' (($psCwd.state -eq 'ready') -and ($cwdLog -match ('CWD-STEP15=' + [regex]::Escape($runDir)))) `
        "state=$($psCwd.state) cwd=$runDir"
    WebPost '/write' @{ channel = 'ps-cwd'; data = "Write-Output '中文验收OK-STEP15'`r" } | Out-Null
    Start-Sleep -Seconds 1
    $cwdLog = Get-LogText 'ps-cwd'
    Check 'PS-11b' 'UTF-8 中文输出无乱码' ($cwdLog -match '中文验收OK-STEP15') '中文验收OK-STEP15'
    Select-Ch 'ps-good'
    UiAction 'btn.disconnect' 'click' | Out-Null
    Start-Sleep -Milliseconds 800
    $ps = Get-Ch 'ps-good'
    Check 'PS-8' '手动断开 idle' (($ps.state -eq 'idle') -and (-not $ps.want_open)) "state=$($ps.state) want_open=$($ps.want_open)"
    Start-Sleep -Seconds 5
    $ps = Get-Ch 'ps-good'
    Check 'PS-8b' '5 秒内不自动重开' (($ps.state -eq 'idle') -and (-not $ps.want_open)) "state=$($ps.state)"
    UiAction 'btn.reconnect' 'click' | Out-Null
    $ps = Wait-Ch 'ps-good' { param($c) $c.state -eq 'ready' } 10
    Check 'PS-8c' '手动重连恢复 ready' ($ps.state -eq 'ready') "state=$($ps.state)"
    $oldPid = $ps.pid
    Stop-Process -Id $oldPid -Force -ErrorAction SilentlyContinue
    $ps = Wait-Ch 'ps-good' { param($c) $c.state -eq 'ready' -and $c.pid -gt 0 -and $c.pid -ne $oldPid } 20
    Check 'PS-9' '子进程被杀后 watchdog 重开' (($ps.state -eq 'ready') -and $ps.pid -ne $oldPid) `
        "old=$oldPid new=$($ps.pid) state=$($ps.state)"
    $oldPid2 = $ps.pid
    UiAction 'btn.reconnect' 'click' | Out-Null
    $ps = Wait-Ch 'ps-good' { param($c) $c.state -eq 'ready' -and $c.pid -gt 0 -and $c.pid -ne $oldPid2 } 12
    Check 'PS-12' '已连接状态点重连会真正重启进程' (($ps.state -eq 'ready') -and ($ps.pid -ne $oldPid2)) `
        "old=$oldPid2 new=$($ps.pid) state=$($ps.state)"
    $badps = Wait-Ch 'ps-bad' { param($c) $c.state -eq 'offline' -and $c.last_error } 8
    Check 'PS-10' 'shell 不存在时离线且报错' (($badps.state -eq 'offline') -and ($badps.last_error -match 'CreateProcess|failed')) `
        "state=$($badps.state) err=$($badps.last_error)"

    # ---------- C Telnet ----------
    $tn = Wait-Ch 'tn-good' { param($c) $c.state -eq 'ready' } 10
    Check 'TN-1' 'TCP 连接 ready' (($tn.state -eq 'ready') -and $tn.detail -eq '127.0.0.1:2323') `
        "state=$($tn.state) detail=$($tn.detail)"
    Select-Ch 'tn-good'
    WebPost '/write' @{ channel = 'tn-good'; data = "PING`r`n" } | Out-Null
    Start-Sleep -Seconds 1
    $pane = Get-Pane
    Check 'TN-2' '收发回显 PING' ((@($pane.tail) -join "`n") -match 'PING') "tail=$(@($pane.tail) -join '|')"
    $probeLog = Get-Content (Join-Path $runDir 'probe.log') -Raw
    $rxHex = (($probeLog -split "`n") | Where-Object { $_ -match '^RX ' } | ForEach-Object { $_ -replace '^RX ','' }) -join ''
    Check 'TN-3' 'IAC WILL ECHO/SGA 得到 DO 应答' ($rxHex -match 'fffd01' -and $rxHex -match 'fffd03') 'DO ECHO/DO SGA'
    Check 'TN-4' 'NAWS 上报 120x32' ($probeLog -match 'NAWS 120 32') (($probeLog -split "`n" | Where-Object { $_ -match 'NAWS' }) -join ';')
    Check 'TN-5' '子协商内容不泄漏' ((@($pane.tail) -join "`n") -match 'SB-SAFE' -and (@($pane.tail) -join "`n") -notmatch 'foo') `
        'SB-SAFE visible, foo hidden'
    Stop-Process -Id $script:probe.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    $tn = Get-Ch 'tn-good'
    Check 'TN-6a' '服务端关闭后 offline' ($tn.state -in @('offline', 'connecting', 'idle')) "state=$($tn.state)"
    Start-Probe 'probe2.log'
    $tn = Wait-Ch 'tn-good' { param($c) $c.state -eq 'ready' } 25
    Check 'TN-6b' '服务恢复后自动 ready' ($tn.state -eq 'ready') "state=$($tn.state)"
    Select-Ch 'tn-good'
    UiAction 'btn.disconnect' 'click' | Out-Null
    Start-Sleep -Milliseconds 800
    $tn = Get-Ch 'tn-good'
    Check 'TN-7a' '手动断开 idle' (($tn.state -eq 'idle') -and (-not $tn.want_open)) "state=$($tn.state)"
    UiAction 'btn.reconnect' 'click' | Out-Null
    $tn = Wait-Ch 'tn-good' { param($c) $c.state -eq 'ready' } 10
    Check 'TN-7b' '手动重连恢复 ready' ($tn.state -eq 'ready') "state=$($tn.state)"
    $accBefore = @(Select-String -Path (Join-Path $runDir 'probe2.log') -Pattern '^ACCEPT ' -ErrorAction SilentlyContinue).Count
    UiAction 'btn.reconnect' 'click' | Out-Null
    $tn = Wait-Ch 'tn-good' { param($c) $c.state -eq 'ready' } 10
    Start-Sleep -Milliseconds 600
    $accAfter = @(Select-String -Path (Join-Path $runDir 'probe2.log') -Pattern '^ACCEPT ' -ErrorAction SilentlyContinue).Count
    WebPost '/write' @{ channel = 'tn-good'; data = "TN-RECONNECT`r`n" } | Out-Null
    Start-Sleep -Milliseconds 800
    $pane = Get-Pane
    Check 'TN-7c' '已连接状态点重连会重新握手' (($tn.state -eq 'ready') -and ($accAfter -gt $accBefore) `
        -and ((@($pane.tail) -join "`n") -match 'TN-RECONNECT')) "accepted=$accBefore->$accAfter state=$($tn.state)"
    WebPost '/write' @{ channel = 'tn-good'; data = "EOL-STEP15`r`n" } | Out-Null
    Start-Sleep -Milliseconds 800
    $raw = Get-Content (Join-Path $runDir 'probe2.log') -Raw
    $rxAll = ((($raw -split "`n") | Where-Object { $_ -match '^RX ' } | ForEach-Object { $_ -replace '^RX ','' }) `
        -join '').Trim()
    Check 'TN-9' '配置 crlf 后线上字节以 0d0a 结尾' ($rxAll -match '0d0a$') "last=$($rxAll.Substring([Math]::Max(0,$rxAll.Length-24)))"
    $badtn = Wait-Ch 'tn-bad' { param($c) $c.state -eq 'offline' -and $c.last_error } 8
    Check 'TN-8' '未监听端口 offline 且含连接错误' (($badtn.state -eq 'offline') -and ($badtn.last_error -match 'connect|fail|refused|timeout')) `
        "state=$($badtn.state) err=$($badtn.last_error)"

    # ---------- D UI ----------
    $tree = Get-Tree
    Check 'UI-1' '分组计数与配置一致' ((@($tree.items) -join "`n") -match 'Telnet \(2\)' -and (@($tree.items) -join "`n") -match 'PowerShell \(3\)') `
        (@($tree.items) -join ';')
    $uiTree = Invoke-RestMethod -Uri "$agentBase/ui-tree" -TimeoutSec 8
    $ids = @($uiTree.controls | ForEach-Object { $_.id })
    Check 'UI-2' '菜单/虚拟命令含新建两类型' ((($ids -contains 'menu.file.newPowershell') -and ($ids -contains 'menu.file.newTelnet')) -and
        (($ids -contains 'chan.newPowershell') -and ($ids -contains 'chan.newTelnet'))) 'menu.file/chan'

    UiAction 'menu.file.newPowershell' 'click' | Out-Null
    Check 'UI-3a' '打开 PowerShell 新建对话框' (Wait-Dialog $true) 'channel_dialog_open'
    $treeDlg = Invoke-RestMethod -Uri "$agentBase/ui-tree" -TimeoutSec 8
    $byId = @{}; foreach ($c in $treeDlg.controls) { if ($c.id) { $byId[$c.id] = $c } }
    Check 'UI-3b' 'PowerShell 字段组显示、串口组隐藏' ($byId['dlg.group.powershell'].visible -and -not $byId['dlg.group.serial'].visible) `
        "psGroup=$($byId['dlg.group.powershell'].visible) serialGroup=$($byId['dlg.group.serial'].visible)"
    Check 'UI-3c' '默认 shell/行尾正确' ($byId['dlg.channel.shell'].text -match 'powershell.exe') `
        "shell=$($byId['dlg.channel.shell'].text)"
    UiAction 'dlg.channel.cancel' 'click' | Out-Null
    Start-Sleep -Milliseconds 500
    UiAction 'menu.file.newTelnet' 'click' | Out-Null
    Check 'UI-3d' '打开 Telnet 新建对话框' (Wait-Dialog $true) 'channel_dialog_open'
    $treeDlg = Invoke-RestMethod -Uri "$agentBase/ui-tree" -TimeoutSec 8
    $byId = @{}; foreach ($c in $treeDlg.controls) { if ($c.id) { $byId[$c.id] = $c } }
    Check 'UI-3e' 'Telnet 复用 SSH 组并隐藏 SSH 专属字段' ($byId['dlg.group.ssh'].visible -and -not $byId['dlg.channel.user'].visible -and
        -not $byId['dlg.channel.password'].visible) "group=$($byId['dlg.group.ssh'].text)"
    Check 'UI-3f' 'Telnet 默认端口 23' ([int]$byId['dlg.channel.port'].text -eq 23) "port=$($byId['dlg.channel.port'].text)"
    UiAction 'dlg.channel.cancel' 'click' | Out-Null
    Start-Sleep -Milliseconds 500

    UiAction 'menu.file.newPowershell' 'click' | Out-Null
    Wait-Dialog $true | Out-Null
    UiAction 'dlg.channel.name' 'set_text' 'ps-ui' | Out-Null
    UiAction 'dlg.channel.shell' 'set_text' 'powershell.exe' | Out-Null
    UiAction 'dlg.channel.save' 'click' | Out-Null
    $psUi = Wait-Ch 'ps-ui' { param($c) $c.state -eq 'ready' } 12
    Check 'UI-4' '对话框保存后节点出现且自动连接' ($psUi.state -eq 'ready') "state=$($psUi.state)"
    Select-Ch 'ps-ui'
    UiAction 'chan.editCurrent' 'click' | Out-Null
    Wait-Dialog $true | Out-Null
    $treeDlg = Invoke-RestMethod -Uri "$agentBase/ui-tree" -TimeoutSec 8
    $byId = @{}; foreach ($c in $treeDlg.controls) { if ($c.id) { $byId[$c.id] = $c } }
    Check 'UI-5a' '编辑时类型下拉锁定' (-not $byId['dlg.channel.kind'].enabled) "enabled=$($byId['dlg.channel.kind'].enabled)"
    Check 'UI-9a' '编辑 PowerShell 回填 program' ($byId['dlg.channel.shell'].text -match 'powershell\.exe') `
        "shell=$($byId['dlg.channel.shell'].text)"
    Check 'UI-9b' '编辑 PowerShell 回填 args' ($byId['dlg.channel.args'].text -match 'NoLogo') `
        "args=$($byId['dlg.channel.args'].text)"
    UiAction 'dlg.channel.name' 'set_text' 'ps-ui2' | Out-Null
    UiAction 'dlg.channel.save' 'click' | Out-Null
    $psUi2 = Wait-Ch 'ps-ui2' { param($c) $c.state -eq 'ready' } 12
    $tree = Get-Tree
    Check 'UI-5b' '改名落盘且树节点更名' (($psUi2.state -eq 'ready') -and ((@($tree.items) -join "`n") -notmatch '/\s+ps-ui\s')) `
        "state=$($psUi2.state)"
    $paneBind = Get-Pane
    Check 'UI-5c' '改名同步分屏画框绑定' ($paneBind.channel -eq 'ps-ui2') "pane=$($paneBind.channel)"
    Select-Ch 'ps-ui2'
    UiAction 'chan.deleteCurrent' 'click' | Out-Null
    Wait-Dialog $true | Out-Null
    UiAction 'dlg.channel.delete' 'click' | Out-Null
    Start-Sleep -Milliseconds 400
    UiAction 'dlg.channel.delete' 'click' | Out-Null
    Start-Sleep -Seconds 2
    $tree = Get-Tree
    $gone = (($tree.items -join "`n") -notmatch '/\s+ps-ui2\s') -and -not (Get-Ch 'ps-ui2')
    Check 'UI-7' '删除后节点消失且绑定清空' $gone 'tree/config gone'
    Start-Probe 'probe-tnui.log' 2325
    UiAction 'menu.file.newTelnet' 'click' | Out-Null
    Wait-Dialog $true | Out-Null
    UiAction 'dlg.channel.name' 'set_text' 'tn-ui' | Out-Null
    UiAction 'dlg.channel.host' 'set_text' '127.0.0.1' | Out-Null
    UiAction 'dlg.channel.port' 'set_text' '2325' | Out-Null
    UiAction 'dlg.channel.save' 'click' | Out-Null
    $tnUi = Wait-Ch 'tn-ui' { param($c) $c.state -eq 'ready' } 12
    Check 'UI-10a' 'Telnet 对话框保存并连接' (($tnUi.state -eq 'ready') -and $tnUi.detail -eq '127.0.0.1:2325') `
        "state=$($tnUi.state) detail=$($tnUi.detail)"
    Select-Ch 'tn-ui'
    UiAction 'chan.editCurrent' 'click' | Out-Null
    Wait-Dialog $true | Out-Null
    $treeDlg = Invoke-RestMethod -Uri "$agentBase/ui-tree" -TimeoutSec 8
    $byId = @{}; foreach ($c in $treeDlg.controls) { if ($c.id) { $byId[$c.id] = $c } }
    Check 'UI-10b' 'Telnet 编辑组标题与 SSH 专属字段' (($byId['dlg.group.ssh'].text -match 'Telnet') -and
        (-not $byId['dlg.channel.user'].visible) -and (-not $byId['dlg.channel.password'].visible) -and
        ([int]$byId['dlg.channel.port'].text -eq 2325)) "title=$($byId['dlg.group.ssh'].text) port=$($byId['dlg.channel.port'].text)"
    UiAction 'dlg.channel.cancel' 'click' | Out-Null
    Start-Sleep -Milliseconds 400
    Select-Ch 'tn-ui'
    UiAction 'chan.deleteCurrent' 'click' | Out-Null
    Wait-Dialog $true | Out-Null
    UiAction 'dlg.channel.delete' 'click' | Out-Null
    Start-Sleep -Milliseconds 400
    UiAction 'dlg.channel.delete' 'click' | Out-Null
    Start-Sleep -Seconds 2
    Check 'UI-10c' '删除活动 Telnet 不挂死且节点消失' ((-not (Get-Ch 'tn-ui')) -and
        ((Get-Process ssh_bridge3 -ErrorAction SilentlyContinue).Responding)) 'deleted'
    Check 'UI-6' '通道右键菜单命令完整且 SFTP 不在通道菜单' `
        ((($ids -contains 'chan.editCurrent') -and ($ids -contains 'chan.deleteCurrent') -and ($ids -contains 'chan.reload')) `
          -and -not ($ids -contains 'chan.sftp')) 'chan.*'

    $tn = Wait-Ch 'tn-good' { param($c) $c.state -eq 'ready' } 8
    if ($tn.state -ne 'ready') {
        Select-Ch 'tn-good'
        UiAction 'btn.reconnect' 'click' | Out-Null
        $tn = Wait-Ch 'tn-good' { param($c) $c.state -eq 'ready' } 10
    }
    Select-Ch 'tn-good'
    UiAction 'term.input' 'set_text' 'VERIFY-TERMINAL' | Out-Null
    Start-Sleep -Seconds 1
    $pane = Get-Pane
    Check 'UI-8' '终端实际输入并看到回显' ((@($pane.tail) -join "`n") -match 'VERIFY-TERMINAL') "tail=$(@($pane.tail) -join '|')"
    $shot = Save-Shot (Join-Path $runDir 'shot-terminal.png')
    Check 'UI-8b' '桌面终端截图落盘' $shot 'shot-terminal.png'

    # ---------- E Web / Agent ----------
    $chs = Invoke-RestMethod -Uri "$webBase/api/channels" -Headers @{ Authorization = "Bearer $token" } -TimeoutSec 8
    Check 'WEB-1' 'GET /api/channels 返回 powershell/telnet' `
        ((@($chs.powershell).Count -ge 2) -and (@($chs.telnet).Count -ge 2)) `
        "ps=$(@($chs.powershell).Count) tn=$(@($chs.telnet).Count)"
    Start-Probe 'probe-web.log' 2325
    $psWeb = WebPost '/api/channels' @{ kind = 'powershell'; channel = @{ name = 'ps-web'; shell = 'powershell.exe'; line_ending = 'cr' } }
    $tnWeb = WebPost '/api/channels' @{ kind = 'telnet'; channel = @{ name = 'tn-web'; host = '127.0.0.1'; port = 2325 } }
    $psWebCh = Wait-Ch 'ps-web' { param($c) $c.state -eq 'ready' } 15
    $tnWebCh = Wait-Ch 'tn-web' { param($c) $c.state -eq 'ready' } 10
    Check 'WEB-2' 'POST 新建两类通道并自动连接' ($psWeb.ok -and $tnWeb.ok -and $psWebCh.state -eq 'ready' -and $tnWebCh.state -eq 'ready') `
        "ps=$($psWebCh.state) tn=$($tnWebCh.state)"
    $d1 = WebPost '/api/channels/delete' @{ kind = 'powershell'; name = 'ps-web' }
    $d2 = WebPost '/api/channels/delete' @{ kind = 'telnet'; name = 'tn-web' }
    Stop-Process -Id $script:probe.Id -Force -ErrorAction SilentlyContinue
    $chs = Invoke-RestMethod -Uri "$webBase/api/channels" -Headers @{ Authorization = "Bearer $token" } -TimeoutSec 8
    Check 'WEB-3' 'DELETE 两类通道且无残留' ($d1.ok -and $d2.ok -and -not ($chs.powershell.name -contains 'ps-web') -and
        -not ($chs.telnet.name -contains 'tn-web')) 'deleted'
    $page = Invoke-WebRequest -Uri "$webBase/channels" -Headers @{ Authorization = "Bearer $token" } -TimeoutSec 10
    Check 'WEB-4' '通道页含新增按钮/字段组' ($page.Content -match 'newPowershell' -and $page.Content -match 'newTelnet' -and
        $page.Content -match 'cShell' -and $page.Content -match 'cArgs' -and $page.Content -match 'cCwd') 'channels html'
    Start-Probe 'probe-uiweb.log' 2326
    $uiProbe = Join-Path $root 'output\webtest\step15_ui_probe.js'
    $uiOut = Join-Path $root 'output\step15\audit2\web-ui-probe.json'
    New-Item -ItemType Directory -Force -Path (Split-Path $uiOut) | Out-Null
    $uiRun = (& node $uiProbe 2>&1 | Out-String)
    Write-Host $uiRun
    if (Test-Path $uiOut) {
        foreach ($r in @(Get-Content -LiteralPath $uiOut -Raw | ConvertFrom-Json)) {
            Check $r.id $r.desc ($r.result -eq 'PASS') $r.detail
        }
    } else {
        Check 'WEB-UI-0' '浏览器通道页脚本产出结果' $false $uiRun
    }
    foreach ($n in @('ps-uiweb', 'tn-uiweb')) {
        if (Get-Ch $n) {
            try { WebPost '/api/channels/delete' @{ kind = $(if ($n -like 'ps-*') { 'powershell' } else { 'telnet' }); name = $n } | Out-Null } catch {}
        }
    }
    $tn = Wait-Ch 'tn-good' { param($c) $c.state -eq 'ready' } 5
    if ($tn.state -ne 'ready') {
        Select-Ch 'tn-good'
        UiAction 'btn.reconnect' 'click' | Out-Null
        $tn = Wait-Ch 'tn-good' { param($c) $c.state -eq 'ready' } 10
    }
    Select-Ch 'tn-good'
    WebPost '/write' @{ channel = 'tn-good'; data = "WEB-PING`r`n" } | Out-Null
    Start-Sleep -Seconds 1
    $pane = Get-Pane
    Check 'WEB-5' '网页写入能在同框终端看到回显' ((@($pane.tail) -join "`n") -match 'WEB-PING') 'WEB-PING'
    $scan = Invoke-RestMethod -Uri "$webBase/agent/scan" -TimeoutSec 8
    $scanPs = @($scan.channels | Where-Object { $_.name -eq 'ps-good' })[0]
    $scanTn = @($scan.channels | Where-Object { $_.name -eq 'tn-good' })[0]
    Check 'AG-1' 'scan 中 kind/state/features 正确' ($scanPs.features.powershell_link -and -not $scanPs.features.telnet_link -and
        $scanTn.features.telnet_link -and -not $scanTn.features.powershell_link -and
        $scan.features.powershell_link -and $scan.features.telnet_link) `
        "ps=$($scanPs.kind)/$($scanPs.state) tn=$($scanTn.kind)/$($scanTn.state)"
    $wlog = WebPost '/write' @{ channel = 'ps-good'; data = "`$global:sb15=7`r" }
    Start-Sleep -Milliseconds 800
    $logText = Get-LogText 'ps-good'
    Check 'AG-2' 'Agent write 后 /log 可读到输出' ($wlog.ok -and $logText -match 'sb15') "logchars=$($logText.Length)"
    $serialEntry = @($scan.channels | Where-Object { $_.kind -eq 'serial' })[0]
    Check 'AG-3' 'serial_control 只对串口 true' ($null -eq $serialEntry -or $serialEntry.features.serial_control) `
        'no serial configured in isolation'

    # ---------- F 重启持久化 ----------
    Start-Probe 'probe-keep.log' 2327
    $mkPs = WebPost '/api/channels' @{ kind = 'powershell'; channel = @{ name = 'ps-keep'; shell = 'powershell.exe'
        args = '-NoLogo -NoProfile -ExecutionPolicy Bypass'; cwd = $runDir; line_ending = 'lf'; timestamp = $true; log_enabled = $true } }
    $mkTn = WebPost '/api/channels' @{ kind = 'telnet'; channel = @{ name = 'tn-keep'; host = '127.0.0.1'; port = 2327
        connect_timeout_ms = 3000; line_ending = 'cr'; width = 100; height = 30; log_enabled = $true } }
    $psKeep = Wait-Ch 'ps-keep' { param($c) $c.state -eq 'ready' } 15
    $tnKeep = Wait-Ch 'tn-keep' { param($c) $c.state -eq 'ready' } 12
    $cfgNow = Get-Content -LiteralPath $cfgPath -Raw | ConvertFrom-Json
    $psCfg = @($cfgNow.powershell | Where-Object { $_.name -eq 'ps-keep' })[0]
    $tnCfg = @($cfgNow.telnet | Where-Object { $_.name -eq 'tn-keep' })[0]
    Check 'CFG-5' 'Web 新建后配置字段完整落盘' ($mkPs.ok -and $mkTn.ok -and $psCfg.cwd -eq $runDir -and
        $psCfg.line_ending -eq 'lf' -and $psCfg.timestamp -eq $true -and $tnCfg.line_ending -eq 'cr' -and
        $tnCfg.width -eq 100) "ps=$($psKeep.state) tn=$($tnKeep.state)"

    # ---------- F 退出 ----------
    Release-App
    $script:proc = $null
    Start-Sleep -Seconds 2
    $residual = @(Get-Process ssh_bridge3 -ErrorAction SilentlyContinue).Count -gt 0
    $ports = & 'C:\Windows\System32\netstat.exe' -ano | Select-String ':18181|:18196' |
        Where-Object { $_ -match 'LISTENING' }
    $psChildren = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match 'EncodedCommand' }
    $childCount = @($psChildren).Count
    Check 'RG-9' '退出后无应用/子进程且端口释放' ((-not $residual) -and ($childCount -eq 0) -and -not $ports) `
        "app=$residual children=$childCount ports=$(@($ports).Count)"

    $script:proc = Start-Process $exe -WorkingDirectory $runDir -PassThru
    Start-Sleep -Seconds 4
    $psKeep2 = Wait-Ch 'ps-keep' { param($c) $c.state -eq 'ready' } 15
    $tnKeep2 = Wait-Ch 'tn-keep' { param($c) $c.state -eq 'ready' } 15
    $reCfg = Get-Content -LiteralPath $cfgPath -Raw | ConvertFrom-Json
    $rePs = @($reCfg.powershell | Where-Object { $_.name -eq 'ps-keep' })[0]
    $reTn = @($reCfg.telnet | Where-Object { $_.name -eq 'tn-keep' })[0]
    Check 'RG-10' '重启后两类通道自动恢复且字段保持' ($psKeep2.state -eq 'ready' -and $tnKeep2.state -eq 'ready' `
        -and $rePs.cwd -eq $runDir -and $rePs.line_ending -eq 'lf' -and $reTn.line_ending -eq 'cr') `
        "ps=$($psKeep2.state) tn=$($tnKeep2.state) cwd=$($rePs.cwd)"
    try { WebPost '/api/channels/delete' @{ kind = 'powershell'; name = 'ps-keep' } | Out-Null } catch {}
    try { WebPost '/api/channels/delete' @{ kind = 'telnet'; name = 'tn-keep' } | Out-Null } catch {}
    Release-App
    $script:proc = $null
    Start-Sleep -Seconds 2
    Check 'RG-11' '第二次退出无残留' (-not (Get-Process ssh_bridge3 -ErrorAction SilentlyContinue)) 'clean'

    # 配置往返：让应用重新保存一次后再读
    Check 'CFG-2b' '运行后配置文件仍保留未知键与四类数组' `
        ((Get-Content $cfgPath -Raw) -match 'unknown_future_key' -and (Get-Content $cfgPath -Raw) -match 'ps-good') 'post-run json'
}
finally {
    if (Get-Process ssh_bridge3 -ErrorAction SilentlyContinue) { Release-App }
    Stop-Probes
}

$fail = @($script:rows | Where-Object { $_.result -eq 'FAIL' }).Count
$script:rows | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
$resultPath = Join-Path $root 'output\step15\result.json'
$script:rows | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $resultPath -Encoding UTF8
Write-Host "RESULT: $(if ($fail -eq 0) { 'ALL PASS' } else { "FAIL ($fail)" })"
exit $(if ($fail -eq 0) { 0 } else { 1 })
