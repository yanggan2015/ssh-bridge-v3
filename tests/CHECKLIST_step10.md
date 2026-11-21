# S10 逐控件验收清单（传输历史 + 失败详情 + 限速设置）

- 构建：`output/build/ssh_bridge3.exe`（最终版本，无 warning）
- 桥接：`http://127.0.0.1:18086`
- 判定：动作送达 + `/state` / `/ui-tree` / `/control` 回读一致；只看 `ok:true` 不算通过。
- 通过标准：十维全部 ≥ 9；控件不互相遮挡、不出界。

## A. 主面板 30 控件

- [x] `sftp.channel` 选择 `ssh-link-demo1`，连接成功
- [x] `sftp.connect` 点击后进入“已连接”，自动列目录
- [x] `sftp.local.path` 写入 `...\output\sftp_test`，刷新成功
- [x] `sftp.local.refresh` 刷新，本地 8 项
- [x] `sftp.local.up` 上级可点，返回正常
- [x] `sftp.local.list` 选中 `big.bin` / `hello.txt`，属性条同步
- [x] `sftp.local.info` 读到大小与时间
- [x] `sftp.remote.path` 写入 `/tmp`，列 7 项
- [x] `sftp.remote.path` 写入不存在目录 `/tmp/no_such_dir_xyz`，错误提示正常
- [x] `sftp.remote.refresh` 刷新正常，约 71 ms
- [x] `sftp.remote.up` 上级可点，返回正常
- [x] `sftp.remote.list` 选中 `big.bin` / `cache` / `hello.txt`
- [x] `sftp.remote.info` 读到 `644 · 911:911 · 10.0 MB · 时间`
- [x] `sftp.name` 选中回填纯名；可手输
- [x] `sftp.mkdir` 新建目录成功（回归）
- [x] `sftp.rename` 重命名成功（回归）
- [x] `sftp.delete` 两步确认删除成功；本轮清理 `big.bin.part`
- [x] `sftp.upload` 取消后重试从 5.0% 续到 7.5%
- [x] `sftp.download` `hello.txt` 17 B、`cache` 830.8 KB 均成功
- [x] `sftp.cancel` 传输中可用，取消响应正常
- [x] `sftp.retry` 取消后启用，从断点续传，重试次数 +1
- [x] `sftp.history.open` 打开历史；计数/列表/详情一致
- [x] `sftp.settings.open` 打开设置；双开自动避让
- [x] `sftp.close` 关闭后 `sftp_open=false`，终端恢复
- [x] `sftp.progress` 传输中增长，完成 100，刷新后 0
- [x] `sftp.queue` 空闲“已连接”，传输中阶段正确
- [x] `sftp.status` 与 `/state` 一致
- [x] `sftp.label.channel` 文案完整
- [x] `sftp.label.local` 文案完整
- [x] `sftp.label.remote` 文案完整
- [x] `sftp.label.name` 文案完整

> 注：上表 4 个 label 合并计为 4 个控件，加上 26 个交互控件，共 30 个 `sftp.*`。

## B. 历史窗口 5 控件

- [x] `sftp.history.summary` 显示总条数、成功/失败/取消、当前限速
- [x] `sftp.history.list` 行文本含时间/方向/文件/大小/耗时/均速/结果/重试
- [x] `sftp.history.list` 选中失败行，下方详情联动
- [x] `sftp.history.detail` 失败原因完整（`No such file` + 完整临时路径）
- [x] `sftp.history.clear` 清空后 `history_count=0`、列表 0 项
- [x] `sftp.history.close` 关闭后 `history_open=false`

> 5 个控件为 summary/list/detail/clear/close；上行 clear/close 各算一项。

## C. 设置窗口 5 控件

- [x] `sftp.settings.label.speed` 文案“限速（KB/s，0 = 不限）”
- [x] `sftp.settings.speed` 是**可编辑下拉**，可选 0/100/500/1024/5120/10240，也可手输
- [x] `sftp.settings.speed` 非法输入 `abc` 保存：窗口不关，提示“只能输入整数”
- [x] `sftp.settings.note` 显示当前值与“保存后生效”，说明不影响串口/SSH
- [x] `sftp.settings.save` 合法 100 保存：关闭窗口，状态显示已设
- [x] `sftp.settings.save` 0 保存：恢复不限速
- [x] `sftp.settings.close` 先填 100 再关闭：不落盘，仍为“不限”
- [x] 限速 100 KB/s 下载 830.8 KB：8,545 ms / 97.2 KB/s
- [x] 恢复不限速下载同文件：1,535 ms / 541.2 KB/s
- [x] 限速不影响串口回环 3 ms、SSH 回显 72–77 ms

## D. 无遮挡 / 不出界

- [x] 主面板 30 控件两两求交：0 重叠
- [x] 主面板 30 控件全部在客户区内：0 出界
- [x] 历史窗口 5 控件：0 重叠、0 出界
- [x] 设置窗口 5 控件：0 重叠、0 出界
- [x] 双窗口同开（设置→历史）：窗口重叠高度 0
- [x] 双窗口同开（历史→设置）：窗口重叠高度 0
- [x] 历史在上 `447,151–1443,671`、设置在下 `447,683–1233,1008`
- [x] 截图目视：无压字、无截断、无按钮被盖住

## E. 状态 / 可观测性

- [x] 空闲 `phase=0` 时 `percent=0`（修复过“UI 归零但 state 报 100”）
- [x] 完成 `phase=5` 时 `percent=100`
- [x] `/state.ui.sftp.history_count` 与列表条数一致
- [x] `/state.ui.sftp.history[]` 含 time/dir/name/result/detail/bytes/total/elapsed_ms/speed_bps/retries
- [x] `/state.ui.sftp.speed_limit_kbps`、`speed_limit_text` 与设置一致
- [x] `/state.ui.sftp.history_open` / `settings_open` 与实际窗口一致
- [x] 传输失败/取消不残留 `phase=5` 或 100%

## F. 收尾

- [x] 远端 `/tmp` 7 项，无 `.part`
- [x] 本地 `output\sftp_test` 无 `.part`
- [x] `sftp.close` 后 `sftp_open=false`
- [x] 关闭测试进程；`Get-Process ssh_bridge3` 计数 0
- [x] 18086 端口 `listeners=0`
- [x] 分发包可独立解压并跑冒烟（见 `output/dist`）
