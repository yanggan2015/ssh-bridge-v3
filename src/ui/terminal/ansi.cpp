// ansi.cpp — ANSI 解析器实现（S4）。

#include "ui/terminal/ansi.h"

#include <algorithm>

namespace sb3 {
namespace term {
namespace {

int param_or(const std::vector<int>& p, std::size_t i, int def) {
    if (i >= p.size() || p[i] == 0) return def;
    return p[i];
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

void AnsiScreen::init(int cols, int rows) {
    cols_ = std::max(1, cols);
    rows_ = std::min(256, std::max(1, rows));
    rows_grid_.assign(static_cast<std::size_t>(rows_), Line());
    for (int r = 0; r < rows_; ++r) {
        rows_grid_[static_cast<std::size_t>(r)].cells.assign(static_cast<std::size_t>(cols_), Cell());
    }
    cur_row_ = 0;
    cur_col_ = 0;
    wrap_pending_ = false;
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
}

void AnsiScreen::resize(int cols, int rows) {
    cols = std::max(1, cols);
    rows = std::min(256, std::max(1, rows));
    if (cols == cols_ && rows == rows_) return;

    std::vector<Line> grid(static_cast<std::size_t>(rows), Line());
    for (int r = 0; r < rows; ++r) {
        grid[static_cast<std::size_t>(r)].cells.assign(static_cast<std::size_t>(cols), Cell());
    }
    int copy_rows = std::min(rows, rows_);
    int copy_cols = std::min(cols, cols_);
    for (int r = 0; r < copy_rows; ++r) {
        Line& dst = grid[static_cast<std::size_t>(r)];
        const Line& src = rows_grid_[static_cast<std::size_t>(r)];
        for (int c = 0; c < copy_cols && c < static_cast<int>(src.cells.size()); ++c) {
            dst.cells[static_cast<std::size_t>(c)] = src.cells[static_cast<std::size_t>(c)];
        }
        dst.wrapped = src.wrapped;
    }
    rows_grid_.swap(grid);
    cols_ = cols;
    rows_ = rows;
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    clamp_cursor();
}

void AnsiScreen::blank_row(int r) {
    if (r < 0 || r >= rows_) return;
    Line& line = row_at(r);
    line.cells.assign(static_cast<std::size_t>(cols_), Cell());
    line.wrapped = false;
}

void AnsiScreen::reset_screen() {
    for (int r = 0; r < rows_; ++r) blank_row(r);
    cur_row_ = 0;
    cur_col_ = 0;
    wrap_pending_ = false;
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    fg_ = kDefaultColor;
    bg_ = kDefaultColor;
    attr_ = 0;
    cur_show_ = true;
    autowrap_ = true;
    title_.clear();
    state_ = 0;
    csi_.clear();
    osc_.clear();
    utf8_.clear();
}

void AnsiScreen::clamp_cursor() {
    cur_row_ = std::max(0, std::min(cur_row_, rows_ - 1));
    cur_col_ = std::max(0, std::min(cur_col_, cols_ - 1));
    wrap_pending_ = false;
}

void AnsiScreen::newline() {
    wrap_pending_ = false;
    if (cur_row_ == scroll_bottom_) {
        scroll_up(1);
    } else if (cur_row_ < rows_ - 1) {
        ++cur_row_;
    }
}

void AnsiScreen::carriage_return() {
    cur_col_ = 0;
    wrap_pending_ = false;
}

void AnsiScreen::scroll_up(int n) {
    if (n <= 0) return;
    n = std::min(n, scroll_bottom_ - scroll_top_ + 1);
    for (int k = 0; k < n; ++k) {
        Line moved = std::move(row_at(scroll_top_));
        for (int r = scroll_top_; r < scroll_bottom_; ++r) {
            row_at(r) = std::move(row_at(r + 1));
        }
        blank_row(scroll_bottom_);
        if (scroll_top_ == 0 && sink_) sink_(std::move(moved));
    }
}

void AnsiScreen::scroll_down(int n) {
    if (n <= 0) return;
    n = std::min(n, scroll_bottom_ - scroll_top_ + 1);
    for (int k = 0; k < n; ++k) {
        for (int r = scroll_bottom_; r > scroll_top_; --r) {
            row_at(r) = std::move(row_at(r - 1));
        }
        blank_row(scroll_top_);
    }
}

void AnsiScreen::put_cp(char32_t cp) {
    if (cp == U'\t') {
        wrap_pending_ = false;
        int next = (cur_col_ / 8 + 1) * 8;
        cur_col_ = std::min(next, cols_ - 1);
        return;
    }
    if (cp < 0x20) return;
    if (autowrap_ && wrap_pending_) {
        row_at(cur_row_).wrapped = true;
        cur_col_ = 0;
        newline();
    }
    wrap_pending_ = false;
    int width = codepoint_wide(cp) ? 2 : 1;
    if (cur_col_ + width > cols_) {
        if (autowrap_) {
            row_at(cur_row_).wrapped = true;
            carriage_return();
            newline();
        } else {
            cur_col_ = std::max(0, cols_ - width);
        }
    }
    Line& line = row_at(cur_row_);
    Cell c;
    c.ch = cp;
    c.fg = fg_;
    c.bg = bg_;
    c.attr = attr_;
    line.cells[static_cast<std::size_t>(cur_col_)] = c;
    if (width == 2 && cur_col_ + 1 < cols_) {
        Cell cont;
        cont.ch = 0;
        cont.fg = fg_;
        cont.bg = bg_;
        cont.attr = attr_;
        cont.wide_cont = true;
        line.cells[static_cast<std::size_t>(cur_col_ + 1)] = cont;
    }
    int next_col = cur_col_ + width;
    if (next_col >= cols_) {
        cur_col_ = cols_ - 1;
        wrap_pending_ = autowrap_;
    } else {
        cur_col_ = next_col;
    }
}

void AnsiScreen::utf8_feed(unsigned char b) {
    utf8_.push_back(static_cast<char>(b));
    std::size_t need = 0;
    unsigned char first = static_cast<unsigned char>(utf8_[0]);
    if ((first & 0x80) == 0) need = 1;
    else if ((first & 0xE0) == 0xC0) need = 2;
    else if ((first & 0xF0) == 0xE0) need = 3;
    else if ((first & 0xF8) == 0xF0) need = 4;
    else {
        utf8_.clear();
        return;
    }
    if (utf8_.size() < need) return;
    char32_t cp = 0;
    if (need == 1) {
        cp = first;
    } else {
        cp = static_cast<char32_t>(first & (0xFF >> (need + 1)));
        for (std::size_t i = 1; i < need; ++i) {
            unsigned char cb = static_cast<unsigned char>(utf8_[i]);
            if ((cb & 0xC0) != 0x80) {
                utf8_.clear();
                return;
            }
            cp = (cp << 6) | static_cast<char32_t>(cb & 0x3F);
        }
    }
    utf8_.clear();
    if (cp == 0 || cp > 0x10FFFF) return;
    put_cp(cp);
}

void AnsiScreen::feed(const std::string& bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        unsigned char b = static_cast<unsigned char>(bytes[i]);
        switch (state_) {
        case 0:
            if (b == 0x1b) {
                state_ = 1;
            } else if (b == '\r') {
                carriage_return();
            } else if (b == '\n') {
                newline();
            } else if (b == '\t') {
                put_cp(U'\t');
            } else if (b == '\b') {
                wrap_pending_ = false;
                if (cur_col_ > 0) --cur_col_;
            } else if (b == 0x07) {
                // 响铃：模型层忽略
            } else if (b < 0x20) {
                // 其它控制字符忽略
            } else {
                utf8_feed(b);
            }
            break;

        case 1:
            if (b == '[') {
                csi_.clear();
                state_ = 2;
            } else if (b == ']') {
                osc_.clear();
                state_ = 3;
            } else if (b == '7') {
                saved_row_ = cur_row_;
                saved_col_ = cur_col_;
                state_ = 0;
            } else if (b == '8') {
                cur_row_ = saved_row_;
                cur_col_ = saved_col_;
                state_ = 0;
            } else if (b == 'c') {
                reset_screen();
                state_ = 0;
            } else {
                state_ = 0;
            }
            break;

        case 2:
            if (b >= 0x40 && b <= 0x7e) {
                csi_dispatch(static_cast<char>(b));
                state_ = 0;
            } else if (b == 0x1b) {
                state_ = 1;
            } else if (csi_.size() < 256) {
                csi_.push_back(static_cast<char>(b));
            }
            break;

        case 3:
            if (b == 0x07) {
                osc_dispatch();
                state_ = 0;
            } else if (b == 0x1b) {
                state_ = 4;
            } else if (osc_.size() < 1024) {
                osc_.push_back(static_cast<char>(b));
            }
            break;

        case 4:
            if (b == '\\') {
                osc_dispatch();
                state_ = 0;
            } else {
                if (osc_.size() < 1024) osc_.push_back(static_cast<char>(0x1b));
                if (osc_.size() < 1024) osc_.push_back(static_cast<char>(b));
                state_ = 3;
            }
            break;

        default:
            state_ = 0;
            break;
        }
    }
}

void AnsiScreen::csi_dispatch(char final_byte) {
    char priv = 0;
    std::size_t start = 0;
    if (!csi_.empty()) {
        char c0 = csi_[0];
        if (c0 == '?' || c0 == '>' || c0 == '!' || c0 == '=') {
            priv = c0;
            start = 1;
        }
    }
    std::vector<int> params;
    int cur = -1;
    for (std::size_t i = start; i < csi_.size(); ++i) {
        char c = csi_[i];
        if (is_digit(c)) {
            cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
            if (cur > 9999) cur = 9999;     // 防止超长数字导致有符号溢出
        } else if (c == ';' || c == ':') {
            params.push_back(cur < 0 ? 0 : cur);
            cur = -1;
        }
    }
    if (cur >= 0 || !params.empty()) params.push_back(cur < 0 ? 0 : cur);

    switch (final_byte) {
    case 'A':
        cur_row_ -= param_or(params, 0, 1);
        clamp_cursor();
        break;
    case 'B':
    case 'e':
        cur_row_ += param_or(params, 0, 1);
        clamp_cursor();
        break;
    case 'C':
    case 'a':
        cur_col_ += param_or(params, 0, 1);
        clamp_cursor();
        break;
    case 'D':
        cur_col_ -= param_or(params, 0, 1);
        clamp_cursor();
        break;
    case 'E':
        cur_row_ += param_or(params, 0, 1);
        cur_col_ = 0;
        clamp_cursor();
        break;
    case 'F':
        cur_row_ -= param_or(params, 0, 1);
        cur_col_ = 0;
        clamp_cursor();
        break;
    case 'G':
        cur_col_ = param_or(params, 0, 1) - 1;
        clamp_cursor();
        break;
    case 'd':
        cur_row_ = param_or(params, 0, 1) - 1;
        clamp_cursor();
        break;
    case 'H':
    case 'f':
        cur_row_ = param_or(params, 0, 1) - 1;
        cur_col_ = param_or(params, 1, 1) - 1;
        clamp_cursor();
        break;
    case 'J': {
        int mode = params.empty() ? 0 : params[0];
        if (mode == 0) {
            for (int c = cur_col_; c < cols_; ++c) {
                Cell blank;
                blank.bg = bg_;
                row_at(cur_row_).cells[static_cast<std::size_t>(c)] = blank;
            }
            for (int r = cur_row_ + 1; r < rows_; ++r) blank_row(r);
        } else if (mode == 1) {
            for (int r = 0; r < cur_row_; ++r) blank_row(r);
            for (int c = 0; c <= cur_col_ && c < cols_; ++c) {
                Cell blank;
                blank.bg = bg_;
                row_at(cur_row_).cells[static_cast<std::size_t>(c)] = blank;
            }
        } else {
            for (int r = 0; r < rows_; ++r) blank_row(r);
        }
        break;
    }
    case 'K': {
        int mode = params.empty() ? 0 : params[0];
        Line& line = row_at(cur_row_);
        int from = 0;
        int to = cols_ - 1;
        if (mode == 0) from = cur_col_;
        else if (mode == 1) to = cur_col_;
        for (int c = from; c <= to; ++c) {
            Cell blank;
            blank.bg = bg_;
            line.cells[static_cast<std::size_t>(c)] = blank;
        }
        if (mode == 2) line.wrapped = false;
        break;
    }
    case 'm':
        sgr_apply(params);
        break;
    case 'h':
    case 'l': {
        bool on = (final_byte == 'h');
        if (priv == '?') {
            for (std::size_t i = 0; i < params.size(); ++i) {
                int mode = params[i];
                if (mode == 25) cur_show_ = on;
                else if (mode == 7) autowrap_ = on;
                else if (mode == 1049 || mode == 47 || mode == 1047) {
                    if (on) reset_screen();
                }
            }
        }
        break;
    }
    case 'r': {
        int top = param_or(params, 0, 1) - 1;
        int bottom = std::min(param_or(params, 1, rows_), rows_) - 1;
        if (top >= 0 && bottom < rows_ && top < bottom) {
            scroll_top_ = top;
            scroll_bottom_ = bottom;
            cur_row_ = top;
            cur_col_ = 0;
        }
        break;
    }
    case 'S':
        scroll_up(param_or(params, 0, 1));
        break;
    case 'T':
        scroll_down(param_or(params, 0, 1));
        break;
    case 'L':
        if (cur_row_ >= scroll_top_ && cur_row_ <= scroll_bottom_) {
            int n = param_or(params, 0, 1);
            for (int k = 0; k < n; ++k) {
                for (int r = scroll_bottom_; r > cur_row_; --r) row_at(r) = std::move(row_at(r - 1));
                blank_row(cur_row_);
            }
        }
        break;
    case 'M':
        if (cur_row_ >= scroll_top_ && cur_row_ <= scroll_bottom_) {
            int n = param_or(params, 0, 1);
            for (int k = 0; k < n; ++k) {
                for (int r = cur_row_; r < scroll_bottom_; ++r) row_at(r) = std::move(row_at(r + 1));
                blank_row(scroll_bottom_);
            }
        }
        break;
    case '@': {
        int n = param_or(params, 0, 1);
        Line& line = row_at(cur_row_);
        for (int c = cols_ - 1; c >= cur_col_ + n; --c) {
            line.cells[static_cast<std::size_t>(c)] = line.cells[static_cast<std::size_t>(c - n)];
        }
        for (int c = cur_col_; c < cur_col_ + n && c < cols_; ++c) {
            line.cells[static_cast<std::size_t>(c)] = Cell();
        }
        break;
    }
    case 'P': {
        int n = param_or(params, 0, 1);
        Line& line = row_at(cur_row_);
        for (int c = cur_col_; c < cols_; ++c) {
            int src = c + n;
            line.cells[static_cast<std::size_t>(c)] =
                (src < cols_) ? line.cells[static_cast<std::size_t>(src)] : Cell();
        }
        break;
    }
    case 'X': {
        int n = param_or(params, 0, 1);
        Line& line = row_at(cur_row_);
        for (int c = cur_col_; c < cur_col_ + n && c < cols_; ++c) {
            Cell blank;
            blank.bg = bg_;
            line.cells[static_cast<std::size_t>(c)] = blank;
        }
        break;
    }
    case 's':
        saved_row_ = cur_row_;
        saved_col_ = cur_col_;
        break;
    case 'u':
        cur_row_ = saved_row_;
        cur_col_ = saved_col_;
        clamp_cursor();
        break;
    default:
        break;
    }
}

void AnsiScreen::set_fg_index(int idx) { fg_ = (idx < 0) ? kDefaultColor : palette_color(idx); }

void AnsiScreen::set_bg_index(int idx) { bg_ = (idx < 0) ? kDefaultColor : palette_color(idx); }

void AnsiScreen::sgr_apply(const std::vector<int>& params) {
    if (params.empty()) {
        fg_ = kDefaultColor;
        bg_ = kDefaultColor;
        attr_ = 0;
        return;
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        int p = params[i];
        if (p == 0) {
            fg_ = kDefaultColor;
            bg_ = kDefaultColor;
            attr_ = 0;
        } else if (p == 1) {
            attr_ |= kAttrBold;
        } else if (p == 4) {
            attr_ |= kAttrUnderline;
        } else if (p == 7) {
            attr_ |= kAttrReverse;
        } else if (p == 22) {
            attr_ &= static_cast<std::uint8_t>(~kAttrBold);
        } else if (p == 24) {
            attr_ &= static_cast<std::uint8_t>(~kAttrUnderline);
        } else if (p == 27) {
            attr_ &= static_cast<std::uint8_t>(~kAttrReverse);
        } else if (p >= 30 && p <= 37) {
            set_fg_index(p - 30);
        } else if (p == 39) {
            fg_ = kDefaultColor;
        } else if (p >= 40 && p <= 47) {
            set_bg_index(p - 40);
        } else if (p == 49) {
            bg_ = kDefaultColor;
        } else if (p >= 90 && p <= 97) {
            set_fg_index(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            set_bg_index(p - 100 + 8);
        } else if (p == 38 || p == 48) {
            bool is_fg = (p == 38);
            if (i + 2 < params.size() && params[i + 1] == 5) {
                int idx = params[i + 2];
                if (is_fg) set_fg_index(idx);
                else set_bg_index(idx);
                i += 2;
            } else if (i + 4 < params.size() && params[i + 1] == 2) {
                std::uint32_t rgb = (static_cast<std::uint32_t>(params[i + 2] & 0xFF) << 16) |
                                    (static_cast<std::uint32_t>(params[i + 3] & 0xFF) << 8) |
                                    static_cast<std::uint32_t>(params[i + 4] & 0xFF);
                if (is_fg) fg_ = rgb;
                else bg_ = rgb;
                i += 4;
            }
        }
    }
}

void AnsiScreen::osc_dispatch() {
    std::size_t semi = osc_.find(';');
    if (semi != std::string::npos) {
        std::string code = osc_.substr(0, semi);
        if (code == "0" || code == "2") {
            std::string t = osc_.substr(semi + 1);
            title_.clear();
            for (std::size_t i = 0; i < t.size() && title_.size() < 200; ++i) {
                unsigned char c = static_cast<unsigned char>(t[i]);
                if (c >= 0x20) title_.push_back(t[i]);
            }
        }
    }
    osc_.clear();
}

}  // namespace term
}  // namespace sb3
