# S9 Web 控制台验收报告

日期：2026-11-22
设计来源：`docs/详细设计.md` §10（第 462–487 行）
结论：**S9 主体已落地并通过 T9 验收**。10 个页面可打开；REST/WS 与本地通道一致；token 鉴权、BINARY WS、3 客户端通道隔离、静态 304、关闭不监听、端口占用错误、手机宽度均实测通过。配置页已做成两 Tab 表单，保存后由 UI 线程手术式重载。

## 1. 实现落点

| 文件 | 内容 |
|---|---|
| `src/services/web/web_console.h` | `WebConsole` Pimpl：`start / stop / running / port / error / token / configure / set_notify / status_json` |
| `src/services/web/web_console.cpp` | mongoose `mg_mgr` 线程、HTTP 路由、10 页面内嵌 HTML/CSS/JS、`/ws/log`、token 鉴权、job、静态 304、配置保存 |
| `src/core/config/config.h/.cpp` | `WebConfig.token`；`web.enable/port/token` 读写；`config_parse_json()`；`config_export_json()` |
| `src/app/app_core.h/.cpp` | `config_json_text()`；`save_config_json()`（校验 + 落盘，不直接改运行通道） |
| `src/ui/shell/shell.h/.cpp` | `WebConsole* web_`、`sync_web()`；启动/重载按 `web.enable` 启停；配置保存后 `kMsgWebReload` 调 `on_command(kIdmChanReload)` |
| `src/ui/ui_messages.h` | 新增 `kMsgWebReload = WM_APP + 15` |
| `src/ui/aibridge/bridge_host.cpp` | `/state` 增加 `web` 快照（on/port/readonly/error） |
| `Makefile` | 加入 `src/services/web/web_console.cpp` 与 `services/web` 构建目录 |

## 2. 路由与端口

单端口 `web.port`，默认 **18081**；HTTP 与 WS 同端口。为避免“页面 `/config`”和“REST `/config`”冲突，页面同名 API 使用 `/api/*`：

| 类型 | 路径 |
|---|---|
| 页面 | `/dashboard /config /actions /button-script /relay /tftp /nfs /share /tcp-forward /tunnel` |
| 只读 REST | `GET /status`、`GET /log?channel=`、`GET /api/config`、`GET /api/actions`、`GET /quick`、`GET /api/relay`、`GET /services`、`GET /api/tcp-forward`、`GET /api/tunnel` |
| 写 REST | `POST /write`、`POST /action`、`POST /quick`、`POST /api/relay`、`POST /service`、`POST /api/config/save` |
| WS | `/ws/log?channel=<通道名>`，服务端只发 **BINARY** 帧 |
| 静态 | `/static/app.css`、`/static/app.js`，带 ETag |
| Agent 说明 | `/help`、`/help/<通道名>` |

鉴权：`Authorization: Bearer <web.token>`。token 为空时只读；`/status`、`/log`、页面可读；所有写接口和 `/api/config` 返回 401。token 错误同样 401。

## 3. 验收环境

- 隔离目录：`output/webtest`，工作目录内独立 `ssh_bridge_config.json`，未改用户 `C:\Users\admin\.ssh-bridge\ssh_bridge_config.json`。
- 三个内置回环通道：`web1=loop:web1`、`web2=loop:web2`、`web3=loop:web3`，均 115200、`auto_open=true`。
- Web：`enable=true`，`port=18082`，`token=t9pw`。
- Agent 仅用于读取 `/state`：`enable=true`，`port=18087`。
- 构建：`output/audit/build-web-final2.log`，`EXIT=0`，`warning:` 计数 0。
- 产物：`output/build/ssh_bridge3.exe`，12,511,025 B，SHA-256 `E45D47B3E4955EF2C3285D75E76298CEE05217DE1E0F4C5C902E66392D3D6DC0`。
- 自测：`--selftest all loop:selftest` 与 `--selftest term` 均退出 0，`RESULT: ALL PASS`。

## 4. T9 验收结果

| 项 | 验收动作 | 结果 |
|---|---|---|
| T9-1 | 10 个页面逐个 GET，检查 200、`<!doctype html>`、viewport | **PASS 10/10** |
| T9-2 | `/status` 返回 web1/web2/web3；`/log` 与写入一致 | **PASS** |
| T9-3 | 写 `WS_A_1` 到 web1，从 WS 收 BINARY 帧并计时 | **PASS；1.0 ms < 50 ms（另一次 16.6 ms 也低于 50 ms）** |
| T9-4 | 错误 token / 无 token 写 `/write`、读 `/api/config` | **PASS 401** |
| T9-5 | `web.enable=false` 启动，18082 无 LISTEN；`/state.web.on=false` | **PASS** |
| T9-6 | 390×844 Edge headless 截图；页面切单列、导航两列、终端/表单不横向溢出（提示行已加 `overflow-wrap`） | **PASS（截图 `output/webtest/shots/final-mobile-dashboard.png`、`final-mobile-config.png`、`final-desktop-dashboard.png`、`mobile-tftp.png`）** |
| T9-7 | token 为空：`/status` 200、`/log` 200、`/write` 401、`/api/config` 401、`readonly=true` | **PASS** |
| T9-8 | 3 个 WS 分别绑定 web1/web2/web3；跨通道写 B/C 不出现在 web1，A 不出现在 web2 | **PASS** |
| T9-9 | `/static/app.css` 首次 200 带 ETag；`If-None-Match` 返回 304 | **PASS** |
| T9-10 | 先占住 18082 再启动；`/state.web.error` 可读 | **PASS：`Web 端口 18082 被占用或无法监听`** |

验收脚本输出见 `output/audit/s9-accept-final.txt`：

```
T9-1 PAGES 10/10 PASS
T9-9 STATIC 200+304 PASS
T9-2 STATUS 3 CHANNELS PASS
T9-4/7 WRITE+CONFIG WITHOUT/WITH WRONG TOKEN 401 PASS
CONFIG FORM SAVE + UI RELOAD PASS
WRITE/LOG PASS
T9-3 WS BINARY latency=1.0ms PASS
T9-8 3 WS CLIENTS NO CROSS PASS
/help + /help/<channel> PASS
RESULT: ALL PASS
```

## 5. 配置页

- 两个 Tab：基础（`default / web / agent / inbound_ssh / ui / boards / ssh`）与详细（全部顶层段）。
- 表单由配置 JSON 递归生成，不暴露原始 JSON 文本框；布尔用 true/false 下拉，数字用 number，长字符串用 textarea。
- 表单覆盖全部叶子：实测基础 **76** 个字段，详细 **89** 个字段，配置叶子数 **89**，完全一致。
- `保存并重载` POST `/api/config/save`：校验 JSON → `config_save()` 落盘 → `PostMessage(kMsgWebReload)`；UI 线程执行 `kIdmChanReload`。
- 实测把 `ui.scrollback_lines` 改为 5002，保存后 `/api/config` 为 5002，`/state.statusbar.note` 出现 `已重载配置（新增 0 / 删除 0 / 变更 0）`；再改回原值成功。
- 配置页 JS 覆盖测试：`output/webtest/config_form_test4.js`，输出 `basic=76 detail=89 leaves=89 CONFIG_FORM PASS`。

## 6. 已知边界

1. 页面与 REST 的 `/config`、`/actions`、`/relay`、`/tcp-forward`、`/tunnel` 使用 `/api/*` 命名，避免同路径冲突。
2. 慢动作使用 `std::async` 作业 + `/job` 轮询；尚未抽成设计里的 `job_submit` 单队列 + `MG_EV_WAKEUP` 回写。当前 poll 线程不执行动作，满足“慢操作不占 poll”的主目标。
3. `tcp-forward`、`tunnel` 页面当前只是 S12 入口占位；转发/隧道引擎本身未实现，接口返回 `running=false` / `S12 未实现`。
4. 配置保存需要 token；页面没有登录态，token 由页头输入后存 `localStorage`。
5. S9 代码仍是单文件 `web_console.cpp`，未按设计拆成 `http_core/router/rest/ws_log/webui/job` 多文件；接口与行为先落地。
6. 工作区改动仍未提交；r11 发布包不包含 S8/S9，新构建只落在 `output/build`。

## 7. 下一步

按设计文档线性顺序，S9 之后是 S10（已提前落地）、S11（已落地）和 **S12 §13 TCP 转发 / 隧道 / UDP 发现**；当前实际缺口是 S12，以及 S6 的完整界面偏好项。S9 的 `tcp-forward` / `tunnel` 页面已留出入口，可直接接 S12。

