# S12 TCP 转发 / 隧道 / UDP 发现 · 验收报告

- 日期：2026-11-22
- 设计来源：`docs/详细设计.md` §13（第 550–568 行）
- 验收环境：Windows 本机，隔离目录 `C:\code\ssh-bridge-windows3\output\s12test`
- 最终构建：`output\build\ssh_bridge3.exe`，13,159,585 B
- SHA-256：`F475CBA3993CFF8FC5F2D5BD8B3B8CF808CE708AB813D9D74F5C5C877BD270CA`

## 1. 交付内容

| 模块 | 文件 | 行为 |
| --- | --- | --- |
| TCP 转发 | `src/services/tcpfwd/tcp_forward.{h,cpp}` | 一次绑定 `port_base..port_base+255`，`port_base + 板 IP 末段` → `板 IP:target_port`；`WSAPoll(500ms)` 统一 accept；每连接独立双向 relay；停止时关闭监听与活动连接 |
| 外网隧道 | `src/services/tunnel/tunnel_manager.{h,cpp}` | `eadk-tunnel.exe -c runtime.json`；HTTP 与入站 SSH 两个独立进程；HTTP 公网 18000–18999、SSH 公网 28000–28999；管理口 18087 健康探测；进程退出自动重启 |
| UDP 发现 | `src/services/discovery/discovery_service.{h,cpp}` | UDP 18071；仅 `EADK1` + `q/discover` 应答；仅返回 `enable && discoverable` 的通道；默认关闭 |
| 配置模型 | `src/core/config/config.{h,cpp}` | `tcp_forward`、`tunnel`、`discovery` 读写；`BoardConfig/SSHLinkConfig.discoverable` |
| 生命周期 | `src/services/service_manager.{h,cpp}`、`src/ui/shell/shell.{h,cpp}` | 启动、配置重载同步启停；`/state.services` 暴露 `tcp_forward/tunnel/discovery` |
| Web | `src/services/web/web_console.cpp` | `/api/tcp-forward`、`/api/tunnel`、`/api/discovery` 真实状态；`/service` 支持三个服务；页面移除占位并适配窄屏 |

## 2. 配置示例

```json
"tcp_forward": {
  "enable": true,
  "listen_ip": "127.0.0.1",
  "port_base": 22000,
  "target_port": 19022,
  "board_cidr": "127.0.0.0/24"
},
"tunnel": {
  "enable": true,
  "map_http": true,
  "map_inbound_ssh": false,
  "exe_path": "C:\\code\\ssh-bridge\\output\\build\\eadk-tunnel.exe",
  "server_addr": "sshbridge.online",
  "server_port": 7321,
  "local_port": 18083,
  "remote_port": 18123,
  "ssh_local_port": 2222,
  "ssh_remote_port": 0
},
"discovery": {"enable": true, "port": 18071}
```

约束：`board_cidr` 必须是 `a.b.c.0/24`；`port_base` 必须满足 `+255 <= 65535`；隧道 HTTP 公网端口必须 18xxx，SSH 公网端口必须 28xxx；`map_http` 与 `map_inbound_ssh` 至少一个为 true。

## 3. T12 验收

测试脚本：`output\s12test\s12_accept.py`；输出：`output\s12test\s12-accept.txt`。

| 项 | 断言 | 结果 | 证据 |
| --- | --- | --- | --- |
| T12-1 | 转发端口可连，双向回显 | PASS | 22001 发送 `one` 收到 `B19022|one` |
| T12-2 | 规则改动即时生效 | PASS | POST `/api/config/save` 将 target 19022→19023，3s 内 22001 回显 `B19023`；再恢复 19022 成功 |
| T12-3 | 隧道 HTTP 可访问 | PASS | `http://sshbridge.online:18123/status` 返回 200、1725 B，内容为本应用状态 |
| T12-4 | 隧道断线自恢复 | PASS | kill 进程 7244 → 新 pid 32276，`restarts=1`，公网 HTTP 恢复 200 |
| T12-5 | 发现包可接收 | PASS | `EADK1 q` 返回 `boards=[s12a]`；`s12b`（discoverable=false）不出现；关闭发现后无应答 |
| T12-6 | 关闭后端口不监听 | PASS | `/service` 关闭 tcp-forward 后 22001 拒绝连接；再启动恢复 |
| T12-7 | 多条规则并存不冲突 | PASS | 22001 与 22002 同时可连；分别映射 `127.0.0.1` / `127.0.0.2` 的同一目标端口 |

结果为 `RESULT: ALL PASS`。

## 4. Web 与移动端

- `GET /api/tcp-forward`、`/api/tunnel`、`/api/discovery` 返回 `ok/running/status`；`/services` 与 Agent `/state.services` 同步。
- `tcp-forward`、`tunnel` 页面已不是占位：显示真实 JSON，格式化缩进、`white-space:pre-wrap`、`overflow-wrap:anywhere`。
- Playwright 390×844：dashboard / tcp-forward / tunnel 三页 `overflow=false`、无 JS 错误；截图在 `output\s12test\shots\`。
- 脚本：`output\s12test\s12_pages.js`；输出 `output\s12test\s12-pages.txt`。

## 5. 构建与自测

- `output\audit\build-s12-6.log`：EXIT=0，无 warning/error。
- `--selftest all loop:selftest`：EXIT=0，`RESULT: ALL PASS`。
- `--selftest term`：EXIT=0，`RESULT: ALL PASS`。

## 6. 验收中发现并修复的问题

1. **S12 首轮 TCP 连接触发进程崩溃 `0x40000015`**
   - 根因：三个服务的后台线程持有 `std::shared_ptr<Impl>`，而管理器同时保存裸指针并在 stop 时 `delete`；线程结束释放 shared_ptr 后再 delete，造成双重释放/未定义行为。
   - 修复：TCP/Tunnel/Discovery 全部改为 `std::shared_ptr` 成员，stop 只 `reset()`；TCP 的活动 relay 线程因此在 stop 后仍能安全持有旧状态直至退出。
   - 复验：连续启停 TCP、关闭/重启发现、kill 隧道进程均无崩溃；Windows 事件日志不再出现新 APPCRASH。

2. **停止 UDP 发现后 Python 收到 10054**
   - Windows 对已关闭 UDP 端口的 ICMP 会以 `WSAECONNRESET` 返回；这属于“无应答”，不是服务异常。验收脚本将 timeout 与 `ConnectionResetError` 都判定为静默。

3. **移动端 S12 页面横向溢出**
   - 原始 JSON 单行过长；页面改为格式化 JSON + `overflow-wrap:anywhere`，三页 `overflow=false`。

## 7. 边界

- 未安装 `eadk-tunnel.exe` / `frpc.exe` 时，隧道启动返回“未找到隧道程序”，不内置替代。
- `remote_port=0` 时真实公网端口由外部程序决定；本轮验收固定 18123，状态回显配置值，未解析外部程序的动态选端口结果。
- 发现只实现服务端应答，不主动广播/组播扫描。
- S9 的单队列 job 与 Web 代码拆分仍未补；工作区未提交，r11 发布包不包含 S8/S9/S12。
- 隔离测试结束后应停止 `ssh_bridge3` / `eadk-tunnel` / echo 服务，避免占用 18071/18083/22000+/18123。
