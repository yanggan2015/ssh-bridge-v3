# web_job_queue_check.ps1 — S9 /job 单后台队列回归
# 用 loop1 的 Lua 动作各 sleep 1.2s，连发两个动作：
#   1.6s 时第 1 个已完成、第 2 个仍在排队；3.4s 时两个都完成。
# 若仍用 std::async 并发执行，第 2 个会在约 1.2s 就完成，本测试会失败。
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$root = Split-Path -Parent $here
$exe = Join-Path $root 'output\build\ssh_bridge3.exe'
$run = Join-Path $root 'output\webjobtest'
$port = 18089
$token = 'jobpw'

function Check([string]$id, [string]$name, [bool]$ok, [string]$detail = '') {
    $tag = if ($ok) { 'PASS' } else { 'FAIL' }
    '{0,-5} {1,-34} {2} {3}' -f $id, $name, $tag, $detail
}

if (-not $SkipBuild) {
    $env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
    Push-Location $root
    mingw32-make -j4 2>&1 | Out-Null
    $buildCode = $LASTEXITCODE
    Pop-Location
    if ($buildCode -ne 0) { throw '构建失败' }
}
if (-not (Test-Path -LiteralPath $exe)) { throw "缺少产物：$exe" }

Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
New-Item -ItemType Directory -Force -Path $run | Out-Null

$cfg = Get-Content -LiteralPath (Join-Path $root 'scripts\dist-config.json') -Raw | ConvertFrom-Json
if (-not $cfg.PSObject.Properties['web']) {
    $cfg | Add-Member -NotePropertyName web -NotePropertyValue ([pscustomobject]@{
        enable = $false; port = 18081; token = ''
    })
}
$cfg.web.enable = $true
$cfg.web.port = $port
$cfg.web.token = $token
$cfg.agent.enable = $false
$cfg.inbound_ssh.enable = $false
$uiCfg = [pscustomobject]@{ split = 1; active_pane = 0; pane_channels = @('loop1') }
[System.IO.File]::WriteAllText((Join-Path $run 'ssh_bridge_ui.json'),
    ($uiCfg | ConvertTo-Json -Depth 10), (New-Object System.Text.UTF8Encoding($false)))
$loop = $cfg.boards | Where-Object { $_.name -eq 'loop1' }
if (-not $loop) { throw '示例配置缺少 loop1' }
$loop.buttons = @(
    [pscustomobject]@{
        id = 'slow1'; label = '队列动作1'; enable = $true; backend = 'script'
        script = "--lua`nsleep(1.2)`nprint('JOB_1')"
    },
    [pscustomobject]@{
        id = 'slow2'; label = '队列动作2'; enable = $true; backend = 'script'
        script = "--lua`nsleep(1.2)`nprint('JOB_2')"
    }
)
$cfgJson = $cfg | ConvertTo-Json -Depth 20
[System.IO.File]::WriteAllText((Join-Path $run 'ssh_bridge_config.json'), $cfgJson,
                               (New-Object System.Text.UTF8Encoding($false)))

$p = Start-Process -FilePath $exe -WorkingDirectory $run -PassThru
$base = "http://127.0.0.1:$port"
$ok = $false
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 250
    try {
        $null = Invoke-WebRequest -UseBasicParsing "$base/status" -TimeoutSec 2
        $ok = $true
        break
    } catch {}
}
Check 'WJ-1' 'Web 服务启动' $ok "pid=$($p.Id)"
if (-not $ok) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; throw 'Web 未启动' }

$headers = @{ Authorization = "Bearer $token" }
function Post-Action([string]$id) {
    $body = @{ channel = 'loop1'; id = $id } | ConvertTo-Json -Compress
    Invoke-WebRequest -Method Post -Uri "$base/action" -Headers $headers `
        -ContentType 'application/json' -Body $body -TimeoutSec 5 -SkipHttpErrorCheck
}
function Get-Job([int]$id) {
    Invoke-RestMethod -Uri "$base/job?id=$id" -TimeoutSec 5
}

$r1 = Post-Action 'slow1'
$b1 = $r1.Content | ConvertFrom-Json
$r2 = Post-Action 'slow2'
$b2 = $r2.Content | ConvertFrom-Json
Check 'WJ-2' '第 1 个受理、同通道第 2 个按并发上限拒绝' `
      ($r1.StatusCode -eq 202 -and $b1.ok -and $b1.job -gt 0 -and
       $r2.StatusCode -eq 429 -and -not $b2.ok -and $b2.error.code -eq 'channel_busy') `
      "job1=$($b1.job) second_http=$($r2.StatusCode) code=$($b2.error.code)"

Start-Sleep -Milliseconds 1600
$j1 = Get-Job ([int]$b1.job)
Check 'WJ-3' '第 1 个动作已完成' ([bool]$j1.done -and [bool]$j1.ok -and $j1.output -match 'JOB_1') `
      "done=$($j1.done) ok=$($j1.ok)"
Check 'WJ-4' '第 2 个动作未被并发放行' ($r2.StatusCode -eq 429) "http=$($r2.StatusCode)"

$r3 = Post-Action 'slow2'
$b3 = $r3.Content | ConvertFrom-Json
Check 'WJ-5a' '第 1 个结束后第 2 个可受理' ($r3.StatusCode -eq 202 -and $b3.ok -and $b3.job -gt 0) `
      "job2=$($b3.job)"
Start-Sleep -Milliseconds 1800
$j2 = Get-Job ([int]$b3.job)
Check 'WJ-5b' '第 2 个动作随后完成' ([bool]$j2.done -and [bool]$j2.ok -and $j2.output -match 'JOB_2') `
      "done=$($j2.done) ok=$($j2.ok)"

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
$left = @(Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue).Count
Check 'WJ-6' '退出后无残留进程' ($left -eq 0) "left=$left"

if (($j1.done -and $j1.ok) -and ($j2.done -and $j2.ok) -and
    $r1.StatusCode -eq 202 -and $r2.StatusCode -eq 429 -and $r3.StatusCode -eq 202 -and
    $left -eq 0) {
    'RESULT: ALL PASS'
    exit 0
}
'RESULT: FAIL'
exit 1
