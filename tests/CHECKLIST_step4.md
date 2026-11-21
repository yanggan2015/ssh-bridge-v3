# 步骤 4 验收清单（S4 · 终端窗格）

> 范围：`src/ui/terminal/`（term_types / ansi / scrollback / pane / input / view）、`src/ui/shell` 的视图接入（搜索框、分屏菜单、数据唤醒）、`src/core/bridge` 的 `read_all`
> 出口：本清单全 PASS + `tests\step4_check.ps1` 退出码 0 + `quality_gate` PASS
> 不在本步：通道增删/热应用（S5）、偏好与快捷键（S6）、文件与脚本（S7）

## 1. 验收项（T4）

| 编号 | 项 | 判定 | 自动化 |
|---|---|---|---|
| T4-0 | 启动即建终端视图 | 主窗口存在，且能 `FindWindowExW` 找到 `SB3_TermView` 子窗口 | 枚举子窗口 |
| T4-1 | 首次打开显示尾部历史 | 打开 `ssh-term`：`scrollback=5000`，`last` 行是灌流最后一行 `L109999` | 读 `output\ui_terminal.json` |
| T4-2a | 切成左右两窗格 | 菜单 `40031` 后 `split=2` | `WM_COMMAND` |
| T4-2 | 分屏独立游标不串数据 | 左窗格通道 `loop1`、右窗格通道 `board2`；右窗格不含只在左窗格出现的内容 | 读 panes[].channel / lines |
| T4-2b | 左窗格输入只落左窗格 | 往活动窗格打字后，目标串只在左窗格出现 | `WM_CHAR` 打到视图 |
| T4-3 | 灌流字节全部被解析（不丢行） | `bytes=2420093`（≥ 期望 990000），`total` 行数 ≥ 5000 | 读 panes[].bytes/total |
| T4-4 | 清屏只清本窗格 | 清屏后本窗格行数 0，另一窗格行数不减 | 菜单 `40036` |
| T4-5 | ANSI 颜色生效且不显示转义原文 | `red=True`、`raw_esc=False`、`fg_sample` 为 `cd0000` | 读 panes[].fg_sample/lines |
| T4-6 | 中文不乱码 | 行内容等于 `中文 OK` | 读 lines |
| T4-7 | 搜索命中可定位 | 搜索框输入后 `search_hit=true`，`hit.len=13`，有 `abs/col` | 逐字 `WM_CHAR` 打进搜索框 |
| T4-8 | 输入无延迟堆积 | 打字前后 `bytes` 增量等于或大于输入字节，且无卡死 | 读 bytes |
| T4-9 | 隐藏窗格不耗 CPU | 窗口隐藏 3 秒，进程 CPU 增量 ≤ 50ms | `GetProcessTimes` 差值 |

## 2. 通用必测（G）

| 编号 | 项 | 判定 |
|---|---|---|
| G-1 | 编译无错无警告 | exit=0，error=0，warning=0 |
| G-2 | 测试 SSH 服务器就绪 | paramiko 服务器写出 ready-file，端口可用 |
| G-3 | 全流程无崩溃对话框 | 无 `#32770` 顶层窗口 |
| G-4 | 本体不监听任何端口 | `netstat` 里该 PID 无 `LISTENING` |
| G-5 | 托盘退出 → 退出码 0 | `WM_APP+2` 后退出码 0 |
| G-6 | 无残留进程 | 手尾 `ssh_bridge3` 与测试服务器实例数 0 |
| G-7 | 代码整理 | `scripts\quality_gate.ps1` 退出码 0 |

## 3. 判定备注

1. 闸门每次在 `output\s4check` 重建配置：`default.buffer_size=4194304`、`loop1=loop:s4`、`board2=COM3`、`ssh-term=127.0.0.1:<随机端口>`（buffer 同样 4MB，因为 SSH ring 每行强制加 13 字节时间戳，110000 行原始字节约 2420093 字节）。
2. 观测面是 `output\ui_terminal.json`：`split/active/font_px/panes[{channel,cols,rows,scrollback,total,offset,cursor_row,cursor_col,title,bytes,search_hit,hit,fg_sample,lines}]`；界面只读内存，这个文件只是给闸门与 Agent 的快照。
3. 键盘一律 `SendMessageW(term, WM_CHAR/WM_KEYDOWN, ...)` 打进视图；分屏/清屏用 `WM_COMMAND 40031/40036`；选通道用 `WM_APP+8`（跨进程 `TVM_SELECTITEM` 不触发 `TVN_SELCHANGED`）。
4. 搜索框必须**逐字发 `WM_CHAR`**：跨进程 `SetWindowTextW` 对 EDIT 实际不生效（`WM_SETTEXT` 的字符串指针无法跨进程编组），且 EDIT 也不会为程序化设值补发 `EN_CHANGE`。
5. `-SkipBuild` 跳过编译；`-LongLoad` 追加 T4-3 的 60s 长跑（默认只跑 110k 行突发）。

## 4. 本步明确不做

- 本地回显（回显必须来自板端真实 RX）。
- 六el / 图片协议 / 内嵌浏览器。
- 终端内搜索 ring 以外的内容（只搜当前窗格回看）。
- 通道增删、右键热应用（S5）。
