# S6 验收清单（通道管理 + 手术式热应用）

> 每完成一步先跑本清单；十维评分见 `output/acceptance/s6/score.json`，每维 ≥9 才进下一步。
> 本清单的“证据”均为真机执行记录（桥接 127.0.0.1:18086），不是脚本跑过即算。

## 一、真机逐控件（表单）

- [x] `chan.newSerial` 打开表单：`/state.ui.dialog` 标题「新建通道」，50 个 `dlg.*` 控件、auto id=0
- [x] 串口态：device 可见、host 隐藏；默认 baud=115200、自动打开/DTR/RTS/日志=勾选
- [x] `dlg.channel.kind` 选 SSH：host/port/user/password/term/timeout/cols/rows/keepalive 可见，device/baud 等隐藏
- [x] 名称空保存：表单不关，hint 红字「通道名不能为空」
- [x] 重名 `loop1` 保存：hint「通道名已存在：loop1」
- [x] 取消：不落盘、不新增通道
- [x] 「测试」串口 loop 端点：hint「内置回环端点…」
- [x] 「测试」SSH demo 主机：hint「TCP 可连通（端口开放）」
- [x] 新建 `s6loop`(loop:ai)：HTTP 143ms、通道 73ms 出现 ready；配置写入
- [x] 编辑波特率 999→115200：121ms 生效；未受影响通道 bytes 与搜索命中不变
- [x] 改名 `s6loop`→`s6loop2`：113ms，旧名消失新名出现
- [x] 日志开关：编辑取消「日志」保存→config `log_enabled=false`；再勾回→`true`（保存 150ms，生效 93ms）
- [x] 两步删除：第一次 hint「再点一次“确认删除”…」+按钮变「确认删除」；第二次 344ms 删除
- [x] 删除后端口释放：COM3 删除后 .NET SerialPort 可再次打开
- [x] SSH 新建/编辑/删除：修复 `save_root` 不写 ssh 数组后，743ms ready，配置落盘，回显 bytes=205、search hit
- [x] 双击保存：第二次 404（窗口已销毁），只有一份通道、配置一条

## 二、布局与遮挡（D3）

- [x] 主窗：11 个可见控件两两求交，无 >0.5% 遮挡、无出界
- [x] 表单串口态：无遮挡无出界
- [x] 表单 SSH 态：无遮挡无出界
- [x] 150% DPI 下真实坐标复核（分组框按容器豁免，字段压在分组框上不算遮挡）
- [x] 表单收紧：client 552x300、分组框 162，消除大片空白

## 三、会话与窗格（S6 附带回归）

- [x] `term.pane` 滑条可切活动窗格（新增，AI 可控）
- [x] 两窗格分别绑 loop1 / s6loop2，输入后 bytes=12 / 25
- [x] `term.clear` 只清活动窗格：loop1→0，另一窗格 25 保留

## 四、稳定性

- [x] 连续 5 次新建→取消：每次 open=true→closed=false，`dlg.*` 控件数回落 0，不累积
- [x] 重启进程：6 通道全部恢复（含 s6loop2），config 的 boards/ssh 数组、log_enabled 保留
- [x] `scripts/quality_gate.ps1`：62 文件 ALL PASS（0 行尾空白/0 超长行）
- [x] 构建 0 warning；`make clean` 全量重建可复现
- [x] 测试结束关闭应用，18086 端口释放

## 五、回显延时（D6）

| 通道 | 类型 | 实测（客户端含 HTTP 往返） |
| --- | --- | --- |
| loop1 | 串口回环 | 17–19 ms |
| s6loop2 | 串口回环 | 17 ms |
| ssh-link-demo1 | SSH | 66 ms |
| ssh-link-demo2 | SSH | 66 ms |
| ztl (COM3@1.5M) | 真实串口 | 端口 open=ready，但板端当前无回显（bytes=0），不计延时 |
| ok3568 (COM5@115200) | 真实串口 | 同上 |

门槛：串口 <150ms、SSH <800ms（`docs/窗口验收标准.md` D5），全部通过。

## 六、出包

- [x] `output/dist/ssh-bridge3-3.0-s6-r1-win64.zip`
- [x] 独立解压运行：ping ok；示例 2 通道 loop1/loop2 ready
- [x] 包内新建→保存→删除 `pkgtest` 走通；主窗遮挡 PASS
- [x] 包内配置已脱敏（无真实 SSH 凭据）
