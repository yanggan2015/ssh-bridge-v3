// pane.h — 一个终端窗格：通道名 + 自己的 ANSI 屏 + 回看 + 自己的 ring 游标。
// 关键约定：每个窗格独立 epoch/seen，只读自己通道的 ring，互不串数据。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/bridge/bridge.h"
#include "ui/terminal/ansi.h"
#include "ui/terminal/scrollback.h"

namespace sb3 {
namespace term {

class Pane {
public:
    void init(const std::string& channel, int cols, int rows, std::size_t scroll_cap);

    const std::string& channel() const { return channel_; }
    bool attached() const { return !channel_.empty(); }

    void resize(int cols, int rows);

    // 读 ring 增量；返回 true 表示画面有更新
    bool pull(IBridge* bridge);
    // 全量重放：清屏 + 清回看，然后从 ring 起点重放（不动服务端、不清别人的游标）
    void replay(IBridge* bridge);
    // 清空本窗格画面与回看（不动 ring）
    void reset_view();

    AnsiScreen& screen() { return screen_; }
    const AnsiScreen& screen() const { return screen_; }
    Scrollback& scrollback() { return scrollback_; }
    const Scrollback& scrollback() const { return scrollback_; }

    // 行寻址：绝对行号 = 回看行（从 sb.base() 起）+ 屏幕行（从 sb.end() 起）
    std::uint64_t base_abs() const { return scrollback_.base(); }
    std::uint64_t screen_base_abs() const { return scrollback_.end(); }
    std::size_t total_lines() const { return scrollback_.count() + screen_.rows(); }
    std::uint64_t last_abs() const;
    const Line* line_at(std::uint64_t abs) const;
    std::string text_at(std::uint64_t abs) const;

    std::uint64_t cursor_abs() const;
    int cursor_col() const { return screen_.cursor_col(); }
    bool cursor_visible() const { return screen_.cursor_visible(); }

    // 滚动：offset 是从底部往上的行数（0 = 贴底）
    int scroll_offset() const { return scroll_offset_; }
    void scroll_by(int delta);
    void scroll_to(int offset);      // 绝对定位（AI 滑条 set_value 用）
    void scroll_to_bottom() { scroll_offset_ = 0; }
    int max_scroll() const;

    struct Hit {
        std::uint64_t abs = 0;
        int           col = 0;
        int           len = 0;
    };
    // 在当前窗格（回看 + 屏幕）里查找；命中后自动滚到可见处
    bool find(const std::string& needle, bool forward, Hit* hit, bool restart = false);
    void clear_search();
    const Hit& search_hit() const { return hit_; }
    bool has_search() const { return has_hit_; }

    // 上位机丢弃过数据（落后超过 ring 容量）
    bool dropped() const { return dropped_; }
    std::uint64_t rx_bytes() const { return parsed_bytes_; }

private:
    std::string strip_timestamps(const std::string& data);
    void feed_bytes(const std::string& clean);

    std::string   channel_;
    AnsiScreen    screen_;
    Scrollback    scrollback_{5000};
    std::uint64_t epoch_ = 0;
    std::uint64_t seen_ = 0;
    std::uint64_t parsed_bytes_ = 0;
    bool          dropped_ = false;
    int           scroll_offset_ = 0;

    // 时间戳剥离的跨块状态
    std::string carry_;
    bool        at_line_start_ = true;   // 本行还没有可见字符（ANSI 不算）
    int         esc_state_ = 0;          // 0 普通 / 1 ESC / 2 CSI / 3 OSC / 4 OSC-ESC

    Hit  hit_;
    bool has_hit_ = false;
};

}  // namespace term
}  // namespace sb3
