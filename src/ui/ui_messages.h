// ui_messages.h — 进程内自定义消息（托盘/刷新/退出）。

#pragma once

#include <windows.h>

namespace sb3 {

constexpr UINT kMsgTrayShow   = WM_APP + 1;   // 托盘双击 / 菜单"显示窗口"
constexpr UINT kMsgTrayExit   = WM_APP + 2;   // 托盘菜单"退出"（完整收尾序列）
constexpr UINT kMsgTrayHide   = WM_APP + 3;   // 托盘菜单"隐藏窗口"
constexpr UINT kMsgTrayEvent  = WM_APP + 4;   // Shell_NotifyIcon 回调
constexpr UINT kMsgRefresh    = WM_APP + 5;   // 外部请求刷新状态
constexpr UINT kMsgStateDump  = WM_APP + 6;   // 立即写 ui_state.json
constexpr UINT kMsgTermData   = WM_APP + 7;   // 通道有新数据（wParam = 通道下标，已合并）
constexpr UINT kMsgSelect     = WM_APP + 8;   // 选中第 N 条通道（脚本/Agent 用，等价点树）
constexpr UINT kMsgBridgeState = WM_APP + 9;  // AI 桥接：在 UI 线程生成 /state 快照
constexpr UINT kMsgBoardButtonsChanged = WM_APP + 10;  // 板卡按钮配置保存，重画按钮条
constexpr UINT kMsgBoardActionResult   = WM_APP + 11;  // 板卡按钮后台执行完成（lParam=AsyncBoardResult*）
constexpr UINT kMsgRelayActionResult   = WM_APP + 12;  // 继电器窗口测试完成（lParam=AsyncBoardResult*）
constexpr UINT kMsgBoardActionProgress = WM_APP + 13;  // 板卡脚本实时输出（lParam=AsyncBoardProgress*）
constexpr UINT kMsgActivePaneChanged   = WM_APP + 14;  // 终端激活画框变化（wParam=画框索引，0 起）
constexpr UINT kMsgWebReload           = WM_APP + 15;  // S9 网页配置保存后请求 UI 线程重载

}  // namespace sb3
