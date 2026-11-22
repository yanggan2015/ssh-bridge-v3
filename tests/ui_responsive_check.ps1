# ui_responsive_check.ps1 — 左侧通道管理器窄窗自动收起 / 按钮展开收起回归
# 前置：ssh_bridge3 已在运行，桥接 http://127.0.0.1:18086。
# 用法：pwsh -NoProfile -File tests\ui_responsive_check.ps1
$ErrorActionPreference = 'Stop'
$base = 'http://127.0.0.1:18086'
$fail = 0
$total = 0

function Check([string]$name, [bool]$ok, [string]$detail = '') {
    $script:total++
    if ($ok) {
        Write-Host ("PASS  {0}  {1}" -f $name, $detail)
    } else {
        $script:fail++
        Write-Host ("FAIL  {0}  {1}" -f $name, $detail) -ForegroundColor Red
    }
}

function Act([string]$id, [string]$action) {
    $body = @{ id = $id; action = $action } | ConvertTo-Json -Compress
    Invoke-RestMethod "$base/action" -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 10
}

function State { Invoke-RestMethod "$base/state" -TimeoutSec 10 }

function Control([string]$id) {
    Invoke-RestMethod "$base/control?id=$id" -TimeoutSec 10
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Sb3ResponsiveWin {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
'@

$p = Get-Process ssh_bridge3 -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { throw '找不到 ssh_bridge3 主窗口' }
$h = $p.MainWindowHandle

function Resize([int]$w, [int]$height) {
    [Sb3ResponsiveWin]::SetWindowPos($h, [IntPtr]::Zero, 40, 40, $w, $height, 0x0014) | Out-Null
    Start-Sleep -Milliseconds 700
}

function Snapshot {
    $s = State
    $t = Invoke-RestMethod "$base/ui-tree" -TimeoutSec 10
    $tog = $t.controls | Where-Object { $_.id -eq 'btn.toggleTree' }
    [pscustomobject]@{
        collapsed = [bool]$s.state.ui.left_collapsed
        leftVisible = [bool]$s.state.ui.left_visible
        treeVisible = [bool]$s.state.tree_visible
        toggleText = [string]$tog.text
        main = @($t.main_rect)
        controls = @($t.controls)
    }
}

function OverlapCount($controls, $main) {
    $items = @($controls | Where-Object {
        $_.visible -eq $true -and $_.virtual -ne $true -and
        $_.rect[2] -gt $_.rect[0] -and $_.rect[3] -gt $_.rect[1] -and
        $_.id -ne 'left.tabs'
    })
    $overlaps = 0
    for ($i = 0; $i -lt $items.Count; $i++) {
        for ($j = $i + 1; $j -lt $items.Count; $j++) {
            $a = $items[$i]; $b = $items[$j]
            $al = $a.rect[0]; $at = $a.rect[1]; $ar = $a.rect[2]; $ab = $a.rect[3]
            $bl = $b.rect[0]; $bt = $b.rect[1]; $br = $b.rect[2]; $bb = $b.rect[3]
            $aContains = ($al -le $bl -and $at -le $bt -and $ar -ge $br -and $ab -ge $bb)
            $bContains = ($bl -le $al -and $bt -le $at -and $br -ge $ar -and $bb -ge $ab)
            if ($aContains -or $bContains) { continue }
            $il = [Math]::Max($al, $bl); $it = [Math]::Max($at, $bt)
            $ir = [Math]::Min($ar, $br); $ib = [Math]::Min($ab, $bb)
            if ($ir -gt $il -and $ib -gt $it) { $overlaps++ }
        }
    }
    return $overlaps
}

# 1) 宽窗：左侧默认展开
Resize 1300 900
$wide = Snapshot
Check 'R1 宽窗左侧展开' (-not $wide.collapsed -and $wide.leftVisible -and $wide.treeVisible) `
    "collapsed=$($wide.collapsed) left=$($wide.leftVisible) tree=$($wide.treeVisible)"
Check 'R2 宽窗按钮文案为收起' ($wide.toggleText -eq '收起') "text=$($wide.toggleText)"

# 2) 窄窗：自动收起，终端占满可用宽度
Resize 500 900
$narrow = Snapshot
Check 'R3 窄窗自动收起' ($narrow.collapsed -and -not $narrow.leftVisible -and -not $narrow.treeVisible) `
    "collapsed=$($narrow.collapsed) left=$($narrow.leftVisible) tree=$($narrow.treeVisible)"
Check 'R4 窄窗按钮文案为展开' ($narrow.toggleText -eq '展开') "text=$($narrow.toggleText)"
$search = $narrow.controls | Where-Object { $_.id -eq 'edit.search' }
Check 'R5 窄窗搜索框仍可见且在窗口内' `
    ($search.visible -and $search.rect[0] -ge $narrow.main[0] -and $search.rect[2] -le $narrow.main[2]) `
    "rect=$($search.rect -join ',') main=$($narrow.main -join ',')"
$ov = OverlapCount $narrow.controls $narrow.main
Check 'R6 窄窗无控件重叠' ($ov -eq 0) "overlaps=$ov"

# 3) 按钮手动展开 / 收起
Act 'btn.toggleTree' 'click' | Out-Null
Start-Sleep -Milliseconds 400
$manualOpen = Snapshot
Check 'R7 窄窗按钮可展开' (-not $manualOpen.collapsed -and $manualOpen.leftVisible) `
    "collapsed=$($manualOpen.collapsed) left=$($manualOpen.leftVisible)"
Act 'btn.toggleTree' 'click' | Out-Null
Start-Sleep -Milliseconds 400
$manualClosed = Snapshot
Check 'R8 窄窗按钮可再次收起' ($manualClosed.collapsed -and -not $manualClosed.leftVisible) `
    "collapsed=$($manualClosed.collapsed) left=$($manualClosed.leftVisible)"

# 4) 再拖宽：自动恢复左侧
Resize 1300 900
$restored = Snapshot
Check 'R9 拖宽后自动恢复左侧' (-not $restored.collapsed -and $restored.leftVisible) `
    "collapsed=$($restored.collapsed) left=$($restored.leftVisible)"

Write-Host ''
if ($fail -eq 0) {
    Write-Host "RESULT: ALL PASS ($total 项)" -ForegroundColor Green
    exit 0
}
Write-Host "RESULT: FAIL ($fail/$total)" -ForegroundColor Red
exit 1
