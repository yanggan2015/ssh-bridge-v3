# 步骤 0 · 测试清单（环境与构建链）

规则：全部 PASS 才允许进入步骤 1。自动化：`tests\step0_check.ps1`。

| 编号 | 测试功能 | 判定标准 | 方式 | 结果 |
|---|---|---|---|---|
| T0-1 | 目录骨架 | `src/app core services ui third_party`、`docs tests scripts output` 全部存在 | 自动 | PASS |
| T0-2 | 关键文件 | README / Makefile / .gitignore / app_main.cpp / bridge.h / service.h / config.h 均在 | 自动 | PASS |
| T0-3 | 编译无错 | `mingw32-make -j4` 退出码 0，输出无 `error` | 自动 | PASS |
| T0-4 | 编译无警告 | 输出无 `warning`（`-Wall -Wextra`） | 自动 | PASS |
| T0-5 | 产物 | `output\build\ssh_bridge3.exe` 存在且 > 100KB | 自动 | PASS |
| T0-6 | 启动存活 | 启动后 4 秒进程仍存活 | 自动 | PASS |
| T0-7 | 窗口标题 | 主窗口标题含 `SSH Bridge 3.0` | 自动 | PASS |
| T0-8 | 菜单可用 | 菜单栏存在 `文件` / `帮助` 两项 | 人工 | PASS |
| T0-9 | 状态栏 | 底部状态栏显示 `骨架就绪 · 步骤 0` | 人工 | PASS |
| T0-10 | 关闭退出 | 关闭窗口后进程退出，退出码 0 | 自动 | PASS |
| T0-11 | 无残留 | 关闭后无同名进程残留 | 自动 | PASS |
| T0-12 | 代码整理 | `scripts\quality_gate.ps1` 全 PASS | 自动 | PASS |
| T0-13 | git 基线 | 仓库已初始化，首次提交完成，工作区干净 | 自动 | PASS |

## 人工项判定记录

- T0-8 菜单：截图可见「文件」「帮助」两个顶级菜单，均可展开。
- T0-9 状态栏：窗口底部有状态栏，首个分区文案为「骨架就绪 · 步骤 0」。

## 本步不做（明确排除，防止越界）

配置读写、串口、终端、托盘、任何服务端口 —— 都属于步骤 1 及以后。
