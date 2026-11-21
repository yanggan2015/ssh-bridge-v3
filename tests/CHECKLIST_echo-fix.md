# 回车双回显修复 · 验收清单

- [x] 确认根因：SSH 默认 `crlf`，一次 Enter 发 `\r\n`，Unix 行规程提交两次
- [x] 确认 WM_KEYDOWN 未重复发送 Enter，问题只在行尾字节
- [x] SSH `crlf` → 单 `\r`
- [x] SSH `cr` → 单 `\r`
- [x] SSH `lf` → 单 `\n`
- [x] SSH `none` → 不发
- [x] 串口 `crlf` → 保持 `\r\n`
- [x] 物理 `WM_CHAR(13)`：修复前两个提示符，修复后一个
- [x] `echo PHYSICAL_ENTER` 输出仅一行
- [x] `term.input` 自动提交：单提示符
- [x] 中文 `echo 中文_OK`：上行与输出正常
- [x] 退格：`EDIT_OKX` → `EDIT_OK`，单提示符
- [x] 串口 `loop1`：`SERIAL_OK` + `CRLF` 共 11 B，回显正常
- [x] 连续命令：每条命令一个提示符
- [x] 空 Enter：只多一个空提示符，不再成对双提交
- [x] 无 warning 编译
- [x] 截图证据 `before.png` / `after-physical.png` / `after-unicode.png`
