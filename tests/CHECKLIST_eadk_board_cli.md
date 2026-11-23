# EADK 板卡 CLI 全功能测试清单

`share/tools/eadk_board.py` 的测试分两层，均纳入 `output/webtest/run-full-suite.ps1` 一键全量套件：

| 层 | 脚本 | 环境 | 断言 | 目的 |
| --- | --- | --- | --- | --- |
| A 全命令契约 | `tests/eadk_board_full_check.py` | 本地假 v3 桥 + UDP 发现器，动态端口 | 90 | 覆盖全部子命令、参数、JSON/错误分支、受控生命周期 |
| B 真实桥冒烟 | `tests/eadk_cli_check.ps1` | 运行中的 18081 + `loop-chars` + `ssh-demo1` | 32 | 验证真实分发、下载、编译、读写、share 文件面、SSH run |

运行方式：

```powershell
cd C:\code\ssh-bridge-windows3
python tests\eadk_board_full_check.py
pwsh -NoProfile -File tests\eadk_cli_check.ps1
pwsh -NoProfile -File output\webtest\run-full-suite.ps1
```

结果文件：

- `output/ux-audit/eadk-board-full-check.json`
- `output/ux-audit/eadk-cli-check.json`
- `output/ux-audit/full-suite-summary.json`（19 套件汇总）

## A. 全命令契约（90/90）

### 分发与编译

| # | 断言 | 覆盖点 |
| --- | --- | --- |
| 1-2 | `share_py_200` / `share_md_200` | `/share/tools/eadk_board.py`、`.md` 均 200 |
| 3 | `py_compile` | 下载副本可编译 |
| 4 | `dist_py_md5_matches` | 下载文件 MD5 与源码一致 |

### 帮助

`noargs_help`、`dash_h_help`、`help`、`help_caps`、`help_to_board`、`help_download`、`help_unknown_topic_fail`：无参、`-h`、`help` 别名、主题帮助、未知主题非零退出。

### 发现与列表

| 断言 | 覆盖点 |
| --- | --- |
| `discover_cli_json_shape` | `discover` 子进程输出 JSON 形状 |
| `discover_wire_parse` | 固定 `EADK1` 回包解析 |
| `discover_command_mapping` | `host/base_url/boards` 映射 |
| `discover_self_compact` | `discover --self --compact` |
| `list_all` / `list_board` / `list_name_positional` | `list`、`-b list`、`list NAME` |
| `feats_alias` | `feats` 与 `list` 等价 |
| `list_json` | `list -j` JSON |

### 扫描与状态

`scan_all`、`scan_named`、`scan_missing_fail`、`scan_disconnected_exit2`、`scan_json`、`state_all_json`、`state_one_json`、`probe_shell`、`health_ok`、`health_down_exit2`、`status_json`、`help_url`：全量/单通道扫描、缺失通道、断线退出码 2、JSON、探测、健康、状态、帮助 URL。

### 日志、写入与命令

`log_all`、`log_lines`、`log_chars`、`clear`、`write_exit0`、`write_recorded`、`write_in_log`、`command_async`、`command_recorded`、`command_log`、`send_output`、`send_json`、`keys_exit0`、`keys_payload`、`ctrlc_exit0`：日志切片、清空、写入落日志、异步命令与 JSON、文本发送、按键与 Ctrl-C。

### 配置与生命周期

`timestamp_on`、`timestamp_off`、`timestamp_bad_fail`、`reconnect`、`serial_close`、`serial_open`、`serial_on_ssh_fail`、`action_enter_uboot`、`action_power_on`、`action_custom`、`action_missing_fail`、`action_no_button_fail`：时间戳开关/非法值、重连、串口开关、SSH 通道拒绝串口、按钮动作、缺按钮/不存在按钮失败。

### 重启、启动与登录

`soft_reboot`、`uboot_action_path`、`uboot_key_fallback`、`bootlog`、`bootlog_timestamp`、`login_explicit`、`ensure_shell`、`ensure_login`、`cmd_exec`、`run_exec`：软重启、U-Boot 按钮与抢键回退、启动日志、显式登录、已在 shell 的 ensure、自动登录 ensure、命令与 run。

### share 文件面

`put_multichunk`、`put_md5_local_remote`、`md5_json`、`ls_file`、`mkdir`、`rm_file`、`rm_dir`、`put_missing_fail`、`md5_missing_fail`：153600 B 按 65536 分 3 片上传、本地/远端 MD5、JSON、列目录、建目录、删文件/目录、缺本地文件/缺远端对象失败。

### to-board / 下载 / 反馈

`to_board_share`、`to_board_missing_fail`、`to_board_bad_method_fail`、`download`、`feedback_ok`、`feedback_payload`、`suggest_ok`、`suggest_payload`、`bug_ok`、`bug_payload`、`feedback_missing_text_fail`、`unknown_subcommand_fail`：share 阶梯正向与两类失败、下载、反馈/suggest/bug 载荷、缺陈述失败、未知子命令失败。

## B. 真实桥冒烟（32/32）

| 分组 | 断言 |
| --- | --- |
| 分发编译 | `share_py_200`、`share_md_200`、`download_nonempty`、`py_compile` |
| 扫描列表 | `scan_exit0`、`scan_channels`、`scan_has_target`、`list_exit0`、`list_connected`、`list_channel_key` |
| 健康状态 | `health_exit0`、`health_connected`、`state_exit0`、`state_shape`、`status_exit0`、`status_board_name` |
| 读写按键 | `clear_body_channel`、`write_exit0`、`log_has_write`、`send_exit0`、`send_output`、`keys_exit0`、`log_has_keys` |
| share 文件 | `put_exit0`、`put_ok_line`、`md5_exit0`、`md5_32`、`ls_exit0`、`rm_exit0`、`rm_dir_exit0` |
| SSH 执行 | `ssh_run_exit0`、`ssh_run_output` |

## 覆盖边界（明确不伪造）

1. `to-board --method=scp-pull/scp-push` 依赖真实板侧 sshd 与密钥，假桥不伪造；A 层只验证 `share` 阶梯与参数失败分支，B 层不覆盖 scp。
2. Windows 子进程 UDP 首包偶发丢失，A 层对 `discover` 采用“CLI 子进程形状 + 同进程协议解析/映射”双段验证，不做子进程真实 UDP 端到端断言。
3. A 层使用假桥验证协议契约，不替代 B 层真实 18081、真实串口/SSH 通道的端到端冒烟；两层必须同时通过。
