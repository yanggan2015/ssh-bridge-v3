# 验收报告：PowerShell / Telnet 正式通道（Step 15）

- 日期：2026-11-24
- 分支：`feature/powershell-telnet-20261124`
- 基线：`37fc1c5`（Step14 交付）
- 设计提交：`1d7016c`
- 验收构建：`output/build/ssh_bridge3.exe`（build-audit）
- 结论：**通过**。PowerShell、Telnet 与串口/SSH 在配置、通道树、编辑器、终端、连接状态、Web、Agent、持久化层面均已对等接入。

## 1. 交付范围

| 能力 | 实现 |
|---|---|
| 功能配置 | `ssh_bridge_config.json` 新增 `powershell[]`、`telnet[]`；字段与串口/SSH 同构，未知键原样保留 |
| 通道装配 | `AppCore` 增加两类 Entry、`build_entries`、`reload_config`、`upsert/delete/retry/state_json` |
| PowerShell | 匿名管道 + `powershell.exe -EncodedCommand` 持久 REPL；UTF-8、本地行缓冲/回显、Backspace、启停、watchdog、状态 JSON |
| Telnet | 原始 TCP、非阻塞 connect + select、TCP_NODELAY、最小 IAC（ECHO/SGA/BINARY/NAWS）、子协商过滤、2/4/8/15 秒退避重连 |
| 通道编辑器 | 类型下拉四选一：串口 / SSH / PowerShell / Telnet；独立 PowerShell 参数组；Telnet 复用 SSH 组并隐藏 SSH 专属字段 |
| 菜单/右键 | 新建 PowerShell（40055）、新建 Telnet（40056）；重连、断开、编辑、删除、重载对四类均可用 |
| Web | `/api/channels` GET/POST/delete 四类型；通道页新增按钮、字段组、卡片与终端切换 |
| Agent | `kind/state/features` 四类；`powershell_link`、`telnet_link`；`serial_control` 只对串口为 true |
| 终端 | 四类共用同框终端、分屏绑定、快捷指令、日志、搜索、清屏 |

## 2. 关键修复：删除活动通道导致 UI 卡死

验收中发现：通过 Web API 删除正在显示/在线的 Telnet 通道后，应用进入 Not Responding。

- 根因：`AppCore::reload_config()` 在持有 `config_mu_`、`entries_mu_` 时析构旧 `TelnetBridge`；`stop()` 先 `shutdown()` 再 `join()`，Windows 下 `shutdown(SD_BOTH)` 不一定唤醒已阻塞的 `recv()`，UI 线程永久等待 RX 线程退出。
- 修复：
  1. `stop()` 在 join 前先 `close_socket()`，用 `closesocket()` 强制唤醒阻塞 `recv()`；
  2. 连接建立时设置 `SO_RCVTIMEO=500ms`，`rx_loop()` 将 `WSAETIMEDOUT/WSAEWOULDBLOCK` 视为空闲继续，其它错误才断开。
- 复验：分别删除正在活动的 PowerShell、Telnet 通道后，`/api/channels` 立即返回，应用 `Responding=True`，树节点与分屏绑定同步清空；随后重建通道可再次 ready。

## 3. Step15 专项验收（74/74 PASS）

脚本：`tests/step15_check.ps1`  
结果：`output/step15/result.json`、`output/step15/check-audit2.log`

| 分组 | 结果 | 覆盖 |
|---|---|---|
| 配置 CFG | 7/7 | 小写键加载、字段默认值、未知键保留、配置拆分、四类重名拒绝、Web 新建字段落盘、运行后往返 |
| PowerShell PS | 15/15 | ready、`PS>`、`2+3=5`、会话变量、本地回显、Backspace、CRLF 多行、`cwd` 生效、UTF-8 中文、断开不自动重开、手动重连、杀子进程 watchdog、已连接重连换 PID、错误 shell 报错 |
| Telnet TN | 13/13 | TCP ready、PING 回显、IAC WILL→DO、NAWS 120×32、子协商不泄漏、服务端断开/恢复自动重连、手动断开/重连、已连接重连重新握手、CRLF 线上字节、端口失败错误 |
| UI | 21/21 | 分组计数、菜单入口、PS/Telnet 字段组与默认值、对话框保存、编辑锁定与字段回填、改名及分屏绑定、删除活动通道、Telnet 编辑组、终端输入、截图 |
| Web/Agent | 15/15 | 四类 GET、POST 新建、DELETE、通道页控件、真实浏览器默认值/创建/删除、Agent 页类型列、网页写入回显、scan features、`/write`+`/log`、serial_control 边界 |
| 退出/重启 RG | 3/3 | 退出后无应用/PowerShell 子进程、端口释放；重启后 PS/TN 自动恢复且字段保持；二次退出无残留 |

证据截图：`output/step15/check/shot-terminal.png`、`output/step15/audit2/web-agent.png`、`output/step15/audit2/web-channels.png`。

## 4. 全量回归

| 套件 | 结果 | 证据 |
|---|---|---|
| 编译 | 0 error / 0 warning | `output/step15/build-audit.log` |
| 代码整理 | 144 文件 ALL PASS | `output/step15/quality_gate-audit.log` |
| Step1 基础 | 18/18 ALL PASS（-SkipBuild） | `output/step15/reg-step1-audit.log` |
| Step3 窗口/生命周期 | 14/14 ALL PASS（-SkipBuild） | `output/step15/reg-step3-audit.log` |
| Step14 串口自愈/配置拆分 | 19/19 ALL PASS（本机 COM5 适配） | `output/step15/reg-step14-audit.log` |
| UI 桥 | 41/41 ALL PASS | `output/step15/reg-uibridge-audit.log` |
| 桌面主流程 | 35/35 | `output/step15/desktop-audit-audit2/desktop-main-result.json` |
| 桌面对话框 | 32/32 | `output/step15/desktop-audit-audit2/desktop-dialogs-result.json` |
| 桌面退出 | 2/2 | `output/step15/desktop-audit-audit2/desktop-exit-result.json` |
| Web 12 页 | 162/162 | `output/step15/web-audit/web-functional-result.json` |

## 5. 真实运行环境恢复

验收后已退出隔离应用并恢复真实运行环境：

- 进程：`ssh_bridge3.exe`（PID 14996，工作目录 `C:\Users\admin\.ssh-bridge`）
- 端口：入站 SSH `2222`、Web `18081`、Agent/UI 桥 `18096` 均在 LISTENING
- 通道：`通道 4 · 在线 4`
  - `ok3568` COM5 @115200 ready
  - `ztl` COM3 @1500000 ready
  - `ssh-link-demo1` 39.97.234.200:52222 ready
  - `ssh-link-demo2` sshbridge.online:52223 ready
- 最终 Web 截图：`output/step15/final-web.png`

说明：

- Step14 原脚本固定 COM4；本机当前仅暴露 COM5，因此用等价副本 `output/step15/step14_check_com5.ps1` 在 COM5 上跑同一组断言，未修改原基线脚本。
- Web 审计 `errors=2`、`bad=2` 均为 `/api/relay` 在无继电器配置时的 400，属于既有只读查询路径；162 项功能断言全部通过，无新增页面错误。

## 6. 设计/实现对齐

- 设计文档 JSON 键统一为全小写 `powershell`，代码、清单、Web、Agent 一致。
- PowerShell 默认：`powershell.exe`、`-NoLogo -NoProfile -ExecutionPolicy Bypass`、行尾 `cr`、不写时间戳。
- Telnet 默认：端口 `23`、超时 `5000ms`、行尾 `crlf`、`120×32`、不写时间戳。
- 新建对话框默认值：PowerShell 行尾 `CR`；Telnet 端口 `23`；类型在 SSH/Telnet 间切换时端口 22/23 自动互换。

## 7. 未纳入本步

- PowerShell 仅本机会话，不提供 WinRM 远程宿主。
- Telnet 仅最小 IAC + NAWS，不实现完整选项族、认证子协议与编码协商。
- 上述两项如需扩展，应在后续独立步骤立项，避免改变本步“与串口/SSH 对等”的验收边界。


## 8. 二次完整复核（2026-11-24）

复核不依赖上一轮结论，重新做静态分支扫描 + 扩展动态测试。共发现并修复 3 个真实缺口：

| 缺口 | 根因 | 修复 | 复验 |
|---|---|---|---|
| Web 新建 PowerShell/Telnet 行尾默认错误 | `blankChannel()` 对所有类型先写 `none`，只有后端缺省值正确，网页提交会显式写入 `none` | PS 默认 `cr`、Telnet 默认 `crlf`，其余类型保持 `none` | `WEB-UI-1/4` 通过 |
| Agent 设备页把新通道类型显示成“串口” | 设备表类型列只判断 `ssh`，其余一律“串口” | 四类型分支，PS/Telnet 正确显示 | `WEB-UI-6` 通过 |
| Telnet 已连接状态点“重连”不重新握手 | `reconnect()` 仅 `shutdown()`，Windows 下不能可靠唤醒阻塞 `recv()` | 与 `stop()` 一致，先 `close_socket()` 真正中断接收，再让 RX 循环重连 | `TN-7c` 通过，探针 `ACCEPT 2→3` |

新增/加强的测试：

- PowerShell：配置 `cwd` 生效、UTF-8 中文输出、已连接状态重连并更换 PID。
- Telnet：已连接状态重连重新握手、CRLF 线上字节校验。
- UI：改名同步分屏绑定、PS 编辑回填 program/args、Telnet 新建/编辑/删除活动通道。
- Web：真实 Edge 浏览器打开通道页，校验 PS/Telnet 默认值，走表单创建/删除，校验 Agent 页类型列。
- 持久化：Web 新建 `ps-keep`/`tn-keep`（含 cwd、行尾、尺寸），退出后重启，确认两类均 ready 且字段保持，再删除并二次退出。

最终结果：

- 扩展专项：**74/74 PASS**（`output/step15/check-audit2.log`、`output/step15/result.json`）
- 浏览器通道页：**7/7 PASS**（`output/step15/audit2/web-ui-probe.json`）
- 全量回归全部通过，详见第 4 节。
- 真实环境恢复：**通道 4 · 在线 4**，2222/18081/18096 LISTENING。

补充说明：Web 审计仍为 `errors=2`，均为无继电器设备时 `/api/relay` 的既有 400 只读查询；162 项功能断言全部通过，非本轮新增错误。
