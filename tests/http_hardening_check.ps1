# http_hardening_check.ps1 — Web 写接口输入校验专项
# 覆盖 /service 非法 JSON / on 非布尔、NFS 别名路径穿越与引号注入。
# 使用独立端口 18085/18097，不碰运行中的 18081/18096。

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'output\build\ssh_bridge3.exe'
$dir = Join-Path $root 'output\hardening-test'
$token = 'hardtest-2026'
$base = 'http://127.0.0.1:18085'
$pass = 0
$fail = 0

function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) {
        $script:pass++
        Write-Host "PASS $name : $detail"
    } else {
        $script:fail++
        Write-Host "FAIL $name : $detail" -ForegroundColor Red
    }
}

function Request([string]$method, [string]$path, [string]$body = '') {
    $headers = @{ Authorization = "Bearer $token" }
    try {
        if ($method -eq 'GET') {
            $r = Invoke-WebRequest -Uri ($base + $path) -Headers $headers -TimeoutSec 5
        } else {
            $r = Invoke-WebRequest -Uri ($base + $path) -Method POST -Headers $headers `
                -ContentType 'application/json' -Body $body -TimeoutSec 5
        }
        return [pscustomobject]@{ Status = [int]$r.StatusCode; Body = $r.Content }
    } catch {
        $resp = $_.Exception.Response
        $status = if ($resp) { [int]$resp.StatusCode } else { 0 }
        $text = if ($_.ErrorDetails.Message) { $_.ErrorDetails.Message } else { $_.Exception.Message }
        return [pscustomobject]@{ Status = $status; Body = $text }
    }
}

New-Item -ItemType Directory -Force -Path $dir | Out-Null
$cfg = [ordered]@{
    eadk = @{ tool = 'ssh-bridge' }
    inbound_ssh = @{ enable = $false; port = 2223 }
    agent = @{ enable = $true; port = 18097; token = '' }
    web = @{ enable = $true; port = 18085; token = $token }
    default = @{ username = 'root'; password = ''; buffer_size = 1048576; log_enabled = $false; work_dir = ''; log_dir = '' }
    boards = @()
    ssh = @()
    powershell = @()
    telnet = @()
    tftp = @{ enable = $false; port = 1069; bind_ip = ''; root = ''; allow_write = $true }
    share = @{ enable = $false; port = 18086; root = '' }
    nfs = @{ enable = $false; alias = '/share'; uid = 0; gid = 0; root = '' }
    tcp_forward = @{ enable = $false; listen_ip = '0.0.0.0'; port_base = 23000; target_port = 22; board_cidr = '192.168.1.0/24' }
    tunnel = @{
        enable = $false; map_http = $true; map_inbound_ssh = $false
        exe_path = ''; server_addr = ''; server_port = 0; token = ''
        local_port = 18085; remote_port = 0; ssh_local_port = 2223; ssh_remote_port = 0
    }
    discovery = @{ enable = $false; port = 18072 }
    relay_devices = @()
}
$cfg | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $dir 'ssh_bridge_config.json') -Encoding utf8
$ui = [ordered]@{
    schema = 'ssh-bridge-ui/1'
    ui = @{ close_to_tray = $false; start_minimized = $false; scrollback_lines = 4000; font_size = 15; split = 1; active_pane = 0; show_statusbar = $true; pane_channels = @('') }
    window = @{ rect = '' }
}
$ui | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $dir 'ssh_bridge_ui.json') -Encoding utf8

$p = Start-Process -FilePath $exe -WorkingDirectory $dir -PassThru
$ready = $false
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 250
    try {
        $null = Invoke-RestMethod ($base + '/health') -TimeoutSec 2
        $ready = $true
        break
    } catch {}
}
if (-not $ready) {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Write-Host 'FAIL startup : health not ready' -ForegroundColor Red
    exit 1
}

try {
    $r = Request 'GET' '/health'
    Check 'health' ($r.Status -eq 200) "status=$($r.Status)"

    $r = Request 'POST' '/service' '{}'
    Check 'service-missing-on' ($r.Status -eq 400) "status=$($r.Status) body=$($r.Body)"

    $r = Request 'POST' '/service' '{"name":'
    Check 'service-invalid-json' ($r.Status -eq 400) "status=$($r.Status) body=$($r.Body)"

    $r = Request 'POST' '/service' '{"name":"tftp","on":"yes"}'
    Check 'service-on-not-bool' ($r.Status -eq 400) "status=$($r.Status) body=$($r.Body)"

    $r = Request 'POST' '/api/nfs/config' '{"enable":false,"alias":"/share/../escape","uid":0,"gid":0,"root":"C:/share"}'
    Check 'nfs-alias-traversal' ($r.Status -eq 400) "status=$($r.Status) body=$($r.Body)"

    $r = Request 'POST' '/api/nfs/config' '{"enable":false,"alias":"/sha''re","uid":0,"gid":0,"root":"C:/share"}'
    Check 'nfs-alias-quote' ($r.Status -eq 400) "status=$($r.Status) body=$($r.Body)"

    $r = Request 'POST' '/api/nfs/config' '{"enable":false,"alias":"/share","uid":0,"gid":0,"root":"C:/share"}'
    Check 'nfs-alias-valid' ($r.Status -eq 200) "status=$($r.Status) body=$($r.Body)"

    $r = Request 'GET' '/api/nfs/config'
    $obj = $r.Body | ConvertFrom-Json
    Check 'nfs-config-saved' ($r.Status -eq 200 -and $obj.config.alias -eq '/share') "status=$($r.Status) alias=$($obj.config.alias)"
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Milliseconds 400
}

Write-Host "RESULT: $pass PASS / $fail FAIL"
if ($fail -ne 0) { exit 1 }
exit 0
