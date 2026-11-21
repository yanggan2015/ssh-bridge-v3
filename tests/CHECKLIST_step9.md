# S9 逐控件验收清单（断点续传 + 队列明细 + 重试）

- 构建：`output/build/ssh_bridge3.exe`
- 桥接：`http://127.0.0.1:18086`
- 判定：动作送达 + `/state` / `/ui-tree` 回读一致；只看 `ok:true` 不算通过。
- 通过标准：十维全部 ≥ 9。

## A. 布局与无遮挡

- [x] 面板打开后 28 个 `sftp.*` 控件 rect 非 0
- [x] 两两求交 0 重叠（重试按钮占第 4 个按钮位，未压其他控件）
- [x] 所有控件在 408,163–1512,877 内，无出界
- [x] 底部按钮：上传 / 下载 / 取消 / 重试 等宽，关闭靠右
- [x] 进度、队列、状态三行依次排列，互不遮挡
- [x] 双属性条同一水平线 y=628，紧贴列表下沿
- [x] 截图目视无压字、无截断（`s9-final.png`）

## B. 断点续传 · 上传

- [x] 10 MB `big.bin` 上传在 8.75% 取消，状态“已取消（断点已保留，可点‘重试’续传）”
- [x] 远端保留 `big.bin.part`，大小与取消进度一致
- [x] 点“重试”从 **8.75%**（done 917,504）继续，不回到 0
- [x] 重试完成后远端 `big.bin` 大小 10,485,760 B，`.part` 消失
- [x] 完成提示带“从 2.1 MB 续传 · 已写入 · 大小校验一致”

## C. 断点续传 · 下载

- [x] 10 MB `big.bin` 下载在 8.75% 取消，本地 `big.bin.part` 917,504 B
- [x] 点“重试”从 **8.75%**（done 917,504）继续
- [x] 重试完成后本地 10,485,760 B，MD5 与源一致，`.part` 消失
- [x] 完成提示带“从 2.1 MB 续传 · 本地 MD5 …”

## D. 断点失效保护

- [x] 上传取消后修改本地源文件（追加 1 字节），重试从 **0.625%（首个 64 KB 块）** 重新开始
- [x] 确认未复用旧 `.part` 的尾部，保护生效
- [x] 守卫条件：上传 `local_mtime < part_mtime`；下载 `part_mtime > remote_mtime`
- [x] 正常未修改场景仍能续传（上传/下载均 8.75%）

## E. 传输队列明细

- [x] 目录上传时队列显示 `上传目录：1/5 个 · f1.bin（3%）· 下一个 f2.bin · 队列 0`
- [x] `/state.transfer.plan` 含 5 个相对路径，`plan_index=1`、`plan_total=5`
- [x] 当前文件完成后 `plan_index` 递增，队列文案切到下一个
- [x] 取消后队列保留计划，`can_retry=true`
- [x] 完成后队列归 0，`can_retry` 归 false

## F. 重试按钮

- [x] 初始空闲：`sftp.retry` 禁用
- [x] 传输中：`sftp.retry` 禁用，`取消` 可用
- [x] 取消/失败后：`sftp.retry` 启用，状态提示可重试
- [x] 点击重试后按钮立即禁用，任务重新入队
- [x] 没有可重试任务时点击返回“没有可重试的传输”
- [x] `sftp.retry` 已出现在 `/ui-tree`，28 个 SFTP 控件

## G. 状态与进度复位

- [x] 成功后进度 100%，状态“上传/下载完成…”
- [x] 完成后刷新列表：进度归 0、phase 归 0、`can_retry=false`
- [x] 取消后刷新列表：进度归 0、phase 归 0，但 `can_retry` 仍为 true（允许继续重试）
- [x] `/state.transfer.busy` 在传输中 true，结束时 false
- [x] 阶段码：0 空闲 / 3 上传 / 4 下载 / 5 完成 / 6 失败 / 7 取消

## H. 远端管理回归

- [x] `sftp.mkdir` 新建成功
- [x] `sftp.rename` 重命名成功
- [x] `sftp.delete` 两步确认删除成功（本轮清理 3 个 `.part`）
- [x] 未选中时重命名/删除按钮禁用
- [x] 删除后列表刷新，名称框清空

## I. 选择与 AI 控制

- [x] `sftp.local.list select big.bin` 正确
- [x] `sftp.remote.list select big.bin.part` 正确
- [x] 不存在名称返回 404，不改动当前选中
- [x] 目录名可带 `/`、`\` 或纯名
- [x] 通道/本地路径/远端路径/名称均为可编辑下拉（能选就不输）

## J. 延时

- [x] 串口环回回显 23 ms
- [x] SSH 按键回显 63 ms
- [x] SFTP 连接 600 ms
- [x] 列目录 71 ms
- [x] 取消到“已取消” < 2 s
- [x] 关闭面板 < 0.5 s

## K. 收尾

- [x] 清理远端 `/tmp/big.bin.part`、`/tmp/guard.bin.part`、`/tmp/tree_cancel/f1.bin.part`
- [x] 清理本地 `output\sftp_test\big.bin.part`（测试残留）
- [x] 关闭 SFTP 面板，结束 `ssh_bridge3.exe`
- [x] 截图保存 `output/acceptance/s9/s9-final.png`
- [x] 十维 score 写入 `output/acceptance/s9/s9-score.json`
