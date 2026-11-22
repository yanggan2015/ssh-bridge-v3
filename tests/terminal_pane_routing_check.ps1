# terminal_pane_routing_check.ps1 — 分屏按键路由验收
# 用法： pwsh -NoProfile -File tests\terminal_pane_routing_check.ps1 [-SkipStart]
# 前提： output\terminal-test 含 loop-chars 与 ssh-demo1；18096 未被占用
# 断言： 分屏后 term.send 与 WM_KEYDOWN 只增加活动窗格通道的 tx，另一通道不变

param([switch]$SkipStart)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'output\build\ssh_bridge3.exe'
$runDir = Join-Path $root 'output\terminal-test'
$port = 18096
$base = "http://127.0.0.1:$port"
$rows = @()

function Check([string]$id, [string]$desc, [bool]$ok, [string]$detail = '') {
    $script:rows += [pscustomobject]@{ 编号 = $id; 项目 = $desc; 结果 = $(if ($ok) { 'PASS' } else { 'FAIL' }); 说明 = $detail }
    return $ok
}

function Send-Action([string]$id, [string]$action, [string]$text = $null, [int]$value = -1) {
    $body = @{ id = $id; action = $action }
    if ($null -ne $text) { $body.text = $text }
    if ($value -ge 0) { $body.value = $value }
    Invoke-RestMethod "$base/action" -Method Post -ContentType 'application/json' -Body ($body | ConvertTo-Json) | Out-Null
}

function Get-Tx([string]$name) {
    ((Invoke-RestMethod "$base/state").state.channels | Where-Object name -eq $name).tx_bytes
}

Add-Type -Namespace PaneKey -Name Native -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
'@

function Find-TermView([IntPtr]$parent) {
    $found = [IntPtr]::Zero
    $cb = [PaneKey.Native+EnumProc]{
        param($h, $l)
        $sb = New-Object System.Text.StringBuilder 128
        [PaneKey.Native]::GetClassName($h, $sb, 128) | Out-Null
        if ($sb.ToString() -eq 'SB3_TermView' -and $script:found -eq [IntPtr]::Zero) { $script:found = $h }
        return $true
    }
    $script:found = [IntPtr]::Zero
    [PaneKey.Native]::EnumChildWindows($parent, $cb, [IntPtr]::Zero) | Out-Null
    return $script:found
}

$proc = $null
$started = $false
try {
    if (-not $SkipStart) {
        $existing = @(Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue)
        if ($existing.Count -gt 0) { throw "端口 $port 已被占用" }
        $proc = Start-Process -FilePath $exe -WorkingDirectory $runDir -PassThru
        $started = $true
    } else {
        $proc = Get-Process -Name ssh_bridge3 -ErrorAction SilentlyContinue | Select-Object -First 1
    }

    $ready = $false
    for ($i = 0; $i -lt 60; $i++) {
        try { Invoke-RestMethod "$base/ping" | Out-Null; $ready = $true; break } catch { Start-Sleep -Milliseconds 300 }
    }
    if (-not $ready) { throw 'UI 桥未就绪' }

    $sshReady = $false
    for ($i = 0; $i -lt 60; $i++) {
        $ch = (Invoke-RestMethod "$base/state").state.channels | Where-Object name -eq 'ssh-demo1'
        if ($ch -and $ch.state -eq 'ready') { $sshReady = $true; break }
        Start-Sleep -Milliseconds 300
    }
    if (-not $sshReady) { throw 'ssh-demo1 未 ready' }

    Send-Action 'tree.channels' 'select' 'loop-chars'
    Send-Action 'term.split2' 'click'
    Start-Sleep -Milliseconds 400
    Send-Action 'term.pane' 'set_value' $null 1
    Send-Action 'tree.channels' 'select' 'ssh-demo1'
    Start-Sleep -Milliseconds 1200

    $term = (Invoke-RestMethod "$base/state").state.terminal
    $pane0 = @($term.panes)[0].channel
    $pane1 = @($term.panes)[1].channel
    Check 'PANE-split' '左右两窗格与通道分配' ($term.split -eq 2 -and $pane0 -eq 'loop-chars' -and $pane1 -eq 'ssh-demo1') "split=$($term.split) pane0=$pane0 pane1=$pane1"

    Send-Action 'term.pane' 'set_value' $null 1
    $l0 = Get-Tx 'loop-chars'; $s0 = Get-Tx 'ssh-demo1'
    Send-Action 'term.send' 'set_text' ([char]3).ToString()
    Start-Sleep -Milliseconds 500
    $dl = (Get-Tx 'loop-chars') - $l0; $ds = (Get-Tx 'ssh-demo1') - $s0
    Check 'PANE-send-ssh' 'term.send Ctrl-C 只到活动 SSH 窗格' ($ds -eq 1 -and $dl -eq 0) "ssh+=$ds loop+=$dl"

    Send-Action 'term.pane' 'set_value' $null 0
    $l0 = Get-Tx 'loop-chars'; $s0 = Get-Tx 'ssh-demo1'
    Send-Action 'term.send' 'set_text' ([char]3).ToString()
    Start-Sleep -Milliseconds 500
    $dl = (Get-Tx 'loop-chars') - $l0; $ds = (Get-Tx 'ssh-demo1') - $s0
    Check 'PANE-send-loop' 'term.send Ctrl-C 只到活动回环窗格' ($dl -eq 1 -and $ds -eq 0) "loop+=$dl ssh+=$ds"

    $p = if ($proc) { $proc } else { Get-Process -Name ssh_bridge3 | Select-Object -First 1 }
    $tv = Find-TermView ([IntPtr]$p.MainWindowHandle)
    Check 'PANE-view' '定位 SB3_TermView' ($tv -ne [IntPtr]::Zero) "handle=$tv"

    if ($tv -ne [IntPtr]::Zero) {
        [PaneKey.Native]::PostMessage($tv, 0x0007, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 100

        Send-Action 'term.pane' 'set_value' $null 0
        $l0 = Get-Tx 'loop-chars'; $s0 = Get-Tx 'ssh-demo1'
        [PaneKey.Native]::PostMessage($tv, 0x0100, [IntPtr]8, [IntPtr]1) | Out-Null
        [PaneKey.Native]::PostMessage($tv, 0x0101, [IntPtr]8, [IntPtr]0xC0000001) | Out-Null
        Start-Sleep -Milliseconds 500
        $dl = (Get-Tx 'loop-chars') - $l0; $ds = (Get-Tx 'ssh-demo1') - $s0
        Check 'PANE-phy-loop' '物理 Backspace 只到活动回环窗格' ($dl -eq 1 -and $ds -eq 0) "loop+=$dl ssh+=$ds"

        Send-Action 'term.pane' 'set_value' $null 1
        $l0 = Get-Tx 'loop-chars'; $s0 = Get-Tx 'ssh-demo1'
        [PaneKey.Native]::PostMessage($tv, 0x0100, [IntPtr]38, [IntPtr]0x01480001) | Out-Null
        [PaneKey.Native]::PostMessage($tv, 0x0101, [IntPtr]38, [IntPtr]0xC1480001) | Out-Null
        Start-Sleep -Milliseconds 500
        $dl = (Get-Tx 'loop-chars') - $l0; $ds = (Get-Tx 'ssh-demo1') - $s0
        Check 'PANE-phy-ssh' '物理 Up 只到活动 SSH 窗格' ($ds -eq 3 -and $dl -eq 0) "ssh+=$ds loop+=$dl"
    }
} catch {
    Check 'PANE-error' '执行异常' $false $_.Exception.Message
} finally {
    if ($started) {
        Get-Process -Name ssh_bridge3 -ErrorAction SilentlyContinue | Stop-Process -Force
    }
}

$rows | Format-Table -AutoSize
$failed = @($rows | Where-Object 结果 -eq 'FAIL')
if ($failed.Count -eq 0) { 'RESULT: ALL PASS'; exit 0 }
"RESULT: FAIL $($failed.编号 -join ',')"; exit 1
