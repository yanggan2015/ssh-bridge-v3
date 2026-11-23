# Agent 能力执行清单

- 日期：2026-11-23
- 目标：让 **18081** 成为 AI Agent 完整控制板子的唯一主路径，同时保留 **18096** 作为独立桌面 UI 调试桥。
- 设计依据：`docs/详细设计.md` §11、`docs/架构总览.md` §3.1、`docs/AGENT-CAPABILITY-PLAN.md`、`docs/功能清单.md` Q 组。
- 执行规则：一批一编译、一批一测试、一批一提交；前一批全绿才进下一批。
- 状态图例：`已完成` 文档/现有能力；`进行中` 本轮正在做；`计划` 尚未开工；`阻塞` 等待前置。

## 0. 文档定稿（本轮）

| ID | 事项 | 载体 | 判定 | 状态 |
|---|---|---|---|---|
| D0-1 | 固定 18081 板控主路径、18096 UI 调试边界 | `docs/架构总览.md` §3.1 | 两条硬规则 + 四组合验收表 | 已完成 |
| D0-2 | 重写 S10：双面接口、模块、路由、动作语义、锁、作业、文件、错误码、T10 | `docs/详细设计.md` §11 | 与本文、功能清单一致 | 已完成 |
| D0-3 | 功能清单拆分 K（18096 UI 调试）/ Q（18081 设备 API） | `docs/功能清单.md` | K7 + Q18，统计 139 条 | 已完成 |
| D0-4 | 能力模型与实施规划同步边界 | `docs/AGENT-CAPABILITY-PLAN.md` | 无“18096 兜底板控”表述 | 已完成 |
| D0-5 | 建立本执行清单 | 本文件 | 每项有验收与状态 | 已完成 |
| D0-6 | 测试闸门补 S10 双面要求 | `docs/每步测试闸门.md` | 隔离/关闭组合列入闸门 | 已完成 |

## 1. P0-A：协议底座与发现

| ID | 事项 | 实现要点 | 文件 | 验收 | 状态 |
|---|---|---|---|---|---|
| A1-1 | `/health` 元数据 | `ok/service/version/build/api_enabled`；不暴露密钥 | `agent_contract.cpp`、`web_console.cpp` | 200；字段齐全；18096 关闭不影响 | 已完成 |
| A1-2 | `/api/help` 契约 | `schema/protocol/surfaces/operations`；每项 `id/surface/method/path/params/returns/auth/implemented` | `agent_contract.cpp` | 只列已实现；POST 405；18096 `enabled:false` 可见 | 已完成 |
| A1-3 | 动态文本帮助 | `/help` 总览；`/help/<channel>` 实时状态、终端态、真实按钮 id、curl | `agent_contract.cpp` | 换通道/端口后内容变化；无密码/token/tunnel | 已完成 |
| A1-4 | 通道路由归一 | `board/link/channel` 三选一；冲突 400；不存在 404 | `agent_contract.cpp`、`web_console.cpp` | 15 组键组合用例 | 已完成 |
| A1-5 | 鉴权与请求约束 | 写 token；body 16MB；方法白名单；request_id；命令 4KB 随 C4 | `web_console.cpp`、`agent_contract.cpp` | 401/405/413；无 token 读边界符合配置 | 已完成（命令 4KB 待 C4） |
| A1-6 | Agent 状态聚合 | `/api/agent/status` 返回 `device` 与 `ui_bridge`，UI 仅状态不代理 | `agent_contract.cpp`、`web_console.cpp` | 两段字段；18096 关闭时 `ui_bridge.enabled:false` | 已完成 |

## 2. P0-B：上下文与探测

| ID | 事项 | 实现要点 | 文件 | 验收 | 状态 |
|---|---|---|---|---|---|
| A2-1 | `/status?view=agent` | 精简通道表、服务、锁摘要；不含 UI 树 | `agent_scan.cpp` | 5 通道、服务、锁字段正确 | 已完成 |
| A2-2 | `/agent/scan` | 连接态、终端态、缓冲区、按钮、features、错误、网段 | `agent_scan.cpp` | 全通道无遗漏；只读不发字节 | 已完成 |
| A2-3 | `/agent/probe` | 被动读日志尾部；五态状态机 + unknown；confidence | `agent_scan.cpp` | shell/login/password/uboot/disconnected 各 1 例 | 已完成 |
| A2-4 | `/agent/state` | 单通道连接、终端、游标、锁、最近动作、last_error | `agent_scan.cpp` | 与 `/status` 同源；无重复状态 | 已完成 |
| A2-5 | `/actions?view=agent` | id/label/backend/enabled/kind/requires_lock；与 UI 按钮同源 | `agent_scan.cpp` | 与 `/api/actions` 数量/id 一致；disabled 正确 | 已完成 |
| A2-6 | 只读保证 | probe/scan/state 不写 cmdq、不发 Enter、不打开串口 | `agent_scan.cpp` | 调用前后 TX 字节数不变 | 已完成 |

## 3. P0-C：日志增量与持久化

| ID | 事项 | 实现要点 | 文件 | 验收 | 状态 |
|---|---|---|---|---|---|
| A3-1 | `/log` 游标 | `since/epoch/chars`；`read_since_ex`；dropped | `web_console.cpp`、`logbuf` | 两次读不重复；游标递增 | 已完成 |
| A3-2 | 响应头 | `X-Log-Epoch/X-Log-Total/X-Log-Dropped` | `web_console.cpp` | 头与 JSON 一致 | 已完成 |
| A3-3 | reset 语义 | epoch 不匹配返回 `reset:true` + 尾部 | `web_console.cpp` | 清空/重建 ring 后旧游标被重置 | 已完成 |
| A3-4 | `/log/disk` | 开关落盘；256MB 上限；失败可读 | `web_console.cpp`、`logbuf` | 开关后文件增长；关闭后停止 | 已完成 |
| A3-5 | 清空不伤读者 | `/log/clear` 递增 epoch，读者收到 reset | `logbuf` | 清空后 Web 终端重取尾部；Agent 收到 reset | 已完成 |

## 4. P0-D：命令与动作

| ID | 事项 | 实现要点 | 文件 | 验收 | 状态 |
|---|---|---|---|---|---|
| A4-1 | `/command` 202 | 入 cmdq；返回 job_id/queued；不返回输出 | `web_console.cpp` | 202；job 可查；串口/loop 回显可见 | 已完成 |
| A4-2 | `/command_block` | JobRunner 等待；默认 2000ms，最大 60000ms；poll 不 sleep | `web_console.cpp` | 3s 命令返回 output；超时 `timed_out` | 已完成 |
| A4-3 | `/agent/exec` | 仅 shell；追加 `__C4_END_<jobid>__`；状态不匹配 409 | `web_console.cpp`、`agent_scan.cpp` | 标记输出完整；loop/uboot 调用 409 | 已完成 |
| A4-4 | `/write` 兼容 | `data` 与 `text` 二选一；原始字节；202 | `web_console.cpp` | 原样写入；不补行尾 | 已完成 |
| A4-5 | `/send_keys` | 字符串数组；`\r/\u0003/\u0004`；interval 10–1000ms | `web_console.cpp`、`key_chars.c` | Ctrl+C、多键序列生效 | 已完成 |
| A4-6 | `/action` | 复用 `/actions` id；backend 校验；未知/禁用错误 | `web_console.cpp` | 按钮真实生效；409/404/400 正确 | 已完成 |
| A4-7 | 动作审计 | request_id/session/channel/action/status/duration/bytes | `web_console.cpp` | `/job?id=` 可查；无原始密码/命令全文 | 已完成 |

**C4 完成记录（2026-11-23）**

- 实现位置：`src/services/web/web_console.cpp`。统一的 `Job`/`Task` 队列按 `Action/Command/CommandBlock/Exec/SendKeys` 分发；`/command_block` 与 `/agent/exec` 使用 pending HTTP + `MG_EV_WAKEUP` 回写，poll 线程不 sleep。
- `/api/help` operations 由 50 增至 54，新增 `command.post`、`command.block.post`、`agent.exec.post`、`send.keys.post`；`write.post`、`action.post`、`job.get` 返回字段同步更新。
- `agent_scan` 的 `features.command` 在完整/全局视图均改为 `true`；C4 时 `lock` 仍为 `false`，C5 已改为 `true`。
- 测试：`output/webtest/agent-command-smoke.js` 84/84，结果 `output/ux-audit/agent-command-result.json`；18096 关闭隔离 7/7，`output/ux-audit/agent-command-ui-disabled.json`。
- 回归：C1 82/82、C2 73/73、C3 59/59、api 58/58、api-v3 43/43、agent 63/63、web-v3 120/120。
- 提交：`feat(agent): add command jobs and action audit`。

## 5. P0-E：租约锁

| ID | 事项 | 实现要点 | 文件 | 验收 | 状态 |
|---|---|---|---|---|---|
| A5-1 | 锁表 | 按 channel；owner/session/expires/ttl | `agent_lock.cpp` | 不同通道互不影响 | 已完成 |
| A5-2 | `/lock` `/unlock` | 默认 1000ms；最大 86400000ms；续租；错 session 不释放 | `agent_lock.cpp` | 423/续租/过期/错误 owner 全通过 | 已完成 |
| A5-3 | 写路由统一拦截 | 先锁、再通道态、再入队 | 所有 Agent 写路由 | 锁定时 command/write/keys/action 全 423 | 已完成 |
| A5-4 | 人工接管 | `/lock/takeover` + 本地键盘信号；清锁并 audit | `agent_lock.cpp`、UI/WS | 人工接管后 Agent 写 423；不锁死用户 | 已完成 |
| A5-5 | 状态可见 | `/agent/state`、`/status?view=agent` 显示锁 | `agent_scan.cpp` | owner/expires 正确 | 已完成 |

**C5 完成记录（2026-11-23）**

- 新增 `src/services/agent/agent_lock.{h,cpp}`：进程内单例 `LeaseLocks`，按 channel 保存 owner/session/ttl/expires 与上次接管审计；TTL 惰性过期，同 session 续租，不同 session 423。
- `web_console.cpp` 新增 `POST /lock`、`POST /unlock`、`POST /lock/takeover`；所有写路由经 `allow_channel_write()`，顺序为鉴权→通道存在→锁→running→入队；读接口不受影响。
- `agent_scan.cpp`：`features.lock=true`；`state.lock`、`scan/status.locks`、`actions.requires_lock` 全部反映真实锁态；`agent_contract.cpp` operations 54→57。
- 本地人工接管：`view.cpp` 的 `send_active/send_bytes` 区分 human；WM_CHAR/WM_KEYDOWN/粘贴/鼠标上报调用 `human_input`，18096 `type_text/send_raw` 不清锁。
- 测试：`agent-lock-smoke.js` 51/51；锁核心单测 `agent-lock-unit.cpp` 19/19；结果 `output/ux-audit/agent-lock-result.json`、`output/ux-audit/agent-lock-unit.json`。
- 回归：C1 82/82、C2 73/73、C3 59/59、C4 84/84；api 58/58、api-v3 43/43、agent 63/63、web-v3 120/120。
- 提交：`feat(agent): add per-channel leases and human takeover`。

## 6. P0-F：作业治理

| ID | 事项 | 实现要点 | 文件 | 验收 | 状态 |
|---|---|---|---|---|---|
| A6-1 | Job 注册表 | id/kind/channel/state/created/started/ended/timeout/result/error | `web_console.cpp` | 5 种状态可查 | 已完成 |
| A6-2 | `/jobs` `/jobs/{id}` | 过滤/分页/摘要；`/job?id=` 兼容 | `web_console.cpp` | 列表与详情一致 | 已完成 |
| A6-3 | 取消与超时 | cancel 请求、协作取消点、timeout 状态 | `web_console.cpp` | 长任务可取消；超时可读 | 已完成 |
| A6-4 | 并发与保留 | 每通道 1 running，超限 429；保留 100 条/10 分钟 | `web_console.cpp` | 双任务 429；旧 job 清理 | 已完成 |
| A6-5 | 进度 | 文件/长任务 `progress` 字段 | `web_console.cpp`、artifacts | `sent/total/state` 正确 | 已完成 |

**C6 完成记录（2026-11-23）**

- Job 注册表在 `web_console.cpp` 内集中维护，状态 `queued/running/done/failed/timeout/cancelled`；每 job 增加 `cancel_requested` 与 `progress_sent/progress_total`，取消标志与进度用原子量，HTTP/列表/worker 并发读取安全。
- `GET /jobs`：`state/channel/limit/before_id` 过滤分页，默认 20、上限 100，摘要不含 `output/error` 全文；`GET /jobs/{id}` 详情含进度与 audit；`/job?id=` 保持兼容。列表前先做保留清理。
- `POST /jobs/{id}/cancel`：token；queued 立即置 cancelled，running 置协作取消标志；`/command_block`、`/agent/exec` 的增量读取与 `/send_keys` 按键循环均检查取消点；终态 409 `job_finished`，不存在 404 `job_not_found`。
- 并发：所有 Agent job（`/command`、`/command_block`、`/agent/exec`、`/send_keys`、`/action`）建 job 时检查同通道是否已有 `queued/running`；有则 429 `channel_busy`，带占用 `job_id/state`。不同通道互不影响。
- 保留：终态 job 超过 10 分钟清理，并裁剪到最近 100 条；运行中的 job 不丢。`features.jobs` 完整/全局均为 true；operations 57→60，新增 `jobs.list.get`、`jobs.get`、`jobs.cancel.post`。
- 超时沿用 C4：`/command_block`、`/agent/exec` 到 `timeout_ms` 置 `state=timeout`、`timed_out=true`，`/jobs` 可按 `state=timeout` 过滤。
- 测试：`output/webtest/agent-jobs-smoke.js` 39/39，结果 `output/ux-audit/agent-jobs-result.json`；覆盖列表/详情/兼容、并发 429、跨通道、运行中取消、进度、终态取消 409、保留 100 条。
- 回归：C5 51/51、C4 84/84、C2 73/73、C1 82/82、C3 59/59。
- 提交：`feat(agent): add job registry list cancel and retention`。

## 7. P1-G：通道生命周期

| ID | 事项 | 实现要点 | 文件 | 验收 | 状态 |
|---|---|---|---|---|---|
| A7-1 | `/boards` 兼容 | 复用 `/api/channels` GET/POST/delete；board/link 映射 | `web_console.cpp` | 与 `/api/channels` 等价 | 已完成 |
| A7-2 | `/reload` | 读配置、diff、手术式热应用；返回变更列表 | `web_console.cpp`、core | 其它通道 ring 不断 | 已完成 |
| A7-3 | `/reconnect` | 单通道重连；attempts/last_error | `web_console.cpp`、bridge | 断线后恢复；不影响其它通道 | 已完成 |
| A7-4 | `/serial/open` `/serial/close` | 开/关串口；锁与通道态校验 | `web_console.cpp`、serial | 真实 COM/loop 生效 | 已完成 |
| A7-5 | `/serial/break` | 发送 Break；长度/时长可配 | serial | 回环可验证 | 已完成 |
| A7-6 | `/serial/lines` | DTR/RTS 设置与回读 | serial | 状态一致 | 已完成 |
| A7-7 | `/serial/configure` | 波特率/数据位/校验/停止位/流控；热重开 | serial、config | 配置落盘；重开成功 | 已完成 |

**C7 完成记录（2026-11-23）**

- `/boards`：GET 公开，复用 `agent::scan_json` 的通道对象，输出版 `kind/name/running/state/detail/connection/peer`；POST/`/boards/delete` 需 token，调用 `AppCore::upsert_serial/upsert_ssh/delete_channel`，复用配置落盘、ring 保留与手术式热应用。支持 `{kind, channel, old_name}` 和直接 board 对象。
- `/reload`：token；直接调用 `AppCore::reload_config()`，返回 `added/removed/changed/kept + error`；未变通道走 `kept`，其 ring 不重建。
- `/reconnect`：token + 通道锁；调用 `AppCore::reconnect`，返回 `running/attempts/last_error`；缺失 404，失败 502。
- `/serial/open/close`：仅串口通道，非串口 400 `not_serial`；锁与 token 校验；open 幂等返回 `already_open`，失败 500 `serial_open_failed`；close 保留 ring。
- `/serial/break`：`duration_ms` 1–1000；真实口 `SETBREAK → sleep → CLRBREAK`，`loop:` 端点记录 `last_break_ms` 并返回 `loop:true`；端口未开 409。
- `/serial/lines`：DTR/RTS 至少给一个，真实口 `EscapeCommFunction`，loop 端点记录状态；回读 `dtr/rts/loop/port/last_break_ms`。
- `/serial/configure`：只改给出的 baud/databits/parity/stopbits/flowcontrol/dtr/rts；校验范围后 `AppCore::upsert_serial` 落盘并热重开，未暴露字段（按钮/密码/快捷指令）保留。
- `features.serial_control` 完整/全局均为 true；operations 60→70，新增 10 个生命周期/串口操作。
- 测试：`output/webtest/agent-lifecycle-smoke.js` 49/49，结果 `output/ux-audit/agent-lifecycle-result.json`；覆盖 boards 发现/创建/删除（运行中需 confirm）、reload、reconnect、open/close、break、DTR/RTS、configure 热生效、锁拦截、非串口与参数边界。
- 回归：C6 39/39、C5 51/51、C4 84/84、C2 73/73、C1 82/82、C3 59/59、api 58/58、api-v3 43/43、agent 63/63。
- 提交：`feat(agent): add board lifecycle and serial controls`。

## 8. P1-H：工件面

| ID | 事项 | 实现要点 | 文件 | 验收 | 状态 |
|---|---|---|---|---|---|
| A8-1 | TFTP 状态/列表 | `/tftp/status`、`/tftp/files`；安全路径 | `web_console.cpp` | 69/root/running 正确；`..` 400 | 已完成 |
| A8-2 | 分片上传 | `offset`、`Content-Range` 可选、16MB/片、临时文件 | `web_console.cpp` | 两片拼接后 MD5 正确 | 已完成 |
| A8-3 | 上传完成校验 | `md5` 参数与服务端 `md5` 比对 | `web_console.cpp` | 错 MD5 400；正确 200 | 已完成 |
| A8-4 | 下载/MD5 | `/tftp/download`、`/tftp/md5`；大文件流式 | `web_console.cpp` | 上传→下载→MD5 一致 | 已完成 |
| A8-5 | 目录操作 | `mkdir/delete`；非空目录拒绝 | `web_console.cpp` | 409/200 正确 | 已完成 |
| A8-6 | 传输进度 | `/send_file/progress`；最近任务 | `web_console.cpp` | sent/total/state/error | 已完成 |

**C8 完成记录（2026-11-23）**

- 新增 8 个路由：`GET /tftp/status`、`GET /tftp/files`、`GET /tftp/md5`、`GET /tftp/download`、`POST /tftp/upload`、`POST /tftp/mkdir`、`POST|DELETE /tftp/delete`、`GET /send_file/progress`。
- 工件根目录统一取 `ServiceManager::tftp_root()`：TFTP 运行中用实际 `tftp_svc_root`，否则用 share 根；端口同理。路径用 `tftp_safe_join` 校验，拒绝 `..`、绝对路径和盘符；查询参数另设 8KB 动态缓冲，避免长路径被 256 字节截断。
- 上传为原始字节分片，单片 16MB；`offset` 必须不超过已有长度，0 偏移截断重建，正偏移续写；父目录不存在 404；带 `md5` 时写后校验，错配 400 `md5_mismatch` 并返回实际 MD5。
- 下载按 64KB 流式发送，`Content-Length` 精确，文件名同时给 ASCII 与 RFC 5987；目录路径 404。`/tftp/md5` 返回 `{md5,size}`。
- `mkdir` 支持多级创建；`delete` 支持 POST 与 DELETE，拒绝根目录、非空目录和不存在路径；删除文件/空目录返回 `dir` 标记。
- 传输进度为进程内单槽快照（互斥锁保护），输出 `path/direction/state/error/sent/total/percent/updated_ms`，初态 `idle`，上传/下载完成后 `done`。
- `features.tftp_artifacts` 完整/全局均为 true；operations 70→78。
- 验证：`output/webtest/agent-artifacts-smoke.js` 51/51，结果 `output/ux-audit/agent-artifacts-result.json`；18096 关闭隔离复跑 51/51，证据 `output/ux-audit/agent-artifacts-ui-disabled.json`。
- 回归：C7 49/49、C6 39/39、C5 51/51、C4 84/84、C2 73/73、C1 82/82、C3 59/59、api 58/58、api-v3 43/43、agent 63/63、web-v3 120/120。
- 工件面全程只走 18081 与本地文件系统，不调用 18096，不使用 `/ui-tree`、`/control`、`X-Ui-Token`。
- 提交：`feat(agent): add tftp artifact api with chunk upload and progress`。

## 9. P1-I：帮助、Agent 页与 Skill

| ID | 事项 | 实现要点 | 文件 | 验收 | 状态 |
|---|---|---|---|---|---|
| A9-1 | Agent 页双面展示 | 上：18081 设备面扫描/探测/作业/锁；下：18096 只读自检 | `web_pages.cpp` | 两面独立；18096 关闭不报设备错 | 已完成 |
| A9-2 | 帮助页 | `/help` 展示闭环、危险项、通道 curl | `agent_contract.cpp`、`web_console.cpp` | 内容与当前配置一致 | 已完成 |
| A9-3 | Skill 对齐 | 更新 `skills/` 或文档，只使用 `/api/help` 公布的路径 | `skills/ssh-bridge/SKILL.md` | 无硬编码过期按钮 id | 已完成 |
| A9-4 | 锁提示 | Web 终端显示“Agent 操作中”；人工接管按钮 | `web_pages.cpp` | 不静默冲突；接管生效 | 已完成 |

**C9 完成记录（2026-11-23）**

- Agent 页重构为四段：上方 `18081 设备控制面`（通道/在线、作业、锁、TFTP、已公布操作 78、通道与终端态表、租约锁表），下方 `18096 桌面 UI 桥（只读自检）`，再接 18081 接口速查与 18096 调试接口。两套数据独立读取；18096 关闭时顶部设备面照常渲染。
- 设备面 `devLoad()` 并行读取 `/agent/scan`、`/jobs?limit=100`、`/tftp/status`、`/api/help`、`/health`；只在地址栏同源 18081 取数，不代理 18096。
- 帮助文本补齐：短命令闭环（scan/probe → lock → command/jobs → log → unlock）、危险项（Tunnel、delete、log clear、配置保存）、18096 边界、TFTP 工件、`/help/<channel>` 可复制 curl。`/help?format=text` 强制文本；浏览器 `Accept: text/html` 返回带导航的 `<pre>` 帮助页并做 HTML 转义。
- 终端 meta 增加 `termLock` 芯片与 `lockBanner`；每 5 秒随 `/agent/scan` 刷新当前通道租约。持锁时显示 owner/剩余时间，发送前拦截并提示“Agent 操作中”，提供“人工接管”按钮；按钮调用 `/lock/takeover`，成功后清锁并恢复写入。
- 新增 `skills/ssh-bridge/SKILL.md`：只走 18081，先 `/health`→`/api/help`，覆盖 discover/scan/probe/lock/command/jobs/log/action/TFTP/serial 与错误契约；明确禁止 18096、`/ui-tree`、`/control`、`X-Ui-Token`。
- 验证：`output/webtest/agent-ui-smoke.js` 33/33，结果 `output/ux-audit/agent-ui-result.json`；截图 `output/ux-audit/c9-agent-device-page.png`、`c9-terminal-lock.png`；无 pageerror/console error。
- 回归：C8 51/51、C7 49/49、C6 39/39、C5 51/51、C4 84/84、C2 73/73、C1 82/82、C3 59/59、api 58/58、api-v3 43/43、agent 63/63、web-v3 120/120。
- 提交：`feat(agent): show device surface help and terminal lock takeover`。

## 10. P0/P1 测试与隔离

| ID | 事项 | 覆盖 | 文件 | 验收 | 状态 |
|---|---|---|---|---|---|
| A10-1 | 契约测试 | health/help/scan/probe/state/log/command/exec/lock/jobs/boards/tftp | `output/webtest/agent-device-smoke.js` | 每个路由正常/错误路径 | 计划 |
| A10-2 | Shell 场景 | scan→probe shell→exec `uname -a`→state/log | 同上 | 输出完整、状态仍 shell | 计划 |
| A10-3 | U-Boot 场景 | probe uboot→command→log；exec 409 | 同上 | 不误用 exec | 计划 |
| A10-4 | 多 Agent 锁 | A lock→B 423→A unlock→B 202；TTL 过期 | 同上 | 锁语义全通过 | 计划 |
| A10-5 | 文件场景 | 分片上传→MD5→下载→本地 MD5 | 同上 + TFTP | 字节一致 | 计划 |
| A10-6 | 长任务 | 命令/传输期间 `/health` 延迟 | 同上 | P95 < 100ms；job 状态推进 | 计划 |
| A10-7 | 关闭 18096 | 只开 18081，跑 A10-1…6 | 测试脚本 | 全通过 | 计划 |
| A10-8 | 关闭 18081 | 只开 18096，跑 63/64 项 | `agent-smoke.js` | 全通过 | 计划 |
| A10-9 | 四组合 | 双开/单开 18081/单开 18096/双关 | PowerShell | 端口/功能互不影响 | 计划 |
| A10-10 | 代码隔离扫描 | 18081 路由无 18096/UI 调用 | `scripts` grep | 0 命中 | 计划 |
| A10-11 | 回归 | 58 + 43 + 120 + 63/64 | 现有测试 | 不下降 | 计划 |

## 11. 提交批次

| 批次 | 范围 | 必过测试 | 提交信息草案 |
|---|---|---|---|
| C1 | A1 协议/发现/鉴权 | A10-1 一阶段 | `feat(agent): add device discovery contract and dynamic help` |
| C2 | A2 上下文/探测 | A10-2/3 probe | `feat(agent): add scan, probe and agent state views` |
| C3 | A3 日志游标 | A3 全部 | `feat(agent): add incremental log cursors and disk toggle` |
| C4 | A4 动作 | A4 + shell/uboot | `feat(agent): add async command, bounded exec and key sending` |
| C5 | A5 锁 | A10-4 | `feat(agent): add per-channel leases and human takeover` |
| C6 | A6 作业 | A6 + 长任务 | `feat(agent): add job registry, cancel and timeout` |
| C7 | A7 生命周期 | A7 全部 | `feat(agent): add channel lifecycle and serial controls` |
| C8 | A8 文件面 | A10-5 | `feat(agent): add tftp file api with md5 and chunk upload` |
| C9 | A9 页面/帮助 | UI 回归 | `feat(agent): show device surface and lock state in web ui` |
| C10 | A10 隔离/全量 | 全部 | `test(agent): prove 18081 and 18096 isolation and full regression` |

## 12. 开工顺序

```text
D0 文档定稿
 → C1 发现/鉴权
 → C2 上下文/探测
 → C3 日志
 → C4 动作
 → C5 锁
 → C6 作业
 → C7 生命周期
 → C8 文件
 → C9 页面/帮助
 → C10 隔离/全量
```

C1 完成前不写 C2；C4 完成前不做 C5；A10-7（关闭 18096）必须在 C10 前全绿。任何“18081 做不了”的能力，必须补在 18081 或明确标记不做的安全边界，不得转向 18096。

## 13. C1 完成记录（2026-11-23）

- 新增 `src/services/agent/agent_contract.{h,cpp}`：发现契约、动态帮助、双面状态、通道路由归一、结构化错误。
- `web_console.cpp` 只负责路由接线；`/health`、`/api/help`、`/help`、`/help/<channel>`、`/api/agent/status` 全部改为新实现。
- 新增 `GET /api/agent/resolve?board=&link=&channel=`：三选一；冲突 400；未知 404；board 必须 serial、link 必须 ssh。
- `/api/help` 列出 45 个已实现操作；每项含 `id/surface/params/returns/implemented`；`POST` 405。
- `/api/agent/status` 保留旧顶层 UI 字段，新增 `device` 与 `ui_bridge` 两段；`POST` 405。
- 请求体上限 16MB，`MG_MAX_RECV_SIZE` 提升到 20MB 以便返回 413；方法错误与上限错误均带 `request_id`。
- 端口默认值统一为 18096：源码默认、README、分发包配置与说明；历史测试/审计证据不改。
- 通道归一设计收敛：已配置但未运行的通道返回 `200 + running:false`；409 留给后续动作/锁接口。
- 验证：`agent-contract-smoke` 82/82、`api-smoke` 58/58、`api-v3-smoke` 43/43、`agent-smoke` 63/63、`web-v3-audit` 120/120。
- 隔离验证：`agent.enable=false` 时 18081 三处 `ui_bridge.enabled` 均为 false、18096 端口关闭，4/4；证据 `output/ux-audit/agent-contract-ui-disabled.json`。
- 旧 `web-audit-round5.js` 仍在等待已不存在的 `#tcpBox`，属过期选择器；现行页面审计以 `web-v3-audit.js` 为准。

## 14. C2 完成记录（2026-11-23）

- 新增 `src/services/agent/agent_scan.{h,cpp}`：统一通道对象、只读上下文、被动终端探测、Agent 按钮视图；不写 cmdq、不发 Enter、不打开串口、不调用 18096。
- 新增路由：`GET /status?view=agent`、`GET /agent/scan`、`GET /agent/probe`、`GET /agent/state`、`GET /actions?view=agent`；非 GET 全部 405 结构化错误。
- `serial_bridge::status_json()` 增加 `epoch`；`ssh_bridge::status_json()` 增加 `total/epoch`，供状态与后续 `/log?since=` 使用。
- `/api/help` 已实现操作由 45 增至 49，新增 `agent.scan.get`、`agent.probe.get`、`agent.state.get`、`actions.agent.get`。
- `agent_scan` 通道对象提供 `name/kind/detail/running/started/state`、`connection`、`buffer(capacity/total/epoch/disk)`、`counters`、`terminal`、`buttons`、`features`；`total` 即下一次增量读取的候选 `since`。
- 终端态按“最靠后命中”判定：未运行 `disconnected/high`；`login:/username:`、`password:`、`U-Boot>`/`=>`/`Hit any key`、`root@`/末行 `$`/`#`；无标记 `unknown/low`；末尾 80 字节内 `high`，否则 `medium`。`tail` clamp 64–16384，默认 2048；evidence 最多 120 字符。
- 锁与动作审计仍显式占位：`locks.enforced=false`、`note=lease lock lands in C5`；`/agent/state.last_action=null`、`note=action audit lands in C4/C6`，不伪造能力。
- 验证：C2 `agent-context-smoke` 73/73；C1 `agent-contract-smoke` 82/82；`api-smoke` 58/58；`api-v3-smoke` 43/43；`agent-smoke` 63/63；`web-v3-audit` 120/120。
- 只读断言：scan/probe/state 调用前后 `tx_bytes` 相等；终端五类注入 + 离线通道六态均通过。
- 隔离验证：`agent.enable=false` 时 18081 的 health/help/status 均报告 `ui_bridge.enabled:false`，18096 端口关闭，且 `/agent/scan` 仍返回 5 通道，5/5；证据 `output/ux-audit/agent-context-ui-disabled.json`。
- 结果与脚本：`output/webtest/agent-context-smoke.js`、`output/ux-audit/agent-context-result.json`；构建日志 `output/wc-build.log`。
- 测试后已恢复基线配置 `output/ux-audit/config-before-web.json`，SHA-256 `2D50FDA278A11F4887F0734EA6E3669B7B59B33064640608C81AEE5443B6743A`；当前运行仅 18096，18081 关闭。

## 15. C3 完成记录（2026-11-23）

- `/log` 支持尾部与增量两种模式：无 `since` 返回尾部，`next_since=total`；有 `since` 必须同时给 `epoch`，返回新字节、`next_since`、`dropped/dropped_bytes`、`reset/reset_reason`、`truncated`。
- 响应头 `X-Log-Epoch/X-Log-Total/X-Log-Dropped` 与 JSON 同源；增量按 `chars` 从最旧未读字节分页，游标连续、不跳字节。
- epoch 不匹配、游标超前、并发清空都返回 `reset:true` + 当前尾部；`/log/clear` 响应新增新 `epoch/total`，旧读者下一次必收 reset。
- `POST /log/disk` 运行时开关落盘（不改配置）；SSH/串口状态都改读 `LogBuffer::disk_enabled()`。打开失败返回 500 结构化错误。
- `LogBuffer` 单文件 256MB 上限：已达上限拒绝打开；写入越界时停止落盘、关闭文件并置 `last_error`，读取与 ring 不受影响。
- `/api/help` 已实现操作增至 50，`log.get` 增加 `since/epoch` 与游标返回字段，新增 `log.disk.post`。
- `/features.incremental_log` 与新增 `/features.log_disk` 均为 true；`/agent/state.buffer.disk` 反映实际落盘状态。
- 验证：C3 `agent-log-smoke` 59/59；C2 `agent-context-smoke` 73/73；C1 82/82；`api-smoke` 58/58；`api-v3-smoke` 43/43；`agent-smoke` 63/63；`web-v3-audit` 120/120。
- 隔离验证：18096 关闭时 18081 的 health/help/status/scan/log 全部可用，6/6；证据 `output/ux-audit/agent-log-ui-disabled.json`。
- 脚本/结果：`output/webtest/agent-log-smoke.js`、`output/ux-audit/agent-log-result.json`；构建日志 `output/wc-build.log`。
- 测试后已恢复基线配置，SHA-256 `2D50FDA278A11F4887F0734EA6E3669B7B59B33064640608C81AEE5443B6743A`；当前运行仅 18096，18081 关闭。
