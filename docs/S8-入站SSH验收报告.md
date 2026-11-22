# S8 入站 SSH 共享网关 · 验收报告

日期：2026-11-22  
设计位置：`docs/详细设计.md` §9「S8 SSH 入站共享网关 :2222」  
实现状态：已完成并验收，r13 发布包已包含  
自动化矩阵：`tests\s8_matrix_check.ps1` + `tests\s8_matrix.py`

## 1. 实现内容

- 新增 `src/services/inbound/inbound_gateway.{h,cpp}`：libssh server 单监听口，默认 `0.0.0.0:2222`。
- 用户名 = 已存在通道名；密码按三级规则解析：
  - 板卡：显式 `password`，未显式配置时继承 `default.password`；
  - SSH 出站通道：使用该通道的 `password`；
  - 空密码 = 允许 `none` 认证。
- 入站会话**不新建 target SSH**：客户端字节写入该通道 cmdq，输出用会话自己的 `epoch/seen` 游标读取同一份 ring。
- 首次启动生成并持久化 hostkey：`work/keys/inbound_host_rsa`；后续复用。
- `inbound_ssh.enable=true` 时随应用启动；文件菜单“重新加载配置”会同步启停；关闭后端口不再监听。
- `/state.services.inbound` 暴露 `on / port / sessions / note`。
- 认证失败、接入、断开写入 `work/keys/inbound.log`。

## 2. 验收结果

环境：`output\inboundtest` 隔离目录，未改动用户配置；真实 `ssh_bridge3.exe`。

| 项目 | 结果 |
|---|---|
| 构建 | `EXIT=0`，0 warning / 0 error |
| `--selftest all` | `RESULT: ALL PASS` |
| `--selftest term` | `RESULT: ALL PASS` |
| 无密码板卡 `loop1` none 认证 | PASS，输入 `INBOUND_LOOP_ECHO` 回显 |
| 继承 default 密码的 `wireless` | `defpw` 认证 PASS，none 被拒 |
| 未知用户 / 错误密码 | 均被拒，监听不退出 |
| 同通道双会话 | 两个客户端都看到 `INBOUND_TWO_A/B` |
| 入站输入 → 本地终端 | `loop1` `tx_bytes=46 / rx_bytes=46`，本地窗格 tail 出现入站标记 |
| 本地输入 → 入站客户端 | `term.input` 发送 `LOCAL_TO_INBOUND`，客户端收到回显 |
| 会话计数 | 会话中 `sessions=1`，关闭后 `sessions=0` |
| 关闭开关（有活动会话） | 客户端收到 EOF，应用保持响应，端口 2222 释放 |
| 重新开启 | 端口恢复监听，`on=true` |
| 启动时开关关闭 | 不 bind，`on=false`，端口 2222 无监听 |
| 停止/重载稳定性 | 首次实现发现停止时 ntdll 堆损坏 `0xc0000374`，修正 hostkey 所有权后连续启停无崩溃 |

### 2.1 自动化完整矩阵（2026-11-22，r13）

环境：`output\s8matrix`，监听 2223，Web 18091；配置同时提供三类用户：

- `nonech`：板卡显式 `password=""`，免密；
- `inherit`：省略 `password` 字段，继承 `default.password=defpw`；
- `sshin`：SSH 出站通道，自身密码 `sshpw`，与 `default.password` 不同。

| # | 项目 | 结果 |
|---|---|---|
| S8M-1 | 空密码通道 none 认证 | PASS |
| S8M-2 | 默认密码通道拒绝 none | PASS |
| S8M-3 | 默认密码继承 `defpw` | PASS |
| S8M-4 | 默认密码拒绝错误密码 | PASS |
| S8M-5 | SSH 出站通道使用自身密码 | PASS |
| S8M-6 | SSH 出站通道拒绝默认密码 | PASS |
| S8M-7 | 未知用户名被拒 | PASS |
| S8M-8 | 同通道双会话共享客户端 A 输入 | PASS |
| S8M-9 | 同通道双会话共享客户端 B 输入 | PASS |
| S8M-10 | 活动会话计数为 2 | PASS |
| S8M-11 | 保存配置关闭入站后 2223 释放 | PASS |
| S8M-12 | 关闭后 `on=false / sessions=0` | PASS |
| S8M-13 | 活动会话收到断开 | PASS |
| S8M-14 | 重新开启后端口恢复 | PASS |
| S8M-15 | 重新开启后仍可回显 | PASS |
| S8M-16 | 全部关闭后会话清零 | PASS |

结果：`RESULT: ALL PASS`；退出后进程 0、端口无监听（`S8M-CLEAN PASS`）。

复现：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tests\s8_matrix_check.ps1 -SkipBuild
```

## 3. 已知边界

- 只开 IPv4 `0.0.0.0`，未做 IPv6 双栈。
- 仅接受 `shell` 会话；`ssh host "命令"` 的 `exec` 请求会明确失败，不映射为通道命令。
- 没有入站空闲超时和速率限制；慢客户端由自身会话写失败/关闭处理，不阻塞其它会话。
- hostkey 为持久化 RSA 2048；密码仍按现有配置明文保存，未额外加密。
- config 开关是主要入口，暂无独立服务面板开关。
- 当前 r13 发布包已包含本功能与 S12/S13；`ssh -p 2222 <通道名>@<主机>` 可用。
- 自动化矩阵覆盖本文件第 2 节的主要认证、多会话和重载路径；S11 继电器硬件项仍按硬件环境单独验收。

## 4. 设计进度

按 `docs/详细设计.md` 的线性顺序：

- S0–S5：已完成（S5 实际为 AI 可控桥接）。
- S6（§7 界面偏好/会话操作/快捷键）：仅有窗口尺寸/字号/菜单快捷键，完整偏好项未落地。
- S7（§8 文件与脚本）：SFTP、脚本、快捷按钮已落地；XMODEM 未落地。
- **S8（§9 入站 SSH）：本次完成。**
- S9（§10 Web 窗口）：未开始。
- S10（§11 Agent 面板）：已提前落地 AI 桥接。
- S11（§12 继电器/文件服务）：已落地。
- S12（§13 TCP 转发/隧道/UDP 发现）：未开始。
- S13（§14 自更新/打包/全量回归）：已提前落地 r11。

因此本步完成后，设计文档的下一线性项是 **S9 §10 Web 窗口**；但 S6 的完整偏好项和 S12 的转发/隧道/发现仍是空缺，按实际产品优先级可先做其中一项。
