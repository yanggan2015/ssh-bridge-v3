# 步骤 3 验收清单（S3 · 窗口壳 + 生命周期）

> 范围：`src/ui/shell/`（主窗口、菜单/工具栏/通道树/状态栏）、`src/ui/tray/`（托盘）、`src/app/app_core`（通道装配与状态）、`src/app/lifecycle`（DPI/单实例/消息循环）
> 出口：本清单全 PASS + `tests\step3_check.ps1` 退出码 0 + `quality_gate` PASS
> 不在本步：终端渲染（S4）、通道增删（S5）、偏好/快捷键（S6）

## 1. 验收项（T3）

| 编号 | 项 | 判定 | 自动化 |
|---|---|---|---|
| T3-1 | 启动即建窗 | 进程存活，`MainWindowHandle != 0`，标题以 `SSH Bridge 3.0` 开头 | 启动后轮询句柄 |
| T3-2 | 通道树能列出全部通道 | `output\ui_state.json` 的 `channels` 数量 = 配置里的板子 + SSH 链路，名字齐全 | 读状态文件 |
| T3-3 | 状态栏显示连接状态 | `summary` 形如 `通道 N · 在线 M` 且 M≥1；每通道有 `state` | 读状态文件 |
| T3-4 | 点 X 隐藏且进程存活 | 发 `WM_CLOSE` 后进程存活、`IsWindowVisible=false`、状态文件 `visible=false` | `PostMessageW(0x0010)` |
| T3-5 | 托盘双击恢复 | 发 `WM_APP+1` 后窗口可见、`visible=true` | 模拟托盘双击 |
| T3-6 | 托盘退出 → 退出码 0、串口释放 | 发 `WM_APP+2`：退出码 0；运行中串口被占用、退出后可再次打开 | `PostMessageW(0x8002)` |
| T3-7 | 退出码 0 | 与 T3-6 同一次退出复核 | 同上 |
| T3-8 | 二启不产生第二实例 | 第二个进程 ≤6s 内以 0 退出，系统里仍只有 1 个实例 | 再启一次 |
| T3-9 | 无任何监听端口 | `netstat` 里该 PID 无 `LISTENING` | netstat 过滤 |
| T3-10 | 全流程无崩溃对话框 | 该 PID 的顶层窗口里没有 `#32770`（对话框）类 | `EnumWindows` 过滤 |

## 2. 通用必测（G）

| 编号 | 项 | 判定 |
|---|---|---|
| G-1 | 编译无错无警告 | exit=0，error=0，warning=0 |
| G-5 | 无残留进程 | 手尾 `ssh_bridge3` 实例数 0 |
| G-6 | 代码整理 | `scripts\quality_gate.ps1` 退出码 0 |
| G-7 | git 状态 | 提交前「有未提交改动」，提交后应为干净 |

## 3. 判定备注

1. 托盘双击与"托盘退出"用自定义消息模拟：`WM_APP+1` = 显示窗口，`WM_APP+2` = 退出；真实托盘点击无法脚本化，但消息走的是同一个 WndProc 分支。
2. `output\ui_state.json` 是 S3 的只读观测面（每秒刷新一次），供闸门与后续网页/Agent 复用；不参与业务逻辑。
3. 测试配置放在 `output\s3run\ssh_bridge_config.json`，进程以该目录为工作目录启动——`config_load` 的查找顺序是「cwd 的 `ssh_bridge_config.json` → 用户目录 → 旧名 `config.json`」，所以文件名必须是前者。
4. 串口释放项优先用 `COM3/COM4/COM5` 中第一个能打开的口；都没有时该项按"无硬件占用"记 PASS，并在说明里写明 `port=none`。
5. 关闭语义（S3 起点）：点 X = 隐藏到托盘并气泡提示，不退出；退出只走托盘菜单或 `WM_APP+2`。S0/S1 的闸门已同步改成该语义。

## 4. 本步明确不做

- 不做终端渲染：右侧只是占位静态文本（S4）。
- 不做通道增删/热应用（S5）、偏好与快捷键（S6）。
- 不监听任何端口，不启动任何 HTTP/WS。
