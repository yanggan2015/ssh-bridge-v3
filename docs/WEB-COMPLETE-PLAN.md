# Web 控制台补齐功能清单与实施计划

- 版本：v1
- 日期：2026-11-23
- 对象：`ssh-bridge-windows3` Web 控制台
- 基线：`docs/功能清单.md` J1–J8 + 2026-11-22/23 Web 审计报告
- 目标：把当前“可观测 + 轻控制”控制台补齐为可实际操作的功能控制台，同时把视觉、信息层级、空态、错误态和移动端体验统一到可交付水平。

## 1. 现状与目标

| 编号 | 要求 | 现状 | 目标 |
| --- | --- | --- | --- |
| J1 | HTTP + WS，WS 只发 BINARY | 通过 | 保持并加回归 |
| J2 | 仪表盘：侧栏通道、终端、在线灯、TFTP/Tunnel 徽章 | 只有下拉、终端 | 通道侧栏、在线灯、服务徽章、最近状态 |
| J3 | 配置页：基础/详细表单 | 通过 | 保持，补密钥脱敏、数组删除 |
| J4 | 按钮页：commands / script 编辑与测试 | 只有快捷列表 + 发送 | 快捷指令 CRUD、板控按钮 CRUD、命令/脚本编辑、测试执行 |
| J5 | 继电器：设备 CRUD + 通道测试 | 只有空态 | 设备新增/编辑/删除、端口/通道配置、ON/OFF/脉冲测试、状态反馈 |
| J6 | 文件页：TFTP / NFS / 共享浏览与上传 | 只有服务开关 + JSON | 共享文件浏览、面包屑、上传、下载、删除、新建目录 |
| J7 | 隧道：映射配置与状态 | 只有状态 JSON | HTTP/SSH 映射表单、保存、启动/停止、状态/错误展示 |
| J8 | Bearer token，无 token 只读 | 通过 | 保持并加写接口回归 |
| 扩展 | TCP 转发配置 | 只有状态 JSON | 监听 IP/端口、目标端口、CIDR 表单与启停 |

## 2. 功能清单（可验收项）

### F1 仪表盘总览（J2）

- F1.1 左侧通道栏：通道名、串口/SSH 类型、在线灯、详情、TX/RX。
- F1.2 点击通道切换终端；当前通道高亮。
- F1.3 顶部服务徽章：TFTP、共享、NFS、入站 SSH、TCP、隧道、发现，显示 on/off。
- F1.4 5 秒轮询状态，不重建 WebSocket；状态刷新不丢终端内容。
- F1.5 无通道/全部离线时显示明确空态。

### F2 快捷指令管理（J4 一部分）

- F2.1 按通道展示快捷指令，显示 label/command。
- F2.2 新增、编辑、删除快捷指令；删除前确认。
- F2.3 保存到配置并刷新原生底部快捷条。
- F2.4 一键发送到所属通道，显示成功/失败。
- F2.5 无快捷指令时显示空态和“新增第一条”。

### F3 板控按钮 / 脚本编辑（J4）

- F3.1 按通道分组展示板控按钮，显示 backend（commands/relay/script）和启用状态。
- F3.2 新增、编辑、删除按钮。
- F3.3 commands 模式：多行命令、每条间隔、执行位置（channel/host）。
- F3.4 script 模式：脚本正文编辑。
- F3.5 relay 模式：继电器设备、通道、模式、脉冲时长、极性。
- F3.6 执行按钮：异步 job，轮询结果，显示 OK/FAIL、message、output 摘要。
- F3.7 表单校验：id 唯一、label 非空、commands/script 非空、relay 必须存在。
- F3.8 保存后原生按钮条刷新。

### F4 继电器设备管理（J5）

- F4.1 设备列表：名称、端口、通道数、使能、每通道状态。
- F4.2 新增/编辑：名称、Windows 端口、通道数、波特率、使能、IO 反相、极性反相、写后延时。
- F4.3 删除：如果被板控按钮引用，返回冲突并列出引用位置；否则删除。
- F4.4 通道测试：ON、OFF、脉冲；结果和错误就地显示。
- F4.5 无设备时显示空态和“新增设备”。

### F5 文件管理（J6）

- F5.1 共享根目录浏览：面包屑、目录优先排序、文件大小/类型。
- F5.2 进入子目录、返回上级。
- F5.3 上传：选择文件，上传到当前目录；重名拒绝并提示。
- F5.4 下载：单文件下载。
- F5.5 删除文件：确认后删除；目录删除需显式确认。
- F5.6 新建目录。
- F5.7 路径安全：禁止 `..`、绝对路径、越出共享根；服务端做 canonical 校验。
- F5.8 TFTP/NFS/共享三页均可进入同一文件管理区。

### F6 TCP 转发配置（扩展）

- F6.1 表单：启用、监听 IP、端口基数、目标端口、板卡 CIDR。
- F6.2 保存配置并 reload。
- F6.3 启动/停止服务，显示运行状态、监听数、活动数、错误。

### F7 隧道配置（J7）

- F7.1 表单：启用、HTTP 映射、SSH 映射、执行文件、服务器地址/端口、token、本地/远端端口。
- F7.2 保存配置并 reload。
- F7.3 启动/停止，显示 `on/healthy/last_error` 和 HTTP/SSH 子状态。
- F7.4 配置为空/未安装时给出明确指引，不显示原始 JSON 作为主界面。

### F8 配置与鉴权增强（J3/J8）

- F8.1 密钥字段默认掩码（`type=password`），提供显隐按钮。
- F8.2 配置数组支持删除单项，保存前确认。
- F8.3 所有新写接口要求 Bearer token；读接口保持只读。
- F8.4 统一错误结构：`{ok:false,error,code?}`。
- F8.5 写操作显示 toast/状态，不静默失败。

### F9 视觉与体验

- F9.1 统一设计 token：颜色、圆角、阴影、间距、字号。
- F9.2 侧栏品牌区、导航分组、当前项高亮、移动端两列导航。
- F9.3 卡片层级：标题、说明、主操作、次操作清晰。
- F9.4 终端卡片有工具栏、连接状态、发送区。
- F9.5 表单两列栅格，移动端单列；标签和帮助文字对齐。
- F9.6 空态、加载态、错误态、成功态统一。
- F9.7 键盘可达：焦点环、按钮 `type`、表单 label、对话框 ESC。
- F9.8 390×844 无横向溢出；桌面 1400×900 主操作首屏可见。

## 3. API 设计

### 3.1 保留接口

- `GET /health`
- `GET /status`
- `GET /log?channel=`
- `POST /write`
- `GET/POST /api/config`、`/api/config/save`
- `GET /api/actions`、`POST /action`、`GET /job`
- `GET/POST /quick`（POST 仍为发送）
- `GET/POST /api/relay`（POST 仍为 ON/OFF/脉冲）
- `GET /services`、`POST /service`
- `GET /api/tcp-forward`、`GET /api/tunnel`
- `WS /ws/log?channel=`

### 3.2 新增接口

| 方法 | 路径 | 作用 | 鉴权 |
| --- | --- | --- | --- |
| GET | `/api/actions?channel=` | 返回按钮完整配置 | 只读 |
| POST | `/api/actions` | 新增/更新按钮 | Bearer |
| POST | `/api/actions/delete` | 删除按钮 | Bearer |
| GET | `/api/quick/commands?channel=` | 返回快捷指令及索引 | 只读 |
| POST | `/api/quick/commands` | 新增/更新/删除快捷指令 | Bearer |
| GET | `/api/relay/devices` | 返回继电器配置 + 状态 | 只读 |
| POST | `/api/relay/devices` | 新增/更新设备 | Bearer |
| POST | `/api/relay/devices/delete` | 删除设备（引用冲突返回 409） | Bearer |
| GET | `/api/files?path=` | 列出共享根下目录 | 只读 |
| POST | `/api/files/upload?path=&name=` | 原始二进制上传 | Bearer |
| GET | `/api/files/download?path=` | 下载文件 | 只读 |
| POST | `/api/files/delete` | 删除文件/目录 | Bearer |
| POST | `/api/files/mkdir` | 新建目录 | Bearer |
| GET/POST | `/api/tcp-forward/config` | TCP 转发配置读写 | 读公开 / 写 Bearer |
| GET/POST | `/api/tunnel/config` | 隧道配置读写 | 读公开 / 写 Bearer |

### 3.3 配置写入策略

- 所有新写接口不直接改内存 `cfg_`，而是：读取当前配置 JSON → 校验 → 修改对应段 → `core->save_config_json()` → `PostMessage(kMsgWebReload)`。
- 由 UI 线程完成 `reload_config()` 和 `services_->apply_config()`，避免 Web 线程与 UI 线程同时改配置。
- 写接口返回 `{ok:true,reload:"posted"}`；前端等待 400–800ms 后重新读取状态。
- 失败时返回 400/409，并给出可读中文错误。

## 4. 视觉规范

### 4.1 设计方向

- 关键词：安静、专业、工程感、清晰、克制。
- 主色：深青绿 `#0b7a70`；强调色只用于主操作和在线状态。
- 背景：暖灰绿 `#f4f7f6`；卡片 `#ffffff`；侧栏 `#0b252b`。
- 字体：Segoe UI / Microsoft YaHei；正文 15px；辅助 12–13px。
- 圆角：卡片 16px；按钮 10px；输入 9px。
- 阴影：一层柔和环境阴影，不使用大面积高饱和渐变。
- 动画：120–180ms，仅 hover/focus/toast；尊重 `prefers-reduced-motion`。

### 4.2 布局

- 桌面：248px 侧栏 + 主内容；主内容最大宽度 1440px。
- 仪表盘：通道栏 280px + 终端自适应；窄屏上下堆叠。
- 表单：两列栅格；短字段并排，长文本/脚本占整行。
- 列表：桌面表格/卡片，移动端卡片化，不横向滚动。
- 工具栏：主操作在左，刷新/次要操作在右或次行。

### 4.3 组件

- `.card`：标题、说明、内容、底部操作。
- `.chip`：服务状态、backend、通道类型。
- `.status-dot`：在线/离线/忙碌。
- `.btn`：primary / ghost / danger / icon。
- `.field`：label + help + input + error。
- `.empty`：图标 + 标题 + 说明 + 主操作。
- `.toast`：成功/失败，3 秒自动消失。
- `.editor`：可折叠编辑区，保存/取消固定底部。

## 5. 分阶段实施计划

### Phase 0：计划、设计系统、测试基线

- 产出本文件。
- 建立 Web 验收清单：`docs/WEB-FEATURE-CHECKLIST.md`。
- 备份当前配置，准备测试配置与测试数据。
- 更新审计脚本以覆盖新页面和新功能。

验收：计划文档评审通过；测试脚本可在 18081 启动；配置可恢复。

### Phase 1：仪表盘 J2 + 全局视觉

- 重写 CSS 设计 token、侧栏、header、卡片、按钮、表单、空态。
- 仪表盘通道栏、在线灯、服务徽章、状态轮询。
- 保持终端发送/清屏/WS。
- 移动端布局。

验收：5 通道可见、在线灯 5、服务徽章 7、无横向溢出、WS 仍为 BINARY、发送 tx 增长。

### Phase 2：快捷指令与板控按钮 J4

- 新 API：`/api/quick/commands`、`/api/actions` POST/delete。
- `button-script` 页快捷指令 CRUD + 发送。
- `actions` 页按钮 CRUD + commands/script/relay 表单 + 测试 job。
- UI 表单校验、空态、toast。

验收：新增/编辑/删除快捷指令和按钮各一次；commands/script 测试返回 job 结果；原生按钮条刷新。

### Phase 3：继电器 J5

- 新 API：`/api/relay/devices` GET/POST/delete。
- 设备表单、删除引用冲突、通道测试。
- 状态轮询和错误展示。

验收：空态新增设备；编辑端口/通道；删除；ON/OFF/脉冲测试；引用冲突提示。

### Phase 4：文件管理 J6

- 新 API：`/api/files` 列表/上传/下载/删除/新建目录。
- 共享根 canonical 路径校验。
- `share` 页完整文件管理器；TFTP/NFS 页入口。
- 上传重名拒绝、删除确认、移动端卡片化。

验收：列出文件；进入子目录；上传小文件；下载；重名 409；删除；新建目录；`..` 越界 400。

### Phase 5：TCP / 隧道 J7

- 新 API：`/api/tcp-forward/config`、`/api/tunnel/config`。
- 表单、保存、启动/停止、状态卡。
- 与 `/service` 联动。

验收：保存配置后 reload；启动/停止状态变化；错误可见；无原始 JSON 主界面。

### Phase 6：回归与视觉验收

- 全量 Playwright 脚本：10 页 + 新功能 + 鉴权 + 移动端。
- 截图：桌面仪表盘、按钮编辑、继电器、文件管理、隧道、移动端。
- 评分表：功能 60 + UX 25 + 稳定性 10 + 安全 5；目标 ≥ 90，无 P0/P1。
- 更新 Web 审计报告与功能清单。

## 6. 验收评分（100 分）

| 维度 | 分值 | 通过标准 |
| --- | --- | --- |
| 页面可用性 | 10 | 10 页 200、标题、导航、无 JS 错误 |
| 鉴权 | 10 | 无 token 写接口全 401，读接口只读 |
| J1 终端/WS | 10 | BINARY 帧、发送、清屏、5 通道 |
| J2 仪表盘 | 10 | 侧栏、在线灯、7 个服务徽章 |
| J3 配置 | 10 | 140 叶子、保存回读、密钥掩码 |
| J4 按钮/快捷 | 15 | 双 CRUD、commands/script 测试 |
| J5 继电器 | 10 | CRUD、引用冲突、通道测试 |
| J6 文件 | 10 | 列表/上传/下载/删除/目录/越界 |
| J7 隧道/TCP | 10 | 配置保存、启停、状态 |
| UX/移动端 | 5 | 390×844 无溢出、空态/错误态、可读性 |
| **合计** | **100** | **≥90 且无 P0/P1** |

## 7. 风险与约束

- 配置写入必须经 UI 线程 reload，避免 Web 线程直接改 `AppCore::cfg_`。
- 文件接口必须做共享根路径校验，禁止目录穿越。
- 继电器没有真实硬件时，UI/CRUD/引用冲突可测，真实 ON/OFF 只能做可执行性验证。
- 隧道依赖外部 eadk/frpc，未安装时只能验证配置保存和错误提示。
- 上传大文件受 mongoose 请求体限制；首版限制 16 MB，后续可改流式。
- 保持原有 J1/J3/J8 行为，避免为了新功能破坏鉴权与 WS。

## 8. 实施状态（2026-11-23）

Phase 0–6 已全部完成：

- 58 项 API 自动化全部通过。
- 84 项浏览器自动化全部通过；10 页无 JS 错误、无 404/5xx。
- 桌面 1440×960、移动端 390×844 截图已完成。
- 测试配置已恢复基线。

详细实现与证据见 `docs/WEB-COMPLETE-REPORT.md`。
