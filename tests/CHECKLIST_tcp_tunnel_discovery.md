# S12 TCP 转发 / 隧道 / UDP 发现 · 验收清单

- 环境：`output\s12test` 隔离实例
- 配置：Web 18083（token `s12pw`）、Agent 18088、TCP 22000+、UDP 18071、隧道公网 18123
- 脚本：`python output\s12test\s12_accept.py`
- 页面：`node output\s12test\s12_pages.js`

## 前置

- [x] 构建 `output\audit\build-s12-6.log` EXIT=0、0 warning
- [x] 启动后 `/state.services` 含 `tcp_forward` / `tunnel` / `discovery`
- [x] 用户原配置 `C:\Users\admin\.ssh-bridge\ssh_bridge_config.json` 未被修改
- [x] TCP 监听 256 个端口（22000–22255）
- [x] 隧道进程运行，管理口 18087 `healthy=true`
- [x] UDP 18071 绑定成功

## T12

- [x] T12-1 转发端口可连，双向回显（22001 → `B19022|one`）
- [x] T12-2 规则改动即时生效（target 19022→19023→19022，保存后 3s 内生效）
- [x] T12-3 公网 HTTP 可访问（`http://sshbridge.online:18123/status` 200）
- [x] T12-4 隧道断线自恢复（kill 后新 pid，`restarts>=1`，公网恢复 200）
- [x] T12-5 发现包可接收（`EADK1 q` → `boards=[s12a]`，`s12b` 不出现）
- [x] T12-5b 关闭发现后无应答
- [x] T12-6 关闭后端口不监听，再启动恢复
- [x] T12-7 多条规则并存不冲突（22001/22002 同时可连）
- [x] `RESULT: ALL PASS`

## Web / 移动端

- [x] `/api/tcp-forward` 返回 `ok/running/status`
- [x] `/api/tunnel` 返回进程、端口、restarts、error
- [x] `/api/discovery` 返回端口、responses
- [x] `tcp-forward` 页面显示真实状态，不再显示“S12 未实现”
- [x] `tunnel` 页面显示真实状态
- [x] 390×844 三页 `overflow=false`
- [x] 页面无 JS 错误
- [x] 截图 `output\s12test\shots\mobile-{dashboard,tcp-forward,tunnel}.png`

## 稳定性

- [x] 连续启停 TCP 转发不崩溃
- [x] 关闭/开启 UDP 发现不崩溃
- [x] kill 隧道进程并自动重启不崩溃
- [x] `--selftest all loop:selftest` ALL PASS
- [x] `--selftest term` ALL PASS
- [x] Windows 事件日志无新增 `ssh_bridge3` APPCRASH

## 退出清理

- [ ] 停止 `ssh_bridge3`
- [ ] 停止 `eadk-tunnel`
- [ ] 停止 echo 测试服务
- [ ] 保留 `output\s12test` 证据，用户配置不动
