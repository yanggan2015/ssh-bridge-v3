# step0_check.ps1 — 步骤 0 验收（环境与构建链）
# 用法： pwsh -File tests\step0_check.ps1 [-SkipBuild]
# 退出码： 0 = 全绿；1 = 有 FAIL

param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$rows = @()

function Check([string]$id, [string]$desc, [bool]$ok, [string]$detail = '') {
    $script:rows += [pscustomobject]@{ 编号 = $id; 项目 = $desc; 结果 = $(if ($ok) { 'PASS' } else { 'FAIL' }); 说明 = $detail }
    return $ok
}

function Any-Fail { return ($script:rows | Where-Object { $_.结果 -eq 'FAIL' }).Count -gt 0 }

# ---------- T0-1 目录骨架 ----------
$dirs = @('src\app', 'src\core\bridge', 'src\core\config', 'src\core\logbuf', 'src\core\util',
         'src\ui\shell', 'src\ui\terminal', 'src\ui\tray', 'src\services', 'src\third_party',
         'docs', 'tests', 'scripts', 'output')
$missingDirs = $dirs | Where-Object { -not (Test-Path (Join-Path $root $_)) }
Check 'T0-1' '目录骨架' ($missingDirs.Count -eq 0) ($(if ($missingDirs) { '缺: ' + ($missingDirs -join ', ') } else { '14 个目录齐备' }))

# ---------- T0-2 关键文件 ----------
$files = @('README.md', 'Makefile', '.gitignore', 'src\app\app_main.cpp',
           'src\core\bridge\bridge.h', 'src\services\service.h', 'src\core\config\config.h',
           'docs\每步测试闸门.md', 'tests\CHECKLIST_step0.md')
$missingFiles = $files | Where-Object { -not (Test-Path (Join-Path $root $_)) }
Check 'T0-2' '关键文件' ($missingFiles.Count -eq 0) ($(if ($missingFiles) { '缺: ' + ($missingFiles -join ', ') } else { "$($files.Count) 个文件齐备" }))

# ---------- T0-3/4 编译 ----------
$buildOut = ''
$buildExit = 0
if (-not $SkipBuild) {
    $env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
    $buildOut = (& 'C:\msys64\mingw64\bin\mingw32-make.exe' -C $root -j4 2>&1 | Out-String)
    $buildExit = $LASTEXITCODE
    if ($buildExit -ne 0) {
        # 增量构建可能因时间戳判定无事可做，但失败时重试一次全量
        $buildOut = (& 'C:\msys64\mingw64\bin\mingw32-make.exe' -C $root -B -j4 2>&1 | Out-String)
        $buildExit = $LASTEXITCODE
    }
}
$errorCount   = ([regex]::Matches($buildOut, '(?im)^.*\berror\b.*$')).Count
$warningCount = ([regex]::Matches($buildOut, '(?im)^.*\bwarning\b.*$')).Count
Check 'T0-3' '编译无错' ($buildExit -eq 0 -and $errorCount -eq 0) "exit=$buildExit error=$errorCount"
Check 'T0-4' '编译无警告' ($warningCount -eq 0) "warning=$warningCount"

# ---------- T0-5 产物 ----------
$exe = Join-Path $root 'output\build\ssh_bridge3.exe'
$exeOk = (Test-Path $exe) -and ((Get-Item $exe).Length -gt 100KB)
Check 'T0-5' '产物存在' $exeOk ($(if (Test-Path $exe) { "$([math]::Round((Get-Item $exe).Length / 1kb)) KB" } else { '未生成' }))

# ---------- T0-6/7 启动与标题 ----------
$proc = $null
if ($exeOk) {
    $proc = Start-Process -FilePath $exe -PassThru
    Start-Sleep -Seconds 4
    $alive = -not $proc.HasExited
    Check 'T0-6' '启动存活' $alive ($(if ($alive) { "pid=$($proc.Id)" } else { "已退出 code=$($proc.ExitCode)" }))

    $title = ''
    if ($alive) {
        $proc.Refresh()
        $title = $proc.MainWindowTitle
    }
    Check 'T0-7' '窗口标题' ($title -like '*SSH Bridge 3.0*') "title='$title'"
}

# ---------- T0-10/11 关闭与残留 ----------
if ($proc -and -not $proc.HasExited) {
    Add-Type -Namespace SB3 -Name W32 -MemberDefinition @'
[DllImport("user32.dll")]
public static extern bool PostMessageW(System.IntPtr hWnd, uint msg, System.IntPtr w, System.IntPtr l);
'@ -ErrorAction SilentlyContinue

    # 用进程自身的 MainWindowHandle（FindWindow 传 $null 会被 marshaling 成空串而找不到窗口）
    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 10; $i++) {
        $proc.Refresh()
        $hwnd = $proc.MainWindowHandle
        if ($hwnd -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 300
    }
    # S3 起点 X 只隐藏到托盘；真正退出用托盘"退出"消息（WM_APP+2）
    if ($hwnd -ne [IntPtr]::Zero) {
        [SB3.W32]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 800
        [SB3.W32]::PostMessageW($hwnd, 0x8002, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    }
    $exited = $proc.WaitForExit(8000)
    $code = if ($exited) { $proc.ExitCode } else { -1 }
    if (-not $exited) { try { $proc.Kill() } catch {} }
    Check 'T0-10' '关闭退出（隐藏到托盘后托盘退出）' ($exited -and $code -eq 0) "hwnd=$hwnd exited=$exited code=$code"
}
else {
    Check 'T0-10' '关闭退出' $false '进程未正常启动，无法测试关闭'
}

Start-Sleep -Milliseconds 400
$left = @(Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue)
Check 'T0-11' '无残留进程' ($left.Count -eq 0) "残留 $($left.Count) 个"

# ---------- T0-12 代码整理 ----------
$gateScript = Join-Path $root 'scripts\quality_gate.ps1'
if (Test-Path $gateScript) {
    $gateOut = (& pwsh -NoProfile -File $gateScript 2>&1 | Out-String)
    Check 'T0-12' '代码整理' ($LASTEXITCODE -eq 0) ($(if ($LASTEXITCODE -eq 0) { 'quality_gate 全 PASS' } else { $gateOut.Trim() -split "`n" | Select-Object -Last 3 }))
}
else {
    Check 'T0-12' '代码整理' $false '缺 scripts\quality_gate.ps1'
}

# ---------- T0-13 git 基线 ----------
$gitOk = Test-Path (Join-Path $root '.git')
$statusOut = ''
if ($gitOk) {
    Push-Location $root
    $statusOut = (& git status --porcelain 2>&1 | Out-String)
    Pop-Location
}
Check 'T0-13' 'git 基线' $gitOk $(
    if (-not $gitOk) { '仓库未初始化' }
    elseif ($statusOut.Trim().Length -gt 0) { "已初始化 · 有 $((@($statusOut.Trim() -split "`n")).Count) 项未提交改动" }
    else { '已初始化 · 工作区干净' })

# ---------- 汇总 ----------
$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
$failCount = @($rows | Where-Object { $_.结果 -eq 'FAIL' }).Count
if ($failCount -gt 0) {
    Write-Host "RESULT: FAIL ($failCount 项)" -ForegroundColor Red
    exit 1
}
Write-Host "RESULT: ALL PASS" -ForegroundColor Green
exit 0
