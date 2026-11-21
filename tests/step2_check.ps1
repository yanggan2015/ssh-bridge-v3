# step2_check.ps1 — 步骤 2 验收（SSH 出站链路）
# 用法： pwsh -File tests\step2_check.ps1 [-SkipBuild]
# 退出码： 0 = 全绿；1 = 有 FAIL
#
# 依赖：python + paramiko（本机 5.1.0 已验证）。测试服务器由 tests\tools\ssh_test_server.py 提供，
# 生命周期由本脚本控制（起来 → 跑自检 → 杀掉模拟拔网 → 再起来模拟恢复）。

param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'output\build\ssh_bridge3.exe'
$resultFile = Join-Path $root 'output\selftest\result.txt'
$srvScript = Join-Path $root 'tests\tools\ssh_test_server.py'
$rows = @()
$servers = @()

function Check([string]$id, [string]$desc, [bool]$ok, [string]$detail = '') {
    $script:rows += [pscustomobject]@{ 编号 = $id; 项目 = $desc; 结果 = $(if ($ok) { 'PASS' } else { 'FAIL' }); 说明 = $detail }
    return $ok
}

function Kill-App {
    Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
}

function Start-Server([string]$mode = 'echo', [int]$port = 0, [string]$tag = 's') {
    $ready = Join-Path $env:TEMP ("sb3_ssh_" + $tag + "_" + [guid]::NewGuid().ToString('N').Substring(0, 6) + ".txt")
    Remove-Item -LiteralPath $ready -ErrorAction SilentlyContinue
    $proc = Start-Process -FilePath 'python' -PassThru -WindowStyle Hidden -ArgumentList @(
        $srvScript, '--port', "$port", '--mode', $mode, '--ready-file', $ready)
    for ($i = 0; $i -lt 60; $i++) {
        if (Test-Path -LiteralPath $ready) { break }
        Start-Sleep -Milliseconds 250
    }
    if (-not (Test-Path -LiteralPath $ready)) { throw "ssh test server failed to start ($mode)" }
    $p = [int](Get-Content -LiteralPath $ready -Raw).Trim()
    Remove-Item -LiteralPath $ready -ErrorAction SilentlyContinue
    $entry = [pscustomobject]@{ proc = $proc; port = $p }
    $script:servers += $entry
    return $entry
}

function Stop-Server($entry) {
    if ($null -eq $entry) { return }
    if ($null -ne $entry.proc -and -not $entry.proc.HasExited) {
        Stop-Process -Id $entry.proc.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 400
}

function Run-Selftest([string]$mode, [string[]]$rest, [int]$timeoutSec = 30) {
    Kill-App
    Remove-Item -Recurse -Force (Join-Path $root 'output\selftest') -ErrorAction SilentlyContinue
    $payload = @('--selftest', $mode) + $rest
    $proc = Start-Process -FilePath $exe -ArgumentList $payload -PassThru
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while (-not $proc.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    if (-not $proc.HasExited) { $proc.Kill() }
    $code = if ($proc.HasExited) { $proc.ExitCode } else { -1 }
    $text = if (Test-Path -LiteralPath $resultFile) { Get-Content -LiteralPath $resultFile -Raw } else { '' }
    return [pscustomobject]@{ code = $code; text = $text }
}

function Line-Of([string]$text, [string]$id) {
    ($text -split "`n" | Where-Object { $_ -match "^PASS $id " } | Select-Object -First 1)
}

function Record-From([string]$id, [string]$desc, $run) {
    $line = Line-Of $run.text $id
    $detail = if ($line) { ($line -replace "^PASS $id ", '').Trim() } else { "未见 PASS 行（exit=$($run.code)）" }
    Check $id $desc ($null -ne $line) $detail
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
    Check 'G-2' '产物存在' $false 'output\build\ssh_bridge3.exe 不存在'
    $rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
    Write-Host "RESULT: FAIL" -ForegroundColor Red
    exit 1
}

try {
    # ---------- T2-1 / T2-2：连接 + 回显顺序 ----------
    $srv = Start-Server 'echo' 0 'echo'
    $run = Run-Selftest 'ssh' @('127.0.0.1', "$($srv.port)", 'test', 'test') 30
    Record-From 'T2-1' '连接成功且拿到 shell' $run
    Record-From 'T2-2' '发命令回显顺序正确' $run
    Stop-Server $srv

    # ---------- T2-6：密码错误可读 ----------
    $srv = Start-Server 'reject' 0 'reject'
    $run = Run-Selftest 'ssh-auth' @('127.0.0.1', "$($srv.port)", 'test', 'wrong-pass') 30
    Record-From 'T2-6' '密码错误给出可读错误' $run
    Stop-Server $srv

    # ---------- T2-3：拔网 5s 内标记断开 ----------
    $srv = Start-Server 'echo' 0 'drop'
    Kill-App
    Remove-Item -Recurse -Force (Join-Path $root 'output\selftest') -ErrorAction SilentlyContinue
    $proc = Start-Process -FilePath $exe -PassThru -ArgumentList @(
        '--selftest', 'ssh-drop', '127.0.0.1', "$($srv.port)", 'test', 'test')
    Start-Sleep -Seconds 2
    Stop-Server $srv
    $deadline = (Get-Date).AddSeconds(20)
    while (-not $proc.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    if (-not $proc.HasExited) { $proc.Kill() }
    $text = if (Test-Path -LiteralPath $resultFile) { Get-Content -LiteralPath $resultFile -Raw } else { '' }
    $run = [pscustomobject]@{ code = $(if ($proc.HasExited) { $proc.ExitCode } else { -1 }); text = $text }
    Record-From 'T2-3' '拔网 5s 内标记断开' $run

    # ---------- T2-4：自动重连 ----------
    $srv = Start-Server 'echo' 0 'reconn'
    $port = $srv.port
    Kill-App
    Remove-Item -Recurse -Force (Join-Path $root 'output\selftest') -ErrorAction SilentlyContinue
    $proc = Start-Process -FilePath $exe -PassThru -ArgumentList @(
        '--selftest', 'ssh-reconnect', '127.0.0.1', "$port", 'test', 'test')
    Start-Sleep -Seconds 2
    Stop-Server $srv
    Start-Sleep -Seconds 2
    $srv2 = Start-Server 'echo' $port 'reconn2'
    $deadline = (Get-Date).AddSeconds(45)
    while (-not $proc.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    if (-not $proc.HasExited) { $proc.Kill() }
    $text = if (Test-Path -LiteralPath $resultFile) { Get-Content -LiteralPath $resultFile -Raw } else { '' }
    $run = [pscustomobject]@{ code = $(if ($proc.HasExited) { $proc.ExitCode } else { -1 }); text = $text }
    Record-From 'T2-4' '自动重连成功' $run
    Stop-Server $srv2

    # ---------- T2-5：两条链路互不影响 ----------
    $s1 = Start-Server 'echo' 0 'two1'
    $s2 = Start-Server 'echo' 0 'two2'
    $run = Run-Selftest 'ssh-two' @('127.0.0.1', "$($s1.port)", "$($s2.port)", 'test', 'test') 35
    Record-From 'T2-5' '两条链路互不影响' $run
    Stop-Server $s1
    Stop-Server $s2
}
finally {
    for ($i = $script:servers.Count - 1; $i -ge 0; $i--) { Stop-Server $script:servers[$i] }
    Kill-App
}

# ---------- 残留 / 整理 / git ----------
Start-Sleep -Milliseconds 400
$left = @(Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue).Count
Check 'G-5' '无残留进程' ($left -eq 0) "残留 $left 个"

$pyLeft = @(Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -like '*ssh_test_server.py*' }).Count
Check 'G-5b' '测试服务器已清理' ($pyLeft -eq 0) "残留 $pyLeft 个"

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
