# dropdown_full_check.ps1 — 桌面窗口下拉菜单逐项遍历 + 截图
param(
    [string]$Base = 'http://127.0.0.1:18096',
    [string]$OutDir = 'C:\code\ssh-bridge-windows3\output\dropdown-audit-20261124'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class DropAudit {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static IntPtr Find(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, lp) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) {
        var s = new StringBuilder(128); GetClassName(h, s, 128);
        if (s.ToString() == cls) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static List<string> Visible(uint pid) {
    var list = new List<string>();
    EnumWindows((h, lp) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) {
        var c = new StringBuilder(128); GetClassName(h, c, 128);
        var t = new StringBuilder(256); GetWindowText(h, t, 256);
        RECT r; GetWindowRect(h, out r);
        list.Add(h.ToString() + "|" + c.ToString() + "|" + t.ToString() + "|" + (r.Right - r.Left) + "x" + (r.Bottom - r.Top));
      }
      return true;
    }, IntPtr.Zero);
    return list;
  }
}
'@
[DropAudit]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null
$shotDir = Join-Path $OutDir 'screens\desktop'
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
$script:rows = @()
$script:inventory = @()
$script:empty = @()
$script:screens = @()

function Check([string]$scope, [string]$item, [bool]$ok, [string]$detail = '') {
    $script:rows += [pscustomobject]@{ scope = $scope; item = $item; ok = [bool]$ok; detail = $detail }
    $tag = if ($ok) { 'PASS' } else { 'FAIL' }
    Write-Host ("{0} [{1}] {2} {3}" -f $tag, $scope, $item, $detail)
}
function Get-J([string]$path) { return (Invoke-RestMethod ($Base + $path) -TimeoutSec 10) }
function State { return (Get-J '/state').state }
function Tree { return (Get-J '/ui-tree') }
function Control([string]$id) { return (Get-J ('/control?id=' + [uri]::EscapeDataString($id))).control }
function Act([string]$id, [string]$action, $value = $null, $text = $null) {
    $body = @{ id = $id; action = $action }
    if ($null -ne $value) { $body.value = $value }
    if ($null -ne $text) { $body.text = $text }
    return Invoke-RestMethod ($Base + '/action') -Method Post -ContentType 'application/json' -Body ($body | ConvertTo-Json -Compress) -TimeoutSec 12
}
function AppPid {
    $p = Get-Process ssh_bridge3 -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { throw 'ssh_bridge3 not running' }
    return [uint32]$p.Id
}
function RootHwnd([IntPtr]$h) { return [DropAudit]::GetAncestor($h, 2) }

function Save-Screen([int]$l, [int]$t, [int]$r, [int]$b, [string]$name) {
    $w = [Math]::Max(1, $r - $l); $h = [Math]::Max(1, $b - $t)
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try { $g.CopyFromScreen($l, $t, 0, 0, $bmp.Size) } finally { $g.Dispose() }
    $file = Join-Path $shotDir ($name + '.png')
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $script:screens += $file
    return $file
}
function Save-Popup([string]$id, [string]$name) {
    $c = Control $id
    if (-not $c -or -not $c.hwnd) { return '' }
    $h = [IntPtr][long]$c.hwnd
    $root = RootHwnd $h
    [DropAudit]::SendMessage($h, 0x014F, [IntPtr]1, [IntPtr]0) | Out-Null
    Start-Sleep -Milliseconds 120
    $pop = [DropAudit]::Find((AppPid), 'ComboLBox')
    $rd = New-Object DropAudit+RECT; [DropAudit]::GetWindowRect($root, [ref]$rd) | Out-Null
    if ($pop -eq [IntPtr]::Zero) {
        return (Save-Screen $rd.Left $rd.Top $rd.Right $rd.Bottom $name)
    }
    $rp = New-Object DropAudit+RECT; [DropAudit]::GetWindowRect($pop, [ref]$rp) | Out-Null
    $l = [Math]::Min($rd.Left, $rp.Left); $t = [Math]::Min($rd.Top, $rp.Top)
    $r = [Math]::Max($rd.Right, $rp.Right); $b = [Math]::Max($rd.Bottom, $rp.Bottom)
    $file = Save-Screen $l $t $r $b $name
    [DropAudit]::SendMessage($h, 0x014F, [IntPtr]0, [IntPtr]0) | Out-Null
    return $file
}
function Save-WindowByClass([string]$class, [string]$name) {
    $h = [DropAudit]::Find((AppPid), $class)
    if ($h -eq [IntPtr]::Zero) { return '' }
    $rd = New-Object DropAudit+RECT; [DropAudit]::GetWindowRect($h, [ref]$rd) | Out-Null
    return (Save-Screen $rd.Left $rd.Top $rd.Right $rd.Bottom $name)
}

function Test-Combo([string]$scope, [string]$id, [bool]$popupShot = $true) {
    $c = Control $id
    if (-not $c) { Check $scope $id $false 'control not found'; return }
    $items = @($c.items)
    $n = [int]$c.item_count
    if ($n -eq 0) {
        $script:empty += [pscustomobject]@{ scope = $scope; id = $id; text = $c.text; editable = (@($c.actions) -contains 'set_text') }
        $old = [string]$c.text
        if ($old -ne '') {
            $r = Act $id 'set_text' $null $old
            $c2 = Control $id
            Check $scope "$id 空列表可编辑" ($r.ok -and $c2.text -eq $old) "text='$old'"
        } else {
            Check $scope "$id 空列表" $true '无可选项，保留空文本'
        }
        return
    }
    $okAll = $true; $bad = @()
    for ($i = 0; $i -lt $n; $i++) {
        $r = Act $id 'select' $i
        Start-Sleep -Milliseconds 80
        $c2 = Control $id
        $want = [string]$items[$i]
        if (-not $r.ok -or $c2.selected -ne $i -or ([string]$c2.text).Trim() -ne $want.Trim()) {
            $okAll = $false
            $bad += ("i={0} want='{1}' gotSel={2} got='{3}' ok={4}" -f $i, $want, $c2.selected, $c2.text, $r.ok)
        }
    }
    Check $scope "$id 逐项可选（$n 项）" $okAll (($bad | Select-Object -First 3) -join '; ')
    $script:inventory += [pscustomobject]@{ scope = $scope; id = $id; item_count = $n; items = $items; selected_after = (Control $id).selected }
    if ($popupShot) {
        $shot = Save-Popup $id (($scope + '-' + $id) -replace '[^A-Za-z0-9_.-]', '_')
        Check $scope "$id 展开下拉截图" ($shot -ne '') $shot
    }
}
function Test-VisibleCombos([string]$scope, [string[]]$Exclude = @()) {
    $tree = Tree
    $combos = @($tree.controls | Where-Object { $_.role -eq 'combobox' -and $_.visible -eq $true -and ($Exclude -notcontains $_.id) })
    Check $scope "可见下拉数量" ($combos.Count -gt 0) ("count=" + $combos.Count)
    foreach ($c in $combos) { Test-Combo $scope $c.id }
    return $combos.Count
}

$started = Get-Date
$baseCount = (Tree).count
Check 'app' 'UI 桥在线' ((State).app -eq 'ssh-bridge3') ("count=$baseCount")

# ---- 新建串口：四种类型布局 + 全部下拉逐项 ----
$r = Act 'menu.file.newSerial' 'click'; Start-Sleep -Milliseconds 700
Check 'channel-new' '新建串口对话框打开' ($r.ok -and (State).ui.channel_dialog_open) 'open'
$kind = Control 'dlg.channel.kind'
foreach ($i in 0..3) {
    $s = Act 'dlg.channel.kind' 'select' $i; Start-Sleep -Milliseconds 350
    $c = Control 'dlg.channel.kind'
    Check 'channel-new' "类型布局 $i" ($s.ok -and $c.selected -eq $i) ("text=" + $c.text)
    $shot = Save-WindowByClass 'SB3_ChannelDialog' ("channel-kind-$i")
    Check 'channel-new' "类型布局 $i 截图" ($shot -ne '') $shot
}
Act 'dlg.channel.kind' 'select' 0 | Out-Null; Start-Sleep -Milliseconds 250
Test-VisibleCombos 'channel-new' | Out-Null
$shot = Save-WindowByClass 'SB3_ChannelDialog' 'channel-new-final'
Check 'channel-new' '整体布局截图' ($shot -ne '') $shot
Act 'dlg.channel.cancel' 'click' | Out-Null; Start-Sleep -Milliseconds 400
Check 'channel-new' '取消关闭' (-not (State).ui.channel_dialog_open) ''

# ---- 新建 SSH ----
$r = Act 'menu.file.newSsh' 'click'; Start-Sleep -Milliseconds 600
Check 'channel-ssh' '新建 SSH 对话框打开' ($r.ok -and (State).ui.channel_dialog_open) ''
Test-VisibleCombos 'channel-ssh' | Out-Null
$shot = Save-WindowByClass 'SB3_ChannelDialog' 'channel-ssh'
Check 'channel-ssh' 'SSH 布局截图' ($shot -ne '') $shot
Act 'dlg.channel.cancel' 'click' | Out-Null; Start-Sleep -Milliseconds 400

# ---- 编辑当前通道：类型必须禁用 ----
$r = Act 'menu.file.editChannel' 'click'; Start-Sleep -Milliseconds 600
$kc = Control 'dlg.channel.kind'
Check 'channel-edit' '编辑通道打开且类型禁用' ($r.ok -and $kc.enabled -eq $false) ("enabled=" + $kc.enabled)
Test-VisibleCombos 'channel-edit' @('dlg.channel.kind') | Out-Null
$shot = Save-WindowByClass 'SB3_ChannelDialog' 'channel-edit'
Check 'channel-edit' '编辑布局截图' ($shot -ne '') $shot
Act 'dlg.channel.cancel' 'click' | Out-Null; Start-Sleep -Milliseconds 400

# ---- SFTP 面板 ----
$r = Act 'menu.file.sftp' 'click'; Start-Sleep -Milliseconds 800
Check 'sftp' 'SFTP 面板打开' ($r.ok -and (State).ui.sftp_open) ''
Test-VisibleCombos 'sftp' | Out-Null
$shot = Save-WindowByClass 'SB3_SftpWindow' 'sftp'
Check 'sftp' 'SFTP 面板截图' ($shot -ne '') $shot
$r = Act 'sftp.settings.open' 'click'; Start-Sleep -Milliseconds 500
$speed = Control 'sftp.settings.speed'
Check 'sftp-settings' 'SFTP 设置打开' ($r.ok -and $null -ne $speed) ("speed_items=" + $speed.item_count)
Test-VisibleCombos 'sftp-settings' | Out-Null
$shot = Save-WindowByClass 'SB3_SftpSettings' 'sftp-settings'
Check 'sftp-settings' '传输设置截图' ($shot -ne '') $shot
Act 'sftp.settings.close' 'click' | Out-Null; Start-Sleep -Milliseconds 300
Act 'sftp.close' 'click' | Out-Null; Start-Sleep -Milliseconds 500

# ---- 文件服务 ----
$r = Act 'menu.fileService.config' 'click'; Start-Sleep -Milliseconds 700
Check 'filesvc' '文件服务面板打开' ($r.ok -and (State).ui.filesvc_open) ''
Test-VisibleCombos 'filesvc' | Out-Null
$shot = Save-WindowByClass 'SB3_FileServicePanel' 'filesvc'
Check 'filesvc' '文件服务面板截图' ($shot -ne '') $shot
Act 'filesvc.close' 'click' | Out-Null; Start-Sleep -Milliseconds 500

# ---- 继电器：三页逐页 ----
$r = Act 'menu.relay.config' 'click'; Start-Sleep -Milliseconds 700
Check 'relay-p0' '继电器面板打开' ($r.ok -and (State).ui.relay_open) ''
foreach ($page in 0..2) {
    Act 'relay.tabs' 'select' $page | Out-Null; Start-Sleep -Milliseconds 450
    $scope = "relay-p$page"
    Test-VisibleCombos $scope | Out-Null
    $shot = Save-WindowByClass 'SB3_RelayPanel' $scope
    Check $scope '继电器页面截图' ($shot -ne '') $shot
}
Act 'relay.close' 'click' | Out-Null; Start-Sleep -Milliseconds 500

$finalCount = (Tree).count
Check 'final' '所有窗口关闭后控件数回基线' ($finalCount -eq $baseCount) "count=$baseCount->$finalCount"

$summary = [pscustomobject]@{
    generated_at = (Get-Date).ToString('o')
    passed = @($script:rows | Where-Object ok).Count
    total = @($script:rows).Count
    bad = @($script:rows | Where-Object { -not $_.ok }).Count
    empty_dropdowns = @($script:empty)
    inventory = @($script:inventory)
    screenshots = @($script:screens)
    rows = @($script:rows)
    started = $started.ToString('o')
}
$file = Join-Path $OutDir 'desktop-dropdown-result.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -Path $file -Encoding UTF8
Write-Host ("SUMMARY desktop passed={0}/{1} bad={2} empty={3} shots={4}" -f $summary.passed, $summary.total, $summary.bad, @($script:empty).Count, @($script:screens).Count)
Write-Host ("RESULT " + $file)
