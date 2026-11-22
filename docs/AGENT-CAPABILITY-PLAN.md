# Agent 能力模型与实施规划

- 日期：2026-11-23
- 参考实现：`C:\code\ssh-bridge`（`DESIGN.md` §4–§5、§17.4、§18；`src/http_manager.c`；`src/job_runner.c`；`skills/ssh-bridge/SKILL.md`）
- 已成熟移植：`C:\code\ssh-bridge-windows2`（`agent接口.md`、`src/http_manager.c`）
- 本轮落地对象：`C:\code\ssh-bridge-windows3` 重写版
- 执行清单：`docs/AGENT-EXECUTION-LIST.md`
- 结论：18081 是**完整的板卡 Agent 控制面**，是软件的核心能力；18096 是**独立的桌面 UI 调试/兜底面**。两套系统互不替代，18081 不得依赖 18096。

---

## 1. 为什么这些能力是基本能力

Agent 不是“远程遥控器”，而是**闭环执行器**。它必须自己完成：

```text
发现运行环境 → 建立通道上下文 → 选择正确动作语义 → 执行 → 读业务真值 → 协调共享资源 → 搬运工件
```

只给“发一条命令”的接口，Agent 只能盲发、猜结果、抢通道，无法用于真实开发板。

### 1.1 硬件的六个现实约束

| 现实约束 | 如果没有对应能力 | 参考仓的解法 |
| --- | --- | --- |
| 端口、通道名、按钮 id 随现场变化 | 静态文档过期，Agent 调不存在的路径/按钮 | `/help`、`/api/help` 按当前配置现场生成 |
| 串口是字节流，不是请求-响应 | 发命令后不知道何时结束，读不到输出 | `/command` 202 + `/log`；`/agent/exec` 结束标记 |
| 同一通道有 login / password / U-Boot / shell 多种态 | 在 U-Boot 里发 shell 命令，或在 login 界面盲跑 | `/agent/probe` + `/agent/scan` |
| 人和多个 Agent 可能同时写同一条通道 | 输入交错、命令污染、长任务被抢 | `/lock` `/unlock` + `X-Agent-Session` + 423 |
| 烧录/升级需要搬镜像并校验 | 只传一半、版本不对、无法确认完整性 | TFTP/share/NFS + MD5 + 分片上传 + 进度 |
| 长任务不能阻塞 HTTP poll 线程 | 一个继电器脉冲或大文件传输让全站卡死 | JobRunner + 202 + 后台完成回写 |

### 1.2 七个设计原则

1. **自描述**：Agent 只读 `/api/help` 和 `/help` 就知道当前能做什么，不靠记忆和猜测。
2. **先看后动**：先 `/agent/scan`、`/agent/probe`，再选动作；状态不符不执行。
3. **动作语义显式**：异步、短阻塞、标记阻塞分开，不能用一种“发命令”包打天下。
4. **业务真值优先**：`ok:true` 只代表已送达；必须读 `/state`、`/log`、`/agent/probe` 验证。
5. **共享资源有租约**：写通道前可加锁，TTL 到期自动释放，冲突返回 423。
6. **控制面不被数据面拖死**：所有可能超过 2ms 的工作进 JobRunner，HTTP 立即 202。
7. **能力白名单**：只暴露已实现动作；未实现不列进 capabilities；Tunnel 等高风险能力不进 Agent 帮助。

---

## 2. 参考仓能力地图

### 2.1 自描述与发现

| 能力 | 接口 | 为什么必须有 |
| --- | --- | --- |
| 存活与版本 | `GET /health`、`GET /version` | Agent 先确认进程、工具名、版本、API 是否可用 |
| 机器可读操作表 | `GET /api/help`、`GET /operations` | 只暴露已实现路径和参数，避免调用不存在的 API |
| 人类/Agent 手册 | `GET /help`、`GET /docs`、`GET /agent-help` | 按当前通道生成可复制 curl、真实按钮 id |
| 单通道手册 | `GET /help/<channel>` | 省 token，只返回当前板/链路的命令块 |
| 全通道扫描 | `GET /agent/scan` | 一次拿到连接态、终端态、按钮、features、网段、登录候选 |

实现要点：帮助不是静态 Markdown，而是**运行时生成**；通道增删、按钮启停、端口变化后自动反映。

### 2.2 通道上下文

| 能力 | 接口 | 为什么必须有 |
| --- | --- | --- |
| 通道总览 | `GET /status?view=agent` | 知道有哪些 board/link、连接否、锁状态 |
| 终端态探测 | `GET /agent/probe?board|link=` | 返回 `disconnected/login/password/uboot/shell/unknown` |
| 状态摘要 | `GET /agent/state` | 按通道汇总状态、按钮 id、日志长度 |
| 可执行按钮 | `GET /actions?board|link=&view=agent` | 只返回真实 id/label/enable/backend，不猜按钮 |

### 2.3 动作面：三种时序语义

| 接口 | 返回 | 适用 | 为什么不能合并 |
| --- | --- | --- | --- |
| `POST /command` | 立即 `202 queued` | 日常串口、U-Boot、非 shell | 串口异步；等命令结束会阻塞 HTTP |
| `POST /command_block` | 最多 `timeout` 秒，返回 `output` | 短命令、已知快速返回 | 有明确上限，适合 2 秒级操作 |
| `POST /agent/exec` | 等到结束标记或超时 | 已在 Linux shell | 自动追加 `; echo end_...`，能拿到完整输出 |
| `POST /write` | `202` | 原始字节、无行尾 | 有些交互不能自动加回车 |
| `POST /send_keys` | `202` | Ctrl+C、进 U-Boot、重复按键 | 需要按时序连续发键，不是单条命令 |
| `POST /action` | 异步 job 或直接结果 | 板控/继电器/脚本按钮 | 复杂动作应复用 UI 同一按钮 id，不复制底层时序 |

关键约定：`board` 与 `link` 互斥；串口用 `board`，SSH-Link 用 `link`；写错键等于操作错通道。

### 2.4 观测面

| 能力 | 接口 | 为什么必须有 |
| --- | --- | --- |
| 增量读日志 | `GET /log?since=&epoch=` | 只取新输出，避免重复大段；响应头带游标 |
| 读末尾 | `GET /log?chars=` / `lines=` | 快速看最近结果 |
| 下载/清空 | `GET /log/download`、`POST /log/clear` | 保留证据、重开实验 |
| 磁盘日志开关 | `POST /log/disk` | 需要持久证据时打开 |
| 传输进度 | `GET /send_file/progress` | 大文件不能只知道“已提交”，要知道 sent/total/state |

### 2.5 协同与作业

| 能力 | 实现 | 为什么必须有 |
| --- | --- | --- |
| 通道租约 | `POST /lock`、`POST /unlock` | 人和多个 Agent 共用一条物理通道，必须避免输入交错 |
| 会话标识 | `X-Agent-Session` 或 `session_id` | 同 session 可续租；他方写返回 423 |
| TTL | 默认 1000ms，上限 86400000ms | 防止 Agent 崩溃后永久占锁 |
| 后台作业 | `JobRunner` + `wakeup` 回写 | 继电器、host 命令、XMODEM、长动作不能阻塞 poll 线程 |
| 异步受理 | `202 queued` | 先确认已排队，再用 job/状态查询结果 |

### 2.6 工件与生命周期

| 能力 | 接口 | 为什么必须有 |
| --- | --- | --- |
| TFTP 文件面 | `/tftp/status|files|mkdir|upload|download|md5|delete` | U-Boot 拉内核/ramdisk，必须能上传、校验、删除 |
| 分片续传 | `POST /tftp/upload?offset=&md5=` | 大镜像不能一次 HTTP 传完 |
| 共享/NFS | `/share/*`、`/nfs/*` | Linux 侧读同一份文件，避免三份副本对不上 |
| 通道生命周期 | `/boards`、`/reload`、`/reconnect` | 现场换板、换串口、重连 |
| 串口控制 | `/serial/open|close|break|lines|configure` | DTR/RTS、Break、波特率是板级调试的基本操作 |
| 热改配置 | `/channel/configure`、`/link/configure` | 不重启进程改时间戳/目标/账号 |

### 2.7 安全边界

- Tunnel 启停**不写入 Agent 帮助**，只留人工网页操作；避免 Agent 把调试口暴露到公网。
- 配置写入、服务关闭、电源动作属于高影响操作，必须有 token/白名单/明确 id。
- 密码不出现在 `/ui-tree`、日志、帮助文；Agent 读凭据要单独授权。
- 只列出已实现动作；未实现返回 400，不出现在 capabilities。

---

## 3. 参考实现的机制

### 3.1 JobRunner

`src/job_runner.c` 把可能阻塞的工作从 mongoose poll 线程移到 worker：

```text
HTTP 线程: 校验 → 组 job → job_submit → 立即 202
Worker:    执行命令/继电器/传输
完成:      wakeup → poll 线程写响应或更新 job
```

这就是为什么 `/command` 立即 202、`/send_file` 有 progress、继电器脉冲不会冻结网页。

### 3.2 结束标记执行

`/agent/exec` 发送：

```text
<command>; echo end_cmd_<id>
```

然后轮询通道 ring，直到看到标记或超时，返回 `output/state/timed_out`。它只适合已有 shell 的场景；U-Boot 不能假设支持 `;` 和 `echo`。

### 3.3 终端态探测

`agent_probe_obj()` 结合“连接状态 + 日志尾部特征”判断：

```text
disconnected / login / password / uboot / shell / unknown
```

Agent 必须先 probe；状态不符时不要盲跑 `exec`。

### 3.4 租约锁

按通道维护 `session_id / expires_us / ttl_ms`；写接口统一 `reject_if_locked()`。不是全局锁，而是**按通道**，避免一块板的长任务锁住所有板。

### 3.5 动态帮助

`/help` 由当前配置生成，包含：

- 当前 Base、board/link 键、真实通道名；
- 该通道已使能按钮的 `POST /action` curl；
- command / log / probe / exec / lock 的常用顺序；
- 不写 tunnel。

静态 Skill 只是入口，现场 `/help` 才是权威。

---

## 4. 当前 Windows 重写版的差距

| 能力面 | 现状 | 差距 |
| --- | --- | --- |
| 发现 | `/health`、`/api/help`、`/help`、`/help/<channel>` 已有雏形 | help 内容太短；无 `surfaces`；无动态按钮 curl；无单通道完整手册 |
| 上下文 | `/status` 有通道/服务/分屏 | 无 `view=agent` 精简视图；无 `/agent/scan`；无 `/agent/probe`；无终端态 |
| 观测 | `/log?chars=`、`/log/download`、`/log/clear` | 无 `since/epoch` 增量游标；无 `/log/disk`；无传输进度 |
| 动作 | `/write`、`/action`、`/job` | 无 `/command` 202；无 `/command_block`；无 `/agent/exec`；无 `/send_keys`；无 serial 控制/重连 |
| 协同 | 无 | 无 `/lock` `/unlock`；无 `X-Agent-Session`；无 423；多 Agent 会抢通道 |
| 作业 | `/job?id=` 仅供板控动作 | 无 job 列表/取消/超时状态；长任务能力不统一 |
| 工件 | `/api/files/*`、`/api/tftp/config` | 无 `/tftp/md5`、无分片上传、无 TFTP 文件 API 兼容层 |
| 通道管理 | `/api/channels` 已有 | 无 `/boards` 兼容语义；无 `reload/reconnect`；无串口 open/close/break/lines/configure |
| 自描述 | `/api/help` 有 operations | 无 implemented 标记、无 surface、无 feature flags、无每通道动态 id |
| UI 桥 | 18096 七个端点完整 | 独立桌面调试面，不是设备 Agent API；远程访问仍失败 |

结论：当前版本有“网页 API”，但还不是“Agent 可闭环的设备 API”。

---

## 5. 目标架构

### 5.1 两套系统，一个发现入口

```text
Agent / CLI
   │
   ├─ 18081  Device Agent API  ── ChannelCore ── 串口 / SSH / 继电器 / TFTP
   │      /health /api/help /help /status /agent/* /command* /log /lock /tftp/*
   │
   └─ 18096  UI Bridge（独立调试） ── bridge_host ── Win32 控件/对话框
          /capabilities /ui-tree /control /state /action /logs
```

- **18081 是唯一板控主路径**：扫描、探测、命令、锁、作业、文件传输必须在这里独立闭环。
- **18096 是独立桌面调试面**：只操作 Win32 窗口，供调试、UI 回归、人工兜底；它不参与 18081 的板控流程，也不做 18081 的 fallback。
- `/api/help` 返回 `surfaces`，让 Agent 知道两个面的地址、鉴权、是否启用；`enabled:false` 也要明示。
- 18081 的 Agent 路由与现有 Web `/api/*` 并存；现有 Web 页面和测试不破坏。
- 代码隔离：18081 路由中禁止出现 `18096`、`/ui-tree`、`/control`、`X-Ui-Token`；板控能力缺失只能补 18081。

### 5.2 统一发现响应草案

```json
{
  "schema": 2,
  "service": "ssh-bridge",
  "protocol": "1.1",
  "build": "2026-11-23",
  "surfaces": {
    "device": { "base": "/", "enabled": true, "auth": "bearer-write" },
    "ui": { "base": "http://127.0.0.1:18096", "enabled": true, "auth": "optional-token" }
  },
  "operations": [
    {
      "name": "agent.exec",
      "surface": "device",
      "method": "POST",
      "path": "/agent/exec",
      "params": ["board|link", "command", "timeout?"],
      "implemented": true,
      "returns": ["output", "state", "timed_out"]
    }
  ]
}
```

原则：`implemented=false` 的路径不得出现在实际可调用列表；需要保留的能力只做兼容说明，不做空路由。

### 5.3 统一错误契约

```json
{
  "ok": false,
  "error": {
    "code": "agent_lock",
    "message": "channel held by another session",
    "detail": { "session_id": "...", "expires_in_ms": 12345 }
  },
  "request_id": "..."
}
```

状态码：

| 码 | 含义 |
| --- | --- |
| 200 | 读成功或同步动作成功 |
| 202 | 已受理，后台执行；返回 job_id |
| 400 | 参数非法 / 未实现动作 / 状态不匹配 |
| 401 | token 缺失或错误 |
| 403 | 操作被策略禁止（Tunnel/危险配置） |
| 404 | 通道或 id 不存在 |
| 405 | 方法不对 |
| 409 | 通道 disabled / 前置状态不满足 / 业务冲突 |
| 413 | body 过大 |
| 423 | 通道被其它 Agent 租用 |
| 429 | 单通道动作在途或队列满 |
| 500 | 内部错误 |
| 504 | UI/工作线程超时 |

---

## 6. 能力实施路线

### P0：先建立 Agent 最小闭环

| 批次 | 内容 | 验收 |
| --- | --- | --- |
| A1 发现与契约 | `/health` 补齐 tool/version/build/api_enabled；`/api/help` 加 `surfaces/implemented/returns`；`/help`、`/help/<channel>` 按现场生成 | Agent 只读 help 就能选对 base、通道键、按钮 id；POST help 405 |
| A2 上下文 | `/status?view=agent`、`/agent/scan`、`/agent/probe`、`/agent/state`、`/actions?view=agent` | 5 通道扫描正确；probe 能区分 shell/login/uboot/disconnected；按钮 id 与 `/api/actions` 一致 |
| A3 观测 | `/log?since=&epoch=` 增量；响应头 `X-Log-Epoch/X-Log-Total`；`/log/disk`；job 状态 | 两次读取不重复；游标递增；清空后 epoch 变化 |
| A4 动作 | `/command` 202、`/command_block`、`/agent/exec`、`/send_keys`；统一 job_id | 串口异步 202；短命令有输出；exec 标记正确；U-Boot 不误用 exec |
| A5 协同 | `/lock` `/unlock`，`X-Agent-Session`，TTL，423 | A 锁后 B 写 423；同 session 续租；过期自动释放；不同通道互不影响 |
| A6 作业治理 | `/jobs`、`/jobs/{id}`、取消、超时状态；动作队列 | 长动作不阻塞 `/health`；取消生效；超时可见；并发有上限 |

### P1：补齐真实开发板工作流

| 批次 | 内容 | 验收 |
| --- | --- | --- |
| B1 通道生命周期 | `/boards` 兼容层（add/delete/enable/set_buttons）、`/reload`、`/reconnect`、`/serial/open|close|break|lines|configure` | 增删通道后热同步；串口重开；Break/DTR/RTS 生效 |
| B2 工件面 | `/tftp/status|files|mkdir|upload|download|md5|delete`；分片续传；`/send_file` + progress | 上传→MD5 一致→下载一致；断点续传；大文件不阻塞 |
| B3 帮助与 Skill | 每通道 help 含真实按钮 curl；`/api/help` 与 Skill 对齐；CLI 封装可选 | 换板名/端口后帮助自动更新；无 tunnel 字样 |
| B4 兼容与迁移 | 保留当前 `/api/*`；旧 Agent 脚本可用 board/link/channel；写一份迁移表 | Web 回归 120/120 不降；旧路径不被破坏 |

### P2：高风险与桌面独立调试

| 批次 | 内容 | 验收 |
| --- | --- | --- |
| C1 服务配置 | `/tftp/config`、`/nfs/config`、`/share/config`、`/tcp_forward/*`；危险写操作 token + 显式确认 | Agent 惯例只开不关；Tunnel 不出现在 help |
| C2 UI 面远程调试 | `/api/agent/proxy/*` 让远程调试器访问 18096；只用于 UI 调试/回归，不接入 18081 板控路径 | 非同机可读 `/ui-tree`；18096 关闭不影响 18081；token 不泄露 |
| C3 运维面 | 更新状态、反馈、发现、版本 | 不把更新/反馈混进核心控制面；失败可读 |

---

## 7. 建议的模块划分

当前 `web_console.cpp` 已经很大，不建议把 20 多个 Agent 路由继续塞进同一个 `handle_http`。

```text
src/services/agent/
  agent_routes.cpp      # /health /api/help /help /agent/* /command* /lock /jobs
  agent_help.cpp        # 动态帮助、按通道 curl、surface 表
  agent_probe.cpp       # 终端态探测
  agent_exec.cpp        # 结束标记执行、超时
  agent_lock.cpp        # 租约、TTL、session
  agent_jobs.cpp        # job 注册/查询/取消/清理
  agent_artifacts.cpp   # TFTP/share 文件面适配
```

- 复用现有 `ChannelCore`、`IBridge`、`Job` 和 `ServiceManager`，不另造一套通道。
- `agent_routes` 只做参数解析、鉴权、调用和 JSON 序列化；业务仍在 core。
- `/api/channels` 与 `/boards` 共用同一个 CRUD 实现，避免两套逻辑漂移。
- `/action` 与 UI 按钮共用同一个执行路径，不绕过互斥和 enable 语义。

---

## 8. 测试策略

### 8.1 契约测试

- `/api/help` 中每个 `implemented=true` 的路径都能用声明的方法调用；`implemented=false` 不出现在运行时列表。
- 方法错误 405；无 token 写 401；错误 token 401；body 超限 413；未知通道 404；未知动作 400。
- `/agent/probe` 六种态至少覆盖 shell/login/uboot/disconnected。
- `/command` 返回 202 且有 job_id；`/command_block` 在 timeout 内返回 output；`/agent/exec` 标记不丢失、不串到下一命令。
- `/lock` 后他方写 423；同 session 续租；过期自动释放；unlock 错误 session 不释放。
- `/jobs` 能看到 queued/running/done/timeout/cancelled；取消有效。

### 8.2 场景测试

| 场景 | 步骤 |
| --- | --- |
| Linux shell | scan → probe=shell → exec `uname -a` → 校验 output → probe 仍 shell |
| U-Boot | probe=uboot → `command` 设环境 → log 校验；不调用 exec |
| 异步串口 | `command` 202 → sleep → `log?since=` 只拿新增 |
| 多 Agent | A lock → B write 423 → A unlock → B write 202 |
| 文件 | TFTP 上传 → md5 → 下载 → 本地 md5 比对；分片续传 |
| 长任务 | 大文件/继电器脉冲 → `/health` 仍快速；job 状态推进 |
| 安全 | `/help` 无 tunnel；日志无密码；只读接口公开边界符合配置 |

### 8.3 不回归

- 现有 18081 Web 页面和 `/api/*` 全量回归保持通过。
- 现有 18096 UI 桥 63/64 项继续通过。
- **隔离验收**：关闭 18096 后，18081 全部板控场景通过；关闭 18081 后，18096 控件树/动作仍通过；四种开关组合互不影响。
- 代码扫描：18081 Agent 路由无 18096 依赖、无 `/ui-tree`/`/control` 调用。

---

## 9. 明确不做

1. 不让 Agent 启停 Tunnel，不把 Tunnel 写入 help/Skill。
2. 不提供任意 host shell；host 动作只能按白名单按钮 id 执行。
3. 不把密码、token、配置密钥写进 `/help`、日志、UI 树。
4. 不用 UI 点击代替已有设备 API；18096 只做独立桌面调试，不是板控兜底。
5. 不在 HTTP poll 线程 sleep；长任务一律 JobRunner。
6. 不做“万能 `/command`”：U-Boot、shell、原始字节、按键必须语义分离。

---

## 10. 一句话总结

参考仓的能力不是“接口多”，而是把 Agent 操作硬件的**不确定性**收敛成可发现、可等待、可验证、可协调的协议。Windows 重写版应先补 P0 的设备闭环（发现、上下文、观测、动作、锁、作业），再把通道生命周期和文件面补齐，最后才做服务配置和 UI 代理。
