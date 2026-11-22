# package.ps1 — 打包可分发的 ssh-bridge v3 Windows 构建（独立发布仓库 + 独立更新器）
# 用法： powershell -NoProfile -File scripts\package.ps1 [-Version 3.0-s13-r16] [-SkipBuild]
# 产物： output\dist\ssh-bridge-v3-<Version>-win64.zip 与同名 .sha256
#
# 包内： ssh_bridge3.exe、脱敏示例配置、README.md、tools\ui_probe.py、tools\smoke.ps1、tools\update.ps1
# 说明： exe 走静态链接（libssh / mongoose / cJSON / Lua 全在体内），目标机不需要装 MSYS2。

param(
    [string]$Version = '3.0-s13-r17',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$here  = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$root  = Split-Path -Parent $here
$exe   = Join-Path $root 'output\build\ssh_bridge3.exe'
$dist  = Join-Path $root 'output\dist'
$stage = Join-Path $dist "ssh-bridge-v3-$Version-win64"
$zip   = Join-Path $dist "ssh-bridge-v3-$Version-win64.zip"
$buildUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')

if (-not $SkipBuild) {
    $env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
    Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    # Makefile 没有头文件依赖：改过结构体就必须 clean，否则旧 .o 布局错位
    mingw32-make -C $root clean 2>&1 | Out-Null
    $log = mingw32-make -C $root -j4 2>&1
    if ($log -match 'error') { $log | Select-Object -Last 12; throw '构建失败' }
}
if (-not (Test-Path -LiteralPath $exe)) { throw "缺少产物：$exe" }
$exeHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash

Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'tools') | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $stage 'ssh_bridge3.exe')

# 分发包固定使用脱敏示例配置：COM4/1 路继电器、OK3568 115200 改为 1500000、
# 五个板控按钮保留，密码字段为空，auto_login 里改成 YOUR_PASSWORD 占位。
$distConfigPath = Join-Path $here 'dist-config.json'
if (-not (Test-Path -LiteralPath $distConfigPath)) { throw "缺少脱敏配置：$distConfigPath" }
Copy-Item -LiteralPath $distConfigPath -Destination (Join-Path $stage 'ssh_bridge_config.json') -Force
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'work') | Out-Null
$versionText = @(
    'product=ssh-bridge v3'
    "version=$Version"
    "built_utc=$buildUtc"
    "exe_sha256=$exeHash"
) -join "`r`n"
Set-Content -LiteralPath (Join-Path $stage 'VERSION.txt') -Value $versionText -Encoding ascii

# 探针：技能自带的 ui_probe.py（默认端口 8086，本程序是 18086，用 --base 指定）
$sk = Join-Path $env:USERPROFILE 'AppData\Local\Doubao\User Data\Profile 9\.doubao\agent_mode\workspace\.user_skills\win32-ai-ui-bridge'
$probe = Join-Path $sk 'scripts\ui_probe.py'
if (Test-Path -LiteralPath $probe) {
    Copy-Item -LiteralPath $probe -Destination (Join-Path $stage 'tools\ui_probe.py')
}

Copy-Item -LiteralPath (Join-Path $here 'dist-smoke.ps1') -Destination (Join-Path $stage 'tools\smoke.ps1')
Copy-Item -LiteralPath (Join-Path $here 'update.ps1') -Destination (Join-Path $stage 'tools\update.ps1')
Copy-Item -LiteralPath (Join-Path $here 'dist-readme.md') -Destination (Join-Path $stage 'README.md')

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
$sizeBytes = (Get-Item -LiteralPath $zip).Length
$size = [Math]::Round($sizeBytes / 1MB, 2)
$manifest = [ordered]@{
    product     = 'ssh-bridge v3'
    version     = $Version
    zip         = (Split-Path -Leaf $zip)
    sha256      = $hash
    size_bytes  = $sizeBytes
    exe_sha256  = $exeHash
    built_utc   = $buildUtc
}
$manifestFile = Join-Path $dist ("ssh-bridge-v3-$Version-win64.UPDATE.json")
$manifestJson = $manifest | ConvertTo-Json
[System.IO.File]::WriteAllText($manifestFile, $manifestJson, (New-Object System.Text.UTF8Encoding($false)))
$sumFile = "$zip.sha256"
Set-Content -LiteralPath $sumFile -Value "$hash  $(Split-Path -Leaf $zip)" -Encoding ascii
Write-Host "zip    : $zip"
Write-Host "size   : $size MB"
Write-Host "sha256 : $sumFile"
Write-Host "update : $manifestFile"




