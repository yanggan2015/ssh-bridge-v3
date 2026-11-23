# Step 14 修复与验收报告：配置拆分、串口自愈、通道右键、Web 恢复

- 分支：`feature/config-selfheal-20261123`
- 关键提交：`1bafe87 fix(step14): 串口自愈、通道右键与配置回归规则`
- 工程：`C:\code\ssh-bridge-windows3`
- 最终运行：用户配置启动，Web `18081`、Agent `18096`、入站 SSH `2222` 全部监听，4 通道在线

## 1. 结论

本轮按“先设计 → 再拆分 → 再自愈 → 再右键 → 全量回归”的顺序完成，未采用叠加补丁方式。
最终同一份二进制通过了 **322 项自动化检查**，覆盖配置、串口、窗口、UI 桥、网页 12 页、桌面菜单/对话框和退出释放。

用户报告的“网页打不开”已确认是运行实例读取了旧用户配置：`web.enable=false`、`agent.port=18086`、串口 `auto_open=false`。
修复后不再依赖手工改配置：分发包默认开启 Web/Agent，旧 `18086` 自动迁移到 `18096`，用户配置已拆成功能文件与窗口文件。

## 2. 配置职责拆分

| 文件 | 负责内容 | 不负责内容 |
| --- | --- | --- |
| `ssh_bridge_config.json` | Web/Agent/入站 SSH、TFTP/NFS/SMB、串口/SSH 通道、继电器、转发/隧道、发现服务 | 窗口矩形、状态栏、分屏、字体、托盘行为 |
| `ssh_bridge_ui.json` | `window.rect`、`show_statusbar`、分屏数量/画框通道、活动画框、字体、回滚行数、托盘/启动行为 | 任何服务端口、通道、继电器、TFTP/NFS 配置 |

查找顺序：

1. 显式指定路径
2. 当前工作目录 `ssh_bridge_config.json`
3. exe 同目录 `ssh_bridge_config.json`
4. `%USERPROFILE%\.ssh-bridge\ssh_bridge_config.json`
5. 当前目录 `config.json`（旧兜底）

UI 文件固定与功能文件同目录，名为 `ssh_bridge_ui.json`。
旧统一配置里的 `ui/window` 仍可读；首次保存 UI 时从功能文件剥离，之后两份互不覆盖。未知键保留。

实际用户文件：

- `C:\Users\admin\.ssh-bridge\ssh_bridge_config.json`：5,666 B，无 `ui/window`
- `C:\Users\admin\.ssh-bridge\ssh_bridge_ui.json`：窗口矩形与状态栏
- 旧文件备份：`.bak-20261123`、`.bak-p3-20261123`

## 3. 串口自愈

实现：

- `SerialBridge` 新增 `io_alive_`；`running()` 必须同时满足 `running_ && io_alive_`。
- RX、loop RX、writer 三个线程入口统一挂 `IoAliveGuard`，任何退出路径都会把接收存活置 false。
- RX 的 overlapped 失败由 `continue` 改为退出线程并记录错误；写线程非 pending 写失败同样退出。
- `AppCore::Entry` 新增 `want_open / retry_at_ms / retry_attempt`。
- `start_entry` 打开失败立即排期；`poll_serial_retry` 在 UI 1 秒定时器中执行。
- 退避：2 / 4 / 8 / 15 秒，成功后清零。
- `WM_TIMER` 先跑 watchdog，再判断窗口是否可见；隐藏到托盘仍重连。
- 手动“断开”置 `want_open=false`，两个以上重试周期也不会自动打开；手动“重连”立即恢复。
- 配置重载后 `want_open` 以新 `auto_open` 为准，不沿用旧运行态。

实测结果：

| 场景 | 结果 |
| --- | --- |
| 外部占用 COM4 后冷启动 | `offline`，`want_open=true`，错误 `open COM4 failed: error 5 (port busy)` |
| 失败重试 | `retry_attempt` 从 1 递增到 3 |
| 托盘隐藏期间 | 重试继续，窗口 `visible=false` |
| 释放 COM4 | 自动恢复 `ready`，无需点按钮 |
| 手动断开 | `idle`、`want_open=false`，10 秒仍不重开 |
| 手动重连 | 立即 `ready`、`want_open=true` |
| 重载 `auto_open=false` | `want_open=false` |
| 退出后 | 无残留进程，PowerShell 可重新打开 COM4 |

## 4. 通道树右键菜单

`Shell::WndProc` 的 `WM_NOTIFY` 增加 `NM_RCLICK`：

- 按光标做 `TVM_HITTEST`，命中节点先选中，再弹菜单。
- 具体通道：打开/重连、断开、编辑配置、删除通道；SSH 额外提供 SFTP。
- 分组节点（SSH/串口/Telnet/PowerShell）：新建串口、新建 SSH、重载配置。
- 命令复用现有 `WM_COMMAND`，不复制业务逻辑。

真实窗口验证：

- 通道行右键出现完整菜单，截图 `output\step14\tree-channel-menu2.png`
- 分组行右键只出现新建/重载，截图 `output\step14\tree-context-menu.png`
- 点击“断开”后状态 `idle / want_open=false`
- 点击“打开/重连”后状态 `ready / want_open=true`
- 点击“编辑配置”能打开对应通道对话框

## 5. 编程规则已固化

- `docs/编程规范.md` 新增第 7 节“修复不许破坏相邻功能”：
  - 动代码前写直接/相邻/全局三圈影响面矩阵
  - 修复必须成组回归：专项 + 相邻 + 全局冒烟
  - 触及配置/窗口/生命周期/端口/线程退出必须加跑上一阶段套件
  - 原 PASS 用例变 FAIL 立即停止叠加，先定性与修根因
  - 完成定义要求失败证据 + 成功证据 + 负向场景
- `docs/每步测试闸门.md` 新增“回归闸门”：影响面 + 回归两道闸门，未绿不得进入下一个功能。

## 6. 全量回归结果

| 套件 | 结果 |
| --- | --- |
| 编译 `-Wall -Wextra` | 0 error / 0 warning |
| `quality_gate.ps1` | 138 文件 ALL PASS |
| 配置自检 T1-11~T1-14 | ALL PASS |
| `step1_check.ps1` | 17/17 PASS |
| `step3_check.ps1` | 14/14 PASS |
| `ui_bridge_check.ps1` | 41/41 PASS |
| `step14_check.ps1` | 19/19 PASS |
| Web 12 页功能审计 | 162/162 PASS |
| 桌面主流程 | 35/35 PASS |
| 桌面菜单与对话框 | 32/32 PASS |
| 桌面退出阶段 | 2/2 PASS |
| 合计 | **322 项通过，0 FAIL** |

Web 审计的 2 条 `400 /api/relay` 是测试环境无继电器硬件的预期错误，与上一轮基线一致，不计失败。

## 7. 最终运行状态

从 `output\build` 启动（该目录无本地配置，验证用户目录兜底）：

- 配置文件实际为 `C:\Users\admin\.ssh-bridge\ssh_bridge_config.json`
- `http://127.0.0.1:18081/` = 200
- `http://127.0.0.1:18096/state` = 200
- 监听：`0.0.0.0:2222`、`0.0.0.0:18081`、`127.0.0.1:18096`
- 状态：`通道 4 · 在线 4`
- 串口：`ok3568 / COM5 / 115200 / ready`、`ztl / COM3 / 1500000 / ready`
- SSH：`ssh-link-demo1`、`ssh-link-demo2` 均 ready

网页最终截图：`output\step14\web-18081-final.png`（标题“仪表盘 · ssh-bridge”，WebSocket 已连接）
桌面最终截图：`output\step14\desktop-final.png`（4 通道在线，状态栏显示“通道 4 · 在线 4”）

## 8. 复现命令

```powershell
# 专项（约 50 秒，会占用 COM4 做失败注入）
pwsh -NoProfile -File tests\step14_check.ps1 -SkipBuild

# 串口/配置/日志
pwsh -NoProfile -File tests\step1_check.ps1 -SkipBuild

# 窗口壳/生命周期
pwsh -NoProfile -File tests\step3_check.ps1 -SkipBuild

# UI 桥 41 项
pwsh -NoProfile -File tests\ui_bridge_check.ps1 -SkipBuild

# 桌面三阶段
pwsh -NoProfile -File tests\ui_full_audit2.ps1 -Stage main
pwsh -NoProfile -File tests\ui_full_audit2.ps1 -Stage dialogs
pwsh -NoProfile -File tests\ui_full_audit2.ps1 -Stage exit

# Web 162 项（需先用测试配置启动 terminal-test 实例）
$env:NODE_PATH='C:\code\ssh-bridge-windows3\output\webtest\node_modules'
node output\ui-full-audit-20261123\web_functional.js
```

## 9. 已知边界

- 本轮没有做物理拔插串口的破坏性试验；RX/写线程异常退出的状态闭环由 `IoAliveGuard` 源码守卫和“端口占用失败 / 手动断开 / 退出释放”负向路径覆盖。
- Web 审计会在测试实例中临时增删通道、动作、继电器和目录；测试结束已恢复 `output\terminal-test` 的测试配置，未改动用户配置。
- 用户配置若被外部程序重新写回 `web.enable=false`，应用会按配置关闭 Web；这是显式配置语义，不是启动缺陷。
