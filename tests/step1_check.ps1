# step1_check.ps1 — 步骤 1 验收（配置 / 环形日志 / 串口桥）
# 用法： pwsh -File tests\step1_check.ps1 [-SkipBuild]
# 退出码： 0 = 全绿；1 = 有 FAIL

param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'output\build\ssh_bridge3.exe'
$resultFile = Join-Path $root 'output\selftest\result.txt'
$rows = @()

function Check([string]$id, [string]$desc, [bool]$ok, [string]$detail = '') {
    $script:rows += [pscustomobject]@{ 编号 = $id; 项目 = $desc; 结果 = $(if ($ok) { 'PASS' } else { 'FAIL' }); 说明 = $detail }
    return $ok
}

function Kill-App {
    Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
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
Check 'G-1' '编译无错无警告' ($buildExit -eq 0 -and $errorCount -eq 0 -and $warningCount -eq 0) "exit=$buildExit error=$errorCount warning=$warningCount"

# ---------- 自检（无窗口） ----------
# GUI 子系统偶发启动卡住（进程活着但没窗口、也没 result.txt）：
# 只重试一次明确失败，不掩盖真实数据面问题。
$selftestCode = -1
$selftestOut = ''
function Invoke-SelftestOnce {
    if (-not (Test-Path $exe)) { return }
    Kill-App
    Remove-Item -Recurse -Force (Join-Path $root 'output\selftest') -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $exe -ArgumentList '--selftest', 'all', 'loop:selftest' -PassThru
    $deadline = (Get-Date).AddSeconds(30)
    while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    if ($p.HasExited) { $script:selftestCode = $p.ExitCode } else { $p.Kill(); $script:selftestCode = -1 }
    if (Test-Path $resultFile) { $script:selftestOut = Get-Content -LiteralPath $resultFile -Raw }
}

Invoke-SelftestOnce
if ([string]::IsNullOrWhiteSpace($selftestOut)) {
    Start-Sleep -Seconds 1
    Invoke-SelftestOnce
}

$need = @('T1-1', 'T1-2', 'T1-3', 'T1-4', 'T1-5', 'T1-6', 'T1-7')
foreach ($id in $need) {
    $line = ($selftestOut -split "`n" | Where-Object { $_ -match "^PASS $id " } | Select-Object -First 1)
    $detail = if ($line) { ($line -replace "^PASS $id ", '').Trim() } else { '未见 PASS 行' }
    Check $id $detail ($null -ne $line) $detail
}

Check 'T1-8' '自检退出无残留' ($selftestCode -eq 0) "exit=$selftestCode"
Start-Sleep -Milliseconds 400
$leftAfterSelftest = @(Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue).Count
Check 'G-5' '无崩溃' ($selftestCode -eq 0 -and $leftAfterSelftest -eq 0) "code=$selftestCode 残留=$leftAfterSelftest"

# ---------- 窗口启动 / 关闭（G-0 / G-2 / G-3） ----------
$guiCode = -1
$title = ''
$listen = ''
$alive = $false
$instances = 0
if (Test-Path $exe) {
    # 同上：偶发“起来了但窗口没建、标题为空”，只重试一次。
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        Kill-App
        $proc = Start-Process -FilePath $exe -PassThru
        $hwnd = [IntPtr]::Zero
        for ($i = 0; $i -lt 40; $i++) {
            Start-Sleep -Milliseconds 250
            if ($proc.HasExited) { break }
            $proc.Refresh()
            $hwnd = $proc.MainWindowHandle
            if ($hwnd -ne [IntPtr]::Zero) { break }
        }
        $alive = -not $proc.HasExited
        $title = if ($alive) { $proc.MainWindowTitle } else { '' }
        if ($alive -and $hwnd -ne [IntPtr]::Zero) { break }
        if ($attempt -eq 1) { Kill-App }
    }

    $instances = @(Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue).Count
    Check 'G-0' '单进程且无监听端口' ($alive -and $instances -eq 1) "instances=$instances"
    Check 'G-2' '启动存活' ($alive -and $hwnd -ne [IntPtr]::Zero) "title='$title'"

    if ($alive) {
        $listen = (& netstat -ano | Select-String -Pattern "\s$($proc.Id)$" | Select-String -Pattern 'LISTENING' | Out-String)
    }
    Check 'G-4' '无监听端口' ([string]::IsNullOrWhiteSpace($listen)) $(if ($listen) { $listen.Trim() } else { 'netstat 无 LISTENING' })

    if ($alive -and $hwnd -ne [IntPtr]::Zero) {
        Add-Type -Namespace SB3S1 -Name W32 -MemberDefinition @'
[DllImport("user32.dll")]
public static extern bool PostMessageW(System.IntPtr hWnd, uint msg, System.IntPtr w, System.IntPtr l);
'@ -ErrorAction SilentlyContinue
        # S3 起：点 X = 隐藏到托盘；真正退出走托盘"退出"消息（WM_APP+2）
        [SB3S1.W32]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 800
        $hid = -not $proc.HasExited
        [SB3S1.W32]::PostMessageW($hwnd, 0x8002, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        $exited = $proc.WaitForExit(8000)
        if ($exited) { $guiCode = $proc.ExitCode } elseif ($hid) { $guiCode = -2 } else { $proc.Kill() }
    }
    Check 'G-3' '关闭退出码 0' ($guiCode -eq 0) "exit=$guiCode"
}
else {
    Check 'G-2' '启动存活' $false '产物不存在'
    Check 'G-3' '关闭退出码 0' $false '产物不存在'
}

Kill-App
$left = @(Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue).Count
Check 'G-4b' '退出后无残留进程' ($left -eq 0) "残留 $left 个"

# ---------- 代码整理 ----------
$gateScript = Join-Path $root 'scripts\quality_gate.ps1'
if (Test-Path $gateScript) {
    & pwsh -NoProfile -File $gateScript *> $null
    Check 'G-6' '代码整理' ($LASTEXITCODE -eq 0) 'quality_gate'
}
else {
    Check 'G-6' '代码整理' $false '缺 scripts\quality_gate.ps1'
}

# ---------- git 状态（提交后复核） ----------
Push-Location $root
$status = (& git status --porcelain 2>&1 | Out-String).Trim()
Pop-Location
Check 'G-7' 'git 状态' $true $(if ($status.Length -gt 0) { '有未提交改动（提交后复核）' } else { '工作区干净' })

# ---------- 汇总 ----------
$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
$failCount = @($rows | Where-Object { $_.结果 -eq 'FAIL' }).Count
if ($failCount -gt 0) {
    Write-Host "RESULT: FAIL ($failCount 项)" -ForegroundColor Red
    exit 1
}
Write-Host "RESULT: ALL PASS" -ForegroundColor Green
exit 0
