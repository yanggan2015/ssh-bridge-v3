param(
    [string]$Out = 'C:\code\ssh-bridge-windows3\output\acceptance\s8\s8-panel.png',
    [string]$Id = 'sftp.status'
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PW3 {
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[PW3]::SetProcessDPIAware() | Out-Null
$base = 'http://127.0.0.1:18096'
$tree = Invoke-RestMethod "$base/ui-tree" -TimeoutSec 10
$ctl = $tree.controls | Where-Object { $_.id -eq $Id } | Select-Object -First 1
$h = [IntPtr]::new([Int64]$ctl.hwnd)
$panel = [PW3]::GetParent($h)
$wr = New-Object PW3+RECT
[PW3]::GetWindowRect($panel, [ref]$wr) | Out-Null
$w = $wr.Right - $wr.Left
$hh = $wr.Bottom - $wr.Top
Write-Output "panel rect=$($wr.Left),$($wr.Top),$($wr.Right),$($wr.Bottom) size=${w}x${hh}"
$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[PW3]::PrintWindow($panel, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$g.Dispose()
$out = $Out
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output $out
