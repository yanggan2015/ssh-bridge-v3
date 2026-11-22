# ui_layout_check.ps1 — 控件遮挡 / 出界检查（真机）
# 用法： pwsh -NoProfile -File tests\ui_layout_check.ps1 [-DialogClass SB3_ChannelDialog]
#
# 做法：主窗口 + 可选附属对话框，各自递归枚举可见子控件，取屏幕矩形：
#   ① 出界：任一控件超出所属窗口矩形即 FAIL（被边缘裁掉，用户看不到/点不到）
#   ② 遮挡：两两求交，交集占较小控件 >0.5% 即 FAIL
# 分组框（Button 且标题以“参数”结尾）是容器，按设计压在它上面的字段不算遮挡。

param(
    [string]$ProcName = 'ssh_bridge3',
    [string]$DialogClass = '',
    [double]$OverlapRatio = 0.005
)

$ErrorActionPreference = 'Stop'

if (-not ('Win32Rect' -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public struct RECTW { public int Left, Top, Right, Bottom; }
public class Win32Rect {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECTW r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr lp);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    public static IntPtr[] Children(IntPtr parent) {
        var list = new System.Collections.Generic.List<IntPtr>();
        EnumChildWindows(parent, (h, lp) => { list.Add(h); return true; }, IntPtr.Zero);
        return list.ToArray();
    }
}
"@
}

function Get-VisibleRows([IntPtr]$root) {
    $rows = @()
    foreach ($h in [Win32Rect]::Children($root)) {
        if (-not [Win32Rect]::IsWindowVisible($h)) { continue }
        $r = New-Object RECTW
        if (-not [Win32Rect]::GetWindowRect($h, [ref]$r)) { continue }
        if (($r.Right - $r.Left) -le 2 -or ($r.Bottom - $r.Top) -le 2) { continue }
        $sb = New-Object System.Text.StringBuilder 256
        [void][Win32Rect]::GetClassName($h, $sb, 256)
        $tb = New-Object System.Text.StringBuilder 256
        [void][Win32Rect]::GetWindowText($h, $tb, 256)
        # 可编辑下拉的 EDIT 是 ComboBox 的内部编辑区，父窗口就是下拉，不算兄弟控件
        $parent = [Win32Rect]::GetParent($h)
        if ($parent -ne [IntPtr]::Zero) {
            $pc = New-Object System.Text.StringBuilder 128
            [void][Win32Rect]::GetClassName($parent, $pc, 128)
            if ($pc.ToString() -eq 'ComboBox') { continue }
        }
        $isGroup = ($sb.ToString() -eq 'Button') -and ($tb.ToString() -match '参数$')
        $rows += [pscustomobject]@{
            hwnd  = [int64]$h
            cls   = $sb.ToString()
            text  = $tb.ToString()
            group = $isGroup
            left  = $r.Left; top = $r.Top; right = $r.Right; bottom = $r.Bottom
            w     = $r.Right - $r.Left
            h     = $r.Bottom - $r.Top
        }
    }
    return $rows
}

function Is-Ancestor([Int64]$a,[Int64]$b) {
    $h = [IntPtr]$b
    for ($k = 0; $k -lt 64; $k++) {
        $p = [Win32Rect]::GetParent($h)
        if ($p -eq [IntPtr]::Zero) { return $false }
        if ([Int64]$p -eq $a) { return $true }
        $h = $p
    }
    return $false
}

function Test-Root([IntPtr]$root, [string]$label) {
    $rows = Get-VisibleRows $root
    Write-Host ("[{0}] 可见控件 {1} 个（hwnd {2}）" -f $label, $rows.Count, $root)
    $rows | ForEach-Object {
        Write-Host ("  {0,-16} {1,-12} {2},{3}-{4},{5}  {6}x{7}" -f $_.cls, $_.text, $_.left, $_.top,
                    $_.right, $_.bottom, $_.w, $_.h)
    }

    $wr = New-Object RECTW
    [void][Win32Rect]::GetWindowRect($root, [ref]$wr)
    $bad = @()
    foreach ($r in $rows) {
        if ($r.left -lt $wr.Left -or $r.top -lt $wr.Top -or $r.right -gt $wr.Right -or $r.bottom -gt $wr.Bottom) {
            $bad += ("{0} {1} {2},{3}-{4},{5} 窗口 {6},{7}-{8},{9}" -f $r.cls, $r.text, $r.left, $r.top,
                     $r.right, $r.bottom, $wr.Left, $wr.Top, $wr.Right, $wr.Bottom)
        }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("FAIL  [{0}] {1} 个控件超出窗口边界：" -f $label, $bad.Count) -ForegroundColor Red
        $bad | ForEach-Object { Write-Host ("  " + $_) }
        return $false
    }

    $overlaps = @()
    for ($i = 0; $i -lt $rows.Count; $i++) {
        for ($j = $i + 1; $j -lt $rows.Count; $j++) {
            $a = $rows[$i]; $b = $rows[$j]
            if ($a.group -or $b.group) { continue }
            # 容器与内部子控件必然重叠（如 Tab 内的 TreeView），不算遮挡。
            # 用祖先链判断，避免把合法的父子嵌套误报成布局缺陷。
            if ((Is-Ancestor $a.hwnd $b.hwnd) -or (Is-Ancestor $b.hwnd $a.hwnd)) { continue }
            # Tab 控件是页面容器，页内控件与其矩形天然重合。
            if ($a.cls -eq 'SysTabControl32' -or $b.cls -eq 'SysTabControl32') { continue }
            $ix = [Math]::Max(0, [Math]::Min($a.right, $b.right) - [Math]::Max($a.left, $b.left))
            $iy = [Math]::Max(0, [Math]::Min($a.bottom, $b.bottom) - [Math]::Max($a.top, $b.top))
            if ($ix -le 0 -or $iy -le 0) { continue }
            $inter = $ix * $iy
            $small = [Math]::Min($a.w * $a.h, $b.w * $b.h)
            $ratio = $inter / $small
            if ($ratio -gt $OverlapRatio) {
                $overlaps += [pscustomobject]@{ a = $a.text; b = $b.text; aw = $a.cls; bw = $b.cls;
                                                w = $ix; h = $iy; ratio = [Math]::Round($ratio, 3) }
            }
        }
    }
    if ($overlaps.Count -gt 0) {
        Write-Host ("FAIL  [{0}] {1} 对控件相互遮挡：" -f $label, $overlaps.Count) -ForegroundColor Red
        $overlaps | ForEach-Object {
            Write-Host ("  {0}({1}) ↔ {2}({3}) 交集 {4}x{5}，占较小 {6:P1}" -f
                        $_.a, $_.aw, $_.b, $_.bw, $_.w, $_.h, $_.ratio)
        }
        return $false
    }
    Write-Host ("PASS  [{0}] 无遮挡、无出界" -f $label)
    return $true
}

$p = Get-Process -Name $ProcName -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Host 'FAIL  找不到主窗口（进程未运行）'; exit 1 }

$ok = Test-Root $p.MainWindowHandle '主窗口'

if ($DialogClass) {
    $dlg = [IntPtr]::Zero
    $pid_ = [uint32]$p.Id
    [void][Win32Rect]::EnumWindows({
        param($h, $lp)
        $sb = New-Object System.Text.StringBuilder 128
        [void][Win32Rect]::GetClassName($h, $sb, 128)
        if ($sb.ToString() -eq $DialogClass -and [Win32Rect]::IsWindowVisible($h)) {
            $pid2 = [uint32]0
            [void][Win32Rect]::GetWindowThreadProcessId($h, [ref]$pid2)
            if ($pid2 -eq $pid_) { $script:dlgFound = $h; return $false }
        }
        return $true
    }, [IntPtr]::Zero)
    $dlg = $script:dlgFound
    if ($dlg -eq $null -or $dlg -eq [IntPtr]::Zero) {
        Write-Host ("SKIP  未找到对话框 {0}（未打开）" -f $DialogClass)
    } else {
        $ok = (Test-Root $dlg '通道表单') -and $ok
    }
}

if ($ok) { Write-Host 'RESULT: LAYOUT PASS'; exit 0 }
Write-Host 'RESULT: LAYOUT FAIL' -ForegroundColor Red
exit 1




