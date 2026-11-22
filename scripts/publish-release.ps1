# publish-release.ps1 — 把 ssh-bridge v3 安装包发布到独立 release 仓库
#
# 发布仓库：yanggan2015/ssh-bridge-v3-releases（只放二进制，不放源码）
# 分支：windows_x86_64，分支根 UPDATE.json 供应用内升级读取
# GitHub Release：v<Version>，资产为 zip + 本地 UPDATE.json + zip.sha256
#
# 用法：
#   powershell -NoProfile -File scripts\publish-release.ps1 -Version 3.0-s13-r6
#   powershell -NoProfile -File scripts\publish-release.ps1 -Version 3.0-s13-r6 -SkipPush
#
# 前置：先运行 scripts\package.ps1 生成 output\dist 下的 zip / UPDATE.json / .sha256。

param(
    [string]$Version = '3.0-s13-r6',
    [string]$Repo = 'yanggan2015/ssh-bridge-v3-releases',
    [string]$ReleaseDir = 'C:\code\ssh-bridge-v3-releases',
    [switch]$SkipPush
)

$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$root = Split-Path -Parent $here
$dist = Join-Path $root 'output\dist'
$stage = Join-Path $dist "ssh-bridge-v3-$Version-win64"
$zip = Join-Path $dist "ssh-bridge-v3-$Version-win64.zip"
$localManifest = Join-Path $dist "ssh-bridge-v3-$Version-win64.UPDATE.json"
$sumFile = "$zip.sha256"
$tag = "v$Version"

foreach ($p in @($stage, $zip, $localManifest, $sumFile)) {
    if (-not (Test-Path -LiteralPath $p)) { throw "缺少打包产物：$p（先运行 scripts\package.ps1" }
}
$m = Get-Content -LiteralPath $localManifest -Raw | ConvertFrom-Json
if ($m.version -ne $Version) { throw "清单版本 $($m.version) 与参数 $Version 不一致" }
$manifestRaw = Get-Content -LiteralPath $localManifest -Raw
$builtMatch = [regex]::Match($manifestRaw, '"built_utc"\s*:\s*"([^"]+)"')
$builtUtc = if ($builtMatch.Success) { $builtMatch.Groups[1].Value } else { [string]$m.built_utc }

function RunGit([string]$work, [string[]]$gitArgs, [switch]$AllowFail) {
    & git -C $work @gitArgs
    if (-not $AllowFail -and $LASTEXITCODE -ne 0) { throw "git $($gitArgs -join ' ') 失败" }
}

if (-not (Test-Path -LiteralPath $ReleaseDir)) {
    New-Item -ItemType Directory -Force -Path $ReleaseDir | Out-Null
    RunGit $ReleaseDir @('init')
}
$gitDir = Join-Path $ReleaseDir '.git'
if (-not (Test-Path -LiteralPath $gitDir)) { RunGit $ReleaseDir @('init') }
$remotes = (& git -C $ReleaseDir remote) 2>$null
if ($remotes -match '^origin$') {
    RunGit $ReleaseDir @('remote', 'set-url', 'origin', "https://github.com/$Repo.git")
} else {
    RunGit $ReleaseDir @('remote', 'add', 'origin', "https://github.com/$Repo.git")
}
if (-not (& git -C $ReleaseDir config user.email)) {
    RunGit $ReleaseDir @('config', 'user.email', 'yanggan2015@users.noreply.github.com')
    RunGit $ReleaseDir @('config', 'user.name', 'yanggan2015')
}

& git -C $ReleaseDir fetch origin --prune 2>$null | Out-Null
$hasBranch = (& git -C $ReleaseDir ls-remote --heads origin windows_x86_64) -match 'refs/heads/windows_x86_64$'
$hasLocalBranch = $false
& git -C $ReleaseDir show-ref --verify --quiet refs/heads/windows_x86_64
if ($LASTEXITCODE -eq 0) { $hasLocalBranch = $true }
if ($hasBranch) {
    RunGit $ReleaseDir @('checkout', '-B', 'windows_x86_64', 'origin/windows_x86_64')
    RunGit $ReleaseDir @('reset', '--hard', 'origin/windows_x86_64')
} elseif ($hasLocalBranch) {
    RunGit $ReleaseDir @('checkout', 'windows_x86_64')
} else {
    RunGit $ReleaseDir @('checkout', '--orphan', 'windows_x86_64')
    RunGit $ReleaseDir @('rm', '-rf', '.') -AllowFail
}

# 发布分支只保留运行所需的二进制与工具；zip 同时放进 packages/，
# UPDATE.json 的 url 指向 raw 分支文件，避免依赖 github.com 页面跳转。
# 历史包不清理，旧版本仍可直接从 packages/ 下载。
Remove-Item -Recurse -Force (Join-Path $ReleaseDir 'ssh_bridge3') -ErrorAction SilentlyContinue
Remove-Item -Force (Join-Path $ReleaseDir 'UPDATE.json') -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $ReleaseDir 'ssh_bridge3\tools') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $ReleaseDir 'packages') | Out-Null
Copy-Item -LiteralPath (Join-Path $stage 'ssh_bridge3.exe') -Destination (Join-Path $ReleaseDir 'ssh_bridge3\ssh_bridge3.exe') -Force
Copy-Item -LiteralPath (Join-Path $stage 'README.md') -Destination (Join-Path $ReleaseDir 'ssh_bridge3\README.md') -Force
Copy-Item -LiteralPath (Join-Path $stage 'VERSION.txt') -Destination (Join-Path $ReleaseDir 'ssh_bridge3\VERSION.txt') -Force
Get-ChildItem -LiteralPath (Join-Path $stage 'tools') -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $ReleaseDir 'ssh_bridge3\tools') -Force
}
$zipName = Split-Path -Leaf $zip
Copy-Item -LiteralPath $zip -Destination (Join-Path $ReleaseDir "packages\$zipName") -Force
$rawZipUrl = "https://raw.githubusercontent.com/$Repo/windows_x86_64/packages/$zipName"
$releaseManifest = [ordered]@{
    product     = 'ssh-bridge v3'
    version     = $Version
    platform    = 'windows'
    arch        = 'x86_64'
    branch      = 'windows_x86_64'
    zip         = $zipName
    url         = $rawZipUrl
    github_url  = "https://github.com/$Repo/releases/download/$tag/$zipName"
    sha256      = [string]$m.sha256
    size_bytes  = [int64]$m.size_bytes
    exe_sha256  = [string]$m.exe_sha256
    built_utc   = $builtUtc
    source      = 'https://github.com/yanggan2015/ssh-bridge-v3'
}
$manifestText = $releaseManifest | ConvertTo-Json
Set-Content -LiteralPath (Join-Path $ReleaseDir 'UPDATE.json') -Value $manifestText -Encoding utf8NoBOM
Write-Output "release UPDATE.json: $Version -> $zipName"

$branchReadme = @"
# ssh-bridge v3 — Windows x86_64 binaries

This branch contains **binary downloads only**. Source code:
<https://github.com/yanggan2015/ssh-bridge-v3>

- App tree: [`ssh_bridge3/`](./ssh_bridge3/)
- Installer package: [`packages/`](./packages/)
- Update manifest: [`UPDATE.json`](./UPDATE.json)
- GitHub Releases: <https://github.com/$Repo/releases>

The app's **Help → Check for updates** button reads
`https://raw.githubusercontent.com/$Repo/windows_x86_64/UPDATE.json` and
downloads the package from the same raw branch with SHA-256 verification.
"@
Set-Content -LiteralPath (Join-Path $ReleaseDir 'README.md') -Value $branchReadme -Encoding utf8NoBOM

RunGit $ReleaseDir @('add', '-A')
$dirty = (& git -C $ReleaseDir status --porcelain)
if ($dirty) {
    RunGit $ReleaseDir @('commit', '-m', "release windows_x86_64 $Version")
} else {
    Write-Output 'release branch unchanged'
}

if (-not $SkipPush) {
    # 先推发布分支，保证空仓库有可挂 Release 的提交；随后立刻创建 Release 资产。
    RunGit $ReleaseDir @('push', '-u', 'origin', 'HEAD:windows_x86_64')
    Write-Output "pushed: https://github.com/$Repo/tree/windows_x86_64"

    & gh release view $tag -R $Repo 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) {
        & gh release upload $tag $zip $localManifest $sumFile -R $Repo --clobber
        if ($LASTEXITCODE -ne 0) { throw "上传 Release 资产失败：$tag" }
    } else {
        $notes = Join-Path $env:TEMP "ssh-bridge-v3-$Version-notes.md"
        @"
# ssh-bridge v3 $Version

Windows x64 便携安装包。

- 主程序：``ssh_bridge3.exe``
- 更新清单：``ssh-bridge-v3-$Version-win64.UPDATE.json``
- 校验文件：``ssh-bridge-v3-$Version-win64.zip.sha256``

应用内升级：**帮助 → 检查更新…** 或 **关于 → 检查更新**。

源码：<https://github.com/yanggan2015/ssh-bridge-v3>
"@ | Set-Content -LiteralPath $notes -Encoding utf8NoBOM
        & gh release create $tag $zip $localManifest $sumFile -R $Repo --title "ssh-bridge v3 $Version" --notes-file $notes
        if ($LASTEXITCODE -ne 0) { throw "创建 GitHub Release 失败：$tag" }
    }
}

# 主分支只放导航 README；发布分支负责二进制与清单。
& git -C $ReleaseDir fetch origin --prune 2>$null | Out-Null
$hasMain = (& git -C $ReleaseDir ls-remote --heads origin main) -match 'refs/heads/main$'
$hasLocalMain = $false
& git -C $ReleaseDir show-ref --verify --quiet refs/heads/main
if ($LASTEXITCODE -eq 0) { $hasLocalMain = $true }
if ($hasMain) {
    RunGit $ReleaseDir @('checkout', '-B', 'main', 'origin/main')
    RunGit $ReleaseDir @('reset', '--hard', 'origin/main')
} elseif ($hasLocalMain) {
    RunGit $ReleaseDir @('checkout', 'main')
} else {
    RunGit $ReleaseDir @('checkout', '--orphan', 'main')
    RunGit $ReleaseDir @('rm', '-rf', '.') -AllowFail
}
Remove-Item -Recurse -Force (Join-Path $ReleaseDir 'ssh_bridge3') -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force (Join-Path $ReleaseDir 'packages') -ErrorAction SilentlyContinue
Remove-Item -Force (Join-Path $ReleaseDir 'UPDATE.json') -ErrorAction SilentlyContinue
$mainReadme = @"
# ssh-bridge v3 — Releases

**This repository contains binaries only. Source code is in
[yanggan2015/ssh-bridge-v3](https://github.com/yanggan2015/ssh-bridge-v3).**

- Windows binaries branch: [`windows_x86_64`](https://github.com/$Repo/tree/windows_x86_64)
- Version manifest: [`UPDATE.json`](https://raw.githubusercontent.com/$Repo/windows_x86_64/UPDATE.json)
- Installer assets: [Releases](https://github.com/$Repo/releases)

The app's **Help → Check for updates** button reads the manifest above and
downloads the matching GitHub Release asset with SHA-256 verification.
"@
Set-Content -LiteralPath (Join-Path $ReleaseDir 'README.md') -Value $mainReadme -Encoding utf8NoBOM
RunGit $ReleaseDir @('add', '-A')
$mainDirty = (& git -C $ReleaseDir status --porcelain)
if ($mainDirty) {
    RunGit $ReleaseDir @('commit', '-m', 'docs: point main to windows_x86_64 binaries')
    if (-not $SkipPush) {
        RunGit $ReleaseDir @('push', 'origin', 'HEAD:main')
    }
} elseif (-not $SkipPush -and -not $hasMain) {
    RunGit $ReleaseDir @('push', 'origin', 'HEAD:main')
}
if (-not $SkipPush) {
    & gh repo edit $Repo --default-branch main 2>$null | Out-Null
}

Write-Output "release manifest: https://raw.githubusercontent.com/$Repo/windows_x86_64/UPDATE.json"
Write-Output "release page: https://github.com/$Repo/releases/tag/$tag"
Write-Output "zip sha256: $($m.sha256)"





