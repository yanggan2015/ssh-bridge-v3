# terminal_physical_key_check.ps1 — 物理键盘消息路径验收
# 用法： pwsh -NoProfile -File tests\terminal_physical_key_check.ps1 [-SkipStart]
# 前提： output\terminal-test 配置含 loop-chars；18096 未被占用
# 断言： 向 SB3_TermView 投递 WM_KEYDOWN，回环计数 tx+ / rx+ 等于期望字节数
# 说明： 锁屏时无法用 SendInput 合成全局键盘，本脚本直接验证窗口消息分发这一段；
#        修饰键映射由 --selftest term 的 T4U-12 覆盖。

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

function Get-Channel([string]$name) {
    (Invoke-RestMethod "$base/state").state.channels | Where-Object name -eq $name
}

function Send-Action([string]$id, [string]$action, [string]$text = $null) {
    $body = @{ id = $id; action = $action }
    if ($null -ne $text) { $body.text = $text }
    Invoke-RestMethod "$base/action" -Method Post -ContentType 'application/json' -Body ($body | ConvertTo-Json) | Out-Null
}

Add-Type -Namespace PhysKey -Name Native -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
'@

function Find-TermView([IntPtr]$parent) {
    $found = [IntPtr]::Zero
    $cb = [PhysKey.Native+EnumProc]{
        param($h, $l)
        $sb = New-Object System.Text.StringBuilder 128
        [PhysKey.Native]::GetClassName($h, $sb, 128) | Out-Null
        if ($sb.ToString() -eq 'SB3_TermView' -and $script:found -eq [IntPtr]::Zero) { $script:found = $h }
        return $true
    }
    $script:found = [IntPtr]::Zero
    [PhysKey.Native]::EnumChildWindows($parent, $cb, [IntPtr]::Zero) | Out-Null
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

    Send-Action 'tree.channels' 'select' 'loop-chars'
    Start-Sleep -Milliseconds 400

    $p = if ($proc) { $proc } else { Get-Process -Name ssh_bridge3 | Select-Object -First 1 }
    $term = Find-TermView ([IntPtr]$p.MainWindowHandle)
    Check 'PHY-view' '定位 SB3_TermView' ($term -ne [IntPtr]::Zero) "handle=$term"

    if ($term -ne [IntPtr]::Zero) {
        [PhysKey.Native]::PostMessage($term, 0x0007, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 100

        $before = Get-Channel 'loop-chars'
        [PhysKey.Native]::PostMessage($term, 0x0100, [IntPtr]8, [IntPtr]1) | Out-Null
        [PhysKey.Native]::PostMessage($term, 0x0101, [IntPtr]8, [IntPtr]0xC0000001) | Out-Null
        Start-Sleep -Milliseconds 600
        $after = Get-Channel 'loop-chars'
        $dtx = $after.tx_bytes - $before.tx_bytes
        $drx = $after.rx_bytes - $before.rx_bytes
        Check 'PHY-backspace' 'WM_KEYDOWN VK_BACK' ($dtx -eq 1 -and $drx -eq 1) "backspace tx+=$dtx rx+=$drx expect=1"

        $before = Get-Channel 'loop-chars'
        [PhysKey.Native]::PostMessage($term, 0x0100, [IntPtr]38, [IntPtr]0x01480001) | Out-Null
        [PhysKey.Native]::PostMessage($term, 0x0101, [IntPtr]38, [IntPtr]0xC1480001) | Out-Null
        Start-Sleep -Milliseconds 600
        $after = Get-Channel 'loop-chars'
        $dtx = $after.tx_bytes - $before.tx_bytes
        $drx = $after.rx_bytes - $before.rx_bytes
        Check 'PHY-up' 'WM_KEYDOWN VK_UP' ($dtx -eq 3 -and $drx -eq 3) "up tx+=$dtx rx+=$drx expect=3"
    }
} catch {
    Check 'PHY-error' '执行异常' $false $_.Exception.Message
} finally {
    if ($started) {
        Get-Process -Name ssh_bridge3 -ErrorAction SilentlyContinue | Stop-Process -Force
    }
}

$rows | Format-Table -AutoSize
$failed = @($rows | Where-Object 结果 -eq 'FAIL')
if ($failed.Count -eq 0) { 'RESULT: ALL PASS'; exit 0 }
"RESULT: FAIL $($failed.编号 -join ',')"; exit 1
