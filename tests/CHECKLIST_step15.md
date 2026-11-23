# CHECKLIST Step 15：PowerShell / Telnet 正式通道

判定：PASS = 有命令输出、状态 JSON 或截图证据；FAIL = 与期望不符。自动化脚本 `tests/step15_check.ps1` 负责带 `[AUTO]` 的条目。

## A. 配置

| 编号 | 项目 | 期望 | 方式 |
|---|---|---|---|
| CFG-1 | 功能配置加载 | `powerShell`、`telnet` 数组被解析，字段默认值正确 | [AUTO] |
| CFG-2 | 配置往返 | 保存重载后四类通道与未知键都不丢 | [AUTO] |
| CFG-3 | 配置拆分 | 新字段只写 `ssh_bridge_config.json`，不写 `ssh_bridge_ui.json` | [AUTO] |
| CFG-4 | 通道名唯一 | 四类之间重名被拒绝，改名同步分屏绑定 | [AUTO] |

## B. PowerShell 通道

| 编号 | 项目 | 期望 | 方式 |
|---|---|---|---|
| PS-1 | 自动启动 | 启动后 `kind=powershell`、`state=ready` | [AUTO] |
| PS-2 | 初始提示符 | 日志出现 `PS> ` | [AUTO] |
| PS-3 | 命令执行 | 写入 `2+3` 后输出含 `5` | [AUTO] |
| PS-4 | 会话保持 | 第一条设置 `$x=41`，第二条读 `$x+1` 得 `42` | [AUTO] |
| PS-5 | 本地回显 | 键入字符在提交前可见，Enter 后行尾换行 | [AUTO]/截图 |
| PS-6 | 编辑键 | Backspace 删除最后一个字符，回显 `\b \b` | [AUTO] |
| PS-7 | 多行提交 | 粘贴带 CRLF 的两行被逐行提交 | [AUTO] |
| PS-8 | 手动断开 | 断开后 `idle`，10 秒不自动重开 | [AUTO] |
| PS-9 | 自动恢复 | 子进程被结束后 watchdog 重开，状态回 ready | [AUTO] |
| PS-10 | 错误路径 | shell 不存在时离线并含 `CreateProcess failed` | [AUTO] |

## C. Telnet 通道

| 编号 | 项目 | 期望 | 方式 |
|---|---|---|---|
| TN-1 | TCP 连接 | 本地测试服务监听后 `state=ready`，peer 正确 | [AUTO] |
| TN-2 | 收发 | 发送 `PING`，回显含 `PING` | [AUTO] |
| TN-3 | IAC 协商 | 服务端发 WILL ECHO/SGA 后客户端回 DO，IAC 不出现在终端文本 | [AUTO] |
| TN-4 | NAWS | 连接后服务端收到 `IAC SB 31` 窗口尺寸 | [AUTO] |
| TN-5 | 子协商 | `IAC SB ... IAC SE` 内容不泄漏到终端 | [AUTO] |
| TN-6 | 断线重连 | 服务端关闭后 offline，服务恢复后自动 ready | [AUTO] |
| TN-7 | 手动断开 | 断开后 idle，不自动重连；手动重连恢复 | [AUTO] |
| TN-8 | 端口失败 | 未监听端口时 offline，`last_error` 含连接失败 | [AUTO] |

## D. UI / 通道树

| 编号 | 项目 | 期望 | 方式 |
|---|---|---|---|
| UI-1 | 分组计数 | Telnet/PowerShell 分组计数与配置一致 | [AUTO] + 截图 |
| UI-2 | 新建入口 | 菜单/右键有新建 Telnet、PowerShell | [AUTO] |
| UI-3 | 编辑器字段 | 两种类型的字段组正确显示/隐藏 | [AUTO] |
| UI-4 | 保存即生效 | 对话框保存后树节点出现且自动连接 | [AUTO] |
| UI-5 | 编辑/改名 | 编辑后参数落盘、树节点更名、终端切到新名 | [AUTO] |
| UI-6 | 右键菜单 | 打开/重连、断开、编辑、删除可用；SFTP 不显示 | [AUTO] + 截图 |
| UI-7 | 删除 | 右键/对话框删除后节点消失、画框绑定清空 | [AUTO] |
| UI-8 | 终端输入 | 在 PowerShell/Telnet 终端实际键入并回车 | 截图 + 日志 |

## E. Web / Agent

| 编号 | 项目 | 期望 | 方式 |
|---|---|---|---|
| WEB-1 | 通道 API GET | 返回 `powerShell`、`telnet` 数组 | [AUTO] |
| WEB-2 | 通道 API POST | 可新建/编辑两种通道，保存后重载 | [AUTO] |
| WEB-3 | 通道 API 删除 | 两种 kind 均可删除 | [AUTO] |
| WEB-4 | 网页通道页 | 新增按钮、字段组、卡片正常渲染 | [AUTO] + 截图 |
| WEB-5 | 网页终端 | 选择新通道可输入并看到回显 | [AUTO] |
| AG-1 | Agent state/scan | 新通道 `kind`、`state`、`features` 正确 | [AUTO] |
| AG-2 | Agent write/log | `/write` 写入后 `/log` 能读到输出 | [AUTO] |
| AG-3 | 串口特性 | `serial_control` 只对串口 true | [AUTO] |

## F. 回归

| 编号 | 项目 | 期望 | 方式 |
|---|---|---|---|
| RG-1 | 编译 | 0 error / 0 warning | make |
| RG-2 | 代码整理 | quality_gate ALL PASS | 脚本 |
| RG-3 | 串口/SSH 通道 | 原有 4 通道仍 ready，状态栏在线数正确 | step14 + 截图 |
| RG-4 | 串口自愈 | step14 19/19 | step14_check |
| RG-5 | 基础功能 | step1 17/17、step3 14/14 | 脚本 |
| RG-6 | UI 桥 | ui_bridge 41/41 | 脚本 |
| RG-7 | Web 全量 | 12 页 162/162 | web_functional.js |
| RG-8 | 桌面全量 | main 35/35、dialogs 32/32、exit 2/2 | ui_full_audit2 |
| RG-9 | 退出残留 | 退出后无 ssh_bridge3/powershell 子进程，端口释放 | netstat |
