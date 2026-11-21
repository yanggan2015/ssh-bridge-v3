# step4_check.ps1 — 步骤 4 验收（终端窗格：ANSI / 中文 / 分屏 / 搜索 / 灌流 / 隐藏不耗 CPU）
# 用法： pwsh -File tests\step4_check.ps1 [-SkipBuild] [-LongLoad]
# 退出码： 0 = 全绿；1 = 有 FAIL
#
# 观测面：output\ui_terminal.json（分屏/通道/尺寸/尾部 40 行文本/字节数/命中位置/颜色样本）
# 说明：键盘输入用 SendMessage(WM_CHAR/WM_KEYDOWN) 打进终端子窗，等价于真的敲键；
#       分屏、清屏、搜索都用菜单命令或控件消息驱动，与真人操作走同一条代码路径。

param([switch]$SkipBuild, [switch]$LongLoad)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'output\build\ssh_bridge3.exe'
$runDir = Join-Path $root 'output\s4check'
$cfgPath = Join-Path $runDir 'ssh_bridge_config.json'
$statePath = Join-Path $runDir 'output\ui_state.json'
$termPath = Join-Path $runDir 'output\ui_terminal.json'
$rows = @()

function Check([string]$id, [string]$desc, [bool]$ok, [string]$detail = '') {
    $script:rows += [pscustomobject]@{ 编号 = $id; 项目 = $desc; 结果 = $(if ($ok) { 'PASS' } else { 'FAIL' }); 说明 = $detail }
    return $ok
}

function Kill-App {
    Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
}

function Kill-Server {
    Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like '*ssh_test_server*' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

Add-Type -Namespace SB3S4 -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessageW(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
[DllImport("user32.dll")] public static extern System.IntPtr SendMessageW(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern System.IntPtr FindWindowExW(System.IntPtr p, System.IntPtr a, string cls, string title);
[DllImport("user32.dll")] public static extern System.IntPtr GetDlgItem(System.IntPtr h, int id);
[DllImport("user32.dll")] public static extern bool GetClientRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, System.IntPtr p);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowTextW(System.IntPtr h, string t);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
public delegate bool EnumProc(System.IntPtr h, System.IntPtr p);
'@ -ErrorAction SilentlyContinue

function State { if (Test-Path -LiteralPath $statePath) { Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json } else { $null } }
function Term  { if (Test-Path -LiteralPath $termPath) { Get-Content -LiteralPath $termPath -Raw | ConvertFrom-Json } else { $null } }

$script:main = [IntPtr]::Zero
$script:term = [IntPtr]::Zero

function Dump-Term {
    [SB3S4.W]::PostMessageW($script:main, 0x8006, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 400
    return (Term)
}

function Send-Char([char]$c) {
    [SB3S4.W]::SendMessageW($script:term, 0x0102, [IntPtr][int]$c, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 25
}

function Send-Key([int]$vk) {
    [SB3S4.W]::SendMessageW($script:term, 0x0100, [IntPtr]$vk, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 25
}

function Send-Text([string]$text) {
    foreach ($ch in $text.ToCharArray()) { Send-Char $ch }
}

function Select-Channel([int]$index) {
    # WM_APP+8：让壳选中第 index 条通道（内部与点树同一条路径）
    [SB3S4.W]::PostMessageW($script:main, 0x8008, [IntPtr]$index, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 800
    return $true
}

function Click-View([double]$fx, [double]$fy) {
    $r = New-Object SB3S4.W+RECT
    [SB3S4.W]::GetClientRect($script:term, [ref]$r) | Out-Null
    $x = [int](($r.R - $r.L) * $fx)
    $y = [int](($r.B - $r.T) * $fy)
    $lp = [IntPtr](($y -shl 16) -bor $x)
    [SB3S4.W]::SendMessageW($script:term, 0x0201, [IntPtr]1, $lp) | Out-Null   # WM_LBUTTONDOWN
    Start-Sleep -Milliseconds 60
    [SB3S4.W]::SendMessageW($script:term, 0x0202, [IntPtr]0, $lp) | Out-Null   # WM_LBUTTONUP
    Start-Sleep -Milliseconds 200
}

function Menu-Cmd([int]$id) {
    [SB3S4.W]::PostMessageW($script:main, 0x0111, [IntPtr]$id, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 500
}

function Get-AppWindows([int]$procId) {
    $found = New-Object System.Collections.ArrayList
    $cb = [SB3S4.W+EnumProc]{
        param($h, $p)
        $wpid = 0
        [SB3S4.W]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
        if ($wpid -eq $procId) {
            $sb = New-Object System.Text.StringBuilder 256
            [SB3S4.W]::GetClassNameW($h, $sb, 256) | Out-Null
            $null = $found.Add([pscustomobject]@{ hwnd = $h; cls = $sb.ToString() })
        }
        return $true
    }
    [SB3S4.W]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $found
}

Kill-App
Kill-Server

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

# ---------- 测试环境 ----------
New-Item -ItemType Directory -Force $runDir | Out-Null
Remove-Item -Recurse -Force (Join-Path $runDir 'output') -ErrorAction SilentlyContinue
$ready = Join-Path $runDir 'ready.txt'
$banner = Join-Path $runDir 'banner.bin'
Remove-Item -LiteralPath $ready -ErrorAction SilentlyContinue
$bannerBytes = "\x1b[31mRED-BANNER\x1b[0m`r`n中文 OK`r`n"
[System.IO.File]::WriteAllBytes($banner, [System.Text.Encoding]::UTF8.GetBytes($bannerBytes))

$floodLines = 110000
$srv = Start-Process -FilePath 'python' -WorkingDirectory $root -PassThru -WindowStyle Hidden -ArgumentList @(
    'tests\tools\ssh_test_server.py', '--port', '0', '--mode', 'echo',
    '--ready-file', $ready, '--banner-file', $banner, '--flood-lines', "$floodLines"
)
$port = ''
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 250
    if (Test-Path -LiteralPath $ready) { $port = (Get-Content -LiteralPath $ready -Raw).Trim(); break }
}
Check 'G-2' '测试 SSH 服务器就绪' (-not [string]::IsNullOrEmpty($port)) "port=$port"
if ([string]::IsNullOrEmpty($port)) { Kill-Server; exit 1 }

$cfg = [ordered]@{
    default = [ordered]@{ username = 'root'; password = 'x'; buffer_size = 4194304; log_enabled = $false }
    boards  = @(
        [ordered]@{ name = 'loop1'; serial_device = 'loop:s4'; baudrate = 115200; auto_open = $true;
                    timestamp = $false; log_enabled = $false; buffer_size = 131072 },
        [ordered]@{ name = 'board2'; serial_device = 'COM3'; baudrate = 115200; auto_open = $true;
                    timestamp = $false; log_enabled = $false }
    )
    ssh     = @([ordered]@{ name = 'ssh-term'; target_host = '127.0.0.1'; target_port = [int]$port;
                            target_username = 'u'; target_password = 'p'; keepalive = 2;
                            buffer_size = 4194304; log_enabled = $false })
    ui      = [ordered]@{ close_to_tray = $true; start_minimized = $false; scrollback_lines = 5000;
                          font_size = 15; split = 1 }
}
$cfg | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $cfgPath -Encoding utf8

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
$script:main = $hwnd
$script:term = [SB3S4.W]::FindWindowExW($hwnd, [IntPtr]::Zero, 'SB3_TermView', $null)
Check 'T4-0' '启动即建终端视图' ($hwnd -ne [IntPtr]::Zero -and $script:term -ne [IntPtr]::Zero) `
    "main=$hwnd term=$($script:term)"

if ($script:term -eq [IntPtr]::Zero) {
    Kill-App; Kill-Server
    $rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
    Write-Host "RESULT: FAIL" -ForegroundColor Red
    exit 1
}

# 等 SSH 通道把灌流数据接住
Start-Sleep -Seconds 6

# ---------- T4-1 首次打开显示尾部历史 ----------
$null = Select-Channel 2
$t = Dump-Term
$pane = $t.panes[0]
$wantLast = 'L{0:D6}' -f ($floodLines - 1)
for ($i = 0; $i -lt 24; $i++) {
    $tail = @($pane.lines | Where-Object { $_ -ne '' })
    $lastNonEmpty = if ($tail.Count -gt 0) { $tail[-1] } else { '' }
    if ($lastNonEmpty -eq $wantLast -and $pane.scrollback -gt 0) { break }
    Start-Sleep -Milliseconds 800
    $t = Dump-Term
    $pane = $t.panes[0]
}
Check 'T4-1' '首次打开显示尾部历史' ($pane.channel -eq 'ssh-term' -and $pane.scrollback -gt 0 -and `
    $lastNonEmpty -eq ('L{0:D6}' -f ($floodLines - 1))) `
    "channel=$($pane.channel) scrollback=$($pane.scrollback) last='$lastNonEmpty'"

# ---------- T4-3 大批量灌流不丢字节 ----------
$expectMin = $floodLines * 9
$bytes = [double]$pane.bytes
Check 'T4-3' '灌流字节全部被解析（不丢行）' ($bytes -ge $expectMin -and $pane.total -ge 5000) `
    "bytes=$([long]$bytes) expect>=$expectMin total=$($pane.total)"

if ($LongLoad) {
    $cpu0 = $proc.TotalProcessorTime.TotalMilliseconds
    Start-Sleep -Seconds 10
    $proc.Refresh()
    $cpu1 = $proc.TotalProcessorTime.TotalMilliseconds
    $rate = [math]::Round(($bytes / 1024.0 / 1024.0) / 10.0, 2)
    Check 'T4-3L' '长跑负载下 CPU 与吞吐' (($cpu1 - $cpu0) -lt 8000) "cpu_delta=$([int]($cpu1-$cpu0))ms 约 $rate MB/10s"
}

# ---------- T4-5 / T4-6 ANSI 颜色 + 中文 ----------
$null = Select-Channel 0   # loop1
Start-Sleep -Milliseconds 800
Send-Key 0x1B              # ESC
Send-Text '[31mRED'
Send-Char ([char]13)       # 回车（CR）
Start-Sleep -Milliseconds 900
$t = Dump-Term
$pane = $t.panes[0]
$joined = ($pane.lines -join "`n")
$hasEsc = $joined -match [regex]::Escape('[31m')
$hasRed = $joined -match 'RED'
$fg = @($pane.fg_sample)
Send-Text '中文 OK'
Send-Char ([char]13)
Start-Sleep -Milliseconds 900
$t = Dump-Term
$pane = $t.panes[0]
$hasCjk = (($pane.lines -join "`n") -match '中文 OK')
Check 'T4-5' 'ANSI 颜色生效且不显示转义原文' ($hasRed -and (-not $hasEsc) -and ($fg -contains 'cd0000')) `
    "red=$hasRed raw_esc=$hasEsc fg=$($fg -join ',')"
Check 'T4-6' '中文不乱码' $hasCjk "line='$($pane.lines | Where-Object { $_ -match '中文' } | Select-Object -First 1)'"

# ---------- T4-2 分屏独立游标 ----------
Menu-Cmd 40031
$t = Dump-Term
Check 'T4-2a' '切成左右两窗格' ($t.split -eq 2) "split=$($t.split)"

Click-View 0.25 0.5        # 左窗格 → 活动
$null = Select-Channel 0   # loop1
Start-Sleep -Milliseconds 800
Click-View 0.75 0.5        # 右窗格 → 活动
$null = Select-Channel 1   # board2（真实串口，无回显）
Start-Sleep -Milliseconds 800
Send-Text 'beta-only-right'
Send-Char ([char]13)
Start-Sleep -Milliseconds 800
$t = Dump-Term
$left  = ($t.panes[0].lines -join "`n")
$right = ($t.panes[1].lines -join "`n")
Check 'T4-2' '分屏独立游标不串数据' ($t.panes[0].channel -eq 'loop1' -and $t.panes[1].channel -eq 'board2' -and `
    $left -notmatch 'beta-only-right' -and $right -notmatch 'RED') `
    "lch=$($t.panes[0].channel) rch=$($t.panes[1].channel) rightHasBeta=$($right -match 'beta')"

Click-View 0.25 0.5
Start-Sleep -Milliseconds 300
Send-Text 'gamma-left'
Send-Char ([char]13)
Start-Sleep -Milliseconds 800
$t = Dump-Term
$left2  = ($t.panes[0].lines -join "`n")
$right2 = ($t.panes[1].lines -join "`n")
Check 'T4-2b' '左窗格输入只出现在左窗格' ($left2 -match 'gamma-left' -and $right2 -notmatch 'gamma-left') `
    "leftHasGamma=$($left2 -match 'gamma') rightHasGamma=$($right2 -match 'gamma')"

# ---------- T4-4 清屏只清本窗格 ----------
Click-View 0.75 0.5
$null = Select-Channel 0
Start-Sleep -Milliseconds 800
$t = Dump-Term
$rightBefore = @($t.panes[1].lines | Where-Object { $_ -ne '' }).Count
Click-View 0.25 0.5
Menu-Cmd 40036
$t = Dump-Term
$leftAfter  = @($t.panes[0].lines | Where-Object { $_ -ne '' }).Count
$rightAfter = @($t.panes[1].lines | Where-Object { $_ -ne '' }).Count
Check 'T4-4' '清屏后本窗格回看为空且不影响他人' ($leftAfter -eq 0 -and $rightAfter -ge $rightBefore -and $rightBefore -gt 0) `
    "left=$leftAfter right=$rightAfter(efore=$rightBefore)"

# ---------- T4-7 搜索命中可定位 ----------
Click-View 0.75 0.5
$search = [SB3S4.W]::GetDlgItem($script:main, 1013)
$null = Select-Channel 0
Start-Sleep -Milliseconds 600
Send-Text 'needle-marker'
Send-Char ([char]13)
Start-Sleep -Milliseconds 800
# 跨进程 SetWindowTextW 对 EDIT 不生效（WM_SETTEXT 的字符串无法跨进程编组），
# 改为像真人一样逐字敲进搜索框，让 EDIT 自己发 EN_CHANGE。
foreach ($ch in 'needle-marker'.ToCharArray()) {
    [SB3S4.W]::SendMessageW($search, 0x0102, [IntPtr][int]$ch, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 20
}
Start-Sleep -Milliseconds 600
$t = Dump-Term
$hit = $null
foreach ($p in $t.panes) { if ($p.search_hit) { $hit = $p; break } }
Check 'T4-7' '搜索命中可定位' ($null -ne $hit -and $hit.hit.len -eq 13) `
    $(if ($hit) { "abs=$($hit.hit.abs) col=$($hit.hit.col) len=$($hit.hit.len)" } else { '未命中' })

# ---------- T4-8 输入无延迟堆积 ----------
$t0 = Dump-Term
$before = [double]$t0.panes[1].bytes
$payload = ''
for ($i = 0; $i -lt 6; $i++) { $payload += 'xxxxxxxxxx' }
foreach ($ch in $payload.ToCharArray()) {
    [SB3S4.W]::SendMessageW($script:term, 0x0102, [IntPtr][int]$ch, [IntPtr]::Zero) | Out-Null
}
Start-Sleep -Milliseconds 1500
$t1 = Dump-Term
$after = [double]$t1.panes[1].bytes
Check 'T4-8' '输入无延迟堆积' (($after - $before) -ge 60) "before=$([long]$before) after=$([long]$after)"

# ---------- T4-9 隐藏窗格不耗 CPU ----------
$proc.Refresh()
$cpu0 = $proc.TotalProcessorTime.TotalMilliseconds
[SB3S4.W]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 3
$proc.Refresh()
$cpu1 = $proc.TotalProcessorTime.TotalMilliseconds
$hidden = -not [SB3S4.W]::IsWindowVisible($hwnd)
Check 'T4-9' '隐藏窗格不耗 CPU' ($hidden -and (($cpu1 - $cpu0) -lt 600)) `
    "hidden=$hidden cpu_delta=$([int]($cpu1-$cpu0))ms/3s"

[SB3S4.W]::PostMessageW($hwnd, 0x8001, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 600

# ---------- G-3 无崩溃对话框 ----------
$wins = Get-AppWindows $proc.Id
$dialogs = @($wins | Where-Object { $_.cls -eq '#32770' }).Count
Check 'G-3' '全流程无崩溃对话框' ($dialogs -eq 0) "dialogs=$dialogs windows=$($wins.Count)"

# ---------- G-4 无监听端口 ----------
$listen = (& netstat -ano | Select-String -Pattern "\s$($proc.Id)$" | Select-String -Pattern 'LISTENING' | Out-String)
Check 'G-4' '本体不监听任何端口' ([string]::IsNullOrWhiteSpace($listen)) `
    $(if ($listen) { $listen.Trim() } else { 'netstat 无 LISTENING' })

# ---------- G-5 退出 ----------
[SB3S4.W]::PostMessageW($hwnd, 0x8002, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
$null = $proc.WaitForExit(10000)
$code = if ($proc.HasExited) { $proc.ExitCode } else { -1 }
Check 'G-5' '托盘退出 → 退出码 0' ($proc.HasExited -and $code -eq 0) "exit=$code"

Kill-App
Kill-Server
Start-Sleep -Milliseconds 400
$left = @(Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue).Count
$pyLeft = @(Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
    Where-Object { $_.CommandLine -like '*ssh_test_server*' }).Count
Check 'G-6' '无残留进程' ($left -eq 0 -and $pyLeft -eq 0) "app=$left server=$pyLeft"

$gateScript = Join-Path $root 'scripts\quality_gate.ps1'
if (Test-Path $gateScript) {
    & pwsh -NoProfile -File $gateScript *> $null
    Check 'G-7' '代码整理' ($LASTEXITCODE -eq 0) 'quality_gate'
}
else {
    Check 'G-7' '代码整理' $false '缺 scripts\quality_gate.ps1'
}

$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
$failCount = @($rows | Where-Object { $_.结果 -eq 'FAIL' }).Count
if ($failCount -gt 0) {
    Write-Host "RESULT: FAIL ($failCount 项)" -ForegroundColor Red
    exit 1
}
Write-Host "RESULT: ALL PASS" -ForegroundColor Green
exit 0
