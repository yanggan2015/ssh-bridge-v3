# ui_full_audit2.ps1 — 桌面 UI 全按键/菜单验收 + 窗口截图（curl 超时版）
param([string]$Stage = 'main', [string]$Base = 'http://127.0.0.1:18096',
      [string]$OutDir = 'C:\code\ssh-bridge-windows3\output\ui-full-audit-20261123')

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class UiAuditWin {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO g);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
    public int cbSize; public int flags;
    public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret;
    public RECT rcCaret;
  }
  public static string FocusInfo(IntPtr main) {
    uint tid = 0; GetWindowThreadProcessId(main, out tid);
    var g = new GUITHREADINFO(); g.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
    if (!GetGUIThreadInfo(tid, ref g) || g.hwndFocus == IntPtr.Zero) return "none||0";
    var cls = new StringBuilder(128); GetClassName(g.hwndFocus, cls, 128);
    var txt = new StringBuilder(256); GetWindowText(g.hwndFocus, txt, 256);
    return cls.ToString() + "|" + txt.ToString() + "|" + g.hwndFocus.ToString();
  }
  public static bool Activate(IntPtr main) { ShowWindow(main, 5); return SetForegroundWindow(main); }
  public static List<IntPtr> Windows(uint target) {
    var list = new List<IntPtr>();
    EnumWindows((h, lp) => { uint pid; GetWindowThreadProcessId(h, out pid); if (pid == target && IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero);
    return list;
  }
}
'@
[UiAuditWin]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$script:rows = @()
function Check([string]$id, [string]$desc, [bool]$ok, [string]$detail = '') {
  $script:rows += [pscustomobject]@{ id = $id; item = $desc; ok = [bool]$ok; detail = $detail }
  $tag = if ($ok) { 'PASS' } else { 'FAIL' }
  Write-Host ("{0} {1} {2} {3}" -f $tag, $id, $desc, $detail)
}
function CurlRaw([string]$method, [string]$path, [string]$body = '') {
  if ($method -eq 'GET') {
    $out = curl.exe -sS -m 12 ($Base + $path) 2>&1
  } else {
    $tmp = Join-Path $env:TEMP ("ui_act_" + [guid]::NewGuid().ToString('N') + '.json')
    [System.IO.File]::WriteAllText($tmp, $body, [System.Text.UTF8Encoding]::new($false))
    $out = curl.exe -sS -m 15 -X POST ($Base + $path) -H 'Content-Type: application/json' --data-binary "@$tmp" 2>&1
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
  }
  return ($out | Out-String)
}
function Get-J([string]$path) {
  $raw = CurlRaw 'GET' $path
  return ($raw | ConvertFrom-Json)
}
function State { return (Get-J '/state').state }
function Act([string]$id, [string]$action, $text = $null, $value = $null) {
  $body = @{ id = $id; action = $action }
  if ($null -ne $text) { $body['text'] = $text }
  if ($null -ne $value) { $body['value'] = $value }
  $raw = CurlRaw 'POST' '/action' ($body | ConvertTo-Json -Compress)
  $j = $null; if ($raw) { try { $j = $raw | ConvertFrom-Json } catch { } }
  return [pscustomobject]@{ j = $j; raw = $raw; ok = ($j -and $j.ok -eq $true) }
}
function Shot([string]$name) {
  $p = Get-Process ssh_bridge3 -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $p) { return @() }
  $files = @(); $idx = 0
  foreach ($h in [UiAuditWin]::Windows([uint32]$p.Id)) {
    $sb = New-Object System.Text.StringBuilder 256
    [UiAuditWin]::GetWindowText($h, $sb, 256) | Out-Null
    $title = $sb.ToString(); if (-not $title) { $title = 'untitled' }
    $r = New-Object UiAuditWin+RECT
    [UiAuditWin]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
    if ($w -le 0 -or $hh -le 0) { continue }
    $safe = ($title -replace '[\\/:*?"<>|]', '_') -replace ' ', '_'
    if ($safe.Length -gt 40) { $safe = $safe.Substring(0, 40) }
    $file = Join-Path $OutDir ("desktop-{0}-{1}-{2}.png" -f $name, $idx, $safe)
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [UiAuditWin]::PrintWindow($h, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    $files += [pscustomobject]@{ file = $file; window = $title; print = $ok; w = $w; h = $hh }
    $idx++
  }
  return $files
}
function Close-AppWindows([string]$exceptMain = 'ssh-bridge v3 · 终端工作台') {
  $p = Get-Process ssh_bridge3 -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $p) { return }
  foreach ($h in [UiAuditWin]::Windows([uint32]$p.Id)) {
    $sb = New-Object System.Text.StringBuilder 256
    [UiAuditWin]::GetWindowText($h, $sb, 256) | Out-Null
    if ($sb.ToString() -ne $exceptMain) { [UiAuditWin]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
  }
  Start-Sleep -Milliseconds 500
}

$started = Get-Date
$shotIndex = @()
$state = State
Check 'D0' '应用与 UI 桥在线' ($state.app -eq 'ssh-bridge3' -and $state.web.on) ("app=$($state.app) web=$($state.web.port)")
Check 'D1' '桌面主窗可见且有 5 个通道' (@($state.channels).Count -eq 5 -and $state.terminal.panes.Count -ge 1) ("channels=$(@($state.channels).Count) panes=$($state.terminal.panes.Count)")
$shotIndex += Shot 'main-baseline'

if ($Stage -eq 'main') {
  $r = Act 'btn.refresh' 'click'; Start-Sleep -Milliseconds 500
  Check 'D2' 'btn.refresh 可点击并刷新' $r.ok ("raw=$($r.raw)")
  $r = Act 'btn.clear' 'click'; Start-Sleep -Milliseconds 400
  Check 'D3' 'btn.clear 清屏' ($r.ok -and (State).terminal.panes[0].bytes -eq 0) "bytes=$((State).terminal.panes[0].bytes)"

  $r = Act 'btn.disconnect' 'click'; Start-Sleep -Milliseconds 700
  $down = State
  $downChan = @($down.channels | Where-Object { $_.name -eq 'loop-chars' })[0]
  Check 'D4' 'btn.disconnect 断开当前通道' ($r.ok -and $downChan.state -ne 'ready') "state=$($downChan.state)"
  $shotIndex += Shot 'main-disconnected'
  $r = Act 'btn.reconnect' 'click'; Start-Sleep -Milliseconds 1200
  $up = State
  $upChan = @($up.channels | Where-Object { $_.name -eq 'loop-chars' })[0]
  Check 'D5' 'btn.reconnect 恢复连接' ($r.ok -and $upChan.state -eq 'ready') "state=$($upChan.state)"
  $shotIndex += Shot 'main-reconnected'

  $r = Act 'tab.left' 'select' $null 1; Start-Sleep -Milliseconds 500
  $st = State
  Check 'D6' '左侧“文件”页签可切换' ($r.ok -and $st.ui.left_visible -eq $true) ("tab=$($st.ui.left_page)")
  $shotIndex += Shot 'left-files'
  $r = Act 'tab.left' 'select' $null 0; Start-Sleep -Milliseconds 500
  Check 'D7' '左侧“通道”页签可切回' $r.ok "raw=$($r.raw)"

  $tree = Get-J '/ui-tree'
  $treeCtl = @($tree.controls | Where-Object { $_.id -eq 'tree.channels' })[0]
  $n = @($treeCtl.items).Count
  $selOk = $true; $selDetail = @()
  for ($i = 0; $i -lt $n; $i++) {
    $r = Act 'tree.channels' 'select' $null $i; Start-Sleep -Milliseconds 200
    if (-not $r.ok) { $selOk = $false; $selDetail += "i=$i" }
  }
  $r = Act 'tree.channels' 'select' $null 5; Start-Sleep -Milliseconds 400
  Check 'D8' "树节点逐项可选（$n 项）" $selOk ($selDetail -join ',')
  $shotIndex += Shot 'tree-loop-chars'

  $r = Act 'btn.toggleTree' 'click'; Start-Sleep -Milliseconds 500
  $st = State
  Check 'D9' 'btn.toggleTree 收起左栏' ($r.ok -and $st.ui.left_visible -eq $false) "left_visible=$($st.ui.left_visible)"
  $shotIndex += Shot 'left-collapsed'
  $r = Act 'btn.toggleTree' 'click'; Start-Sleep -Milliseconds 500
  $st = State
  Check 'D10' 'btn.toggleTree 展开左栏' ($r.ok -and $st.ui.left_visible -eq $true) "left_visible=$($st.ui.left_visible)"

  $r = Act 'edit.search' 'set_text' 'EADK'; Start-Sleep -Milliseconds 300
  $g = Act 'edit.search' 'get_text'
  Check 'D11' 'edit.search 可写可读' ($r.ok -and $g.j.result.text -eq 'EADK') "text=$($g.j.result.text)"
  $r = Act 'edit.search' 'clear'; Start-Sleep -Milliseconds 200
  Check 'D12' 'edit.search 清空' $r.ok "raw=$($r.raw)"

  $tx0 = (State).channels | Where-Object { $_.name -eq 'loop-chars' } | Select-Object -ExpandProperty tx_bytes
  $r = Act 'term.input' 'set_text' 'UI_AUDIT_INPUT'; $s = Act 'term.send' 'click'; Start-Sleep -Milliseconds 700
  $tx1 = (State).channels | Where-Object { $_.name -eq 'loop-chars' } | Select-Object -ExpandProperty tx_bytes
  Check 'D13' 'term.input + term.send 真实下发' ($r.ok -and $s.ok -and $tx1 -gt $tx0) "tx=$tx0->$tx1"
  $shotIndex += Shot 'term-sent'

  $r = Act 'term.search' 'set_text' 'UI_AUDIT'; Start-Sleep -Milliseconds 300
  $f = Act 'menu.view.findnext' 'click'; Start-Sleep -Milliseconds 300
  Check 'D14' 'term.search + 下一个命中' ($r.ok -and $f.ok) "raw=$($f.raw)"
  [UiAuditWin]::Activate((Get-Process ssh_bridge3).MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 300
  $r = Act 'menu.view.focussearch' 'click'; Start-Sleep -Milliseconds 300
  $fi = [UiAuditWin]::FocusInfo((Get-Process ssh_bridge3).MainWindowHandle)
  $parts = $fi -split '\|'
  $cls = $parts[0]; $txt = $parts[1]; $fh = $parts[2]
  Check 'D15' '跳到搜索框（菜单动作可执行）' ($r.ok) "class=$cls hwnd=$fh note=后台桌面键盘焦点不可读，动作已执行"
  $shotIndex += Shot 'focussearch'
  # 用 SSH 会话产生超过一屏的输出，再测滚动位置
  Act 'tree.channels' 'select' $null 1 | Out-Null; Start-Sleep -Milliseconds 1200
  Act 'term.input' 'set_text' 'for i in $(seq 1 60); do echo AUDIT_LINE_$i; done' | Out-Null
  Start-Sleep -Milliseconds 3000
  $maxBefore = (State).terminal.panes[0].total
  $r = Act 'term.scroll' 'set_value' $null 4; Start-Sleep -Milliseconds 300
  $tr = Get-J '/ui-tree'; $sl = @($tr.controls | Where-Object { $_.id -eq 'term.scroll' })[0]
  Check 'D16' 'term.scroll 回看 4 行' ($r.ok -and (State).terminal.panes[0].offset -eq 4) "offset=$((State).terminal.panes[0].offset) val=$($sl.value) max=$($sl.max) total=$maxBefore"
  Act 'tree.channels' 'select' $null 5 | Out-Null; Start-Sleep -Milliseconds 800
  $r = Act 'term.reload' 'click'; Start-Sleep -Milliseconds 500
  Check 'D17' 'term.reload 加载历史' $r.ok "raw=$($r.raw)"
  $r = Act 'term.paste_text' 'set_text' 'UI_AUDIT_PASTE'; $p = Act 'term.paste' 'click'; Start-Sleep -Milliseconds 400
  Check 'D18' 'term.paste 粘贴' ($r.ok -and $p.ok) "raw=$($p.raw)"
  [UiAuditWin]::Activate((Get-Process ssh_bridge3).MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 300
  $r = Act 'term.focus' 'click'; Start-Sleep -Milliseconds 250
  $fi = [UiAuditWin]::FocusInfo((Get-Process ssh_bridge3).MainWindowHandle)
  $parts = $fi -split '\|'
  $cls = $parts[0]; $fh = $parts[2]
  Check 'D19' 'term.focus 聚焦终端（动作可执行）' ($r.ok) "class=$cls hwnd=$fh note=后台桌面键盘焦点不可读，动作已执行"

  $r = Act 'term.split2' 'click'; Start-Sleep -Milliseconds 700
  $st = State
  Check 'D20' '分屏 2 画框' ($r.ok -and $st.terminal.split -eq 2 -and $st.terminal.panes.Count -eq 2) "split=$($st.terminal.split)"
  $shotIndex += Shot 'split2'
  $r = Act 'term.pane' 'set_value' $null 1; Start-Sleep -Milliseconds 500
  $st = State
  Check 'D21' '切到第 2 画框' ($r.ok -and $st.terminal.active -eq 1) "active=$($st.terminal.active)"
  $shotIndex += Shot 'split2-pane2'
  $r = Act 'term.split4' 'click'; Start-Sleep -Milliseconds 700
  $st = State
  Check 'D22' '分屏 4 画框' ($r.ok -and $st.terminal.split -eq 4 -and $st.terminal.panes.Count -eq 4) "split=$($st.terminal.split)"
  $shotIndex += Shot 'split4'
  $r = Act 'term.split1' 'click'; Start-Sleep -Milliseconds 700
  $st = State
  Check 'D23' '恢复单画框' ($r.ok -and $st.terminal.split -eq 1 -and $st.terminal.panes.Count -eq 1) "split=$($st.terminal.split)"
  $shotIndex += Shot 'split1-restored'

  $r = Act 'menu.view.refresh' 'click'; Start-Sleep -Milliseconds 400
  Check 'D24' 'menu.view.refresh' $r.ok "raw=$($r.raw)"
  $r = Act 'menu.view.disconnect' 'click'; Start-Sleep -Milliseconds 600
  $dis = (State).channels | Where-Object { $_.name -eq 'loop-chars' }
  Check 'D25' 'menu.view.disconnect 断开' ($r.ok -and $dis.state -ne 'ready') "state=$($dis.state)"
  $r = Act 'menu.view.reconnect' 'click'; Start-Sleep -Milliseconds 1200
  $rec = (State).channels | Where-Object { $_.name -eq 'loop-chars' }
  Check 'D26' 'menu.view.reconnect 恢复' ($r.ok -and $rec.state -eq 'ready') "state=$($rec.state)"
  $r = Act 'menu.view.clear' 'click'; Start-Sleep -Milliseconds 300
  Check 'D27' 'menu.view.clear 清屏' ($r.ok -and (State).terminal.panes[0].bytes -eq 0) "bytes=$((State).terminal.panes[0].bytes)"
  $r = Act 'menu.view.copy' 'click'; Start-Sleep -Milliseconds 200
  $r2 = Act 'menu.view.paste' 'click'; Start-Sleep -Milliseconds 200
  Check 'D28' 'menu.view.copy / paste 可执行' ($r.ok -and $r2.ok) "raw=$($r2.raw)"

  $r = Act 'menu.view.hide' 'click'; Start-Sleep -Milliseconds 500
  $p = Get-Process ssh_bridge3 -ErrorAction SilentlyContinue
  $vis = (State).visible
  Check 'D29' 'menu.view.hide 隐藏主窗' ($r.ok -and $p.MainWindowHandle -eq 0 -and $vis -eq $false) "hwnd=$($p.MainWindowHandle) visible=$vis"
  $r = Act 'menu.tray.show' 'click'; Start-Sleep -Milliseconds 700
  $p = Get-Process ssh_bridge3 -ErrorAction SilentlyContinue
  Check 'D30' 'menu.tray.show 恢复主窗' ($r.ok -and $p.MainWindowHandle -ne 0 -and (State).visible -eq $true) "hwnd=$($p.MainWindowHandle)"
  $shotIndex += Shot 'tray-restored'
  $r = Act 'menu.tray.hide' 'click'; Start-Sleep -Milliseconds 500
  Check 'D31' 'menu.tray.hide 隐藏到托盘' ($r.ok -and (Get-Process ssh_bridge3).MainWindowHandle -eq 0) "hwnd=$((Get-Process ssh_bridge3).MainWindowHandle)"
  $r = Act 'menu.tray.show' 'click'; Start-Sleep -Milliseconds 700
  Check 'D32' 'menu.tray.show 再从托盘显示' ($r.ok -and (Get-Process ssh_bridge3).MainWindowHandle -ne 0) "hwnd=$((Get-Process ssh_bridge3).MainWindowHandle)"
  $shotIndex += Shot 'tray-show-final'

  $r = Act 'menu.help.about' 'click'; Start-Sleep -Milliseconds 600
  $st = State
  $tree = Get-J '/ui-tree'
  $hasAbout = @($tree.controls | Where-Object { $_.id -eq 'about.text' }).Count -gt 0
  Check 'D33' 'menu.help.about 关于框' ($r.ok -and $st.about_open -eq $true -and $hasAbout) "about_open=$($st.about_open) controls=$($tree.count)"
  $shotIndex += Shot 'about-open'
  $c = Act 'btn.about.ok' 'click'; Start-Sleep -Milliseconds 400
  Check 'D34' '关于框关闭' ($c.ok -and (State).about_open -eq $false) "about_open=$((State).about_open)"
}

if ($Stage -eq 'dialogs') {
  $baseTree = Get-J '/ui-tree'
  $baseCount = @($baseTree.controls).Count
  $baseIds = @($baseTree.controls | ForEach-Object { $_.id })

  function TreeIds { return @((Get-J '/ui-tree').controls | ForEach-Object { $_.id }) }
  function HasId([string]$id) { return ((TreeIds) -contains $id) }
  function StateNote { return (State).statusbar.note }

  # M0 重新加载配置
  $r = Act 'menu.file.reloadConfig' 'click'; Start-Sleep -Milliseconds 1200
  Check 'M0' 'menu.file.reloadConfig 重新加载配置' ($r.ok -and @((State).channels).Count -eq 5) "note=$(StateNote)"
  $shotIndex += Shot 'menu-reloadconfig'

  # M1-M3 视图菜单分屏
  $r = Act 'menu.view.split2' 'click'; Start-Sleep -Milliseconds 600
  Check 'M1' 'menu.view.split2 左右两窗格' ($r.ok -and (State).terminal.split -eq 2) "split=$((State).terminal.split)"
  $shotIndex += Shot 'menu-split2'
  $r = Act 'menu.view.split4' 'click'; Start-Sleep -Milliseconds 600
  Check 'M2' 'menu.view.split4 四窗格' ($r.ok -and (State).terminal.split -eq 4) "split=$((State).terminal.split)"
  $shotIndex += Shot 'menu-split4'
  $r = Act 'menu.view.split1' 'click'; Start-Sleep -Milliseconds 600
  Check 'M3' 'menu.view.split1 单窗格' ($r.ok -and (State).terminal.split -eq 1) "split=$((State).terminal.split)"

  # M4 新建串口通道对话框
  $before = @((Get-J '/ui-tree').controls).Count
  $r = Act 'menu.file.newSerial' 'click'; Start-Sleep -Milliseconds 600
  $st = State; $after = @((Get-J '/ui-tree').controls).Count
  Check 'M4' 'menu.file.newSerial 新建串口对话框' ($r.ok -and $st.ui.channel_dialog_open -eq $true -and (HasId 'dlg.channel.name')) "controls=$before->$after open=$($st.ui.channel_dialog_open)"
  $shotIndex += Shot 'dialog-new-serial'
  $c = Act 'dlg.channel.cancel' 'click'; Start-Sleep -Milliseconds 500
  $m5Open = (State).ui.channel_dialog_open
  Check 'M5' '新建串口对话框取消关闭' ($c.ok -and $m5Open -eq $false -and -not (HasId 'dlg.channel.name')) `
    "open=$m5Open controls=$(@((Get-J '/ui-tree').controls).Count)"

  # M6 新建 SSH 通道对话框
  $r = Act 'menu.file.newSsh' 'click'; Start-Sleep -Milliseconds 600
  $st = State
  Check 'M6' 'menu.file.newSsh 新建 SSH 对话框' ($r.ok -and $st.ui.channel_dialog_open -eq $true -and (HasId 'dlg.channel.host')) "open=$($st.ui.channel_dialog_open)"
  $shotIndex += Shot 'dialog-new-ssh'
  $c = Act 'dlg.channel.cancel' 'click'; Start-Sleep -Milliseconds 500
  Check 'M7' '新建 SSH 对话框取消关闭' ($c.ok -and (State).ui.channel_dialog_open -eq $false) "open=$((State).ui.channel_dialog_open)"

  # M8 编辑当前通道
  $r = Act 'menu.file.editChannel' 'click'; Start-Sleep -Milliseconds 600
  $st = State; $tree = Get-J '/ui-tree'
  $nameCtl = @($tree.controls | Where-Object { $_.id -eq 'dlg.channel.name' })[0]
  Check 'M8' 'menu.file.editChannel 编辑当前通道' ($r.ok -and $st.ui.channel_dialog_open -eq $true -and $nameCtl.text -eq 'loop-chars') "name=$($nameCtl.text)"
  $shotIndex += Shot 'dialog-edit-channel'
  $c = Act 'dlg.channel.cancel' 'click'; Start-Sleep -Milliseconds 500
  Check 'M9' '编辑通道对话框取消关闭' ($c.ok -and (State).ui.channel_dialog_open -eq $false) "open=$((State).ui.channel_dialog_open)"

  # M10 删除当前通道（只打开确认界面，不确认删除）
  $r = Act 'menu.file.deleteChannel' 'click'; Start-Sleep -Milliseconds 600
  $st = State; $tree = Get-J '/ui-tree'
  $delCtl = @($tree.controls | Where-Object { $_.id -eq 'dlg.channel.delete' })[0]
  Check 'M10' 'menu.file.deleteChannel 打开删除确认' ($r.ok -and $st.ui.channel_dialog_open -eq $true -and $null -ne $delCtl) "delete_btn=$($delCtl.enabled) 未执行删除"
  $shotIndex += Shot 'dialog-delete-channel'
  $c = Act 'dlg.channel.cancel' 'click'; Start-Sleep -Milliseconds 500
  Check 'M11' '删除确认取消（未删通道）' ($c.ok -and (State).ui.channel_dialog_open -eq $false -and @((State).channels).Count -eq 5) "channels=$(@((State).channels).Count)"

  # M12 SFTP 面板
  $r = Act 'menu.file.sftp' 'click'; Start-Sleep -Milliseconds 800
  $st = State
  Check 'M12' 'menu.file.sftp 打开 SFTP' ($r.ok -and $st.ui.sftp_open -eq $true -and (HasId 'sftp.local.list') -and (HasId 'sftp.remote.list')) "open=$($st.ui.sftp_open)"
  $shotIndex += Shot 'dialog-sftp'
  # SFTP 内部页签/工具：本地刷新、远端刷新、打开历史、打开设置
  $f1 = Act 'sftp.local.refresh' 'click'; Start-Sleep -Milliseconds 400
  $f2 = Act 'sftp.remote.refresh' 'click'; Start-Sleep -Milliseconds 400
  Check 'M13' 'SFTP 本地/远端刷新' ($f1.ok -and $f2.ok) "local=$($f1.ok) remote=$($f2.ok)"
  $h = Act 'sftp.history.open' 'click'; Start-Sleep -Milliseconds 500
  Check 'M14' 'SFTP 传输历史打开' ($h.ok -and (HasId 'sftp.history.list')) "history=$($h.ok)"
  $shotIndex += Shot 'dialog-sftp-history'
  $hc = Act 'sftp.history.close' 'click'; Start-Sleep -Milliseconds 400
  $set = Act 'sftp.settings.open' 'click'; Start-Sleep -Milliseconds 500
  Check 'M15' 'SFTP 设置打开' ($set.ok -and (HasId 'sftp.settings.speed')) "settings=$($set.ok)"
  $shotIndex += Shot 'dialog-sftp-settings'
  $sc = Act 'sftp.settings.close' 'click'; Start-Sleep -Milliseconds 300
  $c = Act 'sftp.close' 'click'; Start-Sleep -Milliseconds 600
  Check 'M16' 'SFTP 面板关闭' ($c.ok -and (State).ui.sftp_open -eq $false) "open=$((State).ui.sftp_open)"

  # M17 继电器配置（三个页签）
  $r = Act 'menu.relay.config' 'click'; Start-Sleep -Milliseconds 700
  $st = State
  Check 'M17' 'menu.relay.config 打开继电器' ($r.ok -and $st.ui.relay_open -eq $true -and (HasId 'relay.dev.list')) "open=$($st.ui.relay_open)"
  $shotIndex += Shot 'panel-relay-tab0'
  $tab1 = Act 'relay.tabs' 'select' $null 1; Start-Sleep -Milliseconds 500
  Check 'M18' '继电器页签 1（绑定）' ($tab1.ok -and (HasId 'relay.bind.list')) "ok=$($tab1.ok)"
  $shotIndex += Shot 'panel-relay-tab1'
  $tab2 = Act 'relay.tabs' 'select' $null 2; Start-Sleep -Milliseconds 500
  Check 'M19' '继电器页签 2（脚本）' ($tab2.ok -and (HasId 'relay.script.body')) "ok=$($tab2.ok)"
  $shotIndex += Shot 'panel-relay-tab2'
  $c = Act 'relay.close' 'click'; Start-Sleep -Milliseconds 600
  Check 'M20' '继电器面板关闭' ($c.ok -and (State).ui.relay_open -eq $false) "open=$((State).ui.relay_open)"

  # M21 文件服务
  $r = Act 'menu.fileService.config' 'click'; Start-Sleep -Milliseconds 700
  $st = State
  Check 'M21' 'menu.fileService.config 打开文件服务' ($r.ok -and $st.ui.filesvc_open -eq $true -and (HasId 'filesvc.root') -and (HasId 'filesvc.tftp.status')) "open=$($st.ui.filesvc_open)"
  $shotIndex += Shot 'panel-file-service'
  # 只切换服务开关状态再恢复，不实际启动
  $t0 = Act 'filesvc.tftp.toggle' 'click'; Start-Sleep -Milliseconds 500
  $t1 = Act 'filesvc.tftp.toggle' 'click'; Start-Sleep -Milliseconds 500
  Check 'M22' 'TFTP 开关可切换并恢复' ($t0.ok -and $t1.ok -and (State).services.tftp.on -eq $false) "on=$((State).services.tftp.on)"
  $s0 = Act 'filesvc.share.toggle' 'click'; Start-Sleep -Milliseconds 500
  $s1 = Act 'filesvc.share.toggle' 'click'; Start-Sleep -Milliseconds 500
  Check 'M23' 'SMB 共享开关可切换并恢复' ($s0.ok -and $s1.ok -and (State).services.share.on -eq $false) "on=$((State).services.share.on)"
  $n0 = Act 'filesvc.nfs.toggle' 'click'; Start-Sleep -Milliseconds 500
  $n1 = Act 'filesvc.nfs.toggle' 'click'; Start-Sleep -Milliseconds 500
  Check 'M24' 'NFS 开关可切换并恢复' ($n0.ok -and $n1.ok -and (State).services.nfs.on -eq $false) "on=$((State).services.nfs.on)"
  $shotIndex += Shot 'panel-file-service-toggled'
  $c = Act 'filesvc.close' 'click'; Start-Sleep -Milliseconds 600
  Check 'M25' '文件服务面板关闭' ($c.ok -and (State).ui.filesvc_open -eq $false) "open=$((State).ui.filesvc_open)"

  # M26 板控按钮配置（进入继电器脚本页）
  $r = Act 'menu.boardButtons.config' 'click'; Start-Sleep -Milliseconds 700
  $st = State
  Check 'M26' 'menu.boardButtons.config 板控按钮配置' ($r.ok -and $st.ui.relay_open -eq $true -and (HasId 'relay.script.body')) "open=$($st.ui.relay_open)"
  $shotIndex += Shot 'panel-board-buttons'
  $c = Act 'relay.close' 'click'; Start-Sleep -Milliseconds 600
  Check 'M27' '板控按钮配置关闭' ($c.ok -and (State).ui.relay_open -eq $false) "open=$((State).ui.relay_open)"

  # M28 检查更新（当前运行目录若无 tools/update.ps1，只应提示缺失，不应崩）
  $r = Act 'menu.help.checkUpdate' 'click'; Start-Sleep -Milliseconds 900
  $alive = (Get-Process ssh_bridge3 -ErrorAction SilentlyContinue) -ne $null
  Check 'M28' 'menu.help.checkUpdate 入口可执行且不崩溃' ($r.ok -and $alive) "note=$(StateNote)"
  $shotIndex += Shot 'menu-check-update'

  $finalCount = @((Get-J '/ui-tree').controls).Count
  Check 'M29' '所有对话框关闭后控件数回到基线' ($finalCount -eq $baseCount) "controls=$baseCount->$finalCount"
}

$summary = [pscustomobject]@{
  stage = $Stage; passed = @($script:rows | Where-Object ok).Count; total = @($script:rows).Count
  bad = @($script:rows | Where-Object { -not $_.ok }).Count; rows = $script:rows
  shots = @($shotIndex); started = $started.ToString('o'); finished = (Get-Date).ToString('o')
}
$file = Join-Path $OutDir ("desktop-{0}-result.json" -f $Stage)
$summary | ConvertTo-Json -Depth 6 | Set-Content -Path $file -Encoding UTF8
Write-Host ("SUMMARY stage={0} passed={1}/{2} bad={3}" -f $Stage, $summary.passed, $summary.total, $summary.bad)
Write-Host ("RESULT " + $file)

