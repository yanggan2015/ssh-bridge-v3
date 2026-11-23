# quality_gate.ps1 — 代码整理闸门（每步都要过）
# 检查 src/ 下所有 .c/.cpp/.h：无 TAB 缩进、无行尾空白、文件以换行结尾、单行不超 200 字符。
# src\third_party\ 是 vendored 第三方（mongoose / win_ui_bridge），保持上游原样，不参与整理。
# 退出码： 0 = 全 PASS；1 = 有问题

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$targets = @()
$targets += Get-ChildItem -Path (Join-Path $root 'src') -Recurse -File -Include *.c, *.cpp, *.h |
    Where-Object { $_.FullName -notlike (Join-Path $root 'src\third_party\*') }
$targets += Get-ChildItem -Path (Join-Path $root 'scripts') -Recurse -File -Include *.py, *.ps1
$targets += Get-ChildItem -Path (Join-Path $root 'tests') -Recurse -File -Include *.py, *.ps1

$issues = @()
foreach ($f in $targets) {
    $rel = $f.FullName.Substring($root.Length + 1)
    $lines = [System.IO.File]::ReadAllLines($f.FullName)
    $n = 0
    # 嵌入的 HTML/JS/CSS 原始字符串是资源内容，不是 C++ 排版；只豁免“超长行”，
    # TAB、行尾空白、文件末尾换行仍照常检查。C++ 拼接生成的 HTML 不豁免。
    $rawEnd = $null
    foreach ($line in $lines) {
        $n++
        $inRaw = $false
        if ($null -ne $rawEnd) {
            $inRaw = $true
            if ($line.Contains($rawEnd)) { $rawEnd = $null }
        } else {
            $m = [regex]::Match($line, 'R"([A-Za-z0-9_]*)\(')
            if ($m.Success) {
                $end = ')' + $m.Groups[1].Value + '"'
                if (-not $line.Contains($end)) { $rawEnd = $end }
                $inRaw = $true
            }
        }
        if ($line.Contains("`t")) { $issues += "$rel`:$n TAB" }
        if ($line -match '\s+$') { $issues += "$rel`:$n 行尾空白" }
        if (-not $inRaw -and $line.Length -gt 200) {
            $issues += "$rel`:$n 超长行（$($line.Length)）"
        }
    }
    $raw = [System.IO.File]::ReadAllText($f.FullName)
    if ($raw.Length -gt 0 -and -not $raw.EndsWith("`n")) { $issues += "$rel 文件末尾缺换行" }
}

Write-Host "扫描 $($targets.Count) 个源文件"
if ($issues.Count -eq 0) {
    Write-Host "RESULT: ALL PASS" -ForegroundColor Green
    exit 0
}
$issues | Select-Object -First 40 | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
Write-Host "RESULT: $($issues.Count) 项待整理" -ForegroundColor Red
exit 1
