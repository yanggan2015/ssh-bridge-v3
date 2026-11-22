# S13 自更新 / 打包 / 全量回归 · 验收清单

- 版本：`3.0-s13-r12`
- 包：`output\dist\ssh-bridge-v3-3.0-s13-r12-win64.zip`
- 报告：`docs\S13-自更新打包全量回归验收报告.md`
- 安装基线：`output\dist\ssh-bridge-v3-3.0-s13-r11-win64.zip`

## T13

- [x] T13-1 r11 → r12 在线升级；旧进程停止、新进程自动重启、版本变 r12
- [x] T13-2 篡改 zip 1 字节 + 原 SHA-256 → 退出码 1，版本仍 r11，未替换
- [x] T13-3 TFTP / 共享 HTTP / NFS 三个开关真实启停，端口跟随释放
- [x] T13-4 解压到空目录 `output\s13-dist-test2` 独立启动，`tools\smoke.ps1` 9/9 PASS
- [x] T13-5 包内 `VERSION.txt = 3.0-s13-r12`，清单 exe SHA-256 匹配
- [x] T13-6 S0–S12 回归窗口全 PASS
- [x] T13-7 冷启动不读用户配置；退出后进程 0、端口无监听
- [x] T13-8 README / S13 报告 / S13 清单 / 命令齐全

## 发布包

- [x] `ssh_bridge3.exe` 12,633,447 B
- [x] `VERSION.txt` 含 product/version/built_utc/exe_sha256
- [x] 脱敏 `ssh_bridge_config.json`，默认画框 `loop1`
- [x] `README.md`、`tools\smoke.ps1`、`tools\update.ps1`、`tools\ui_probe.py`
- [x] 不含源码、真实密码、日志、用户配置
- [x] `.zip.sha256` 与 `.UPDATE.json` 同步生成

## 全量回归

- [x] S0 `step0_check` ALL PASS
- [x] S1 `step1_check` ALL PASS
- [x] S2 `step2_check` ALL PASS
- [x] S3 `step3_check` ALL PASS
- [x] S4 `step4_check` ALL PASS
- [x] S5 `ui_delete_channel_check` ALL PASS
- [x] S6 `ui_responsive_check` ALL PASS
- [x] S7 `ui_file_sidebar_check` ALL PASS
- [x] S8 入站 SSH 2223 登录 + 回显 PASS
- [x] S9 `s9_accept.py` + 10 页 Playwright ALL PASS
- [x] S10 `ui_bridge_check` 41 项 ALL PASS
- [x] S11 TFTP/共享/NFS 开关 PASS；继电器沿用既有硬件记录
- [x] S12 `s12_accept.py` T12-1～T12-7 ALL PASS；3 页移动端 ALL PASS

## 构建与质量

- [x] `mingw32-make` EXIT=0、0 warning
- [x] `--selftest all loop:selftest` EXIT=0、ALL PASS
- [x] `--selftest term` EXIT=0、ALL PASS
- [x] `step0 T0-12` quality_gate 全 PASS
- [x] `scripts\package.ps1` 在 `powershell` 5.1 与 `pwsh` 7 下均可执行

## 收尾

- [x] 测试进程、隧道、echo 服务已停止
- [x] 用户配置数据保留；回归迁移后已备份 `ssh_bridge_config.json.bak_s13_20261122`
- [x] 启动不再重写配置：G-8 隔离配置 SHA-256 启动前后不变
- [x] 后续回归全部改用隔离工作目录
- [x] 工作区改动 commit
- [x] r12 推送独立发布仓库并更新 UPDATE.json
