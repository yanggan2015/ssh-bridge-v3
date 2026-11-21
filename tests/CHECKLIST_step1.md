# 步骤 1 · 测试清单（配置 / 环形日志 / 串口桥）

规则：全部 PASS 才允许进入步骤 2。自动化：`tests\step1_check.ps1`。
无板环境下，串口回环用桥内置的 `loop:` 端点（走同一条 RX 泵 / writer / ring 路径）；
真实 COM 只做打开与占用检测，不写数据。

| 编号 | 测试功能 | 判定标准 | 方式 | 结果 |
|---|---|---|---|---|
| T1-1 | 配置往返一致 | 加载值与样例逐字段相等；保存后重载不变；`patch/delete` 只动目标通道 | 自动 | PASS |
| T1-2 | 1 MiB 无丢字节 | 追加 1 MiB 后 `total` 精确；落后读者只拿到最后 cap 字节且 `dropped=true` | 自动 | PASS |
| T1-3 | 半满 / 回绕后 `get_since` 连续 | 6 轮追加跨过容量边界，增量与期望逐字节一致 | 自动 | PASS |
| T1-4 | clear 后 epoch 变化且游标被夹紧 | `epoch+1`、`total=0`；旧游标不报错，新数据仍可读 | 自动 | PASS |
| T1-5 | 通道回环收发一致 | 写入的字节从 RX 回到 ring；stop 后 `running=false`，重启后仍可收发 | 自动（`loop:` 端点） | PASS |
| T1-6 | 落盘与 ring 逐行一致 | 日志文件内容与写进 ring 的字节完全相同（含 CRLF） | 自动 | PASS |
| T1-7 | 端口被占用时错误明确 | 同一真实 COM 二次打开失败，`last_error` 含 `port busy` | 自动 | PASS |
| T1-8 | 退出无残留线程 | 自检进程退出码 0，退出后无同名进程；bridge 反复 start/stop 无死锁 | 自动 | PASS |
| G-0 | 形态 | 仍是单个 `ssh_bridge3.exe`；S1 不监听任何端口（界面直读内存） | 自动 | PASS |
| G-1 | 编译 | `-Wall -Wextra` 下 error=0 且 warning=0 | 自动 | PASS |
| G-2 | 启动 | 窗口启动 5 秒后仍存活，标题含 `SSH Bridge 3.0` | 自动 | PASS |
| G-3 | 关闭 | `WM_CLOSE` 后进程退出，退出码 0 | 自动 | PASS |
| G-4 | 残留 | 退出后无残留进程；`netstat` 无本进程监听端口 | 自动 | PASS |
| G-5 | 崩溃 | 全程退出码不是 `0xC0000005` 等异常码，无错误对话框 | 自动 | PASS |
| G-6 | 代码整理 | `scripts\quality_gate.ps1` 全 PASS | 自动 | PASS |
| G-7 | 提交 | 工作区干净，提交信息含 `step 1` 与闸门结论 | 提交后核 | PASS |

## 本步范围与排除

做：`util`（编码/路径/时间）、`logbuf`（ring/时间戳/CR-hold/落盘 flusher）、
`config`（路径查找/读写/patch/delete/三级密码）、`serial_bridge`（DCB/overlap 读泵/写队列/重连/占用错误）、
`selftest`（无窗口自检）。

不做：窗口功能（S3）、终端渲染（S4）、SSH（S2）、任何网络监听（S8+）。

## 判定备注

- T1-5 的“串口”在本机没有回环接线（COM3/COM4 是同一块板上的两个独立 CH340 口），
  因此自动化用 `loop:` 端点验证同一套 RX 泵 / writer / ring；接上回环线后可用
  `--selftest serial COM3 COM4` 复验真实端到端。
- T1-7 只用真实 COM 做“二次打开失败”检测，不写任何字节，不干扰板上终端。
