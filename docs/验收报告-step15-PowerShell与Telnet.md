# 验收报告：PowerShell / Telnet 正式通道（Step 15）

- 日期：2026-11-24
- 分支：`feature/powershell-telnet-20261124`
- 基线：`37fc1c5`（Step14 交付）
- 设计提交：`1d7016c`
- 验收构建：`output/build/ssh_bridge3.exe`（build12）
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

## 3. Step15 专项验收（53/53 PASS）

脚本：`tests/step15_check.ps1`  
结果：`output/step15/result.json`、`output/step15/check-run6.log`

| 分组 | 结果 | 覆盖 |
|---|---|---|
| 配置 CFG | 6/6 | 小写键加载、字段默认值、未知键保留、配置拆分、四类重名拒绝 |
| PowerShell PS | 11/11 | ready、`PS>`、`2+3=5`、会话变量、本地回显、Backspace、CRLF 多行、断开不自动重开、手动重连、杀子进程 watchdog、错误 shell 报错 |
| Telnet TN | 9/9 | TCP ready、PING 回显、IAC WILL→DO、NAWS 120×32、子协商不泄漏、服务端断开/恢复自动重连、手动断开/重连、端口失败错误 |
| UI | 13/13 | 分组计数、菜单入口、PS/Telnet 字段组与默认值、对话框保存、编辑锁定、改名、删除、终端输入、截图 |
| Web/Agent | 9/9 | 四类 GET、POST 新建、DELETE、通道页控件、网页写入回显、scan features、`/write`+`/log`、serial_control 边界 |
| 退出 RG-9 | 1/1 | 退出后无应用/PowerShell 子进程、18181/18196 无 LISTENING |

证据截图：`output/step15/check/shot-terminal.png`。

## 4. 全量回归

| 套件 | 结果 | 证据 |
|---|---|---|
| 编译 | 0 error / 0 warning | `output/step15/build12.log` |
| 代码整理 | 144 文件 ALL PASS | `output/step15/quality_gate.log` |
| Step1 基础 | 19/19 ALL PASS | `output/step15/step1.log` |
| Step3 窗口/生命周期 | 15/15 ALL PASS | `output/step15/step3.log` |
| Step14 串口自愈/配置拆分 | 19/19 ALL PASS（本机 COM5 适配） | `output/step15/step14-com5.log`、`output/step14/result-com5.json` |
| UI 桥 | 41/41 ALL PASS | `output/step15/ui_bridge.log` |
| 桌面主流程 | 35/35 | `output/step15/desktop-audit/desktop-main-result.json` |
| 桌面对话框 | 32/32 | `output/step15/desktop-audit/desktop-dialogs-result.json` |
| 桌面退出 | 2/2 | `output/step15/desktop-audit/desktop-exit-result.json` |
| Web 12 页 | 162/162 | `output/step15/web-audit/web-functional-result.json` |

## 5. 真实运行环境恢复

验收后已退出隔离应用并恢复真实运行环境：

- 进程：`ssh_bridge3.exe`（工作目录 `C:\Users\admin\.ssh-bridge`）
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
