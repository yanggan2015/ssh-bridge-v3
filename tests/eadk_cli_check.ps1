# eadk_cli_check.ps1 — 验证 v3 分发与 eadk_board.py CLI 的核心闭环
# 用法: pwsh -NoProfile -File tests\eadk_cli_check.ps1 [-Base http://127.0.0.1:18081] [-Token web-audit-2026] [-Channel loop-chars] [-SshChannel ssh-demo1]
param(
    [string]$Base = 'http://127.0.0.1:18081',
    [string]$Token = 'web-audit-2026',
    [string]$Channel = 'loop-chars',
    [string]$SshChannel = 'ssh-demo1'
)

$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$root = Split-Path -Parent $here
$py = 'python'
$tmp = Join-Path $env:TEMP ("eadk-cli-check-" + [DateTime]::UtcNow.Ticks)
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$results = New-Object System.Collections.Generic.List[object]
$script:failed = 0

function Assert-True([string]$name, [bool]$ok, [string]$detail = '') {
    $results.Add([ordered]@{ name = $name; ok = $ok; detail = $detail })
    if (-not $ok) { $script:failed++ }
}
function Run-Cli([string[]]$cliArgs) {
    $out = & $py (Join-Path $tmp 'eadk_board.py') -B $Base --token $Token @cliArgs 2>&1
    return [pscustomobject]@{ code = $LASTEXITCODE; text = ($out -join "`n") }
}

try {
    # 1) 分发面：脚本与手册可直接从 v3 下载
    $pyUrl = "$Base/share/tools/eadk_board.py"
    $mdUrl = "$Base/share/tools/eadk_board.md"
    $pyResp = Invoke-WebRequest -Uri $pyUrl -UseBasicParsing
    $mdResp = Invoke-WebRequest -Uri $mdUrl -UseBasicParsing
    Assert-True 'share_py_200' ($pyResp.StatusCode -eq 200) "status=$($pyResp.StatusCode) len=$($pyResp.RawContentLength)"
    Assert-True 'share_md_200' ($mdResp.StatusCode -eq 200) "status=$($mdResp.StatusCode) len=$($mdResp.RawContentLength)"
    [System.IO.File]::WriteAllBytes((Join-Path $tmp 'eadk_board.py'), [System.Text.Encoding]::UTF8.GetBytes([string]$pyResp.Content))
    [System.IO.File]::WriteAllText((Join-Path $tmp 'eadk_board.md'), ($mdResp.Content | Out-String), (New-Object System.Text.UTF8Encoding($false)))
    Assert-True 'download_nonempty' ((Get-Item (Join-Path $tmp 'eadk_board.py')).Length -gt 80000)
    $compile = & $py -m py_compile (Join-Path $tmp 'eadk_board.py') 2>&1
    Assert-True 'py_compile' ($LASTEXITCODE -eq 0) ($compile -join ' ')

    # 2) 发现/列表/状态
    $scan = Run-Cli @('scan', '-j')
    Assert-True 'scan_exit0' ($scan.code -eq 0) $scan.text
    $scanJson = $scan.text | ConvertFrom-Json
    Assert-True 'scan_channels' (@($scanJson.channels).Count -ge 1) "count=$(@($scanJson.channels).Count)"
    Assert-True 'scan_has_target' (@($scanJson.channels | Where-Object { $_.name -eq $Channel }).Count -eq 1)

    $list = Run-Cli @('-b', $Channel, 'list')
    Assert-True 'list_exit0' ($list.code -eq 0) $list.text
    Assert-True 'list_connected' ($list.text -match 'connected: True') $list.text
    Assert-True 'list_channel_key' ($list.text -match '键: channel')

    $health = Run-Cli @('-b', $Channel, 'health')
    Assert-True 'health_exit0' ($health.code -eq 0) $health.text
    Assert-True 'health_connected' ($health.text -match 'connected=true')

    $state = Run-Cli @('-b', $Channel, 'state', '-j')
    Assert-True 'state_exit0' ($state.code -eq 0) $state.text
    $stateJson = $state.text | ConvertFrom-Json
    Assert-True 'state_shape' ($stateJson.name -eq $Channel -and $null -ne $stateJson.connected)

    $status = Run-Cli @('-b', $Channel, 'status')
    Assert-True 'status_exit0' ($status.code -eq 0) $status.text
    $statusJson = $status.text | ConvertFrom-Json
    Assert-True 'status_board_name' ($statusJson.board_name -eq $Channel)

    # 3) 写闭环：clear → write → log；send；keys
    $stamp = 'EADK_CLI_' + [DateTime]::UtcNow.ToString('HHmmss')
    $clear = Run-Cli @('-b', $Channel, 'clear')
    Assert-True 'clear_body_channel' ($clear.code -eq 0) $clear.text
    $write = Run-Cli @('-b', $Channel, 'write', $stamp)
    Assert-True 'write_exit0' ($write.code -eq 0) $write.text
    Start-Sleep -Milliseconds 300
    $log = Run-Cli @('-b', $Channel, 'log', 'chars', '400')
    Assert-True 'log_has_write' ($log.code -eq 0 -and $log.text.Contains($stamp)) $log.text

    $sendMark = $stamp + '_SEND'
    $send = Run-Cli @('-b', $Channel, '-w', '1', 'send', $sendMark)
    Assert-True 'send_exit0' ($send.code -eq 0) $send.text
    Assert-True 'send_output' ($send.text.Contains($sendMark)) $send.text

    $keysMark = $stamp + '_KEYS'
    $keys = Run-Cli @('-b', $Channel, 'keys', $keysMark, '0.2', '0.05')
    Assert-True 'keys_exit0' ($keys.code -eq 0) $keys.text
    Start-Sleep -Milliseconds 1200
    $log2 = Run-Cli @('-b', $Channel, 'log', 'chars', '1200')
    Assert-True 'log_has_keys' ($log2.text.Contains($keysMark)) $log2.text

    # 4) 共享文件面：put → md5 → ls → rm
    $local = Join-Path $tmp 'put-check.txt'
    [System.IO.File]::WriteAllText($local, "eadk-cli-check $stamp`r`n", (New-Object System.Text.UTF8Encoding($false)))
    $relDir = 'eadk-check-' + [DateTime]::UtcNow.ToString('HHmmss')
    $rel = "$relDir/put-check.txt"
    $put = Run-Cli @('put', $local, $rel)
    Assert-True 'put_exit0' ($put.code -eq 0) $put.text
    Assert-True 'put_ok_line' ($put.text -match 'put_ok path=')
    $md5 = Run-Cli @('md5', $rel)
    Assert-True 'md5_exit0' ($md5.code -eq 0) $md5.text
    $md5Json = $md5.text | ConvertFrom-Json
    Assert-True 'md5_32' ($md5Json.md5.Length -eq 32)
    $ls = Run-Cli @('ls', $relDir)
    Assert-True 'ls_exit0' ($ls.code -eq 0 -and $ls.text.Contains('put-check.txt')) $ls.text
    $rm = Run-Cli @('rm', $rel)
    Assert-True 'rm_exit0' ($rm.code -eq 0) $rm.text
    $rmDir = Run-Cli @('rm', $relDir)
    Assert-True 'rm_dir_exit0' ($rmDir.code -eq 0) $rmDir.text

    # 5) 真实 SSH 通道阻塞执行（无该通道时跳过）
    if ($SshChannel) {
        $sshHealth = Run-Cli @('-b', $SshChannel, 'health')
        if ($sshHealth.code -eq 0 -and $sshHealth.text -match 'state=shell') {
            $sshMark = $stamp + '_SSH_RUN'
            $run = Run-Cli @('-b', $SshChannel, '-t', '30', 'run', "echo $sshMark")
            Assert-True 'ssh_run_exit0' ($run.code -eq 0) $run.text
            Assert-True 'ssh_run_output' ($run.text.Contains($sshMark)) $run.text
        } else {
            Assert-True 'ssh_run_skipped' $true $sshHealth.text
        }
    }
} catch {
    Assert-True 'uncaught' $false ($_ | Out-String)
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

$okCount = @($results | Where-Object { $_.ok }).Count
$summary = [ordered]@{
    ok = ($script:failed -eq 0)
    passed = $okCount
    total = $results.Count
    base = $Base
    channel = $Channel
    checks = $results
}
$outDir = Join-Path $root 'output\ux-audit'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$jsonPath = Join-Path $outDir 'eadk-cli-check.json'
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $jsonPath -Encoding utf8NoBOM
$summary.checks | ForEach-Object { "{0} {1} {2}" -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
"RESULT passed=$okCount total=$($results.Count) bad=$($script:failed) json=$jsonPath"
if ($script:failed -ne 0) { exit 1 }
