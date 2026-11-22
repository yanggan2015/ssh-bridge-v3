// bridge_host.h — AI 可控桥接层的装配（S5 前半）：把窗口控件/虚拟动作/业务状态
// 接到 win_ui_bridge（协议 1.1，仅 127.0.0.1）。
// 只在 enabled 时启动；关掉时进程不监听任何端口（沿用形态铁律）。

#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

namespace sb3 {

class AppCore;
class SftpWindow;
class TerminalView;
class ServiceManager;

namespace aibridge {

struct Options {
    bool        enabled = false;
    int         port    = 18086;
    std::string token;
};

// 在主窗口与子控件建好之后调用；返回是否真的开始监听。
bool start(HWND main, AppCore* core, TerminalView* view, const Options& opt);
void stop();

// WndProc 第一行调用：消费 UI_BRIDGE_WM_ACTION 与 kMsgBridgeState。
bool handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// UI 线程上生成 /state 的业务快照（通道 + 终端 + 搜索框）。
std::string build_state_json();

bool running();
int  port();

// 关于窗口（非模态）建好后登记它的两个控件 id：about.text / btn.about.ok。
void register_about(HWND box);

// 通道表单：登记顶层窗口并把每个字段挂稳定 id；关闭后的旧 HWND 自动摘除。
void register_channel_dialog(HWND dlg);

// SFTP 面板：登记顶层窗口与全部控件 id（sftp.*），并把 /state.ui.sftp 接上。
void register_sftp(HWND wnd, SftpWindow* panel);

// SFTP 附属窗口：传输历史 / 限速设置。每个控件挂稳定 id，关闭时摘除。
void register_sftp_history(HWND wnd);
void register_sftp_settings(HWND wnd);
void unregister_sftp_dialog(HWND wnd);

// S11 服务与继电器面板：状态常驻 /state.services；窗口控件挂 svc.*。
void set_service_manager(ServiceManager* mgr);
void register_service_panel(HWND wnd);

// S12 独立继电器窗口：窗口控件挂 relay.*，页签单独登记以便切换。
void register_relay_panel(HWND wnd, HWND tabs);

// S12 独立文件服务窗口：窗口控件挂 filesvc.*。
void register_file_service_panel(HWND wnd);

// 主窗口板卡按钮条：每个按钮按配置里的按钮 id 登记成 board.btn.<id>。
void register_board_button(HWND btn, const std::string& action_id);

// S13 底部快捷指令：按序号登记 quick.btn.<n>，供桥接真实点击。
void register_quick_button(HWND btn, std::size_t index);

// 终端内容变了：把回看滑条的 max 同步到真实上限（必须在 UI 线程调用）
void notify_term_changed();

}  // namespace aibridge
}  // namespace sb3
