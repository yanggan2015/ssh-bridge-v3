# 终端控制键 / 行编辑键 · 发布前回归清单

适用：任何改动 `term.send`、键盘映射、ANSI 行编辑、SSH/串口读写泵的版本。真实端点，不接受只跑回环。

## 前置

- [ ] 隔离实例从 `output\terminal-test` 启动，UI 桥 `http://127.0.0.1:18096`
- [ ] 至少一条 SSH 通道 ready，`ssh-demo1` 与 `ssh-demo2` 均可用时两条都跑
- [ ] COM3（`ztl-com3`，1500000）与 COM5（`ok3568-com5`，115200）均为 ready
- [ ] COM5 板端若被 `systemd-journald` 刷屏，先 `dmesg -n 1`；测完必须 `dmesg -n 7`，并用 `cat /proc/sys/kernel/printk` 确认为 `7 4 1 7`
- [ ] 确认 `/state` 的 `terminal.panes[].tail` 最多 2 行且**最新行在前**；脚本断言必须读 `lines[0]`
- [ ] 真实凭据只留在本机 `output`，不进入仓库或外发

## 执行

```powershell
# 回环：逐项断言 tx+ == rx+ == 期望字节数
python tests\terminal_key_matrix.py --base http://127.0.0.1:18096 --skip-ssh --tag L1

# SSH 与物理串口：各 5 项
python tests\terminal_key_matrix.py --base http://127.0.0.1:18096 --skip-loop --ssh ssh-demo1 --tag K11
python tests\terminal_key_matrix.py --base http://127.0.0.1:18096 --skip-loop --ssh ssh-demo2 --tag K22
python tests\terminal_key_matrix.py --base http://127.0.0.1:18096 --skip-loop --ssh ztl-com3 --tag S3K
python tests\terminal_key_matrix.py --base http://127.0.0.1:18096 --skip-loop --ssh ok3568-com5 --tag S5K
```

每台必须 5/5，且结果为 `RESULT: ALL PASS`：

- [ ] `ech` + TAB 补全为 `echo`（补全后不自动加空格的板端由脚本显式补一个空格）
- [ ] 补全结果执行后输出 `TAB_<tag>_OK`
- [ ] `Ctrl-C` 在 2.2s 内打断 `sleep 5`，随后输出 `KEYC_<tag>_OK`
- [ ] 上方向键召回上一条 `printf 'HIST_<tag>_ONE\n'`
- [ ] 多打 `Z` 后 DEL 再补 `K`，输出 `BACK_<tag>_K`

## 配套门禁

- [ ] `output\build\ssh_bridge3.exe --selftest term` → `RESULT: ALL PASS`，含 `T4U-11`、`T4U-12`
- [ ] `pwsh -NoProfile -File tests\ui_bridge_check.ps1 -SkipBuild` → 41 项 ALL PASS
- [ ] `pwsh -NoProfile -File tests\step3_check.ps1 -SkipBuild`、`tests\step4_check.ps1 -SkipBuild` → ALL PASS
- [ ] 测完退出隔离实例，确认无 `ssh_bridge3.exe` 残留

## 最近一次结果

- 回环 11/11、`ssh-demo1` 5/5、`ssh-demo2` 5/5、`ztl-com3` 5/5、`ok3568-com5` 5/5
- 产品代码与 r15 一致；本轮仅测试脚本与文档变化，未重发安装包
