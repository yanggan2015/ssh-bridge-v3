# EADK SSH Bridge — Agent 设备控制 Skill（18081）

本 Skill 只走 **18081 设备控制面**。18096 是桌面窗口调试桥，不是板控路径；Agent 不得调用 18096，不得使用 `/ui-tree`、`/control`、`X-Ui-Token`。

## 0. 铁律

1. 先发现，再行动：`GET /health` → `GET /api/help`，后续**只使用 `/api/help.operations` 公布的路径**。
2. 18081 是唯一板控主路径；18096 关闭时，18081 的扫描、日志、命令、动作、锁、作业、串口、工件面必须照常工作。
3. 写操作默认需要 `Authorization: Bearer <web.token>`；通道写还要过租约锁。
4. 长任务先占板（`/lock`），结束释放（`/unlock`）；他人持锁返回 `423 agent_lock`。
5. 动作/命令返回 `ok:true` 或 `202` 只代表受理；必须读 `/jobs/{id}`、`/log` 或 `/agent/state` 验证业务结果。

## 1. 发现

```bash
curl -s http://HOST:18081/health
curl -s http://HOST:18081/api/help
curl -s http://HOST:18081/help
curl -s http://HOST:18081/help/CHANNEL
```

- `/health`：`ok/service/version/build/web_port/protocol/ui_bridge`。
- `/api/help`：`surfaces`、`pages`、`channels`、`operations`；每项含 `id/method/path/auth/params/returns/implemented`。
- `/help` 与 `/help/<channel>`：人类可读闭环、危险项与可复制 curl；`?format=text` 强制纯文本。
- `GET /agent/scan`：统一通道对象、终端态、按钮、features、锁列表。
- `GET /agent/probe?channel=..`：只读终端探测，不发 Enter、不写串口。

## 2. 通道语义

- `board` = 串口板通道（serial），`link` = SSH Link，`channel` = 任意通道。
- `GET /api/agent/resolve?board=..|link=..|channel=..` 三选一；冲突 400，未知 404。
- 已配置未运行：`200 + running:false`，不是 404。
- `/boards` 是通道生命周期入口；`/serial/*` 是串口开合/Break/DTR/RTS/参数。

## 3. 标准闭环（串口 / U-Boot）

```bash
BASE=http://HOST:18081
TOKEN='<web.token>'
CH='ok3568-com5'
SID='agent-<uuid>'

# 1) 看上下文与终端态
curl -s "$BASE/agent/scan"
curl -s "$BASE/agent/probe?channel=$CH"

# 2) 占板（长任务建议 1h）
curl -s -X POST "$BASE/lock" -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"channel\":\"$CH\",\"session_id\":\"$SID\",\"ttl_ms\":3600000}"

# 3) 发命令：U-Boot 用 /command；Linux shell 可用 /command_block 或 /agent/exec
curl -s -X POST "$BASE/command" -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"channel\":\"$CH\",\"command\":\"help\"}"

# 4) 异步作业：列表 / 详情 / 取消
curl -s "$BASE/jobs?channel=$CH&limit=20"
curl -s "$BASE/jobs/JOB_ID"
curl -s -X POST "$BASE/jobs/JOB_ID/cancel" -H "Authorization: Bearer $TOKEN"

# 5) 读增量日志（since + epoch 必须成对）
curl -s "$BASE/log?channel=$CH&chars=8192"
curl -s "$BASE/log?channel=$CH&since=123&epoch=1"

# 6) 释放
curl -s -X POST "$BASE/unlock" -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"channel\":\"$CH\",\"session_id\":\"$SID\"}"
```

- `/write`：原始字节写入，202；`data` 自己负责 `\r\n`。
- `/send_keys`：`<ctrl+c>`、`<enter>` 等按键序列，202；可选 `duration_ms`（按住时长）与 `repeat`（1..6000），服务端按时长/间隔连发，不要用大量 HTTP 轮询。
- `/action`：板控按钮 `{channel,id}`，202 + `job_id`；不要用 18096 点击代替。
- `/command_block`：同步等命令结束，200；有超时上限，长任务用 `/command`。
- `/agent/exec`：仅 shell 态；U-Boot/登录态返回 `409 state_mismatch`。

## 4. 锁与人工接管

- `POST /lock`：`{channel,session_id,ttl_ms}`；默认 1000ms，范围 100–86400000；同 session 续租。
- `POST /unlock`：只释放自己的锁；错误 session 不释放。
- `POST /lock/takeover`：人工显式接管，`{channel,reason,session_id}`；记录 `last_takeover`。
- 锁被他人持有时，通道写路由 `423 agent_lock`；读接口（scan/probe/state/log）不受影响。
- 网页终端显示 `Agent 操作中` 与“人工接管”按钮；接管后人类写入才可继续。

## 5. 作业治理

- `GET /jobs`：`state/channel/limit/before_id`；默认 20、上限 100；摘要不含 output/error。
- `GET /jobs/{id}` 或 `GET /job?id=`：详情含 output、progress、audit。
- `POST /jobs/{id}/cancel`：queued 立即 cancelled；running 协作取消；终态 409。
- 同通道已有 `queued/running` 作业时，新写请求返回 `429 channel_busy`，带 `job_id/state`。
- 终态作业保留约 10 分钟，总量最多 100 条。

## 6. 日志

- `GET /log?channel=..`：尾部，`next_since=total`。
- `GET /log?channel=..&since=..&epoch=..`：增量；响应含 `dropped/dropped_bytes`、`reset/reset_reason`、`truncated`。
- epoch 不匹配/游标超前返回 `reset:true` + 当前尾部；按响应游标继续，不跳字节。
- `POST /log/disk`：运行时开关落盘；单文件 256MB 上限；已满停止落盘但 ring 仍可读。
- `GET /log/disk`、`/log/download` 可用于落盘状态与下载。

## 7. TFTP 工件面（U-Boot / 传文件）

根目录 = 运行中的 TFTP root，否则 share root；写需要 token。路径禁止 `..`、绝对路径与盘符。

```bash
curl -s "$BASE/tftp/status"
curl -s "$BASE/tftp/files?path="
curl -s -X POST "$BASE/tftp/mkdir" -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" -d '{"path":"boards/ok3568"}'

# 分片上传：offset 断点续传；单片 ≤16MB
curl -s -X POST "$BASE/tftp/upload?path=boards%2Fok3568%2Fimage.bin&offset=0" \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/octet-stream" \
  --data-binary @chunk0.bin

# 完成后校验
curl -s "$BASE/tftp/md5?path=boards%2Fok3568%2Fimage.bin"
curl -s "$BASE/tftp/download?path=boards%2Fok3568%2Fimage.bin" -o image.bin
curl -s "$BASE/send_file/progress"
```

- `offset` 0 截断重建；正偏移续写；`offset > 现有 size` 409。
- 带 `md5` 时写后校验，错配 400 `md5_mismatch`。
- `POST|DELETE /tftp/delete`：拒绝根目录、非空目录、缺失路径。
- 给板分配与 TFTP `bind_ip` 同网段 IP 后再拉文件；TFTP 端口来自 `/tftp/status`。

## 8. 错误契约

```json
{
  "ok": false,
  "channel": "ok3568-com5",
  "error": { "code": "agent_lock", "message": "channel held by another session", "detail": { "owner": "agent-a" } },
  "request_id": "..."
}
```

常见：400 参数/路径、401 无 token、404 通道/文件、405 方法、409 状态冲突、413 超过 16MB、423 锁、429 channel_busy、500 执行失败、502 重连失败、504 超时。

## 9. 安全边界

- 不调用 18096；不使用 `/ui-tree`、`/control`、`X-Ui-Token`。
- Tunnel 启停、网络/服务全局配置不进入 Agent 自动闭环；只允许人工或管理接口。
- `/boards/delete`、`/tftp/delete`、`/log/clear`、配置保存属于不可逆/共享状态操作，必须显式确认。
- 不把 `/api/help` 之外的旧路径写死；接口变化以 `/api/help` 为准。

## 10. 板卡 CLI（eadk_board.py）

- 分发：`GET /share/tools/eadk_board.py`、`GET /share/tools/eadk_board.md`；18082 共享服务也可 `GET /tools/eadk_board.py`。
- 默认 Base `http://127.0.0.1:18081`；写接口用 `EADK_TOKEN` 或 `--token <web.token>`。
- 键统一 `channel`；旧 `board/link` 由 CLI 自动映射到 `/agent/state?channel=`。
- 典型闭环：`scan` → `-b CHANNEL list` → `health` → `clear` → `write/send/keys` → `log`；文件用 `put/ls/md5/rm`；Linux shell 用 `run`。
- CLI 只是 `/agent/*` 的封装，动作后仍要读 `/log`、`/agent/state` 或 `/jobs` 验证业务结果。
