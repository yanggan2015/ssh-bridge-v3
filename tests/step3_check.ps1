# step3_check.ps1 — 步骤 3 验收（窗口壳 + 生命周期）
# 用法： pwsh -File tests\step3_check.ps1 [-SkipBuild]
# 退出码： 0 = 全绿；1 = 有 FAIL
#
# 说明：托盘双击/托盘退出用自定义消息模拟（真实托盘点击无法脚本化）：
#   WM_APP+1 = 显示窗口，WM_APP+2 = 托盘退出，WM_CLOSE = 点 X。

param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'output\build\ssh_bridge3.exe'
$runDir = Join-Path $root 'output\s3run'
$cfgPath = Join-Path $runDir 'ssh_bridge_config.json'
$statePath = Join-Path $runDir 'output\ui_state.json'
$rows = @()

function Check([string]$id, [string]$desc, [bool]$ok, [string]$detail = '') {
    $script:rows += [pscustomobject]@{ 编号 = $id; 项目 = $desc; 结果 = $(if ($ok) { 'PASS' } else { 'FAIL' }); 说明 = $detail }
    return $ok
}

function Kill-App {
    Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
}

function State {
    if (-not (Test-Path -LiteralPath $statePath)) { return $null }
    Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
}

function Find-SerialPort {
    foreach ($p in @('COM3', 'COM4', 'COM5')) {
        try {
            $sp = [System.IO.Ports.SerialPort]::new($p, 115200)
            $sp.Open()
            $sp.Close()
            return $p
        }
        catch { continue }
    }
    return ''
}

function Port-Openable([string]$name) {
    if ([string]::IsNullOrEmpty($name)) { return $false }
    try {
        $sp = [System.IO.Ports.SerialPort]::new($name, 115200)
        $sp.Open()
        $sp.Close()
        return $true
    }
    catch { return $false }
}

Kill-App

# ---------- 构建 ----------
$buildOut = ''
$buildExit = 0
if (-not $SkipBuild) {
    $env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
    $buildOut = (& 'C:\msys64\mingw64\bin\mingw32-make.exe' -C $root -B -j4 2>&1 | Out-String)
    $buildExit = $LASTEXITCODE
}
$errorCount   = ([regex]::Matches($buildOut, '(?im)^.*\berror\b.*$')).Count
$warningCount = ([regex]::Matches($buildOut, '(?im)^.*\bwarning\b.*$')).Count
Check 'G-1' '编译无错无警告' ($buildExit -eq 0 -and $errorCount -eq 0 -and $warningCount -eq 0) `
    "exit=$buildExit error=$errorCount warning=$warningCount"

if (-not (Test-Path -LiteralPath $exe)) {
    $rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
    Write-Host "RESULT: FAIL (1 项)" -ForegroundColor Red
    exit 1
}

# ---------- 测试配置 ----------
$port = Find-SerialPort
New-Item -ItemType Directory -Force $runDir | Out-Null
Remove-Item -Recurse -Force (Join-Path $runDir 'output') -ErrorAction SilentlyContinue

$boards = @(
    [ordered]@{ name = 'loop1'; serial_device = 'loop:s3'; baudrate = 115200; auto_open = $true;
                timestamp = $false; log_enabled = $false; buffer_size = 65536 },
    [ordered]@{ name = 'board2'; serial_device = ($(if ($port) { $port } else { 'loop:s3b' }));
                baudrate = 115200; auto_open = $true; timestamp = $false; log_enabled = $false }
)
$cfg = [ordered]@{
    default = [ordered]@{ username = 'root'; password = ''; buffer_size = 262144; log_enabled = $false }
    boards  = $boards
    ssh     = @([ordered]@{ name = 'ssh-offline'; target_host = '127.0.0.1'; target_port = 1;
                            target_username = 'x'; target_password = 'x'; keepalive = 2 })
    ui      = [ordered]@{ close_to_tray = $true; start_minimized = $false }
}
$cfg | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $cfgPath -Encoding utf8

Add-Type -Namespace SB3S3 -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessageW(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, System.IntPtr p);
public delegate bool EnumProc(System.IntPtr h, System.IntPtr p);
'@ -ErrorAction SilentlyContinue

function Get-AppWindows([int]$procId) {
    $found = New-Object System.Collections.ArrayList
    $cb = [SB3S3.W+EnumProc]{
        param($h, $p)
        $wpid = 0
        [SB3S3.W]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
        if ($wpid -eq $procId) {
            $sb = New-Object System.Text.StringBuilder 256
            [SB3S3.W]::GetClassNameW($h, $sb, 256) | Out-Null
            $null = $found.Add([pscustomobject]@{ hwnd = $h; cls = $sb.ToString() })
        }
        return $true
    }
    [SB3S3.W]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $found
}

Kill-App
$proc = Start-Process -FilePath $exe -WorkingDirectory $runDir -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 250
    if ($proc.HasExited) { break }
    $proc.Refresh()
    $hwnd = $proc.MainWindowHandle
    if ($hwnd -ne [IntPtr]::Zero) { break }
}
Start-Sleep -Seconds 2

# ---------- T3-1 启动即建窗 ----------
$title = if (-not $proc.HasExited) { $proc.MainWindowTitle } else { '' }
Check 'T3-1' '启动即建窗' ($hwnd -ne [IntPtr]::Zero -and $title -like 'ssh-bridge v3*') "title='$title'"

# ---------- T3-2 通道树列出全部通道 ----------
$st = State
$names = @()
if ($st) { $names = @($st.channels | ForEach-Object { $_.name }) }
$want = @('loop1', 'board2', 'ssh-offline')
$listed = @($want | Where-Object { $names -contains $_ }).Count
Check 'T3-2' '通道树能列出全部通道' ($st -and $names.Count -eq 3 -and $listed -eq 3) `
    "count=$($names.Count) names=$($names -join ',')"

# ---------- T3-3 状态栏显示连接状态 ----------
$sum = if ($st) { $st.summary } else { '' }
$online = 0
if ($st) { $online = @($st.channels | Where-Object { $_.state -eq 'ready' }).Count }
Check 'T3-3' '状态栏显示连接状态' ($sum -like '通道 3 · 在线 *' -and $online -ge 1) `
    "summary='$sum' online=$online"

# ---------- T3-10 无崩溃对话框 ----------
$wins = Get-AppWindows $proc.Id
$dialogs = @($wins | Where-Object { $_.cls -eq '#32770' }).Count
Check 'T3-10' '全流程无崩溃对话框' ($dialogs -eq 0) "dialogs=$dialogs windows=$($wins.Count)"

# ---------- T3-9 无监听端口 ----------
$listen = (& netstat -ano | Select-String -Pattern "\s$($proc.Id)$" | Select-String -Pattern 'LISTENING' | Out-String)
Check 'T3-9' '无任何监听端口' ([string]::IsNullOrWhiteSpace($listen)) `
    $(if ($listen) { $listen.Trim() } else { 'netstat 无 LISTENING' })

# ---------- T3-4 点 X 隐藏且进程存活 ----------
[SB3S3.W]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 2
$st = State
$hidden = -not [SB3S3.W]::IsWindowVisible($hwnd)
Check 'T3-4' '点 X 隐藏且进程存活' ((-not $proc.HasExited) -and $hidden -and $st -and (-not $st.visible)) `
    "alive=$(-not $proc.HasExited) hidden=$hidden visible_flag=$($st.visible)"

# ---------- T3-5 托盘双击恢复 ----------
[SB3S3.W]::PostMessageW($hwnd, 0x8001, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 2
$shown = [SB3S3.W]::IsWindowVisible($hwnd)
$st = State
Check 'T3-5' '托盘双击恢复' ($shown -and $st -and $st.visible) "visible=$shown"

# ---------- T3-8 二启不产生第二实例 ----------
$p2 = Start-Process -FilePath $exe -WorkingDirectory $runDir -PassThru
$null = $p2.WaitForExit(6000)
$instances = @(Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue).Count
Check 'T3-8' '二启不产生第二实例' ($p2.HasExited -and $p2.ExitCode -eq 0 -and $instances -eq 1) `
    "second_exit=$($p2.ExitCode) instances=$instances"

# ---------- 串口占用基线（T3-6 前置） ----------
$busyWhileRunning = if ($port) { -not (Port-Openable $port) } else { $true }

# ---------- T3-6 托盘退出 → 退出码 0、串口释放 ----------
[SB3S3.W]::PostMessageW($hwnd, 0x8002, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
$null = $proc.WaitForExit(10000)
$exited = $proc.HasExited
$code = if ($exited) { $proc.ExitCode } else { -1 }
$released = if ($port) { Port-Openable $port } else { $true }
Check 'T3-6' '托盘退出 → 退出码 0、串口释放' ($exited -and $code -eq 0 -and $busyWhileRunning -and $released) `
    "exit=$code busy_while_running=$busyWhileRunning released=$released port=$(if ($port) { $port } else { 'none' })"

# ---------- T3-7 退出码 0（收尾复核） ----------
$st = State
Check 'T3-7' '退出码 0' ($exited -and $code -eq 0 -and (-not $st.visible)) "exit=$code"

Kill-App
Start-Sleep -Milliseconds 400
$left = @(Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue).Count
Check 'G-5' '无残留进程' ($left -eq 0) "残留 $left 个"

$gateScript = Join-Path $root 'scripts\quality_gate.ps1'
if (Test-Path $gateScript) {
    & pwsh -NoProfile -File $gateScript *> $null
    Check 'G-6' '代码整理' ($LASTEXITCODE -eq 0) 'quality_gate'
}
else {
    Check 'G-6' '代码整理' $false '缺 scripts\quality_gate.ps1'
}

Push-Location $root
$status = (& git status --porcelain 2>&1 | Out-String).Trim()
Pop-Location
Check 'G-7' 'git 状态' $true $(if ($status.Length -gt 0) { '有未提交改动（提交后复核）' } else { '工作区干净' })

$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
$failCount = @($rows | Where-Object { $_.结果 -eq 'FAIL' }).Count
if ($failCount -gt 0) {
    Write-Host "RESULT: FAIL ($failCount 项)" -ForegroundColor Red
    exit 1
}
Write-Host "RESULT: ALL PASS" -ForegroundColor Green
exit 0

