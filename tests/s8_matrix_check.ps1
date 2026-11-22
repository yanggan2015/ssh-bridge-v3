# s8_matrix_check.ps1 — S8 入站 SSH 完整矩阵
# 覆盖：显式空密码 none、默认密码继承、SSH 出站通道自身密码、错误/未知用户拒绝、
# 同通道双会话共享、会话计数、配置热关闭/重开、活动会话断开。
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$root = Split-Path -Parent $here
$exe = Join-Path $root 'output\build\ssh_bridge3.exe'
$run = Join-Path $root 'output\s8matrix'
$port = 2223
$web = 18091
$token = 's8pw'

if (-not $SkipBuild) {
    $env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
    Push-Location $root
    mingw32-make -j4 2>&1 | Out-Null
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { throw '构建失败' }
}
if (-not (Test-Path -LiteralPath $exe)) { throw "缺少产物：$exe" }

Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
New-Item -ItemType Directory -Force -Path $run | Out-Null

$cfg = Get-Content -LiteralPath (Join-Path $root 'scripts\dist-config.json') -Raw | ConvertFrom-Json
$cfg.default.password = 'defpw'
$cfg.agent.enable = $false
$cfg.inbound_ssh.enable = $true
$cfg.inbound_ssh.port = $port
if (-not $cfg.PSObject.Properties['web']) {
    $cfg | Add-Member -NotePropertyName web -NotePropertyValue ([pscustomobject]@{
        enable = $false; port = 18081; token = ''
    })
}
$cfg.web.enable = $true
$cfg.web.port = $web
$cfg.web.token = $token
$cfg.ui.split = 2
$cfg.ui.pane_channels = @('nonech', 'inherit')

$proto = $cfg.boards | Where-Object { $_.name -eq 'loop1' }
if (-not $proto) { throw '示例配置缺少 loop1' }
$nonech = $proto | ConvertTo-Json -Depth 12 | ConvertFrom-Json
$nonech.name = 'nonech'
$nonech.serial_device = 'loop:nonech'
$nonech.password = ''
$inherit = $proto | ConvertTo-Json -Depth 12 | ConvertFrom-Json
$inherit.name = 'inherit'
$inherit.serial_device = 'loop:inherit'
$inherit.PSObject.Properties.Remove('password')
$cfg.boards = @($nonech, $inherit)

$sshin = [pscustomobject]@{
    name = 'sshin'
    host = '192.0.2.10'
    port = 22
    username = 'demo'
    password = 'sshpw'
    term = 'xterm-256color'
    connect_timeout_ms = 5000
    keepalive = 3
    width = 120
    height = 32
    buffer_size = 0
    log_enabled = $false
    log_dir = ''
}
$cfg.ssh = @($sshin)

$cfgPath = Join-Path $run 'ssh_bridge_config.json'
[IO.File]::WriteAllText($cfgPath, ($cfg | ConvertTo-Json -Depth 20),
                        (New-Object Text.UTF8Encoding($false)))
$raw = [IO.File]::ReadAllText($cfgPath)
$reloaded = $raw | ConvertFrom-Json
$inh = $reloaded.boards | Where-Object { $_.name -eq 'inherit' }
if (-not $inh) { throw 'inherit 通道未写入' }
if ($inh.PSObject.Properties.Name -contains 'password') {
    throw 'inherit 应省略 password 以继承 default'
}

$p = Start-Process -FilePath $exe -WorkingDirectory $run -PassThru
$ready = $false
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 250
    try {
        $null = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$web/status" `
            -Headers @{ Authorization = "Bearer $token" } -TimeoutSec 2
        $ready = $true
        break
    } catch {}
}
if (-not $ready) {
    Get-Content (Join-Path $run 'output\boot.log') -ErrorAction SilentlyContinue | Select-Object -Last 8
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    throw 'S8 测试应用未启动'
}

$result = 1
try {
    & python (Join-Path $here 's8_matrix.py') --port $port --web $web --token $token --config $cfgPath
    $result = $LASTEXITCODE
} finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}
$left = @(Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue).Count
$listen = @(Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue).Count
if ($left -ne 0 -or $listen -ne 0) {
    "S8M-CLEAN 退出清理 FAIL left=$left listen=$listen"
    $result = 1
} else {
    'S8M-CLEAN 退出清理 PASS left=0 listen=0'
}
exit $result
