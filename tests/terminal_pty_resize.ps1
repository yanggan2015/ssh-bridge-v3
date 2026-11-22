# terminal_pty_resize.ps1 — 验证 SSH PTY 跟随终端窗格尺寸
# 前置：隔离实例已启动，UI 桥在 -Base；活动通道为 SSH。
param([string]$Base = 'http://127.0.0.1:18096')

$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PTYResize {
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
"@

function Get-J([string]$path) {
    return Invoke-RestMethod "$Base$path" -TimeoutSec 10
}
function Act([string]$id, [string]$action, [string]$text) {
    $body = @{ id = $id; action = $action; text = $text } | ConvertTo-Json -Compress
    return Invoke-RestMethod -Method Post -Uri "$Base/action" -ContentType 'application/json; charset=utf-8' -Body $body -TimeoutSec 15
}
function Pane {
    return (Get-J '/state').state.terminal.panes[0]
}
function SttySize {
    Act 'term.clear' 'click' '' | Out-Null
    Act 'term.input' 'set_text' 'stty size' | Out-Null
    Start-Sleep -Milliseconds 900
    $p = Pane
    foreach ($line in @($p.tail)) {
        if ($line -match '^\s*(\d+)\s+(\d+)\s*$') {
            return @([int]$matches[1], [int]$matches[2])
        }
    }
    return @(-1, -1)
}

$tree = Get-J '/ui-tree'
$term = $tree.controls | Where-Object { $_.id -eq 'term.view' } | Select-Object -First 1
if (-not $term) { throw 'term.view 不存在' }
$root = [PTYResize]::GetAncestor([IntPtr]::new([Int64]$term.hwnd), 2)

$p0 = Pane
$s0 = SttySize
$ok0 = ($s0[0] -eq $p0.rows -and $s0[1] -eq $p0.cols)
"PTY-initial pane=$($p0.cols)x$($p0.rows) stty=$($s0[1])x$($s0[0]) $(if($ok0){'PASS'}else{'FAIL'})"

[PTYResize]::SetWindowPos($root, [IntPtr]::Zero, 0, 0, 1000, 600, 0x0014) | Out-Null
Start-Sleep -Milliseconds 900
$p1 = Pane
$s1 = SttySize
$changed = ($p1.cols -ne $p0.cols -or $p1.rows -ne $p0.rows)
$ok1 = $changed -and ($s1[0] -eq $p1.rows -and $s1[1] -eq $p1.cols)
"PTY-resized pane=$($p1.cols)x$($p1.rows) stty=$($s1[1])x$($s1[0]) $(if($ok1){'PASS'}else{'FAIL'})"

[PTYResize]::SetWindowPos($root, [IntPtr]::Zero, 0, 0, 1280, 800, 0x0014) | Out-Null
Start-Sleep -Milliseconds 900
$p2 = Pane
$s2 = SttySize
$ok2 = ($s2[0] -eq $p2.rows -and $s2[1] -eq $p2.cols)
"PTY-restored pane=$($p2.cols)x$($p2.rows) stty=$($s2[1])x$($s2[0]) $(if($ok2){'PASS'}else{'FAIL'})"

# 重连后远端 PTY 必须仍使用当前窗格尺寸，而不是通道配置里的 120x32
Act 'btn.reconnect' 'click' '' | Out-Null
$ready = $false
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 300
    $ch = (Get-J '/state').state.channels | Where-Object { $_.name -eq $p2.channel } | Select-Object -First 1
    if ($ch -and $ch.state -eq 'ready') { $ready = $true; break }
}
$s3 = SttySize
$ok3 = $ready -and ($s3[0] -eq $p2.rows -and $s3[1] -eq $p2.cols)
"PTY-reconnect ready=$ready pane=$($p2.cols)x$($p2.rows) stty=$($s3[1])x$($s3[0]) $(if($ok3){'PASS'}else{'FAIL'})"

if ($ok0 -and $ok1 -and $ok2 -and $ok3) {
    'RESULT: ALL PASS'
    exit 0
}
'RESULT: FAIL'
exit 1
