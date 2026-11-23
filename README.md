# ssh-bridge v3

原生 Win32 单进程终端工作台：串口 / SSH / SFTP / 继电器 / 板控按钮 / 文件服务，
界面直读同一份环形日志；附带只监听 `127.0.0.1` 的 AI 控件桥。

- 窗口自适应：客户区 `<820 DIP` 自动收起左侧通道管理器，`>=820 DIP` 自动恢复；工具栏“收起/展开”和 `Ctrl+B` 可手动切换，最小窗口 `480 × 480 DIP`。
- 删除通道：两次确认后落盘；删除当前通道会切到第一条剩余通道，不会留下空终端。删空后保留一个空画框并提示“没有通道，点‘新建’创建一条”，新建通道后自动接回该画框。
- 底部快捷指令条：按画框当前绑定的通道各保存一套按钮；点击即向该通道发送配置命令并回车，按钮随窗口宽度自动换行，支持“＋ 添加”和右键编辑/删除。
- 左侧文件页：串口通道浏览本机共享目录（`tftp.root`，空则 `work\share`），支持目录进入、上级、刷新、双击文件用系统默认程序打开；SSH 通道提供“打开 SFTP”入口；过滤隐藏/系统项与临时垃圾文件。

- 源码仓库：<https://github.com/yanggan2015/ssh-bridge-v3>
- 二进制发布仓库：<https://github.com/yanggan2015/ssh-bridge-v3-releases>
- 发布仓库与源码仓库完全独立；发布仓库只放安装包、版本清单和更新说明，不放源码。

## 更新

应用内：**帮助 → 检查更新…** 或 **关于 → 检查更新**。

默认清单：

```text
https://raw.githubusercontent.com/yanggan2015/ssh-bridge-v3-releases/windows_x86_64/UPDATE.json
```

更新器只替换 `ssh_bridge3.exe`、`README.md`、`VERSION.txt` 和 `tools\` 下的文件，
从不修改 `ssh_bridge_config.json`、`work\`、`logs\`、`keys`。

下载优先走清单里的 raw `url`；连续失败或 zip 完整性校验不通过时，会自动切换到同一 Release 的 `github_url` 资产，再校验 SHA-256 后安装。

清单本身连续失败时，更新器会退到最新 Release 的固定名资产 `releases/latest/download/UPDATE.json`，不再因为 raw 清单一次不可用就整体失败。

发布包里的示例 SSH 通道使用文档保留地址 `192.0.2.10` / `198.51.100.10` 和 `CHANGE_ME` 占位密码，不含真实主机或凭据；使用前请改成自己的配置。

**切分口径：先造一个完整的终端工具，再往外挂扩展。** S1–S7 造出 `desktop.exe` 本体（原生 Win32 终端工具，做完即可日常使用、零端口）；S8–S13 是挂在这个工具上的扩展，每个独立开关，全关掉本体照常工作。整体框图见 `docs/架构总览.md`。

## 文档地图

| 文档 | 看什么 |
|---|---|
| `docs/README.md` | 文档总索引：当前有效 / 专项设计 / 验收标准 / 历史验收 |
| `docs/架构总览.md` | 三层结构、RX 单写多读数据面、进程边界、端口、文档地图 |
| `docs/详细设计.md` | 每一步的模块、接口签名、行为细则、验收编号 |
| `docs/阶梯架构图.md` | 14 张阶梯图，相邻两张只差颜色（橙=本步新增） |
| `docs/功能清单.md` | 141 条功能（本体 68 / 扩展 73），标注来源、所属步骤与已知缺口 |
| `docs/全功能清单与完整测试报告-20261124.md` | 桌面 69 控件、Web 12 页、Agent 74 项操作的逐项盘点与测试证据 |
| `docs/代码审计与整理报告-20261124.md` | 全量代码审计、输入校验加固、告警归零、工程整理与回归结论 |
| `docs/工程目录与产物说明.md` | 源码分层、tests/scripts/share/work 职责、output 保留与重建规则 |
| `docs/设计初衷与实现对照.md` | 每个功能组的初衷、源码落点、已修问题与未实现项（2026-11-23 审计基线） |
| `docs/每步测试闸门.md` | 每步的出口条件（清单全 PASS + 代码整理） |
| `tests/CHECKLIST_eadk_board_cli.md` | EADK 板卡 CLI 全命令测试清单（90 项契约 + 32 项真实桥） |
| `docs/编程规范.md` | 提交粒度、提交信息四段、warning 清零 |
| `docs/执行记录.md` | 时间线、关键决策、踩坑表、接手清单（**换会话先读这个**） |
| `docs/终端回显与特殊字符测试报告.md` | 真实 SSH/串口回显矩阵、Emoji 与末列换行修复证据、COM5 现状 |
| `docs/pdf-print.css` | Markdown → PDF 的打印样式（A4、标题不与图分离） |

## 实现计划（两层 14 步）

| 层 | 步骤 | 内容 | 不含 | 完成标志 |
|---|---|---|---|---|
| — | S0 | 工程骨架（已完成） | 全部业务 | `make` 出 exe，窗口、菜单、状态栏可交互 |
| 本体 | S1 | 配置 / 环形日志 / 串口桥 | 界面、SSH 出站 | 命令行自检能打开 COM 并收发，ring 与落盘一致 |
| 本体 | S2 | SSH 出站链路（libssh PTY） | 界面 | 自检能连目标主机并收发，断线可退避重连 |
| 本体 | S3 | 窗口壳 + 托盘生命周期 + 单实例 | 终端渲染 | X 到托盘、托盘退出干净、状态栏与通道树可见 |
| 本体 | S4 | 终端窗格（直读 ring、ANSI、回看、分屏）（已完成） | 文件、按钮 | 按键回显 p50 < 8ms，无端口 |
| 本体 | S5 | 通道管理 + 手术式热应用 + 自动发现 | — | 增删改通道不重启、不断其它会话 |
| 本体 | S6 | 界面偏好 + 会话操作 + 快捷键 | — | 主题/字号/快捷键持久化，自动跟随不跳 |
| 本体 | S7 | 文件与脚本（SFTP / xmodem / 按钮 / Lua） | 网页文件页 | **本体出口**：完整终端工具，可独立交付 |
| 扩展 | S8 | SSH 入站共享网关 :2222 | 网页、Agent | 外部 `ssh -p 2222 <通道名>@<主机>` 进同一终端 |
| 扩展 | S9 | HTTP 基元 + 网页端 | Agent | 浏览器可用；关掉不影响本体 |
| 扩展 | S10 | Agent 接口 :18096 | — | 能读控件树、点按钮、取状态；可单独关 |
| 扩展 | S11 | 继电器 / TFTP / NFS / 共享目录 | — | 板端 tftp 取文件、nfs mount、继电器脉冲 |
| 扩展 | S12 | TCP 转发 / 隧道 / UDP 发现（已完成） | — | 公网可映射 HTTP 与入站 SSH |
| 扩展 | S13 | 自更新 / 打包 / 全量回归（已完成） | — | 单目录 zip、可升级、S0–S12 一键回归 |

每步的测试清单与出口条件见 `docs/每步测试闸门.md`；`tests\CHECKLIST_stepN.md` 全量 PASS 才能进入下一步。

## 架构约定（防乱的硬规则）

0. **形态铁律**：desktop 是**原生 Win32 应用本体**（自绘窗口、消息循环），**不是给网页套壳**；串口桥、SSH 出站桥、环形日志跑在**同一个 desktop.exe 进程**内，界面直接读同一份内存。禁止“界面 → 本地 HTTP → 桥”的绕行；端口只对进程外的浏览器 / ssh 客户端 / Agent 开放。
1. **单向依赖**：`ui → core`，`services → core`，`core` 不反依赖任何上层；`app` 是唯一装配层，只有它可以同时碰三者。
2. **数据面只有一条路**：`Bridge` 写自己的 ring；所有读者（终端窗格、SSH 入站会话、网页 WS、Agent 快照）各自持游标读**同一份** ring，不做数据复制、不新开缓冲。当前实现以短锁完成一致性拷贝，读者仍只推进自己的游标。
3. **服务可插拔**：每个服务实现 `IService`（`start/stop/running`），由 `app` 按配置启停；S8–S13 每步只往 `src/services/` 加一个目录，不动已有代码。
4. **扩展只准加、不准改本体**：扩展只能用本体已有的能力面（读 ring、写队列、按钮 id、状态快照）；要改本体接口就先停下改设计。
5. **单进程生命周期**：一个 `desktop.exe` 同时是界面与后端。关闭行为由 `ui.close_to_tray` 决定：默认隐藏到托盘、服务继续；设为 false 时关闭即退出并停服务。托盘“退出”始终逆序停服务、释放串口、结束进程。
6. **界面不走网络**：同进程直读 ring，不占端口。

## 目录

```
src/
  app/        装配层：main、生命周期、服务编排（唯一允许跨层调用的地方）
  core/       与界面无关的领域核心：config / bridge / logbuf / util
  ui/         Win32 界面：shell（主窗/菜单/状态栏/搜索框）、terminal（ansi/scrollback/pane/input/view）、tray
  services/   扩展服务：inbound(S8) / web(S9) / agent(S10) / relay,tftp,nfs(S11) / tcpfwd,tunnel,discovery(S12)
  third_party/ 第三方源码（mongoose 等）
docs/         设计文档（架构框图、每步的实施说明）
scripts/      构建与打包脚本
tests/        每步的验收脚本
```

## 构建

```powershell
# 环境：MSYS2 + mingw64（g++ 16）
$env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
mingw32-make -j4
# 产物：output\build\ssh_bridge3.exe
```

## 文档导出（Markdown → PDF）

`docs\` 下的 Markdown 可直接转 PDF（mermaid 图由 Chromium 渲染为矢量图）。转换用 markdown2pdf Skill 的脚本，样式固定用 `docs\pdf-print.css`：

```powershell
$sk = "$env:USERPROFILE\AppData\Local\Doubao\User Data\Profile 9\.doubao\agent_mode\workspace\.user_skills\markdown2pdf"
$py = "$sk\.venv\Scripts\python.exe"
New-Item -ItemType Directory -Force output\pdf | Out-Null
foreach ($n in '架构总览','详细设计','阶梯架构图','功能清单','每步测试闸门','编程规范','执行记录') {
    & $py "$sk\scripts\convert.py" "docs\$n.md" -o "output\pdf\$n.pdf" -c docs\pdf-print.css
}
```

`阶梯架构图.md` 每两步插一个 `<div class="pb"></div>`，PDF 里正好两图一页，便于同页对照。

注意：该 Skill 的 `requirements.txt` 钉的是 playwright 1.40，其 `greenlet==3.0.1` 在 Python 3.13 下没有轮子会编译失败；在 venv 里改装最新版 `playwright` 即可，然后 `python -m playwright install chromium`。

## 当前状态

- **Web 可选 token（2026-11-24）**：新增 `web.require_token`，默认不启用 token，写接口开放；启用后恢复 `Authorization: Bearer <web.token>` 校验。Web 顶部“安全”面板可勾选启用并保存，`GET/POST /api/web/auth` 提供状态与切换，`/status` 增加 `token_required`/`token_set`。干净重建 **0 warning / 0 error**；Token 专项 **8/8**、Web 功能 **162/162**、Web 下拉 **201/201**、桌面 **69/69**、桌面下拉 **139/139**、HTTP 加固 **8/8**。当前二进制 `13,564,962 B`，SHA-256 `69E6B8D176912FD248C76A423B5BFF7A7202A83DD63A03B8D7DF2A3A646ADA17`；详见 `docs/Web-可选Token认证-20261124.md`。
- **早前基线（2026-11-24 下午）**：分支 `feature/powershell-telnet-20261124`；本轮完成全量代码审计与输入加固，业务代码加严告警 **70 → 0**，`make clean` 后干净重建 **0 warning / 0 error**，质量闸门 **145 文件 ALL PASS**；19 套件 **19/19、1093/1093、bad=0**，Web 12 页逐页功能 **162/162**，新增 HTTP 输入加固 **8/8**。当时二进制 `13,549,928 B`，SHA-256 `5D95119BA1BF28AE839ADAF56DDC48B7C932FBEC32606F1A75CB24E027628D8A`。详见 `docs/代码审计与整理报告-20261124.md`。
- **审计修复**：`POST /service` 对非法 JSON、缺失/非布尔 `on`、空 `name` 返回 400，不再静默停服务；NFS 别名增加绝对路径与字符白名单，Linux `mount --bind`/`umount` 改用安全单引号转义；Web NFS 配置保存阶段拒绝非法别名；修复 16 处 shadow 与 1 处 cast-qual 告警。
- **完整盘点（2026-11-24）**：桌面 69 个可见控件、Web 12 页、Agent `/api/help` 74 项操作均已逐项盘点，141 条功能台账为“完整 126 / 部分 3 / 未实现 12”；证据与明细见 `docs/全功能清单与完整测试报告-20261124.md`。
- **本体**：原生 Win32 单进程终端工作台，串口/SSH 出站、共享 ring、ANSI 终端、1/2/4 分屏、SFTP、板控按钮、Lua/DSL、快捷指令、托盘与单实例均已落地。
- **网页端**：单端口 18081，HTTP + `/ws/log` BINARY；当前 12 页（仪表盘、通道、动作、按钮脚本、继电器、TFTP/NFS/共享、TCP 转发、隧道、Agent、配置）。仪表盘与桌面 `TerminalView` 采用完全一致的输入方式：**没有独立输入框**，键入直接落在 xterm 输出区当前光标处，字符按键 8ms 有序合批后立即 `/write` 并同区回显；方向键/Tab/退格/Home/End/Ctrl 键按桌面 `input.cpp` 的字节原样下发，Enter 按通道 `kind` + `line_ending` 发送（SSH 默认单 CR，串口按 cr/lf/crlf/none，loop-chars 为 CRLF）。桌面端不显示键条，移动端 390×844 才出现底部 16 键辅助条，它不是输入框。页面加载 120ms 自动聚焦 xterm，切通道 80ms 再聚焦；桌面 1440×960 实测 xterm 底边距会话框底边 **1px**，终端占视口 **66.3%**；移动端终端占 **69.6%**、xterm **41×38**；全站最小字号 **13px**，通道下拉旁常驻 **RX** 计数；CDN 不可用时回退到 ANSI/CR/退格净化后的 `<pre>`。
- **Agent**：18081 是唯一设备控制面；`/api/help` 当前公布 **74** 项设备操作，Tunnel、`/service` 与完整配置读写已移出 Agent 发现面（M7/Q18 边界）；配置页仍由 token 管理面直接访问；18096 仅本机 UI 调试，不参与板控。
- **板卡 CLI**：`GET /share/tools/eadk_board.py` 与 `GET /share/tools/eadk_board.md` 直接由 18081 分发（运行期文件在 `work\share\tools`）。CLI 统一按 v3 的 `channel` 键工作，自动把旧版 `board/link`、`/status`、`/actions`、`/log` 形状适配到 `/agent/state`、`/actions?channel=`、`/log?channel=`；写接口读取 `EADK_TOKEN` / `--token`（即配置里的 `web.token`）。**全部子命令已纳入测试**：`tests/eadk_board_full_check.py` 用假 v3 桥覆盖 90 项契约，`tests/eadk_cli_check.ps1` 在真实桥覆盖 32 项冒烟，逐项清单见 `tests/CHECKLIST_eadk_board_cli.md`；安装包同时带 `share\tools` 与 `share\skills`。
- **已知缺口**：C6 串口自动发现、C9 自动跟随、D4 XMODEM/YMODEM、D5/D6 上传到板目录/common、F1 主题、F3 快捷键冲突检测、F4 多窗口、G5 崩溃日志、H6 开机自启、L7 双网段、N5 180 天 API 门禁，以及 P7 全部；A10/L6/M8 为部分实现。逐项证据与修复记录见 `docs/设计初衷与实现对照.md`。
- **仍需硬件复核**：真实 COM5/COM3 电气链路、断网级 WS 重连、NFS/入站 SSH/TCP/隧道跨服务重复启停矩阵；EADK `to-board` 的 `scp-pull/scp-push` 阶梯需真实板侧 sshd/密钥。

```powershell
# S1 验收（构建 + 8 条自检 + 窗口/进程/整理/git 共 18 项）
pwsh -NoProfile -File tests\step1_check.ps1          # 加 -SkipBuild 跳过编译
# S2 验收（起测试服务器 → 连接/回显/拔网/重连/双链路，共 11 项）
pwsh -NoProfile -File tests\step2_check.ps1          # 需要 python + paramiko
# S3 验收（建窗/通道树/状态栏/隐藏/恢复/二启/退出与串口释放，共 14 项）
pwsh -NoProfile -File tests\step3_check.ps1          # 会写 output\s3run 下的测试配置
# S4 验收（终端窗格/灌流/ANSI/中文/分屏/搜索/隐藏省 CPU，共 19 项）
pwsh -NoProfile -File tests\step4_check.ps1          # 需 python + paramiko；-SkipBuild / -LongLoad
# S5 验收（桥接 41 项；不加 -SkipBuild 会先 make clean，避免旧 .o 布局错位）
pwsh -NoProfile -File tests\ui_bridge_check.ps1
# 响应式验收（9 项；需应用已在 18096 运行）
pwsh -NoProfile -File tests\ui_responsive_check.ps1
# 删除通道闭环（11 项；自行起隔离实例，串口/SSH/最后一个通道/删空恢复）
pwsh -NoProfile -File tests\ui_delete_channel_check.ps1
# 打包分发（exe + 示例配置 + 使用说明 + 更新器 + ui_probe）
pwsh -NoProfile -File scripts\package.ps1 -Version 3.0-s13-r19
# 独立包冒烟（解压后执行）
pwsh -NoProfile -File tools\smoke.ps1
# 本地升级验证（Manifest 可传本地 UPDATE.json）
pwsh -NoProfile -File tools\update.ps1 -Manifest <UPDATE.json路径> -InstallDir <安装目录>
# S9 验收（隔离 output\webtest，起 18082；REST/WS/T9）
python output\webtest\s9_accept.py
# S9 页面/手机宽度/配置表单（需 Playwright-core + Edge）
node output\webtest\pw_pages_test.js
# S12 验收（隔离 output\s12test，TCP 22000+/UDP 18071/公网 18123）
python output\s12test\s12_accept.py
node output\s12test\s12_pages.js
# EADK 板卡 CLI 全命令契约（假桥，90 项）
python tests\eadk_board_full_check.py
# EADK 真实桥冒烟（需 18081 + loop-chars + ssh-demo1，32 项）
pwsh -NoProfile -File tests\eadk_cli_check.ps1
# 全量一键回归（19 个套件：17 个 Node + EADK 契约 + EADK 真实桥）
pwsh -NoProfile -File output\webtest\run-full-suite.ps1
# Web 输入加固（独立端口，8 项：非法 JSON / 缺 on / 非布尔 on / NFS 别名）
pwsh -NoProfile -File tests\http_hardening_check.ps1
# Web 12 页逐页功能（162 项；需 5 通道夹具 + token web-audit-2026）
node output\webtest\web_functional_step15.js
# 质量闸门（145 个源文件）
pwsh -NoProfile -File scripts\quality_gate.ps1
# 只看数据面
.\output\build\ssh_bridge3.exe --selftest all loop:selftest
.\output\build\ssh_bridge3.exe --selftest ssh 127.0.0.1 22 root <密码>
Get-Content output\selftest\result.txt
```

进度、决策与踩坑见 `docs\执行记录.md`。
