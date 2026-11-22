# CHECKLIST — S8 入站 SSH 共享网关

日期：2026-11-22  
被测程序：`output\build\ssh_bridge3.exe`  
隔离配置：`output\inboundtest\ssh_bridge_config.json`（临时，验收后删除）  
监听：`0.0.0.0:2222`

## 验收项

| # | 项目 | 操作 | 期望 | 结果 |
|---|---|---|---|---|
| 1 | 启动监听 | `inbound_ssh.enable=true` 启动应用 | 2222 LISTEN，`/state.services.inbound.on=true` | PASS |
| 2 | 免密登录 | `auth_none("loop1")` | 认证成功 | PASS |
| 3 | 通道回显 | 入站发送 `INBOUND_LOOP_ECHO` | 客户端收到同一标记，本地 `tx=rx` | PASS |
| 4 | 继承密码 | `wireless` 未显式密码，`default.password=defpw` | `defpw` 成功，none 被拒 | PASS |
| 5 | 错误密码 | `auth_password("wireless","wrong")` | 拒绝，监听继续 | PASS |
| 6 | 未知用户 | `auth_none("no-such-channel")` | 拒绝，监听继续 | PASS |
| 7 | 双会话同流 | 两个 `loop1` 会话分别发送 A/B | 两边都看到 A/B | PASS |
| 8 | 入站 → 本地 | 入站发送后读 `/state` | `loop1` tx/rx 增加，本地窗格 tail 出现标记 | PASS |
| 9 | 本地 → 入站 | UI `term.input` 发送 `LOCAL_TO_INBOUND` | 入站客户端收到回显 | PASS |
| 10 | 会话计数 | 打开一个 shell 后读 state | `sessions=1`；关闭后 `sessions=0` | PASS |
| 11 | 关闭开关 | 有活动会话时改 `enable=false` 并重载 | 客户端 EOF，应用响应，2222 释放 | PASS |
| 12 | 重新开启 | 改回 `enable=true` 并重载 | 2222 恢复 LISTEN | PASS |
| 13 | 启动即关闭 | `enable=false` 启动应用 | 2222 不监听，`on=false` | PASS |
| 14 | 停止稳定性 | 连续关闭/开启/重载 | 无 `0xc0000374`、无崩溃 | PASS |
| 15 | 构建 | `mingw32-make -j4` | EXIT=0，0 warning | PASS |
| 16 | 自测 | `--selftest all` / `--selftest term` | 均 `ALL PASS` | PASS |

## 复现命令

```powershell
# 构建
C:\msys64\mingw64\bin\mingw32-make.exe -j4

# 数据面自测
.\output\build\ssh_bridge3.exe --selftest all
.\output\build\ssh_bridge3.exe --selftest term

# 入站实连（无密码通道；交互 shell）
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -p 2222 loop1@127.0.0.1

# 继承密码通道
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -p 2222 wireless@127.0.0.1
```

## 备注

- 入站会话只接受 `shell`，带远端命令的 `exec` 会失败。
- 空密码通道允许 `none` 认证，局域网内可被同网段访问；生产使用应显式设置密码。
- 本轮未重新打 r11 包，发布资产仍是旧构建。
