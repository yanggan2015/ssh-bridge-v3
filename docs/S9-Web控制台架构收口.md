# S9 Web 控制台架构收口（单队列 / 页面拆分）

- 日期：2026-11-22
- 版本：`3.0-s13-r13`
- 范围：`src/services/web/web_console.cpp`、`src/services/web/web_pages.{h,cpp}`、`tests/web_job_queue_check.ps1`

## 1. 本次改了什么

### 1.1 板控动作改为单后台队列

改前：每次 `POST /action` 都通过 `std::async(std::launch::async, ...)` 起一个线程，多个动作可能同时打开串口、同时抢继电器或同一通道，`/job` 只负责轮询结果。

改后：

- `WebConsole::Impl` 增加一个常驻 worker 线程、`std::deque<ActionTask>`、`condition_variable`。
- `spawn_action()` 只做参数快照、创建 job、入队；队列上限 64，满时返回 HTTP 429。
- worker 串行执行 `run_board_action()`，结果写回 `Job`。
- worker 写完后调用 `mg_wakeup(mgr, listener_id, "job", 3)`，poll 线程收到 `MG_EV_WAKEUP` 后清理已完成的旧 job。
- `GET /job?id=` 协议保持不变，前端仍轮询 `{id,done,ok,message,output}`。
- `stop()` 先置停止位、唤醒队列、join poll/worker，再释放 mongoose，避免退出时动作线程继续碰已释放对象。

### 1.2 页面资产拆文件

- 新增 `web_pages.h` / `web_pages.cpp`：CSS、公共 JS、`page_body()`、`page_script()`。
- `web_console.cpp` 只保留 HTTP/WS、鉴权、状态、队列、job 和页面外壳 `page_shell()`。
- 页面文案和 10 页路由保持不变；`Makefile` 增加 `web_pages.cpp`。

## 2. 验收

### 2.1 队列时序

`tests/web_job_queue_check.ps1` 用 loop1 的两个 Lua 动作，各 `sleep(1.2)` 秒，连续提交：

```text
WJ-1  Web 服务启动                           PASS pid=27464
WJ-2  两次 /action 均受理                     PASS job1=1 job2=2
WJ-3  第 1 个动作已完成                         PASS done=True ok=True
WJ-4  第 2 个动作仍在排队                        PASS done=False
WJ-5  第 2 个动作随后完成                        PASS done=True ok=True
WJ-6  退出后无残留进程                           PASS left=0
RESULT: ALL PASS
```

判定口径：1.6 秒时第 2 个 job 必须仍未完成；若退回并发 `std::async`，它会在约 1.2 秒完成，WJ-4 会失败。

### 2.2 S9 原有能力回归

- `python output\webtest\s9_accept.py`：T9-1 10 页、T9-9 静态 304、T9-2 状态、T9-4/7 鉴权、配置表单热重载、`/write` + `/log`、T9-3 WS BINARY、T9-8 三 WS 不串、`/help` 全 PASS。
- `node output\webtest\pw_pages_test.js`：10 页移动端 `overflow=false`、配置表单两个 Tab、桌面截图全 PASS。
- `tests\step0_check.ps1 -SkipBuild`：ALL PASS（构建、整理、文档检查）。

### 2.3 打包与升级

- r13 zip：4,217,680 B，SHA-256 `86718DE1DE48D82DF117132C6E4ADC91FBB8C9DD6E5EBCC22B32C3348091601D`
- exe：SHA-256 `5797513875B70E7D491AA12EE8C0E606D28AB8CA8176C8FE75CF7035DD291894`
- 独立包 `output\s13-dist-test3` 冒烟 9/9 PASS。
- 本地 r12 → r13 升级：旧 pid 27220 被停止，新 pid 30676 自动启动，`VERSION.txt=3.0-s13-r13`，安装后 exe SHA 与清单一致。

## 3. 边界

- 队列是“整台 Web 控制台一个”，不是“每通道一个”；同一时刻只执行一个板控动作，符合串口/继电器实际独占语义。
- `/action` 仍是异步 202 + `/job` 轮询，没有改成 WebSocket 推送；这样兼容原有 10 页前端和脚本。
- 动作队列满（64）返回 429；正常人工点按不会触发。
- 本改动不改任何配置字段，不触碰用户配置写入路径。
