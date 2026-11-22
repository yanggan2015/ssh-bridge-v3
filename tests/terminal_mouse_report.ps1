# terminal_mouse_report.ps1 — 验证终端鼠标上报（SGR / X10 / 1002 拖动 / 1003 移动）
# 前置：隔离实例已启动，UI 桥在 -Base；配置含 ssh-demo1、ssh-demo2。
param([string]$Base = 'http://127.0.0.1:18096')

$ErrorActionPreference = 'Stop'
$script:fail = 0

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
function Send-Raw([string]$text) { Act 'term.send' 'set_text' $text | Out-Null }
function Send-Cmd([string]$text) { Act 'term.input' 'set_text' $text | Out-Null }
function Wait-Tail([string]$needle, [int]$ms = 3000) {
    for ($i = 0; $i -lt ($ms / 100); $i++) {
        $p = Pane
        foreach ($line in @($p.tail)) {
            if ($line -and $line.Contains($needle)) { return $true }
        }
        Start-Sleep -Milliseconds 100
    }
    return $false
}
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) { "PASS $name $detail" } else { "FAIL $name $detail"; $script:fail++ }
}
function Reset-Remote {
    Send-Raw ([string]([char]3))
    Start-Sleep -Milliseconds 600
    Send-Cmd 'printf "\033[?1000l\033[?1002l\033[?1003l\033[?1006l"; stty sane; echo MOUSE_READY'
    Wait-Tail 'MOUSE_READY' 3000 | Out-Null
}
function Start-Cat([int]$mode, [bool]$sgr) {
    Send-Raw ([string]([char]3))
    Start-Sleep -Milliseconds 500
    Act 'term.clear' 'click' '' | Out-Null
    $seq = "\033[?${mode}h"
    if ($sgr) { $seq += "\033[?1006h" } else { $seq += "\033[?1006l" }
    Send-Cmd "printf `"$seq`"; echo MOUSE_${mode}; cat -v"
    Wait-Tail "MOUSE_$mode" 4000 | Out-Null
    Start-Sleep -Milliseconds 500
}
function Mouse([string]$payload) {
    Act 'term.mouse' 'set_text' $payload | Out-Null
}

foreach ($ch in @('ssh-demo1', 'ssh-demo2')) {
    Act 'tree.channels' 'select' $ch | Out-Null
    Start-Sleep -Milliseconds 800
    Reset-Remote

    # 1000 + 1006：SGR 按下/释放/滚轮/Shift
    Start-Cat 1000 $true
    $p = Pane
    Check "$ch-mode1000" ($p.mouse_mode -eq 1) "mode=$($p.mouse_mode)"
    Check "$ch-sgr" ($p.mouse_sgr -eq $true) "sgr=$($p.mouse_sgr)"

    Mouse '19,21,0' | Out-Null
    Check "$ch-press" (Wait-Tail '^[[<0;19;21M') ''
    Mouse '19,21,0,1' | Out-Null
    Check "$ch-release" (Wait-Tail '^[[<0;19;21m') ''
    Mouse '19,21,64' | Out-Null
    Check "$ch-wheel" (Wait-Tail '^[[<64;19;21M') ''
    Mouse '19,21,0,0,0,4' | Out-Null
    Check "$ch-shift" (Wait-Tail '^[[<4;19;21M') ''

    # 1002：按键拖动只上报按住移动
    Start-Cat 1002 $true
    $p = Pane
    Check "$ch-mode1002" ($p.mouse_mode -eq 2) "mode=$($p.mouse_mode)"
    Mouse '19,21,0' | Out-Null
    Check "$ch-drag-press" (Wait-Tail '^[[<0;19;21M') ''
    Mouse '20,22,0,0,1' | Out-Null
    Check "$ch-drag-move" (Wait-Tail '^[[<32;20;22M') ''
    Mouse '20,22,0,1' | Out-Null
    Check "$ch-drag-up" (Wait-Tail '^[[<0;20;22m') ''

    # 1003：无按键移动也上报（button=35）
    Start-Cat 1003 $true
    $p = Pane
    Check "$ch-mode1003" ($p.mouse_mode -eq 3) "mode=$($p.mouse_mode)"
    Mouse '25,20,3,0,1' | Out-Null
    Check "$ch-hover" (Wait-Tail '^[[<35;25;20M') ''

    # X10 兼容编码（1000 但不带 1006）
    Start-Cat 1000 $false
    $p = Pane
    Check "$ch-mode-x10" ($p.mouse_mode -eq 1 -and $p.mouse_sgr -eq $false) "mode=$($p.mouse_mode) sgr=$($p.mouse_sgr)"
    Mouse '19,21,0' | Out-Null
    Check "$ch-x10-press" (Wait-Tail '^[[M 46') ''
    Mouse '19,21,0,1' | Out-Null
    Check "$ch-x10-release" (Wait-Tail '^[[M#46') ''

    Reset-Remote
}

# 鼠标模式必须能在远端关闭；关闭后 term.mouse 不应再发字节
Act 'tree.channels' 'select' 'ssh-demo1' | Out-Null
Start-Sleep -Milliseconds 600
Reset-Remote
Start-Sleep -Milliseconds 500
$p = Pane
Check 'mouse-off' ($p.mouse_mode -eq 0 -and $p.mouse_sgr -eq $false) "mode=$($p.mouse_mode) sgr=$($p.mouse_sgr)"
$tx0 = ((Get-J '/state').state.channels | Where-Object { $_.name -eq 'ssh-demo1' }).tx_bytes
Mouse '19,21,0' | Out-Null
Start-Sleep -Milliseconds 600
$tx1 = ((Get-J '/state').state.channels | Where-Object { $_.name -eq 'ssh-demo1' }).tx_bytes
Check 'mouse-off-no-send' ($tx1 -eq $tx0) "tx=$tx0->$tx1"

if ($script:fail -eq 0) {
    'RESULT: ALL PASS'
    exit 0
}
'RESULT: FAIL'
exit 1
