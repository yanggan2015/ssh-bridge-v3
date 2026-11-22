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
| `docs/架构总览.md` | 三层结构、RX 单写多读数据面、进程边界、端口、文档地图 |
| `docs/详细设计.md` | 每一步的模块、接口签名、行为细则、验收编号 |
| `docs/阶梯架构图.md` | 14 张阶梯图，相邻两张只差颜色（橙=本步新增） |
| `docs/功能清单.md` | 119 条功能（本体 68 / 扩展 45），标注来源与所属步骤 |
| `docs/每步测试闸门.md` | 每步的出口条件（清单全 PASS + 代码整理） |
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
| 扩展 | S10 | Agent 接口 :18086 | — | 能读控件树、点按钮、取状态；可单独关 |
| 扩展 | S11 | 继电器 / TFTP / NFS / 共享目录 | — | 板端 tftp 取文件、nfs mount、继电器脉冲 |
| 扩展 | S12 | TCP 转发 / 隧道 / UDP 发现（已完成） | — | 公网可映射 HTTP 与入站 SSH |
| 扩展 | S13 | 自更新 / 打包 / 全量回归（已完成） | — | 单目录 zip、可升级、S0–S12 一键回归 |

每步的测试清单与出口条件见 `docs/每步测试闸门.md`；`tests\CHECKLIST_stepN.md` 全量 PASS 才能进入下一步。

## 架构约定（防乱的硬规则）

0. **形态铁律**：desktop 是**原生 Win32 应用本体**（自绘窗口、消息循环），**不是给网页套壳**；串口桥、SSH 出站桥、环形日志跑在**同一个 desktop.exe 进程**内，界面直接读同一份内存。禁止“界面 → 本地 HTTP → 桥”的绕行；端口只对进程外的浏览器 / ssh 客户端 / Agent 开放。
1. **单向依赖**：`ui → core`，`services → core`，`core` 不反依赖任何上层；`app` 是唯一装配层，只有它可以同时碰三者。
2. **数据面只有一条路**：`Bridge` 写自己的 ring；所有读者（终端窗格、SSH 入站会话、网页 WS、Agent 快照）各自持游标读**同一份** ring，不做数据复制、不新开缓冲。
3. **服务可插拔**：每个服务实现 `IService`（`start/stop/running`），由 `app` 按配置启停；S8–S13 每步只往 `src/services/` 加一个目录，不动已有代码。
4. **扩展只准加、不准改本体**：扩展只能用本体已有的能力面（读 ring、写队列、按钮 id、状态快照）；要改本体接口就先停下改设计。
5. **单进程生命周期**：一个 `desktop.exe` 同时是界面与后端。窗口 X = 隐藏到托盘，服务继续；托盘"退出" = 逆序停服务、释放串口、结束进程。
6. **界面不走网络**：同进程直读 ring（先比 `notify_seq`，无新数据直接跳过），不占端口。

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

- 步骤 0：骨架已可编译运行。`src/core/bridge/bridge.h`、`src/services/service.h` 已落下接口契约（无实现）。
- 步骤 1：**已完成并验收**。`--selftest ring|config|serial|all` 自检 + `tests\step1_check.ps1` 18 项全 PASS。已有能力：配置读写（原子替换、单板 patch/delete、三级密码）、环形日志（单写多读游标、时间戳、CR-hold、可选落盘）、串口桥（overlap 读泵、写队列、重连、`loop:` 内置回环端点）。
- 步骤 2：**已完成并验收**。SSH 出站链路（libssh 静态链接）：PTY+shell、非阻塞读、保活、指数退避重连、双链路隔离；`tests\step2_check.ps1` 11 项全 PASS（自带 paramiko 测试服务器）。
- 步骤 3：**已完成并验收**。原生窗口壳（菜单/工具栏/通道树/状态栏）、托盘（X=隐藏、双击恢复、菜单退出）、单实例、退出序列停通道并释放串口；`tests\step3_check.ps1` 14 项全 PASS。
- 步骤 5：**已完成并验收（AI 可控桥接）**。`agent.enable=true` 时监听 `127.0.0.1:18086`，暴露 `/capabilities /ui-tree /control /state /action /logs`；72–73 个控件/动作（随配置）全部可读可写（含菜单、托盘、关于窗、终端输入/原始发送/无剪贴板粘贴/鼠标事件/搜索/绝对回滚/分屏），`tests\ui_bridge_check.ps1` 41 项全 PASS；关掉开关则一个端口都不监听。
- 设计文档 §9 / S8：**已完成并验收（入站 SSH 共享网关）**。`inbound_ssh.enable=true` 时单口监听 `0.0.0.0:2222`，用户名=通道名，密码按三级规则解析，空密码允许 none；入站会话读写该通道同一份 ring/cmdq，支持多会话、配置重载启停和 `/state.services.inbound`；16 项自动化矩阵覆盖 none、默认密码继承、SSH 通道自身密码、错误/未知用户、双会话和热关闭重开。详见 `docs\S8-入站SSH验收报告.md`。
- 设计文档 §10 / S9：**已完成并验收（Web 控制台）**。`web.enable=true` 时单端口同时提供 HTTP + WS；10 个页面、REST、`/ws/log` BINARY、token 鉴权、静态 304、配置页两 Tab 表单保存后热重载。板控动作走单后台队列，`MG_EV_WAKEUP` 回写完成状态，避免并发抢串口；页面资产已拆到 `web_pages.cpp`。TCP 转发/隧道页面已由 S12 接上真实状态。详见 `docs\S9-Web控制台验收报告.md`。
- 设计文档 §13 / S12：**已完成并验收（TCP 转发 / 隧道 / UDP 发现）**。`tcp_forward.enable` 按 `port_base + 板 IP 末段` 一次绑定 256 个监听口，`WSAPoll(500ms)` 统一 accept，双向 relay 不阻塞 UI；`tunnel.enable` 拉起 `eadk-tunnel.exe -c runtime.json`，HTTP 映射 18xxx、入站 SSH 映射 28xxx，进程退出自动重启；`discovery.enable` 在 UDP 18071 应答 `EADK1 q`，只返回 `enable && discoverable` 的通道。配置保存后热重载即时生效，Web `/api/tcp-forward`、`/api/tunnel`、`/api/discovery` 与 `/state.services` 同步暴露状态。详见 `docs\S12-TCP隧道发现验收报告.md`。
- 设计文档 §14 / S13：**已完成并验收（自更新 / 打包 / 全量回归）**。`scripts\package.ps1` 生成 `output\dist\ssh-bridge-v3-3.0-s13-r17-win64.zip`（含 exe、脱敏配置、README、`tools\smoke.ps1`、`tools\update.ps1`、`tools\ui_probe.py`）；`update.ps1` 校验 zip 与 exe SHA-256、白名单替换、失败回滚、更新后自动重启，安装目录只停自身实例。r12 → r13 在线升级、r13 → r14 本地升级、r14 → r15 在线升级、r15 → r16 在线升级、r15 → r16 本地升级、坏包拒绝、TFTP/共享/NFS 三开关、独立包冒烟、S0–S12 回归均已通过。r17 新增 `term.paste_text`，r18 增加 SSH PTY 尺寸联动并重新打包，r19 增加终端鼠标上报；发布页：<https://github.com/yanggan2015/ssh-bridge-v3-releases/releases/tag/v3.0-s13-r19>。详见 `docs\S13-自更新打包全量回归验收报告.md`。
- 步骤 4：**已完成并验收**。终端窗格（`SB3_TermView`）：每通道回看 5000 行、ANSI/256 色、中文与宽字符、1/2/4 分屏独立游标、选区复制/右键粘贴、Ctrl+滚轮字号、搜索框输入即定位；界面直读内存、无端口。`tests\step4_check.ps1` 19 项全 PASS（含 110k 行灌流不丢字节、隐藏不耗 CPU）。
- 终端输入/回显加固：修复 BMP 外字符被 16 位截断、满最后一列覆盖末格、真实串口 TX 正常但 RX 恒 0 三个缺陷；串口读泵改为持续 overlapped `ReadFile`。新增 `tests\terminal_echo_matrix.py`，两条真实 SSH 各 13 项、COM3 13 项、COM5 ASCII 7 项、串口回环 12 项通过；COM5 在干净提示符下 `echo 中文_OK` 复测通过；内置自测新增 `T4U-11`。`term.send` 改为原样字节发送，新增 `tests\terminal_key_matrix.py`：回环 11 项、两条 SSH 与 COM3/COM5 各 5 项控制键/方向键/Tab/退格通过，自测新增 `T4U-12`。r16 增加 bracketed paste：解析 `ESC[?2004h/l`，粘贴用 `ESC[200~…ESC[201~` 包裹，多行内容按 Enter 才执行；`T4U-13` 与 `tests\terminal_bracketed_paste.py` 覆盖。r17 增加桥动作 `term.paste_text`，AI 可直接提交文本并由应用按活动窗格 bracketed 状态包装，不依赖系统剪贴板。r18 增加 SSH PTY 尺寸联动：窗口/分屏变化发送 `change_pty_size`，重连沿用当前目标尺寸，远端 `stty size` 与窗格一致。r19 增加终端鼠标上报：解析 `?1000/1002/1003/1006`，点击/释放/滚轮/拖动编码为 SGR 或 X10 发给远端，Shift 保留本地选词；UI 桥新增 `term.mouse`，`/state` 暴露 `mouse_mode`/`mouse_sgr`，`tests\terminal_mouse_report.ps1` 在两条真实 SSH 各 16 项通过，`nano -m` 点击第 20 行输入后远端文件为 `LINE-20X`。发布前按 `tests\CHECKLIST_terminal-keys.md` 对真实端点复跑；`tests\terminal_physical_key_check.ps1` 验证 `WM_KEYDOWN` 到终端的窗口消息路径，`tests\terminal_pane_routing_check.ps1` 验证分屏按键只落到活动窗格。详见 `docs\终端回显与特殊字符测试报告.md`。

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
# 响应式验收（9 项；需应用已在 18086 运行）
pwsh -NoProfile -File tests\ui_responsive_check.ps1
# 删除通道闭环（11 项；自行起隔离实例，串口/SSH/最后一个通道/删空恢复）
pwsh -NoProfile -File tests\ui_delete_channel_check.ps1
# 打包分发（exe + 示例配置 + 使用说明 + 更新器 + ui_probe）
pwsh -NoProfile -File scripts\package.ps1 -Version 3.0-s13-r16
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
# 只看数据面
.\output\build\ssh_bridge3.exe --selftest all loop:selftest
.\output\build\ssh_bridge3.exe --selftest ssh 127.0.0.1 22 root <密码>
Get-Content output\selftest\result.txt
```

进度、决策与踩坑见 `docs\执行记录.md`。
