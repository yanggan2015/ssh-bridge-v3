# Web 控制台与 Agent 接口第二轮补齐报告

- 日期：2026-11-23
- 分支：`feature/web-console-complete-20261123`
- 源码：`C:\code\ssh-bridge-windows3`
- 二进制：`output/build/ssh_bridge3.exe`
- 审计：`docs/WEB-AGENT-AUDIT.md`
- 计划：`docs/WEB-COMPLETE-PLAN.md`
- 功能清单：`docs/功能清单.md`（O 段已实现，P 段为后续缺口）

## 1. 结论

本轮以 `C:\code\ssh-bridge` 的浏览器功能为参考，保留本应用独有的 Agent UI 桥、快捷指令、继电器、TCP 转发/隧道等能力，把 Web 控制台从 10 页扩展到 **12 页**，并补齐参考仓的通道管理、终端工具箱、服务配置和机器可读帮助。

- 页面：12 页，桌面 1440×960、移动端 390×844。
- 新增/增强接口：8 组，覆盖通道 CRUD、日志下载/清空、TFTP/NFS/Share 配置、`/api/help`、Agent 状态。
- Agent UI 桥：修复焦点状态与只读端点方法门禁，补齐 K1–K7、错误码和 token 场景。
- 测试：**348 项断言全部通过**（58 + 43 + 120 + 63 + 64）。
- 浏览器：零 JS 错误、零请求失败、零 404/5xx；移动端 12 页无横向溢出。
- 测试数据已清理，测试配置已恢复基线并核验 SHA-256。

## 2. 功能与页面

| 页面 | 路由 | 本轮内容 |
| --- | --- | --- |
| 仪表盘 | `/dashboard` | 通道切换、终端发送/清屏、WS 状态、7 个服务徽章、终端查找/复制/下载/字号/全屏 |
| 通道管理 | `/channels` | 串口/SSH 卡片、新增/编辑/改名/删除、字段联动、删除确认 |
| 板控动作 | `/actions` | commands/relay/script 三类按钮 CRUD、Lua/DSL 速查 |
| 快捷/脚本 | `/button-script` | 按通道新增/编辑/删除快捷指令 |
| 继电器 | `/relay` | 设备 CRUD、通道状态、ON/OFF/脉冲测试 |
| TFTP | `/tftp` | 配置、启停、运行状态、共享文件 |
| NFS | `/nfs` | 别名/UID/GID/根配置、状态、共享文件 |
| 共享目录 | `/share` | HTTP 端口/根配置、访问状态、文件浏览 |
| TCP 转发 | `/tcp-forward` | 监听/端口基数/目标/CIDR、启停、状态 |
| 隧道 | `/tunnel` | HTTP/SSH 映射、Token 掩码、启停、状态 |
| Agent 接口 | `/agent` | 18081 设备面、作业/锁/TFTP、接口速查；18096 状态只读 |
| 配置 | `/config` | 分组表单、密钥掩码、搜索、数组删除、保存确认与重载 |

本应用独有能力的 Web 呈现：

- 18096 桌面 UI 桥：18081 只展示启用/端口状态，不直连、不代理、不下发控件树或 UI token；窗口自检在桌面端完成。
- 快捷指令：保留每通道快捷条，Web 与原生窗口共用一份配置。
- Lua 脚本：板控页内嵌速查，与 `docs/脚本参考-S12.md` 对齐。
- 1/2/4 分屏：本轮仍只在原生窗口可用，Web 只读展示列入后续 P6。

## 3. 新增接口

| 方法 | 路径 | 作用 |
| --- | --- | --- |
| GET | `/api/help` | 机器可读 operations/pages；POST 返回 405 |
| GET/POST | `/api/channels` | 通道列表；新增/编辑串口或 SSH |
| POST | `/api/channels/delete` | 删除通道，清理画框引用 |
| GET | `/log/download?channel=` | 下载当前通道日志，`text/plain` attachment |
| POST | `/log/clear?channel=` | 清空通道日志缓冲，需 token |
| GET/POST | `/api/tftp/config` | TFTP 端口/绑定 IP/根/允许写入 |
| GET/POST | `/api/nfs/config` | NFS 别名/UID/GID/根 |
| GET/POST | `/api/share/config` | Share HTTP 端口/根 |
| GET | `/api/agent/status` | Agent 开关、端口、token 要求、base URL、端点 |

## 4. 本轮修复的真实问题

1. **Agent `focus_id` 永远为空**：`/state` 从 HTTP 线程调用 `GetFocus()`，不是 UI 线程。改为在 `ub_execute` 记录 `g_focus_id`，状态快照读该值。
2. **只读端点接受 POST**：`/ui-tree`、`/state`、`/capabilities`、`/logs`、`/ping` 对 POST 返回 200。加 GET-only 门禁，统一 405。
3. **通道写接口堆损坏 `0xc0000374`**：`json_text(root)` 内部已 `cJSON_Delete(root)`，调用方再次删除导致堆损坏。去掉重复删除，并补 `save_config_json` 确保落盘。
4. **帮助 POST 未拒绝、重名未 409、改名/删除 404**：随通道保存逻辑一起修复。
5. **TFTP 运行状态标题显示 `__TITLE__`**：模板只替换第一个占位符，改为循环替换全部。
6. **移动端通道页头部按钮竖排挤压**：760px 以下卡片头改纵向、操作行左对齐，12 页无横向溢出。

## 5. 测试证据

### 5.1 API：58/58 + 43/43

- `api-smoke.js`：旧 10 页 API 回归，58/58。
- `api-v3-smoke.js`：通道 CRUD、日志下载/清空、三服务配置、help/agent status、错误码，43/43。
- 结果：`output/ux-audit/api-smoke-result.json`、`output/ux-audit/api-v3-result.json`。

覆盖的错误面包括 400 非法字段、401 无/错 token、404 未知通道或路由、405 方法不允许、409 重名或引用冲突、413 超限请求。

### 5.2 Agent UI 桥：63/63 + 64/64

- 空 token：63/63，覆盖 K1–K7、动作白名单、树不变量、日志 seq、400/401/404/405/409/413。
- 带 token：64/64，覆盖无 token/错 token 401、正确 token 200。
- 结果：`output/ux-audit/agent-smoke-result.json`、`output/ux-audit/agent-smoke-token-result.json`。

### 5.3 浏览器：120/120

- 12 页标题/导航/无 pageerror。
- 仪表盘 5 通道、7 服务徽章、WS 已连接、发送回显、查找高亮、字号、下载链接、复制、清空。
- 通道页 5 卡 → UI 新建 `ui-loop-audit` → 6 卡 → 删除确认 → 回到 5 卡。
- 板控动作/快捷指令/继电器新增删除、TFTP 配置 69/root 回读、共享文件上传删除、TCP/隧道保存。
- Agent 页 7 个端点速查、自检通过、树 72、通道 5、协议 1.1。
- 移动端 12 页无横向溢出，侧栏高度 179.8px。
- 零 console error、零 request failure、零 404/5xx。
- 结果：`output/ux-audit/v3-web-audit-result.json`。

### 5.4 截图

- 桌面 11 张：`output/ux-audit/v3-desktop-{dashboard,channels,actions,quick,relay,tftp,share,tcp,tunnel,agent,config}.png`
- 移动 3 张：`output/ux-audit/v3-mobile-{dashboard,relay,channels}.png`

## 6. 已知限制与后续缺口

1. **UI 调试页远程访问（已否决）**：C10 起 18081 不再代理或探测 18096，Agent 页仅显示 18096 状态；远程 UI 调试另立独立入口，不接入 18081 板控。
2. **服务生命周期**：TFTP/NFS/Share 配置读写已测，启动/停止/重复启停/端口释放未系统覆盖（P2）。
3. **Job 可观测性（已由 C6 完成）**：`/jobs` 列表/详情/取消、并发 429、进度与终态保留已落地。
4. **WS 断线恢复**：非法通道、断线重连、重复订阅未测（P4）。
5. **继电器无真实硬件**：CRUD、状态结构、引用冲突已验收，真实 COM 口 ON/OFF/脉冲只能做可执行性验证。
6. **隧道依赖外部 eadk-tunnel/frpc**：配置保存、启停入口与错误展示已完成，未验证真实外网映射。
7. **上传上限 16 MB**：mongoose 一次性读取请求体；大文件后续改流式。
8. **Web 监听 `0.0.0.0` + HTTP**：写接口需 token，读接口公开；建议下一轮加 `web.bind` 或默认回环。
9. **`/api/config` 对 token 持有者返回明文密钥**：页面已掩码；如需彻底防护，增加“掩码读/原值写”协议。
10. **参考仓对齐项**：主题/语言、XMODEM/YMODEM、更新/反馈、SFTP 专用页列入 P7。

缺口与实现方案已追加到 `docs/WEB-AGENT-AUDIT.md` §9 和 `docs/功能清单.md` P 段。

## 7. 配置恢复

- 测试配置：`output/terminal-test/ssh_bridge_config.json`
- 基线备份：`output/ux-audit/config-before-web.json`
- 恢复后 SHA-256：`2D50FDA278A11F4887F0734EA6E3669B7B59B33064640608C81AEE5443B6743A`
- 恢复后运行：Web 18081 拒绝连接；Agent 18096 `/ping` 正常、`/ui-tree` 200；5 通道恢复。
- 用户真实配置 `C:\Users\admin\.ssh-bridge\ssh_bridge_config.json` 未修改。

## 8. 交付

- 最新二进制：`output/build/ssh_bridge3.exe`
- 构建产物：`output/wc-build/build/ssh_bridge3.exe`
- 旧二进制备份：`output/build/ssh_bridge3.exe.bak-20261123`
- 回归脚本：`output/webtest/{api-smoke,api-v3-smoke,agent-smoke,web-v3-audit}.js`
- 审计/计划/报告：`docs/WEB-AGENT-AUDIT.md`、`docs/WEB-COMPLETE-PLAN.md`、本文件
