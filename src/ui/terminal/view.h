// view.h — 终端窗格视图（S4）：1/2/4 分屏、绘制、选区复制、粘贴、搜索定位、字体缩放。
// 每个分屏槽位拥有独立的 Pane（独立游标），同一条通道开两个槽互不影响。

#pragma once

#include <windows.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "app/app_core.h"
#include "ui/terminal/pane.h"

namespace sb3 {

class TerminalView {
public:
    struct Options {
        int         font_px   = 15;
        std::size_t scrollback = 5000;
        int         split      = 1;   // 1 / 2 / 4
    };

    bool create(HINSTANCE inst, HWND parent, AppCore* core, const Options& opt, int dpi);
    HWND hwnd() const { return hwnd_; }

    void set_channel(const std::string& name);      // 当前活动槽位切通道（首开补尾部历史）
    void set_slot_channel(int idx, const std::string& name);  // 指定画框切通道（恢复布局用）
    void restore_layout(const std::vector<std::string>& channels, int active);  // 启动恢复各画框
    std::vector<std::string> slot_channels() const;
    std::string active_channel() const;
    void split(int n);
    // 通道被删除后清掉指向它的窗格（否则会停在旧画面上）
    void prune_channels(const std::vector<std::string>& names);
    int  split_count() const { return static_cast<int>(slots_.size()); }
    // AI 桥接：按序号激活窗格（0 起），返回是否真的切换
    bool set_active(int idx);
    int  active_index() const { return active_; }

    bool pull_visible();                            // 只拉可见窗口的可见槽位
    void clear_active();
    void replay_active();
    bool has_selection() const;
    bool copy_selection();
    bool paste_clipboard();
    bool find(const std::string& needle, bool forward, bool restart = false);
    void clear_search();
    // AI 桥接：向活动窗格写按键（submit=true 末尾补回车）；滚动/取窗口句柄
    bool type_text(const std::string& utf8, bool submit);
    // AI 桥接：原样写字节，不做 UTF-8 重编码，用于 Ctrl/ESC/Tab 等 C0 与功能键序列
    bool send_raw(const std::string& bytes);
    void scroll_active(int lines);
    void scroll_active_to(int offset);   // 绝对回看位置（AI 滑条）
    int  active_offset() const;
    int  active_max_scroll() const;
    void zoom(int delta);
    int  font_px() const { return opt_.font_px; }

    std::string info() const;                       // "loop1 · 96x28 · 回看 12 行"
    // 观测面：分屏/活动槽/各窗格尾部文本（供闸门与新会话接手人核对）
    std::string state_json() const;
    void invalidate();
    void focus_active();

private:
    struct Slot {
        std::string                channel;
        std::unique_ptr<term::Pane> pane;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void ensure_font();
    void measure(HDC dc);
    RECT slot_rect(int idx, const RECT& client) const;
    int  slot_at(POINT pt) const;
    void sync_slot(Slot& slot, const RECT& rc);
    void draw_all(HDC dc);
    void draw_slot(HDC dc, int idx, RECT rc, bool active);
    void draw_run(HDC dc, int x, int y, const std::wstring& text, int run_cols,
                  std::uint32_t fg, std::uint32_t bg, std::uint8_t attr, bool selected);
    int  cell_col_at(const term::Pane& pane, int x, RECT body) const;
    std::uint64_t cell_row_at(const term::Pane& pane, int y, RECT body) const;
    void begin_selection(int slot, POINT pt);
    void extend_selection(POINT pt);
    std::string selection_text() const;
    bool copy_to_clipboard(const std::string& utf8);
    void send_active(const std::string& bytes);
    std::string line_ending_bytes() const;   // 按当前通道配置生成回车字节
    void on_wheel(POINT pt, int delta);
    void show_menu(POINT pt);
    void normalize_active();
    void notify_active_changed();   // 通知主窗口板控条跟随激活画框

    AppCore*  core_ = nullptr;
    HWND      hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    int       dpi_  = 96;
    Options   opt_{};
    HFONT     font_ = nullptr;
    HBRUSH    brush_bg_ = nullptr;
    HBRUSH    brush_header_ = nullptr;
    HBRUSH    brush_active_ = nullptr;
    HBRUSH    brush_sel_ = nullptr;
    HBRUSH    brush_hit_ = nullptr;
    int       cell_w_ = 8;
    int       cell_h_ = 16;
    int       header_h_ = 20;

    std::vector<Slot> slots_;
    int active_ = 0;

    struct Selection {
        bool          has      = false;
        int           slot     = 0;
        std::uint64_t anchor   = 0;
        int           anchor_col = 0;
        std::uint64_t cur      = 0;
        int           cur_col  = 0;
    } sel_;

    bool swallow_char_ = false;
};

// 从 ui_prefs.ini 读/写终端字号（[terminal] font）
int  load_terminal_font_px(int fallback);
void save_terminal_font_px(int px);

}  // namespace sb3
