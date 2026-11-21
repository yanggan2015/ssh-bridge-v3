# 步骤 2 验收清单（S2 · SSH 出站链路）

> 范围：`src/core/ssh/`（libssh 会话封装）、`src/core/bridge/ssh/`（SSH 通道实现）、`src/app/selftest_ssh.cpp`
> 出口：本清单全 PASS + `tests\step2_check.ps1` 退出码 0 + `quality_gate` PASS
> 不在本步：入站监听（S8）、SFTP（S7）、终端渲染（S4）、界面（S3）

## 1. 验收项（T2）

| 编号 | 项 | 判定 | 自动化 |
|---|---|---|---|
| T2-1 | 连接成功且拿到 shell | `SshBridge::wait_connected` 为真，`status_json.state == ready` | `--selftest ssh <host> <port> <user> <pass>` |
| T2-2 | 发命令回显顺序正确 | 先后写 A、B 两行，ring 里 A 的下标小于 B，且都出现（回显来自对端字节） | 同上 |
| T2-3 | 拔网 5s 内标记断开 | 杀掉测试服务器后 `wait_disconnected` 为真，`drop_ms <= 5000` | `--selftest ssh-drop ...` |
| T2-4 | 自动重连成功 | 断线后 30s 内重新 `ready`，并且能再次收到回显 | `--selftest ssh-reconnect ...` |
| T2-5 | 两条链路互不影响 | 两条各自连上并回显；停掉其中一条后另一条仍能回显 | `--selftest ssh-two <host> <p1> <p2> <user> <pass>` |
| T2-6 | 密码错误给出可读错误 | 不进 `ready`，`last_error` 含「认证/auth/denied/拒绝」之一 | `--selftest ssh-auth <host> <port> <user> <badpass>` |

## 2. 通用必测（G）

| 编号 | 项 | 判定 |
|---|---|---|
| G-1 | 编译无错无警告 | exit=0，error=0，warning=0 |
| G-5 | 无残留进程 | 跑到这里 `ssh_bridge3` 实例数 0 |
| G-5b | 测试服务器已清理 | 没有残留 `ssh_test_server.py` 的 python 进程 |
| G-6 | 代码整理 | `scripts\quality_gate.ps1` 退出码 0 |
| G-7 | git 状态 | 有改动时提示「提交后复核」；提交后应为干净 |

## 3. 测试环境与判定备注

1. 测试服务器：`tests\tools\ssh_test_server.py`（paramiko）。`--port 0` 自动选口并把实际端口写进 `--ready-file`，脚本读它再传参。
2. `echo` 模式：接受任意密码、开 pty + shell、先发 `SB3-TEST-READY`，之后把收到的字节原样回显（模拟设备侧回显）。
3. `reject` 模式：握手成功但密码一律拒绝，用于 T2-6。
4. T2-3 的「拔网」= 直接杀服务器进程，客户端会收到 TCP 断开或保活失败；`drop_ms` 从「确认已连上」开始计时，因此涵盖真实的检测延迟。
5. T2-4 的恢复 = 同端口重启服务器。客户端退避为 1s→2s→4s，30s 上限足够覆盖两次失败。
6. 真机联调时把同一条命令换成板子地址即可：
   `.\output\build\ssh_bridge3.exe --selftest ssh 192.168.1.10 22 root <密码>`。

## 4. 本步明确不做

- 不监听任何端口（T2 全流程 `netstat` 不应出现本进程 LISTENING）。
- 不做本地回显：`write` 只进 `CmdQueue`，回显只来自对端 RX 字节。
- 不做 SFTP / 端口转发 / 入站跳板。
