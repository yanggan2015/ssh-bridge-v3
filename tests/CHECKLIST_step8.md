# S8 逐控件验收清单（属性条 + 递归目录传输）

- 构建：`output/build/ssh_bridge3.exe`
- 桥接：`http://127.0.0.1:18086`
- 判定：动作送达 + `/state` / `/ui-tree` 回读一致；仅 `ok:true` 不算通过。
- 通过标准：十维全部 ≥ 9。

## A. 布局与无遮挡

- [x] 面板打开后 27 个 `sftp.*` 控件 rect 非 0
- [x] 两两求交 0 重叠（曾 6 处：列表压名称行）
- [x] 所有控件在 408,163–1512,877 内，无出界
- [x] 左右属性条同一 y=628，紧贴列表下沿
- [x] 本地路径 `C:\code\ssh-bridge-windows3\output\sftp_test` 39 字符完整可见
- [x] 刷新/上级在标题行，不挤压路径
- [x] 目视：名称框、三个按钮、进度条、队列、状态、底部按钮无压字

## B. 属性条

- [x] 远端文件 `cache`：`文件 · 755 · 911:911 · 830.8 KB · 2026-11-16 21:49`
- [x] 远端目录 `tree_remote`：`目录 · 755 · 911:911 · — · 2026-11-21 21:29`
- [x] 远端 10 MB `big.bin`：`文件 · 644 · 911:911 · 10.0 MB · 2026-11-21 21:35`
- [x] 本地目录：`目录 · — · 2026-11-21 21:29`
- [x] 本地 10 MB 文件：`文件 · 10.0 MB · 2026-11-21 21:17`
- [x] 选中远端项名称框自动回填纯名（无尾斜杠）
- [x] 无选中时显示占位提示
- [x] `/ui-tree` 可见 `sftp.local.info` / `sftp.remote.info`

## C. 递归上传

- [x] 选中本地目录 `tree_src`，上传按钮自动走目录分支
- [x] 远端生成 `/tmp/tree_src`，保留源目录名
- [x] `a.txt`、`sub/b.bin`（2048 B）、`sub/deep/c.txt`（12 B）内容正确
- [x] 空目录 `empty/` 保留
- [x] 完成文案：`目录上传完成：tree_src · 3 个文件`
- [x] 3 文件上传 1.82 s

## D. 递归下载

- [x] 选中远端目录 `tree_remote`，下载走目录分支
- [x] 本地生成 `output\sftp_test\tree_remote`
- [x] `r1.txt` 8 B、`sub/r2.bin` 10 B、`sub/deep/r3.txt` 11 B 正确
- [x] 空目录 `empty/` 保留
- [x] 完成文案：`目录下载完成：tree_remote · 3 个文件`
- [x] 3 文件下载 2.00 s

## E. 单文件传输

- [x] 830 KB `cache` 下载 1.59 s，本地 MD5 正确
- [x] 10 MB `big.bin` 上传 12.76 s，`wc -c` = 10485760
- [x] 大文件完成文案：`已写入 · 大小校验一致`（>4 MB 不再全量回读）
- [x] 10 MB 下载完成，本地 10485760 B，本地 MD5 已显示

## F. 取消与清理

- [x] 5 文件 4 MB 目录上传 1.2 s 后取消
- [x] 状态 `已取消（临时文件已清理）`
- [x] 远端无 `*.part`
- [x] 队列归 0，按钮恢复

## G. 远端管理回归

- [x] `sftp.mkdir` 新建 `s8check` 83 ms
- [x] `sftp.rename` `s8check → s8check2` 86 ms
- [x] 未选中时重命名按钮禁用，返回 409
- [x] `sftp.delete` 两步确认，615 ms 删除，远端消失

## H. 选择与 AI 控制

- [x] `sftp.local.list select tree_src` 返回 `selected=1`（修复前误选 0）
- [x] `sftp.remote.list select tree_remote` 正确
- [x] 不存在名称返回 404 `select target not found`，不改动当前选中
- [x] 目录名可带 `/`、`\` 或纯名
- [x] 背景态 `sftp.channel/local.path/remote.path/name` 均为下拉

## I. 延时

- [x] 串口环回回显 23 ms
- [x] SSH 按键回显 63 ms
- [x] 列目录 71 ms
- [x] 新建 83 ms / 重命名 86 ms
- [x] 关闭面板 < 0.5 s

## J. 收尾

- [x] 关闭 SFTP 面板
- [x] 结束 `ssh_bridge3.exe`
- [x] 截图保存 `output/acceptance/s8/s8-panel.png`
- [x] 十维 score 写入 `output/acceptance/s8/s8-score.json`
