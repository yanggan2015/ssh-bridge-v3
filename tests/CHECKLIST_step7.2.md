# S7.2 逐控件验收清单（远端文件管理）

- 构建：`output/build/ssh_bridge3.exe`
- 桥接：`http://127.0.0.1:18086`
- 判定：每项必须“动作送达 + `/state` 回读一致”；仅 `ok:true` 不算通过。
- 通过标准：十维全部 ≥ 9。

## A. 连接与导航

- [x] `menu.file.sftp` 打开面板（F7 同路径），`sftp_open=true`
- [x] `sftp.channel` select demo2 → 连接成功
- [x] `sftp.channel` select demo1 → 重连成功，`channel=ssh-link-demo1`
- [x] `sftp.connect` 连接中按钮禁用，完成后恢复
- [x] `sftp.local.path` set_text → 生效
- [x] `sftp.local.refresh` → 列出 3 个文件
- [x] `sftp.local.up` → 上到父目录
- [x] `sftp.local.list` select big.bin → `local_sel=0`
- [x] `sftp.remote.path` set_text `/tmp` → 生效
- [x] `sftp.remote.refresh` → `/tmp` 列表（cache）
- [x] `sftp.remote.up` → `/`
- [x] `sftp.remote.list` select 目录 → 自动回填名称（无尾斜杠）

## B. 新建目录

- [x] 空名称：`先在“名称”框输入目录名`（可见 5 s）
- [x] 非法名称 `a/b`：`名称不能包含路径分隔符，也不能是 . 或 ..`
- [x] 正常新建 `demo_a`：287 ms，`已新建目录：demo_a`，列表出现 `demo_a/`
- [x] 重名新建 `cache`：`新建失败：目录可能已存在或无权限（…）`

## C. 重命名

- [x] 未选中：`先在右侧选择要重命名的文件或目录`
- [x] 选中后空新名：`先在“名称”框输入新名称`
- [x] 新旧同名：`新名称与原名相同，未执行`
- [x] 目录重命名 `demo_a → demo_b`：`已重命名：demo_a → demo_b`
- [x] 文件重命名 `hello.txt → hello2.txt`：成功，远端列表更新
- [x] 目标已存在：`重命名失败：目标可能已存在或无权限（…）`

## D. 删除（两步确认）

- [x] 第一次点击：按钮变“确认删除”，状态“再次点击…即删除：x（文件|目录）”，`delete_armed=true`
- [x] 3.2 s 未确认：自动复位为“删除”，`delete_armed=false`
- [x] 第二次点击（3 s 内）：删除文件 `hello2.txt` 成功
- [x] 删除空目录 `demo_b` 成功：`已删除：demo_b（目录）`
- [x] 删除非空目录：`删除失败：目录可能非空或无权限（…）`，目录保留
- [x] 删除成功后名称框清空

## E. 传输回归（S7.1 能力不回归）

- [x] 28 B 上传：MD5 一致
- [x] 10 MB 上传：进度连续（0→10.6%+）
- [x] 10.6% 取消：阶段“已取消”，远端无 `.part`
- [x] 下载新文件：本地 MD5 一致
- [x] 覆盖下载同名文件：成功（MoveFileEx 替换）
- [x] 面板关闭：158 ms，`sftp_open=false`
- [x] 重开面板：自动重连，25 个控件重新可驱动

## F. 布局与可观测

- [x] 25 个 sftp 控件两两求交：0 重叠（阈值 >2 px）
- [x] 控件边界 408,163–1512,877 在面板客户区内，无出界
- [x] 上传中状态“正在上传…（进度见上方进度条）”，队列显示百分比
- [x] 完成/失败/取消后状态保留结果，不被列表刷新覆盖
- [x] `sftp.progress` 0–1000 与 percent 一致
- [x] `/state.ui.sftp` 字段：connected、remote_path、remote_items、remote_sel、name_text、delete_armed、transfer.phase_code/percent/note、status

## G. 延时

- [x] SSH 回显 61–78 ms
- [x] 新建/重命名/删除 288–372 ms（含刷新）
- [x] 列目录 288 ms
- [x] 面板关闭 158 ms

## H. 10 维评分

| 维度 | 分数 |
| --- | --- |
| 用户便利性 | 9.3 |
| 布局合理性 | 9.2 |
| 功能合理性 | 9.4 |
| 控件功能正确性 | 9.5 |
| 视觉美观与终端空间最大化 | 9.1 |
| 操作流畅无卡顿 | 9.4 |
| 状态反馈与可观测性 | 9.3 |
| 无遮挡无出界 | 9.8 |
| AI 可控制性 | 9.8 |
| 低延时 | 9.2 |

结论：全部 ≥ 9，S7.2 通过。
