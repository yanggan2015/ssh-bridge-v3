# Web 控制台补齐功能实施报告

- 日期：2026-11-23
- 分支：`feature/web-console-complete-20261123`
- 源码：`C:\code\ssh-bridge-windows3`
- 产物：`output/build/ssh_bridge3.exe`
- 计划与功能清单：`docs/WEB-COMPLETE-PLAN.md`
- 测试脚本：`output/webtest/api-smoke.js`、`output/webtest/web-v2-audit.js`
- 测试结果：`output/ux-audit/api-smoke-result.json`、`output/ux-audit/v2-web-audit-result.json`

## 1. 结论

这次把 Web 控制台从“能看状态、能发命令”补齐为可操作的运营控制台：仪表盘、板控按钮、快捷指令、继电器、共享文件、TCP 转发、隧道和配置页都形成了前端可操作闭环，写操作统一走 Bearer token。

- 新增/重写后端接口 14 组，接入 `handle_http` 路由。
- 重写 10 个页面的视觉、信息层级、空态与移动端布局。
- API 自动化：58 项全部通过。
- 浏览器自动化：84 项全部通过；10 页无浏览器 JS 错误、无 404/5xx、无请求失败。
- 真实浏览器桌面 1440×960、移动端 390×844 验收通过；移动端无横向溢出。
- 在测试配置上完成新增/编辑/删除闭环，测试后恢复原配置基线。

综合评分：**94 / 100**。无 P0；已知 P2 为 Web 仍监听 `0.0.0.0` + HTTP，且只读接口不要求 token。

## 2. 功能清单与实现状态

| 功能 | 内容 | 状态 |
| --- | --- | --- |
| F1 仪表盘 | 通道侧栏、在线灯、TX/RX、终端 WS、发送/清屏、7 个服务徽章 | 完成 |
| F2 快捷指令 | 按通道展示、新增/编辑/删除、发送、空态 | 完成 |
| F3 板控按钮 | commands/relay/script 三类表单、CRUD、异步 job 结果 | 完成 |
| F4 继电器 | 设备 CRUD、通道状态、ON/OFF/脉冲、删除引用冲突 409 | 完成 |
| F5 文件管理 | 目录浏览/面包屑、上传、下载、删除、新建目录、路径越界拦截 | 完成 |
| F6 TCP 转发 | 启用/监听 IP/端口基数/目标端口/CIDR 表单、保存、启停、状态 | 完成 |
| F7 隧道 | HTTP/SSH 映射、服务器/Token/端口表单、保存、启停、状态 | 完成 |
| F8 配置/鉴权 | 密钥掩码/显隐、数组删除、保存确认、写接口 token | 完成 |
| F9 视觉体验 | 设计 token、卡片层级、空态、响应式、移动端紧凑导航 | 完成 |

## 3. 新增接口

| 方法 | 路径 | 作用 |
| --- | --- | --- |
| GET/POST | `/api/actions` | 读取完整按钮字段；新增/编辑 |
| POST | `/api/actions/delete` | 删除按钮 |
| GET/POST | `/api/quick/commands` | 快捷指令读取；新增/编辑/删除 |
| GET/POST | `/api/relay/devices` | 继电器设备读取；新增/编辑 |
| POST | `/api/relay/devices/delete` | 删除设备；被按钮引用时 409 + refs |
| GET | `/api/files` | 共享根目录列表 |
| POST | `/api/files/upload` | 原始二进制上传，最大 16 MB |
| GET | `/api/files/download` | 文件下载 |
| POST | `/api/files/delete` | 删除文件/空目录 |
| POST | `/api/files/mkdir` | 新建目录 |
| GET/POST | `/api/tcp-forward/config` | TCP 转发配置读写 |
| GET/POST | `/api/tunnel/config` | 隧道配置读写 |

## 4. 实现要点

### 4.1 配置写入线程模型

新接口不直接改 `AppCore::cfg_`。统一流程：

1. 读取 `core->config_json_text()`。
2. 在 JSON 上校验、修改目标段。
3. `core->save_config_json()` 落盘。
4. `PostMessage(kMsgWebReload)`。
5. UI 线程执行 `reload_config()` + `services_->apply_config()`。

这样避免 Web 线程与 UI 线程同时修改配置；前端在保存后做两段延迟刷新（1.3s / 2.4s），覆盖 UI 线程重载窗口。

### 4.2 文件安全

- `safe_share_path` 统一替换反斜杠、拒绝绝对路径与 `..`。
- canonical 校验确保路径仍在共享根内。
- 上传文件名拒绝 `/`、`\`、`.`、`..`。
- 删除共享根目录直接 400；非空目录 409。
- 下载与上传限制 64 MB / 16 MB。

### 4.3 继电器引用完整性

- 删除设备前扫描 `boards` / `ssh` 通道的 `buttons`。
- 有引用时返回 `409`，响应中列出引用按钮 ID。
- 设备改名时自动同步引用它的板控按钮。

### 4.4 视觉与移动端

- 深青绿侧栏 + 暖灰绿背景 + 白色卡片；主色 `#0b7a70`。
- 统一卡片 16px 圆角、10px 按钮、柔和阴影。
- 桌面 248px 侧栏；移动端侧栏压缩为品牌 + 横向换行导航，全部 10 页可见。
- 空态统一为图标 + 标题 + 说明 + 主操作；危险操作红色按钮 + 确认框。
- 配置搜索会把没有匹配字段的分组/数组项一起隐藏，避免空白边框。
- 布尔字段改为“开启/关闭、启用/停用、正常/反相”，不再直接暴露 true/false。

## 5. 测试证据

### 5.1 API 自动化：58/58

覆盖：

- 无 token 写接口 401；读接口 200。
- 按钮新增、字段回读、执行 job、停用后执行 409、编辑、删除。
- Lua 脚本按钮执行，job 输出包含 `AUDIT_SCRIPT_OK`。
- 快捷指令新增、索引回读、编辑、发送、删除。
- 继电器新增、状态通道、改名、删除；被按钮引用时删除 409 并返回 refs。
- 文件新建目录、上传、列表、下载、删除文件、删除空目录。
- 路径穿越 `/api/files?path=../` 400；上传名 `../evil.txt` 400。
- TCP / 隧道配置读写。

结果文件：`output/ux-audit/api-smoke-result.json`。

### 5.2 浏览器自动化：84/84

覆盖：

- 10 页标题、导航高亮、桌面/移动端加载。
- 仪表盘 5 通道、7 个服务徽章、WS 已连接、发送与清屏。
- 板控按钮编辑器、relay 字段切换、保存、确认框删除。
- 快捷指令保存/删除、继电器设备编辑取消/删除。
- 文件上传/列表/删除。
- TCP / 隧道配置加载与保存、Token 掩码/显隐。
- 配置读取、109 个字段、密钥 `type=password`、搜索、保存确认与重载。
- 390×844 六个页面无横向溢出；移动端侧栏高度 < 230px。
- 全程无浏览器 JS 错误、无 404/5xx、无请求失败。

结果文件：`output/ux-audit/v2-web-audit-result.json`。

### 5.3 截图

- `output/ux-audit/v2-desktop-dashboard.png`
- `output/ux-audit/v2-desktop-actions.png`
- `output/ux-audit/v2-desktop-quick.png`
- `output/ux-audit/v2-desktop-relay.png`
- `output/ux-audit/v2-desktop-share.png`
- `output/ux-audit/v2-desktop-tcp.png`
- `output/ux-audit/v2-desktop-tunnel.png`
- `output/ux-audit/v2-desktop-config.png`
- `output/ux-audit/v2-mobile-dashboard.png`
- `output/ux-audit/v2-mobile-relay.png`

## 6. 已知限制

1. **继电器无真实硬件**：CRUD、状态结构、引用冲突已验收；真实 COM 口 ON/OFF/脉冲只能做可执行性验证。
2. **隧道依赖外部 eadk-tunnel / frpc**：配置保存、启停入口与错误展示已完成；未安装时可验证保存和错误提示，不能验证真实外网映射。
3. **上传上限 16 MB**：mongoose 请求体一次性读取；大文件后续应改流式。
4. **Web 监听 `0.0.0.0` + HTTP**：写接口需要 token，但只读接口无 token；局域网暴露面仍在。建议下一轮增加 `web.bind` 配置或默认回环。
5. **`/api/config` 服务端仍返回明文密钥**：页面已经掩码显示；如果要防止 API 直接读密钥，需要增加“掩码读 / 原值写”的协议。
6. V4 原生 UI 审计中的 P1（目录选择器默认桌面、小窗口关键操作不可达）不在本次 Web 范围内。

## 7. 测试配置恢复

- 测试配置：`output/terminal-test/ssh_bridge_config.json`
- 基线备份：`output/ux-audit/config-before-web.json`
- 恢复后 SHA-256 应为 `2D50FDA278A11F4887F0734EA6E3669B7B59B33064640608C81AEE5443B6743A`。
- 用户真实配置 `C:\Users\admin\.ssh-bridge\ssh_bridge_config.json` 未修改。

## 8. 交付

- 最新二进制：`output/build/ssh_bridge3.exe`
- 构建目录：`output/wc-build/build/ssh_bridge3.exe`
- 旧二进制备份：`output/build/ssh_bridge3.exe.bak-20261123`
- 计划：`docs/WEB-COMPLETE-PLAN.md`
- 报告：`docs/WEB-COMPLETE-REPORT.md`
