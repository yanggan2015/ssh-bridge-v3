param(
    [string]$Out = 'C:\code\ssh-bridge-windows3\output\acceptance\s13\window.png',
    [string]$Id = '',
    [int]$W = 0,
    [int]$H = 0,
    [string]$Base = 'http://127.0.0.1:18086'
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PW4 {
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[PW4]::SetProcessDPIAware() | Out-Null
$tree = Invoke-RestMethod "$Base/ui-tree" -TimeoutSec 10
$ctl = if ($Id) { $tree.controls | Where-Object { $_.id -eq $Id } | Select-Object -First 1 } else { $tree.controls | Select-Object -First 1 }
$root = [PW4]::GetAncestor([IntPtr]::new([Int64]$ctl.hwnd), 2)
if ($W -gt 0 -and $H -gt 0) {
    [PW4]::SetWindowPos($root, [IntPtr]::Zero, 0, 0, $W, $H, 0x0014) | Out-Null
    Start-Sleep -Milliseconds 500
}
$wr = New-Object PW4+RECT
[PW4]::GetWindowRect($root, [ref]$wr) | Out-Null
$w = $wr.Right - $wr.Left
$hh = $wr.Bottom - $wr.Top
$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[PW4]::PrintWindow($root, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$g.Dispose()
New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "root=$root size=${w}x${hh} -> $Out"
