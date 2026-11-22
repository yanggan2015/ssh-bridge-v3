// shell.h — 主窗口壳（S3/S12）：菜单 / 工具栏 / 左侧通道页签 / 板卡按钮条 / 状态栏。
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <cstdint>
#include <string>
#include <vector>

#include "app/app_core.h"
#include "core/config/config.h"
#include "ui/dialogs/channel_dialog.h"
#include "ui/dialogs/quick_dialog.h"
#include "ui/shell/file_browser.h"
#include "ui/sftp/sftp_window.h"
#include "ui/terminal/view.h"
#include "ui/tray/tray.h"

namespace sb3 {

class RelayPanel;
class FileServicePanel;
class ServiceManager;

class Shell {
public:
    ~Shell();
    bool create(HINSTANCE inst, int nShow, AppCore* core);
    HWND hwnd() const { return hwnd_; }
    HACCEL accel() const { return accel_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR id, DWORD_PTR ref);
    static LRESULT CALLBACK QuickBtnProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR ref);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void build_menu();
    void build_children();
    void layout();
    void fill_tree();
    void update_status();
    void apply_font();
    void show_self();
    void hide_self();
    void show_about();
    void check_update();
    void poll_update_check();
    void next_hit();
    void note_status(const std::wstring& text);
    void dump_state();
    void on_command(int id);
    void on_tray_event(LPARAM lp);
    void show_tray_menu();
    void on_tree_change();
    void on_left_tab_change();
    bool select_channel_index(std::size_t idx);
    void find_typed();
    std::string selected_channel() const;
    std::string current_channel() const;   // 激活画框的通道；单窗格时回落左树选择
    void on_active_pane_changed();
    bool sync_tree_to_channel(const std::string& name);
    void restore_layout_from_config();
    bool channel_exists(const std::string& name) const;
    void save_layout_state();
    void show_channel_dialog(const std::string& edit_name, bool ssh);
    void refresh_channels(const std::string& prefer);
    void on_channel_applied(const std::string& name, int elapsed_ms);

    void show_relay_panel(int page);
    void show_file_panel();
    void refresh_board_buttons();
    void toggle_left_panel();
    int layout_board_buttons(int x, int y, int avail_w);
    void on_board_button(std::size_t index);

    // S13 底部快捷指令条：按画框通道显示，多按钮自动换行
    void refresh_quick_buttons();
    int  layout_quick_buttons(int x, int y, int avail_w, bool place);
    void on_quick_button(std::size_t index);
    void show_quick_dialog(std::size_t edit_index, bool editing);
    void open_quick_dialog(std::size_t edit_index, bool editing);
    void delete_quick_button(std::size_t index);

    AppCore*  core_    = nullptr;
    HINSTANCE inst_    = nullptr;
    HWND      hwnd_    = nullptr;
    HWND      about_   = nullptr;
    HWND      about_update_note_ = nullptr;
    HWND      tree_    = nullptr;
    HWND      left_tabs_ = nullptr;
    HWND      status_  = nullptr;
    HWND      buttons_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    HFONT     font_    = nullptr;
    TrayIcon       tray_;
    TerminalView   view_;
    FileBrowser    file_browser_;
    ChannelDialog  dialog_;
    SftpWindow     sftp_;
    RelayPanel*    relay_panel_ = nullptr;
    FileServicePanel* file_panel_ = nullptr;
    ServiceManager* services_ = nullptr;
    HWND           search_ = nullptr;
    HWND           search_label_ = nullptr;
    HWND           keyhint_ = nullptr;
    HWND           board_scope_ = nullptr;   // 分屏时按钮条归属：画框N · 通道
    HACCEL    accel_   = nullptr;
    bool      sync_tree_binding_ = false;    // 正在把左树选中同步到激活画框，避免回写通道
    bool      layout_ready_ = false;         // 启动恢复完成前不写分屏布局，避免临时状态覆盖配置
    std::wstring last_note_;
    std::wstring update_log_path_;
    std::uint64_t update_deadline_ms_ = 0;
    bool update_checking_ = false;
    std::wstring active_button_id_;
    std::wstring active_button_label_;
    std::wstring active_script_line_;
    std::uint64_t button_start_ms_ = 0;
    bool      button_busy_ = false;
    bool      button_last_ok_ = true;
    bool      button_has_run_ = false;
    int       dpi_     = 96;
    int       left_page_ = 0;   // 0=通道 1=文件
    bool      left_collapsed_ = false;       // 当前是否收起左侧通道管理器
    bool      left_auto_collapsed_ = false;  // 本次收起由窄窗自动触发，变宽时可自动恢复
    bool      left_user_override_ = false;   // 用户手动切换后，直到跨越宽度断点才恢复自动
    bool      last_compact_ = false;         // 上一次布局是否处于窄窗模式
    int       board_strip_h_ = 0;
    std::vector<HTREEITEM> channel_items_;
    std::vector<BoardActionConfig> board_actions_;
    std::vector<HWND> board_buttons_;
    HWND quick_label_ = nullptr;
    HWND quick_add_   = nullptr;
    HWND quick_sep_   = nullptr;
    std::vector<HWND> quick_buttons_;
    std::vector<QuickCommandConfig> quick_commands_;
    std::string quick_channel_;
    int quick_rows_ = 1;
    int quick_height_ = 0;
};

// 把窗口矩形存进 output\ui_prefs.ini（异常配置时回落默认尺寸）
void save_window_rect(HWND hwnd);
RECT load_window_rect();

}  // namespace sb3
