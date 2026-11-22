// ansi.h — 终端 ANSI 解析器（S4）：字节流 → 屏幕行模型 + 颜色/光标/滚屏。
// 只做模型，不绘制；滚出屏幕的自然行通过 sink 交给 Scrollback。
//
// 明确支持：SGR 颜色/粗体/反显/下划线、CSI 光标定位与移动、K/J 擦除、
// H/f 定位、S 上下滚、L/M 插删行、@/P/X 插删字符、r 滚屏区、?25 光标显隐、
// OSC 0/2 标题、CR 重写、BS、TAB、UTF-8（含中文/表情宽字符）。
// 其余序列吞掉不显示（不报错）。

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ui/terminal/term_types.h"

namespace sb3 {
namespace term {

class AnsiScreen {
public:
    // 尺寸变化会重建网格；旧内容按左上角裁剪保留
    void init(int cols, int rows);
    void resize(int cols, int rows);

    // 硬复位：清屏、清 SGR、光标归位（不动回看）
    void reset_screen();

    // 输入一段字节（可任意切分；状态跨调用保持）
    void feed(const std::string& bytes);

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    const std::vector<Line>& lines() const { return rows_grid_; }

    int cursor_row() const { return cur_row_; }
    int cursor_col() const { return cur_col_; }
    bool cursor_visible() const { return cur_show_; }

    const std::string& title() const { return title_; }
    void clear_title() { title_.clear(); }

    // 滚出屏幕的行交给回看（只在整屏滚动时调用）
    void set_scroll_sink(std::function<void(Line&&)> sink) { sink_ = std::move(sink); }

private:
    Line& row_at(int r) { return rows_grid_[static_cast<std::size_t>(r)]; }
    void blank_row(int r);
    void newline();
    void carriage_return();
    void scroll_up(int n);
    void scroll_down(int n);
    void put_cp(char32_t cp);
    void utf8_feed(unsigned char b);
    void csi_dispatch(char final_byte);
    void osc_dispatch();
    void sgr_apply(const std::vector<int>& params);
    void set_fg_index(int idx);
    void set_bg_index(int idx);
    void clamp_cursor();

    int cols_ = 80;
    int rows_ = 24;
    std::vector<Line> rows_grid_;

    int  cur_row_ = 0;
    int  cur_col_ = 0;
    bool wrap_pending_ = false;   // 已写满最后一列，下一个字符才换行（xterm 延迟换行）
    bool cur_show_ = true;
    bool autowrap_ = true;

    int  scroll_top_ = 0;
    int  scroll_bottom_ = 23;

    std::uint32_t fg_ = kDefaultColor;
    std::uint32_t bg_ = kDefaultColor;
    std::uint8_t  attr_ = 0;

    int  saved_row_ = 0;
    int  saved_col_ = 0;

    int state_ = 0;          // 0 普通 / 1 ESC / 2 CSI / 3 OSC / 4 OSC-ESC
    std::string csi_;
    std::string osc_;
    std::string utf8_;

    std::string title_;
    std::function<void(Line&&)> sink_;
};

}  // namespace term
}  // namespace sb3
