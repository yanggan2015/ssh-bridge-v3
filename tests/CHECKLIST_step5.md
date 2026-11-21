# S5 验收清单 — AI 可控桥接（每个控件可读可操作）

闸门脚本：`tests\ui_bridge_check.ps1`（`pwsh -NoProfile -File tests\ui_bridge_check.ps1`，退出码 0 = 全绿）。
默认 41 项；`-SkipBuild` 复用当前 exe。**不加 `-SkipBuild` 时会先 `make clean` 再全量构建**，
因为 Makefile 没有头文件依赖跟踪，改了结构体后旧 `.o` 会用旧内存布局，表现为启动即 `0xC0000005`。

## 断言口径

`/action` 的 `ok:true` 只代表动作已送达 UI 线程；**每一项都必须紧接 `/state` 或 `/control` 复核业务真值**。
反向用例（agent 关闭 / token）单独起进程验证，避免和正向用例互相污染。

## 覆盖矩阵（41 项）

| 组 | 编号 | 断言 |
| --- | --- | --- |
| 协议 | T5-1 | `/ping` 协议 1.1、service=win_ui_bridge |
| 协议 | T5-2 | `/capabilities` 声明 11 个 P0 动作 |
| 控件树 | T5-3 | 无 `auto/` 兜底 id（全部显式登记） |
| 控件树 | T5-4 | id 唯一无重复 |
| 控件树 | T5-5 | 基线 34 个 id 全部在册 |
| 控件树 | T5-6 | 每个控件至少 1 个动作且都在白名单 |
| 关于窗 | T5-7 | `menu.help.about` 打开非模态窗，`about.text`/`btn.about.ok` 在册 |
| 关于窗 | T5-8 | `about.text` 可读、`btn.about.ok` 可关（state.about_open 回落 false） |
| 单控件 | T5-9 | `/control?id=edit.search` 快照 |
| 单控件 | T5-10 | 不存在 id 返回 404 |
| 搜索框 | T5-11~13 | set_text→get_text 回读、UI 搜索态同步、clear 清空 |
| 通道树 | T5-14 | `tree.channels` select 后 `/control.selected` == 0 |
| 通道树 | T5-15 | `tree.channels` get_text 可读 |
| 按钮 | T5-16 | `btn.refresh` 后 `/state.ts` 前进 |
| 按钮 | T5-17 | `btn.clear` 后缓冲 bytes == 0 |
| 终端 | T5-18 | `term.input` 输入并回车，回显进 tail |
| 终端 | T5-19 | `term.send` 只发不回车，tail 含原文 |
| 终端 | T5-20 | 灌 40 行后 `term.search` found=1 且 search_hit=true |
| 终端 | T5-21 | `term.scroll` 绝对定位：offset==4、value==4、min/max 可读 |
| 分屏 | T5-22~24 | `term.split2/4/1` → state.split 与 panes 数量一致 |
| 终端 | T5-25 | `term.focus` 可执行 |
| 终端 | T5-26 | `term.copy/paste/reload` 均可执行 |
| 终端 | T5-27 | `term.clear` 清空缓冲 |
| 菜单 | T5-28~29 | `menu.view.split2/split1` 走 WM_COMMAND 生效 |
| 菜单 | T5-30 | `menu.view.clear` 清屏 |
| 菜单 | T5-31 | `menu.view.refresh/reconnect` 后通道 ready |
| 菜单 | T5-32 | `menu.view.copy/paste/reload` 可执行 |
| 显隐 | T5-33 | 隐藏/显示经 `state.visible` 双向复核（含托盘两条命令） |
| 按钮 | T5-34 | `btn.reconnect` 后通道 ready |
| 日志 | T5-35~36 | `/logs` 有记录、`since` 增量游标之后为空 |
| 负向 | T5-37 | 未知 id 404、未实现动作 400、控件不支持的动作 400 |
| 扫描 | T5-38 | 全部控件逐个驱动成功（跳过退出/隐藏 4 项与已测关于窗） |
| 生命周期 | T5-39 | 进程退出后端口随之关闭 |
| 反向 | T5-40 | `agent.enable=false`：不监听、`/ping` 不通 |
| 反向 | T5-41 | token 开启：缺头 401、带头 200、`/action` 缺头 401 |

> 注：T5-41 与前一条不是合并项；脚本末尾会先用 401 确认端口已起，再验证带头 200 与 `/action` 401。
> 异常兜底：脚本 try 块抛异常时记 `T5-ERR` 并退出 1，`exit` 在 finally 里不会再吞异常报假绿。

## 已知边界

- 文本输入走真实 Edit 控件（`WM_SETTEXT` 会补 `EN_CHANGE`），不要用跨进程 `SetWindowTextW`。
- 搜索要有“回看行”才谈得上命中：回显端点按 CRLF 分行，单行输入会不断覆盖当前行。
- 关于窗口必须是**非模态**：模态 MessageBox 会阻塞 UI 线程，`/action` 在 8s 后 504。
- 虚拟滑条的 `value/min/max` 不是自动来的，需要 `ui_bridge_set_virtual_slider` 补；否则只能写不能读。
