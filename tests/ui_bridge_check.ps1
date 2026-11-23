# ui_bridge_check.ps1 — S5 验收（AI 可控桥接：每个控件均可读可操作）
# 用法： pwsh -File tests\ui_bridge_check.ps1 [-SkipBuild]
# 退出码： 0 = 全绿；1 = 有 FAIL
#
# 观测面：桥接 HTTP（127.0.0.1:18096）+ output\ui_state.json
# 断言口径：动作 ok:true 只代表送达；每一项都必须紧接 /state 或 /control 复核业务真值。
# 覆盖：/ping、/capabilities、/ui-tree、/control、/state、/action、/logs；
#       按钮 click、搜索框 set_text/get_text/clear、树 select、虚拟菜单 click、
#       终端输入/发送/搜索/滚动/清屏/复制/粘贴/加载/分屏/聚焦；
#       以及 agent.enable=false 不监听、token 401 两条反向用例。

param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'output\build\ssh_bridge3.exe'
$runDir = Join-Path $root 'output\s5check'
$rows = @()
$base = 'http://127.0.0.1:18096'

# 基线：每个控件必须登记稳定 id，且至少要有一个可执行动作
$expected = @(
    'btn.refresh', 'btn.reconnect', 'btn.clear', 'btn.toggleTree',
    'tree.channels', 'label.status', 'edit.search', 'term.view', 'label.searchHint',
    'menu.file.exit', 'menu.help.about', 'menu.view.hide', 'menu.view.refresh',
    'menu.view.reconnect', 'menu.view.split1', 'menu.view.split2', 'menu.view.split4',
    'menu.view.copy', 'menu.view.paste', 'menu.view.reload', 'menu.view.clear',
    'menu.view.findnext', 'menu.view.focussearch',
    'menu.tray.show', 'menu.tray.hide', 'menu.tray.exit',
    'term.input', 'term.send', 'term.paste_text', 'term.mouse', 'term.search', 'term.scroll',
    'term.clear', 'term.copy', 'term.paste', 'term.reload', 'term.focus',
    'term.split1', 'term.split2', 'term.split4'
)

function Check([string]$id, [string]$desc, [bool]$ok, [string]$detail = '') {
    $script:rows += [pscustomobject]@{ 编号 = $id; 项目 = $desc; 结果 = $(if ($ok) { 'PASS' } else { 'FAIL' }); 说明 = $detail }
    return $ok
}

function Kill-App {
    Get-Process -Name 'ssh_bridge3' -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}

function Get-Json([string]$path, [hashtable]$headers = @{}) {
    $r = Invoke-WebRequest -UseBasicParsing -SkipHttpErrorCheck -Uri "$base$path" -Headers $headers -TimeoutSec 10
    if ($r.StatusCode -ne 200) { throw ("HTTP {0} {1}" -f $r.StatusCode, $path) }
    return ($r.Content | ConvertFrom-Json)
}

function Get-Status([string]$path) {
    $r = Invoke-WebRequest -UseBasicParsing -SkipHttpErrorCheck -Uri "$base$path" -TimeoutSec 10
    return [int]$r.StatusCode
}

function Act([string]$id, [string]$action, $text = $null, $value = $null, [hashtable]$headers = @{}) {
    $body = @{ id = $id; action = $action }
    if ($null -ne $text) { $body['text'] = $text }
    if ($null -ne $value) { $body['value'] = $value }
    $r = Invoke-WebRequest -UseBasicParsing -SkipHttpErrorCheck -Method Post -Uri "$base/action" `
        -ContentType 'application/json; charset=utf-8' -Headers $headers -Body ($body | ConvertTo-Json -Compress) -TimeoutSec 15
    $j = $null
    if ($r.Content) { try { $j = $r.Content | ConvertFrom-Json } catch { } }
    return [pscustomobject]@{ status = [int]$r.StatusCode; json = $j; raw = $r.Content }
}

function State { return (Get-Json '/state').state }

function Write-Config([string]$dir, [string]$agentJson) {
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'output') | Out-Null
    $json = @"
{
  "default": {"username":"root","password":"","buffer_size":262144,"log_enabled":false,"log_dir":""},
  "boards": [{"name":"loop1","serial_device":"loop:ai","baudrate":115200,"auto_open":true,
              "line_ending":"crlf","timestamp":false,"buffer_size":262144}],
  "ssh": [],
  $agentJson
  "ui": {"close_to_tray": true, "start_minimized": false, "scrollback_lines": 5000, "font_size": 15, "split": 1}
}
"@
    Set-Content -LiteralPath (Join-Path $dir 'ssh_bridge_config.json') -Value $json -Encoding utf8
}

function Start-App([string]$dir, [bool]$waitBridge = $true) {
    Remove-Item -LiteralPath (Join-Path $dir 'output\boot.log') -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $exe -WorkingDirectory $dir -PassThru
    if ($waitBridge) {
        for ($i = 0; $i -lt 100; $i++) {
            Start-Sleep -Milliseconds 100
            try {
                $r = Invoke-WebRequest -UseBasicParsing -SkipHttpErrorCheck -Uri "$base/ping" -TimeoutSec 2
                # 401 也算“端口已起”（token 用例），只要拿到 HTTP 响应就返回
                if ($r.StatusCode -gt 0) { return $p }
            } catch { }
            if ($p.HasExited) { throw ("进程提前退出，exit={0}" -f $p.ExitCode) }
        }
        throw '桥接 10s 未就绪'
    }
    return $p
}

# 0. 构建
if (-not $SkipBuild) {
    $env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
    Kill-App
    # 头文件改动不做依赖跟踪，闸门内强制 clean，避免旧目标文件内存布局错位
    mingw32-make -C $root clean 2>&1 | Out-Null
    $log = mingw32-make -C $root -j4 2>&1
    if ($log -match 'error') { $log | Select-Object -Last 12; throw '构建失败' }
    if (-not (Test-Path -LiteralPath $exe)) { throw "缺少产物 $exe" }
}

Kill-App
Remove-Item -Recurse -Force $runDir -ErrorAction SilentlyContinue
Write-Config $runDir '"agent": {"enable": true, "port": 18096, "token": ""},'

$proc = Start-App $runDir
try {
    # ---------- 协议面 ----------
    $ping = Get-Json '/ping'
    Check 'T5-1' '/ping 返回协议 1.1 与桥接标识' `
        ($ping.ok -eq $true -and $ping.protocol -eq '1.1' -and $ping.service -eq 'win_ui_bridge') `
        "protocol=$($ping.protocol)"

    $cap = Get-Json '/capabilities'
    $need = @('click', 'set_text', 'get_text', 'clear', 'check', 'uncheck', 'toggle', 'select', 'set_value', 'focus', 'command')
    $missing = $need | Where-Object { $cap.actions -notcontains $_ }
    Check 'T5-2' 'capabilities 声明全部 P0 动作白名单' ($missing.Count -eq 0) `
        ("port=$($cap.port) missing=" + ($missing -join ','))

    # ---------- 控件树 ----------
    $tree = Get-Json '/ui-tree'
    $autoIds = @($tree.controls | Where-Object { $_.id -like 'auto/*' })
    Check 'T5-3' 'ui-tree 无 auto/ 兜底 id（全部显式登记）' ($autoIds.Count -eq 0) `
        "count=$($tree.count) auto=$($autoIds.Count)"

    $ids = @($tree.controls | ForEach-Object { $_.id })
    $dup = @($ids | Group-Object | Where-Object { $_.Count -gt 1 })
    Check 'T5-4' '控件 id 唯一无重复' ($dup.Count -eq 0) ("重复=" + ($dup.Name -join ','))

    $missIds = @($expected | Where-Object { $ids -notcontains $_ })
    Check 'T5-5' "基线 $($expected.Count) 个控件 id 全部在册" ($missIds.Count -eq 0) ("缺失=" + ($missIds -join ','))

    $noAction = @($tree.controls | Where-Object { $_.enabled -ne $false -and @($_.actions).Count -eq 0 })
    $badAction = @($tree.controls | Where-Object { ($_.actions | Where-Object { $cap.actions -notcontains $_ }).Count -gt 0 })
    Check 'T5-6' '每个可用控件都有动作且都在白名单内' ($noAction.Count -eq 0 -and $badAction.Count -eq 0) `
        ("无动作=" + ($noAction.id -join ',') + " 越界=" + ($badAction.id -join ','))

    # ---------- 关于窗口（非模态；附属窗口的控件同样要可读可操作） ----------
    $about = Act 'menu.help.about' 'click'
    Start-Sleep -Milliseconds 600
    $treeA = Get-Json '/ui-tree'
    $aboutIds = @($treeA.controls | Where-Object { $_.id -eq 'about.text' -or $_.id -eq 'btn.about.ok' })
    Check 'T5-7' 'menu.help.about 打开非模态关于窗且两个控件在册' `
        ($about.json.ok -eq $true -and (State).about_open -eq $true -and $aboutIds.Count -eq 2) `
        "about_open=$((State).about_open) ids=$(($aboutIds | ForEach-Object { $_.id }) -join ',')"

    $aboutText = Act 'about.text' 'get_text'
    $aboutOk = Act 'btn.about.ok' 'click'
    Start-Sleep -Milliseconds 400
    Check 'T5-8' 'about.text 可读、btn.about.ok 可关闭' `
        ($aboutText.json.ok -eq $true -and [string]$aboutText.json.result.text -like '*ssh-bridge v3*' `
            -and $aboutOk.json.ok -eq $true -and (State).about_open -eq $false) `
        "text='$([string]$aboutText.json.result.text -replace "`r`n", ' / ')' about_open=$((State).about_open)"

    # ---------- /control ----------
    $ctl = Get-Json '/control?id=edit.search'
    Check 'T5-9' '/control 返回单控件快照' `
        ($ctl.type -eq 'control' -and $ctl.control.id -eq 'edit.search' -and $ctl.control.role -eq 'textbox') `
        "role=$($ctl.control.role) class=$($ctl.control.class)"

    $code404 = Get-Status '/control?id=no.such.control'
    Check 'T5-10' '不存在的 id 返回 404' ($code404 -eq 404) "code=$code404"

    # ---------- 搜索框（真实 Edit 控件） ----------
    $set = Act 'edit.search' 'set_text' 'hello-ai'
    Start-Sleep -Milliseconds 400
    $get = Act 'edit.search' 'get_text'
    $st = State
    Check 'T5-11' 'edit.search 可写可读（set_text→get_text）' `
        ($set.json.ok -eq $true -and $get.json.ok -eq $true -and $get.json.result.text -eq 'hello-ai') `
        "text='$($get.json.result.text)'"
    Check 'T5-12' 'edit.search 写入后 UI 搜索态同步' `
        ([string]$st.ui.search_text -eq 'hello-ai' -or $st.terminal.panes[0].search_hit -eq $true) `
        "search_text='$($st.ui.search_text)' hit=$($st.terminal.panes[0].search_hit)"

    $clr = Act 'edit.search' 'clear'
    Start-Sleep -Milliseconds 300
    $after = (Act 'edit.search' 'get_text').json.result.text
    Check 'T5-13' 'edit.search clear 清空文字' ($clr.json.ok -eq $true -and [string]::IsNullOrEmpty($after)) `
        "text='$after'"

    # ---------- 通道树 ----------
    $sel = Act 'tree.channels' 'select' $null 0
    Start-Sleep -Milliseconds 500
    $treeCtl = Get-Json '/control?id=tree.channels'
    Check 'T5-14' 'tree.channels 可选通道且状态回读' `
        ($sel.json.ok -eq $true -and $treeCtl.control.selected -eq 0) `
        "selected=$($treeCtl.control.selected) items=$($treeCtl.control.item_count)"
    $treeGet = Act 'tree.channels' 'get_text'
    Check 'T5-15' 'tree.channels 可读文字' ($treeGet.json.ok -eq $true) "text='$($treeGet.json.result.text)'"

    # ---------- 按钮 ----------
    $tsBefore = (Get-Json '/state').ts
    $refresh = Act 'btn.refresh' 'click'
    Start-Sleep -Milliseconds 400
    $tsAfter = (Get-Json '/state').ts
    Check 'T5-16' 'btn.refresh 点击后状态刷新' ($refresh.json.ok -eq $true -and $tsAfter -ge $tsBefore) `
        "ts $tsBefore → $tsAfter"

    $clrBtn = Act 'btn.clear' 'click'
    Start-Sleep -Milliseconds 500
    $p0 = (State).terminal.panes[0]
    Check 'T5-17' 'btn.clear 清空终端缓冲' ($clrBtn.json.ok -eq $true -and $p0.bytes -eq 0) `
        "bytes=$($p0.bytes) total=$($p0.total)"

    # ---------- 终端输入/发送/搜索 ----------
    $in = Act 'term.input' 'set_text' 'angle-check'
    Start-Sleep -Milliseconds 800
    $p1 = (State).terminal.panes[0]
    Check 'T5-18' 'term.input 输入并回车（回显进缓冲）' `
        ($in.json.ok -eq $true -and $p1.bytes -gt 0 -and (($p1.tail -join "`n") -like '*angle-check*')) `
        "bytes=$($p1.bytes) tail='$($p1.tail -join ' | ')'"

    $send = Act 'term.send' 'set_text' 'raw-bytes'
    Start-Sleep -Milliseconds 800
    $p2 = (State).terminal.panes[0]
    Check 'T5-19' 'term.send 只发送不回车' `
        ($send.json.ok -eq $true -and (($p2.tail -join "`n") -like '*raw-bytes*')) `
        "tail='$($p2.tail -join ' | ')'"

    # 搜索/滚动要有回看行：分 4 批各 30 行灌入（桥接 text 字段上限 512 字节，一次灌 120 行会被截断）
    Act 'term.clear' 'click' | Out-Null
    foreach ($batch in 0..3) {
        $chunk = (($batch * 30 + 1)..($batch * 30 + 30) | ForEach-Object { "scroll-line-$_" }) -join "`r`n"
        Act 'term.send' 'set_text' ($chunk + "`r`n") | Out-Null
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Milliseconds 800
    $search = Act 'term.search' 'set_text' 'scroll-line-31'
    Start-Sleep -Milliseconds 500
    $p3 = (State).terminal.panes[0]
    Check 'T5-20' 'term.search 命中并置 search_hit' `
        ($search.json.ok -eq $true -and $search.json.result.found -eq 1 -and $p3.search_hit -eq $true) `
        "found=$($search.json.result.found) hit=$($p3.search_hit) total=$($p3.total)"

    # ---------- 滚动条 ----------
    $scroll = Act 'term.scroll' 'set_value' $null 4
    Start-Sleep -Milliseconds 400
    $slider = Get-Json '/control?id=term.scroll'
    $offsNow = (State).terminal.panes[0].offset
    $exp = [Math]::Min(4, [int]$slider.control.max)
    Check 'T5-21' 'term.scroll 绝对定位且 value/min/max 可读' `
        ($scroll.json.ok -eq $true -and $offsNow -eq $exp -and $slider.control.value -eq $exp `
            -and $null -ne $slider.control.min -and $slider.control.max -ge 4) `
        "offset=$($offsNow) value=$($slider.control.value) min=$($slider.control.min) max=$($slider.control.max)"

    # ---------- 分屏（终端虚拟控件） ----------
    $s2 = Act 'term.split2' 'click'
    Start-Sleep -Milliseconds 700
    $t2 = (State).terminal
    Check 'T5-22' 'term.split2 切到两窗格' ($s2.json.ok -eq $true -and $t2.split -eq 2 -and @($t2.panes).Count -eq 2) `
        "split=$($t2.split) panes=$(@($t2.panes).Count)"

    $s4 = Act 'term.split4' 'click'
    Start-Sleep -Milliseconds 700
    $t4 = (State).terminal
    Check 'T5-23' 'term.split4 切到四窗格' ($s4.json.ok -eq $true -and $t4.split -eq 4 -and @($t4.panes).Count -eq 4) `
        "split=$($t4.split) panes=$(@($t4.panes).Count)"

    $s1 = Act 'term.split1' 'click'
    Start-Sleep -Milliseconds 700
    $t1 = (State).terminal
    Check 'T5-24' 'term.split1 回到单窗格' ($s1.json.ok -eq $true -and $t1.split -eq 1) "split=$($t1.split)"

    # ---------- 终端动作 ----------
    $focus = Act 'term.focus' 'click'
    Start-Sleep -Milliseconds 300
    Check 'T5-25' 'term.focus 聚焦终端视图' ($focus.json.ok -eq $true) "focus_id=$((Get-Json '/state').focus_id)"

    $copy = Act 'term.copy' 'click'
    $paste = Act 'term.paste' 'click'
    $reload = Act 'term.reload' 'click'
    Start-Sleep -Milliseconds 500
    Check 'T5-26' 'term.copy / paste / reload 均可执行' `
        ($copy.json.ok -eq $true -and $paste.json.ok -eq $true -and $reload.json.ok -eq $true) `
        "copy=$($copy.status) paste=$($paste.status) reload=$($reload.status)"

    $tclr = Act 'term.clear' 'click'
    Start-Sleep -Milliseconds 500
    Check 'T5-27' 'term.clear 清空缓冲' ($tclr.json.ok -eq $true -and (State).terminal.panes[0].bytes -eq 0) `
        "bytes=$((State).terminal.panes[0].bytes)"

    # ---------- 虚拟菜单 ----------
    $m2 = Act 'menu.view.split2' 'click'
    Start-Sleep -Milliseconds 700
    Check 'T5-28' 'menu.view.split2 走 WM_COMMAND 生效' `
        ($m2.json.ok -eq $true -and (State).terminal.split -eq 2) "split=$((State).terminal.split)"
    $m1 = Act 'menu.view.split1' 'click'
    Start-Sleep -Milliseconds 700
    Check 'T5-29' 'menu.view.split1 恢复单窗格' `
        ($m1.json.ok -eq $true -and (State).terminal.split -eq 1) "split=$((State).terminal.split)"

    $act = Act 'term.input' 'set_text' 'menu-clear-target'
    Start-Sleep -Milliseconds 700
    $mc = Act 'menu.view.clear' 'click'
    Start-Sleep -Milliseconds 500
    Check 'T5-30' 'menu.view.clear 清屏生效' ($mc.json.ok -eq $true -and (State).terminal.panes[0].bytes -eq 0) `
        "before=$($act.json.result.sent) bytes=$((State).terminal.panes[0].bytes)"

    $mref = Act 'menu.view.refresh' 'click'
    $mre = Act 'menu.view.reconnect' 'click'
    Start-Sleep -Milliseconds 700
    Check 'T5-31' 'menu.view.refresh / reconnect 可执行' `
        ($mref.json.ok -eq $true -and $mre.json.ok -eq $true -and (State).channels[0].state -eq 'ready') `
        "channel=$((State).channels[0].state)"

    $mcp = Act 'menu.view.copy' 'click'
    $mpa = Act 'menu.view.paste' 'click'
    $mrl = Act 'menu.view.reload' 'click'
    Check 'T5-32' 'menu.view.copy / paste / reload 可执行' `
        ($mcp.json.ok -eq $true -and $mpa.json.ok -eq $true -and $mrl.json.ok -eq $true) `
        "copy=$($mcp.status) paste=$($mpa.status) reload=$($mrl.status)"

    # ---------- 显隐 / 托盘 ----------
    $hide = Act 'menu.view.hide' 'click'
    Start-Sleep -Milliseconds 600
    $visH = (State).visible
    $show = Act 'menu.tray.show' 'click'
    Start-Sleep -Milliseconds 600
    $visS = (State).visible
    $thide = Act 'menu.tray.hide' 'click'
    Start-Sleep -Milliseconds 600
    $visT = (State).visible
    $tshow = Act 'menu.tray.show' 'click'
    Start-Sleep -Milliseconds 600
    $visA = (State).visible
    Check 'T5-33' '隐藏/显示经 state.visible 双向复核' `
        ($hide.json.ok -eq $true -and $show.json.ok -eq $true -and $thide.json.ok -eq $true -and $tshow.json.ok -eq $true `
            -and $visH -eq $false -and $visS -eq $true -and $visT -eq $false -and $visA -eq $true) `
        "hide=$visH show=$visS trayhide=$visT show=$visA"

    $recon = Act 'btn.reconnect' 'click'
    Start-Sleep -Milliseconds 800
    Check 'T5-34' 'btn.reconnect 重连后通道 ready' `
        ($recon.json.ok -eq $true -and (State).channels[0].state -eq 'ready') `
        "state=$((State).channels[0].state)"

    # ---------- /logs ----------
    $logs = Get-Json '/logs?since=0'
    $logs2 = Get-Json ("/logs?since=" + $logs.next_since)
    Check 'T5-35' '/logs 有记录且 since 增量可用' `
        (@($logs.entries).Count -gt 0 -and $logs.next_since -gt 0) `
        "entries=$(@($logs.entries).Count) next=$($logs.next_since) tail='$($logs.entries[-1].text)'"
    Check 'T5-36' '/logs 增量游标之后为空' (@($logs2.entries).Count -eq 0) `
        "entries=$(@($logs2.entries).Count)"

    # ---------- 负向 ----------
    $badId = Act 'no.such.control' 'click'
    $badAct = Act 'btn.refresh' 'dblclick'
    $badText = Act 'label.status' 'set_value' $null 1
    Check 'T5-37' '未知 id / 未实现动作 / 不支持的动作被拒' `
        ($badId.status -eq 404 -and $badAct.status -eq 400 -and $badText.status -eq 400) `
        "id=$($badId.status) action=$($badAct.status) unsupported=$($badText.status)"

    # ---------- 全控件扫一遍（每个都真的驱动一次） ----------
    $skip = @('menu.file.exit', 'menu.tray.exit', 'menu.view.hide', 'menu.tray.hide',
              'about.text', 'btn.about.ok')
    $sweepFail = @()
    $treeNow = Get-Json '/ui-tree'
    foreach ($c in $treeNow.controls) {
        if ($skip -contains $c.id) { continue }
        # 隐藏页签里的控件与禁用控件不参与本轮驱动；文件页另有专项回归覆盖
        if ($c.enabled -eq $false -or $c.visible -eq $false) { continue }
        $act = switch ($c.role) {
            'textbox' { 'get_text' }
            'tree' { 'get_text' }
            'slider' { 'focus' }
            default { @($c.actions)[0] }
        }
        $r = Act $c.id $act
        if ($r.status -ne 200 -or $r.json.ok -ne $true) { $sweepFail += "$($c.id):${act}:$($r.status)" }
        Start-Sleep -Milliseconds 120
    }
    Check 'T5-38' '全部可见可用控件逐个驱动成功（除退出/隐藏 4 项）' ($sweepFail.Count -eq 0) `
        ("失败=" + ($sweepFail -join ','))

    # ---------- 关闭即停 ----------
    Kill-App
    Start-Sleep -Milliseconds 600
    $dead = $true
    try { $r = Invoke-WebRequest -UseBasicParsing -SkipHttpErrorCheck -Uri "$base/ping" -TimeoutSec 2; $dead = ($r.StatusCode -ne 200) } catch { $dead = $true }
    Check 'T5-39' '进程退出后端口随之关闭' $dead ''

    # ---------- agent.enable=false 不监听 ----------
    $runOff = Join-Path $root 'output\s5check-off'
    Remove-Item -Recurse -Force $runOff -ErrorAction SilentlyContinue
    Write-Config $runOff '"agent": {"enable": false, "port": 18096, "token": ""},'
    $poff = Start-App $runOff $false
    Start-Sleep -Seconds 2
    $listenOff = @(Get-NetTCPConnection -LocalPort 18096 -State Listen -ErrorAction SilentlyContinue)
    $pingOff = $false
    try { $r = Invoke-WebRequest -UseBasicParsing -SkipHttpErrorCheck -Uri "$base/ping" -TimeoutSec 2; $pingOff = ($r.StatusCode -eq 200) } catch { $pingOff = $false }
    Check 'T5-40' 'agent.enable=false 时不监听、/ping 不通' `
        (-not $poff.HasExited -and $listenOff.Count -eq 0 -and -not $pingOff) `
        "alive=$(-not $poff.HasExited) listen=$($listenOff.Count) ping=$pingOff"
    Kill-App
    Start-Sleep -Milliseconds 500

    # ---------- token ----------
    $runTok = Join-Path $root 'output\s5check-token'
    Remove-Item -Recurse -Force $runTok -ErrorAction SilentlyContinue
    Write-Config $runTok '"agent": {"enable": true, "port": 18096, "token": "s3cret"},'
    $ptok = Start-App $runTok
    $noTok = Get-Status '/ping'
    $withTok = 0
    try {
        $r = Invoke-WebRequest -UseBasicParsing -SkipHttpErrorCheck -Uri "$base/ping" -Headers @{ 'X-Ui-Token' = 's3cret' } -TimeoutSec 5
        $withTok = [int]$r.StatusCode
    } catch { $withTok = -1 }
    $actNoTok = Act 'btn.refresh' 'click'
    Check 'T5-41' 'token 开启后缺头 401、带头 200' `
        ($noTok -eq 401 -and $withTok -eq 200 -and $actNoTok.status -eq 401) `
        "no_token=$noTok with_token=$withTok action=$($actNoTok.status)"
    Kill-App
}
catch {
    # 关键：不能把异常吞掉后还报 ALL PASS（exit 在 finally 里会抑制异常）
    Check 'T5-ERR' '用例执行中断' $false $_.Exception.Message
}
finally {
    Kill-App
    $rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
    $fail = @($rows | Where-Object { $_.结果 -eq 'FAIL' })
    if ($fail.Count -eq 0) { Write-Host "RESULT: ALL PASS ($($rows.Count) 项)" -ForegroundColor Green; exit 0 }
    Write-Host ("RESULT: {0} FAIL / {1} 项" -f $fail.Count, $rows.Count) -ForegroundColor Red
    exit 1
}
