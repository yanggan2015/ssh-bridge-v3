# S9 Web 控制台验收清单（T9）

日期：2026-11-22
报告：`docs/S9-Web控制台验收报告.md`
隔离环境：`output/webtest`（`web1/web2/web3`，`web.port=18082`，`web.token=t9pw`）

| 编号 | 检查项 | 判定 | 证据 |
|---|---|---|---|
| T9-1 | 10 个页面可打开：dashboard/config/actions/button-script/relay/tftp/nfs/share/tcp-forward/tunnel | [x] PASS | `s9-accept-final.txt`：`PAGES 10/10` |
| T9-2 | REST 与本地通道一致：`/status` 3 通道、`/log` 有写入字节 | [x] PASS | `STATUS 3 CHANNELS`、`WRITE/LOG PASS` |
| T9-3 | `/ws/log` 下行 BINARY，延迟 < 50ms | [x] PASS | `WS BINARY latency=1.0ms`（另一次 16.6ms） |
| T9-4 | 错误 token / 无 token 写接口 401 | [x] PASS | 无 token、`wrong` 均 401 |
| T9-5 | `web.enable=false` 时端口不监听 | [x] PASS | `listen=False on=False port=18082` |
| T9-6 | 手机宽度可用（390×844，无横向溢出） | [x] PASS | Playwright：10 页 `overflow=false`；`final-mobile-dashboard.png`、`final-mobile-config.png` |
| T9-7 | 无 token 只读：`/status`/`/log` 200，写 401，`readonly=true` | [x] PASS | `status=200 log=200 write=401 config=401 readonly=True` |
| T9-8 | 3 个 WS 客户端不串（各自绑定通道） | [x] PASS | `3 WS CLIENTS NO CROSS` |
| T9-9 | 静态资源 304 生效 | [x] PASS | `STATIC 200+304` |
| T9-10 | 端口占用时错误可读 | [x] PASS | `/state.web.error = Web 端口 18082 被占用或无法监听` |
| 附加 | 配置页两 Tab 表单覆盖全部字段 | [x] PASS | 基础 76 / 详细 89 / 叶子 89；`CONFIG_FORM PASS` |
| 附加 | 配置保存后 UI 热重载 | [x] PASS | `已重载配置（新增 0 / 删除 0 / 变更 0）` |
| 附加 | `/help`、`/help/<通道>` | [x] PASS | 200 / 200 / 404 |
| 构建 | `EXIT=0`、0 warning | [x] PASS | `output/audit/build-web-final2.log` |
| 自测 | `--selftest all`、`--selftest term` | [x] PASS | 退出 0，`RESULT: ALL PASS` |

## 复现

```powershell
# 构建（需 MSYS2）
C:\msys64\usr\bin\bash.exe -lc "cd /c/code/ssh-bridge-windows3 && export PATH=/mingw64/bin:$PATH && mingw32-make -j4"

# 起隔离实例（Web 18082，Agent 18087 仅用于读 /state）
.\output\build\ssh_bridge3.exe   # 工作目录 output\webtest

# REST/WS 验收
python output\webtest\s9_accept.py

# 页面/手机宽度/配置表单（Playwright + 已装 Edge）
node output\webtest\pw_pages_test.js
```

## 未覆盖/后续

- TCP 转发、隧道页面是 S12 入口占位，转发引擎未实现。
- 慢动作仍是 `std::async` + `/job` 轮询，尚未改为设计里的单队列 + `MG_EV_WAKEUP`。
- 工作区未提交，r11 包未包含 S8/S9。
