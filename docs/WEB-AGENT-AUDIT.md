# Web / Agent 功能覆盖审计（第二轮）

- 日期：2026-11-23
- 参考实现：`C:\code\ssh-bridge`（`webui_static/*`、`DESIGN.md`、`src/http_manager.c`）
- 当前实现：`C:\code\ssh-bridge-windows3`
- 范围：Web 控制台 + S10 Agent UI 桥 + 设计文档中的 K1–K7
- 结论：现有 84 项浏览器 / 58 项 API 覆盖的是“已经存在的 10 页”，**没有覆盖参考仓的通道管理、终端工具箱、服务配置和 Agent 接口错误面**。

## 1. 参考仓功能面

| 参考功能 | 参考实现 | 当前状态 | 判定 |
| --- | --- | --- | --- |
| 通道新增/编辑/删除/启停 | `/boards` + Dashboard 通道对话框 | 只能去配置页改 JSON | 缺失 |
| 串口/SSH 字段化编辑 | Dashboard 通道编辑 | 配置页通用叶子字段 | 缺失专用表单 |
| 终端复制/粘贴 | Dashboard 工具菜单 | 无 | 缺失 |
| 终端日志下载/清空 | `/log/download`、`/log/clear` | 只有本地清屏 | 缺失 |
| 终端查找/字号/全屏 | Dashboard 工具菜单 | 无 | 缺失 |
| 日志落盘开关 | `/log/disk` | 配置页可改，Web 无即时入口 | 缺失 |
| 主题 / 语言 | `themes.css`、`i18n.js`、`ui_settings` | 单主题、单语言 | 未对齐，P2 |
| TFTP 配置（端口/根/IP/写权限） | `/tftp/config` | 只有启停与状态 | 缺失 |
| NFS 配置（别名/UID/GID/根） | `/nfs/config` | 只有启停与状态 | 缺失 |
| Share 配置（端口/根） | `/share/config` | 只有启停与状态 | 缺失 |
| 配置页基础/详细 Tab、数组删除、密钥掩码 | `config.html` | 已有 | 已覆盖 |
| 板控/继电器/共享/TCP/隧道 | 各子页 | 已有 | 已覆盖 |
| 更新/反馈 | `config.html` | 原生有更新，Web 无 | 不在本轮 P1 |
| XMODEM/YMODEM 等文件传输 | Dashboard | 无 | 不在本轮 P1 |
| 机器可读帮助 `/api/help` | `DESIGN.md §17.4` | `/help` 只有纯文本 | 缺失 |
| Agent HTTP 帮助 `/help` | `DESIGN.md §5.1` | 仅 Web 设备 API 文本 | 未对齐 |

## 2. ssh-bridge-windows3 独有能力

这些能力参考仓没有，Web 应该显式呈现，而不是只藏在原生窗口：

| 独有能力 | 当前 Web | 建议 |
| --- | --- | --- |
| Agent UI 桥 `/ui-tree` `/state` `/control` `/action` | 无页面、无测试 | 新增 Agent 接口页 + 自检 |
| 1/2/4 分屏与活动画框 | 无 | 首版只读展示，后续再做 Web 分屏控制 |
| 快捷指令（每通道） | 已有 CRUD | 保持 |
| Lua 脚本按钮 | 有编辑框，无速查 | 增加 Lua/DSL 速查与示例 |
| 入站 SSH / 发现服务 | 只有徽章 | 后续单独页 |
| 自更新 | 原生有 | Web 暂不纳入 P1 |

## 3. Agent 接口（K1–K7）测试缺口

| 设计项 | 接口 | 现有测试 | 缺口 |
| --- | --- | --- | --- |
| K1 控件树 | `GET /ui-tree` | 旧审计只读 count | 未验证 id 唯一、rect 越界、visible/actions、password 脱敏 |
| K2 操作 | `GET /control`、`POST /action` | 旧审计只点击少量控件 | 未覆盖 set_text/get_text/clear/check/toggle/select/set_value/focus/command |
| K3 状态快照 | `GET /state` | 旧审计只读 summary | 未验证 action_count/last_action/focus_id 与动作后的业务状态 |
| K4 动作 | `POST /action` | 未测 | 未验证 unknown id/action、disabled 409、body 过大 413、方法 405 |
| K5 独立开关 | `agent.enable` | 未测 | 未验证 Agent 关闭不影响 Web，Web 关闭不影响 Agent |
| K6 令牌 | `X-Ui-Token` | 未测 | 未验证空 token/错误 token 401、正确 token 200 |
| K7 与网页分离 | 18081 / 18096 | 未测 | 未验证两个端口独立监听、独立关闭 |
| 日志 | `GET /logs?since=` | 未测 | 未验证 seq 递增、动作写日志 |
| 基础 | `GET /ping`、`GET /capabilities` | 旧审计读过 | 未验证 actions 列表与真实实现一致 |

## 4. 现有 Web 测试的未覆盖点

| 类别 | 未覆盖 |
| --- | --- |
| 鉴权/方法 | 405、错误 token、读接口公开边界、`/api/config` 只有 token 可读 |
| Web 通道 | 新增/改名/删除串口与 SSH、重名、非法名称、删除后引用清理 |
| 终端 | `/log` 的 `channel/chars` 边界、`/log/download`、`/log/clear`、WS 非法通道 |
| 服务配置 | TFTP/NFS/Share 配置读写与非法端口/路径 |
| 服务生命周期 | TFTP 停止后端口释放、NFS/Share 重复启停 |
| Job | 未知 job、并发 `/action`、停用动作 409（已有）、超时展示 |
| 页面 | 移动端只测了 6 页，不是全部页面 |
| 视觉 | 所有新页面/编辑器/错误态截图 |
| Agent | 见第 3 节，完全没有自动化套件 |

## 5. 本轮追加功能清单

| ID | 功能 | 优先级 | 实现方式 |
| --- | --- | --- | --- |
| F10 | Web 通道管理 | P1 | `GET/POST /api/channels`、`POST /api/channels/delete`；串口/SSH 表单；重名与名称校验；删除后清理画框引用 |
| F11 | 终端工具箱 | P1 | `/log/download`、`/log/clear`；前端复制、查找、字号 A-/A+、全屏、清空缓冲 |
| F12 | 服务配置 | P1 | `GET/POST /api/tftp/config`、`/api/nfs/config`、`/api/share/config`；端口/根/别名/UID/GID/写权限 |
| F13 | Agent 接口页 | P1 | `GET /api/agent/status`；18081 设备面 + 18096 状态只读（不直连、不代理）；展示帮助与 curl 示例 |
| F14 | 机器可读帮助 | P1 | `GET /api/help` JSON operations；`/help` 文本补充入口 |
| F15 | Lua/DSL 速查 | P2 | 板控页内嵌速查卡与最小示例 |
| F16 | Agent 回归套件 | P1 | 新 `agent-smoke.js` 覆盖 K1–K7 与错误码；单独启停 token 场景 |
| F17 | Web 全页回归 | P1 | 新 `web-v3-audit.js` 覆盖 12 页、桌面/移动端、通道/终端/服务/Agent 页 |

## 6. 实施顺序

1. **Batch A**：先写 `agent-smoke.js`，跑出真实失败项；修 Agent 桥行为。
2. **Batch B**：通道 CRUD API + 通道页；编译、API、浏览器回归。
3. **Batch C**：终端工具箱 + 日志接口；编译、回归。
4. **Batch D**：TFTP/NFS/Share 配置 + Agent 页 + `/api/help` + 脚本速查；编译、回归。
5. **Batch E**：12 页全量桌面/移动端 + Agent token 场景 + 配置恢复；报告。

每批规则：改一批 → 编译 → 跑对应测试 → 通过后再进下一批；失败不进入下一批。

## 7. 验收口径

- Agent：每个接口正常/错误路径都有断言；`ok:true` 后必须读 `/state` 或 `/control`。
- Web：所有新页面桌面 1440×960、移动端 390×844；无横向溢出。
- 错误面：400/401/404/405/409/413 至少各一条。
- 测试数据：新增后删除，最终 actions 0、relay 0、quick 保持基线、共享根恢复 4 项。
- 配置：测试结束恢复基线 SHA-256，Web 默认关闭。

## 8. 本轮实施与测试结果

状态：**F10–F17 已实现并验证**（2026-11-23，分支 `feature/web-console-complete-20261123`）。

| 套件 | 结果 | 覆盖 |
| --- | --- | --- |
| `api-smoke.js` | 58/58 | 旧 10 页 API 回归 |
| `api-v3-smoke.js` | 43/43 | 通道 CRUD、日志下载/清空、三服务配置、help/agent status、错误码 |
| `agent-smoke.js` 空 token | 63/63 | K1–K7、动作白名单、400/401/404/405/409/413、树不变量 |
| `agent-smoke.js` 带 token | 64/64 | 无 token/错 token 401、正确 token 200 |
| `web-v3-audit.js` | 120/120 | 12 页桌面/移动、终端工具、通道 UI CRUD、服务配置、Agent 自检、无 JS/请求错误 |

本轮测试直接发现并修复的问题：

1. **Agent `focus_id` 永远为空**：`/state` 从 HTTP 线程调用 `GetFocus()`，不是 UI 线程。改为在 `ub_execute` 记录 `g_focus_id`，状态快照读该值。
2. **只读端点接受 POST**：`/ui-tree`、`/state`、`/capabilities`、`/logs`、`/ping` 对 POST 返回 200。加 GET-only 门禁，统一 405。
3. **通道写接口崩溃 `0xc0000374`**：`json_text(root)` 内部已 `cJSON_Delete(root)`，调用方再次删除导致堆损坏。去掉重复删除，并补 `save_config_json` 确保落盘。
4. **帮助 POST 未拒绝、重名未 409、改名/删除 404**：随通道保存逻辑一起修复。
5. **TFTP 运行状态标题显示 `__TITLE__`**：模板只替换第一个占位符，改为循环替换全部。
6. **移动端通道页头部按钮竖排挤压**：760px 以下卡片头改纵向、操作行左对齐，12 页无横向溢出。

产物：`output/ux-audit/v3-web-audit-result.json`、`v3-desktop-*.png`（11 张）、`v3-mobile-*.png`（3 张）。

## 9. 仍未完成的功能缺口与实现计划

这些缺口不阻塞本轮 F10–F17，但属于下一轮应补的完整体验。已同步追加到 `docs/功能清单.md` 的 O/P 段。

| ID | 缺口 | 当前问题 | 实现方案 | 优先级 |
| --- | --- | --- | --- | --- |
| P1 | Agent 页远程访问 | 页面直连 `127.0.0.1:18096`，浏览器不在同机时失败 | **已否决（C10）**：18081 不代理 18096，Agent 页改为 18096 状态只读；远程 UI 调试另立独立入口，不进入 18081 | P1 |
| P2 | 服务生命周期验收 | 配置读写已测，启动/停止/重复启停/端口释放未系统测 | 增加浏览器与 API 用例：TFTP 启动→端口占用→停止→端口释放；NFS/Share 重复启停；失败态提示 | P1 |
| P3 | Job 可观测性 | 只显示动作结果，缺排队/超时/取消/并发视图 | 增加 `/api/jobs`、`/api/jobs/cancel`，前端任务面板显示 queued/running/done/timeout，并限制并发 | P1 |
| P4 | WS 断线恢复 | 只显示“已连接”，未测非法通道、断线重连、重复订阅 | 前端指数退避重连与状态条；后端校验 channel；测试覆盖非法通道与重连 | P1 |
| P5 | 日志落盘开关 | 仅配置页可改，Web 无即时入口 | 在仪表盘/日志卡增加 `logging.disk` 开关，保存后提示重启或即时应用范围 | P2 |
| P6 | 分屏 Web 化 | 1/2/4 分屏只在原生窗口，Web 不可见 | 先做只读 `GET /api/panes` 展示布局与活动画框；再做切换 API 与权限保护 | P2 |
| P7 | 参考仓对齐项 | 主题/语言、XMODEM/YMODEM、更新/反馈、SFTP 专用页未做 | 主题/语言先做 `ui_settings` + CSS 变量；传输协议与 SFTP 单独立项；更新/反馈沿用原生能力做 Web 入口 | P2 |

验收顺序建议：P2 → P4 → P6 → P7。P1 已否决（违反 18081 隔离）；P3 已由 C6 完成；P5 已由 C3 完成。每项仍按“一批一编译一测试一提交”，不允许把多个高风险后端改动混在一次提交。

