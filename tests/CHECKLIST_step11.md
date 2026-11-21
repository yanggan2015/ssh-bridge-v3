# S11 逐控件验收清单（继电器 / TFTP / NFS / 共享目录）

- 构建：`output/build/ssh_bridge3.exe`（最终版本，`--selftest` 退出码 0）
- 桥接：`http://127.0.0.1:18086`
- 判定：动作送达 + `/state` / `/ui-tree` / 端口 / 文件回读一致；只看 `ok:true` 不算通过。
- 通过标准：十维全部 ≥ 9；控件不互相遮挡、不出界。

## A. 继电器（15 项）

- [x] `svc.relay.device` 选择 `relay1`
- [x] `svc.relay.port` 选择 COM3，保存；再选择 COM5，保存
- [x] `svc.relay.baud` 选择 `9600`
- [x] `svc.relay.refresh` 点击，回执 `✓ 串口列表已刷新`
- [x] `svc.relay.enable` 取消勾选，CH1 返回“继电器设备未启用”；勾回恢复
- [x] `svc.relay.save` 点击，回执 `✓ 继电器配置已保存`
- [x] `svc.relay.ch1` 点击吸合，`on=true`；再点释放
- [x] `svc.relay.ch2` 点击吸合，`on=true`；再点释放
- [x] `svc.relay.hold` 选择 `1 秒` / `0.5 秒`
- [x] `svc.relay.pulse1` 点击，300 ms busy=true，约 2 s 后 false
- [x] `svc.relay.pulse2` 点击，回执“脉冲已下发”，CH2 `last_ms` 更新
- [x] `svc.relay.inverted` 点击两次，勾选状态往返
- [x] `svc.relay.info` 读取 CH1/CH2 状态，随操作更新
- [x] `svc.relay.status` 读取“状态正常”
- [x] COM5 被 `ok3568` 占用时点 CH1，出现中文占用提示；断开通道后可复用

## B. 文件服务（25 项）

- [x] `svc.root` 路径完整；保存并重启后保持
- [x] `svc.root.pick` 真实弹出“浏览文件夹”对话框；取消后应用恢复
- [x] `svc.root.layout` 创建 `common/` 与板卡目录
- [x] `svc.tftp.ip` 自动/127.0.0.1/本机 IP 均可选；127.0.0.1 仅监听本机
- [x] `svc.tftp.port` 选择 69 / 1069
- [x] `svc.tftp.write` 允许写入可勾选/取消；取消后 WRQ 被 ERROR 2 拒绝
- [x] `svc.tftp.toggle` 启动/停止；冲突时中文提示
- [x] `svc.tftp.status` 启动后显示 `监听 UDP 69 · 根 ...`
- [x] `svc.share.port` 选择 18082 / 18083；18083 实起 HTTP 200
- [x] `svc.share.toggle` 启动/停止；18082 返回 200
- [x] `svc.share.open` 未启动提示；启动后拉起浏览器
- [x] `svc.share.status` 与 `/state` 一致
- [x] `svc.nfs.alias` `/share` 与 `/mnt/share` 均 MNT 成功
- [x] `svc.nfs.uid` 选择 0 / 1000
- [x] `svc.nfs.gid` 选择 0 / 1001
- [x] `svc.nfs.toggle` 启动/停止；111 冲突中文提示
- [x] `svc.nfs.status` 与 `/state` 一致
- [x] `svc.save` 保存服务配置；连续保存不覆盖继电器值
- [x] `svc.stopall` 0.04 s 全停，三服务 false，端口释放
- [x] `svc.message` 每个动作均有中文回执
- [x] `svc.close` 关闭面板，`svc.*=0`；服务继续运行；菜单可重开
- [x] `svc.title` 标题正确
- [x] `svc.group.relay` 几何完整，不遮挡
- [x] `svc.group.files` 几何完整，不遮挡
- [x] 11 个标签文案完整（设备/串口/波特率/脉冲时长/共用目录/TFTP/端口/共享 HTTP/NFS/UID/GID）

## C. 布局 / 遮挡

- [x] 面板外框 `109,2,1811,1006`，工作区 bottom=1008，关闭按钮不被任务栏遮挡
- [x] 47 个业务控件两两求交：0 重叠
- [x] 业务控件最大 bottom=992，最小 left=150，最大 right=1740
- [x] 关闭按钮 `1590,947,1740,992`，完整可见
- [x] 主窗口 6 个按钮无重叠；搜索框与快捷键提示间隔 90 px

## D. 延时 / 双回显

- [x] `loop1` 发送 14 B，回显 14 B，36 µs
- [x] `ssh-link-demo1` 常态 55–76 ms；一次尖峰 122.6 ms
- [x] `ssh-link-demo2` 58.3–76.4 ms
- [x] 真实 Enter 路径：TX +2（一个 CRLF），echo +1，无双回显
- [x] 未见 1.98 s 级 SSH 回显退化

## E. 启停 / 持久化 / 退出

- [x] 启动 6 通道，在线 4；COM3/COM5 默认 idle
- [x] `ok3568` 打开 → 断开 idle → 继电器可复用 COM5
- [x] `ssh-link-demo1` 断开 idle → 重连 ready → 回显 62.9 ms
- [x] `ztl` 打开 ready → 断开 idle → 重连 ready
- [x] 保存后退出重启：COM5、9600、69、允许写入、18082、`/share`、UID/GID 0 保持
- [x] `menu.file.exit` 退出，18086/18082/69/111/1058/2049 全部无监听
- [x] 测试浏览器进程已关闭

## F. 回归数据

- [x] TFTP 70,000 B 上传 0.008 s / 下载 0.007 s，MD5 一致
- [x] TFTP 中文文件 190 B 上传/下载一致
- [x] TFTP `../evil.txt` → `ERR:bad path`
- [x] 共享 HTTP 多部分上传 302、下载 200、MD5 `2a43f79ceb44831d96f6e456839744e4`
- [x] 共享 HTTP 中文目录/文件名上传 302、下载 MD5 一致
- [x] 共享 HTTP `/delete` 302（浏览器/带 Content-Length 表单）
- [x] NFS `/share` MNT 0、LOOKUP 0、READ 255 B，MD5 一致
- [x] NFS `/mnt/share` MNT 0
- [x] 端口冲突：TFTP 69、共享 18082、NFS 111 均中文提示且 `on=false`
