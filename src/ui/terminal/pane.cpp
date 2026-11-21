// pane.cpp — 窗格模型实现（S4）：拉取 ring、剥时间戳、喂解析器、行寻址与搜索。

#include "ui/terminal/pane.h"

#include <algorithm>
#include <cstring>

namespace sb3 {
namespace term {
namespace {

bool ts_digit(char c) { return c >= '0' && c <= '9'; }

bool ts_char_ok(std::size_t pos, char c) {
    if (pos == 2 || pos == 5) return c == ':';
    if (pos == 8) return c == '.';
    if (pos == 12) return c == ' ';
    return ts_digit(c);
}

// ring 里行首的时间戳固定 13 字节："HH:MM:SS.mmm "
constexpr std::size_t kTsLen = 13;

std::size_t ts_prefix(const std::string& s, std::size_t i) {
    std::size_t k = 0;
    while (i + k < s.size() && ts_char_ok(k, s[i + k])) ++k;
    return k;
}

}  // namespace

void Pane::init(const std::string& channel, int cols, int rows, std::size_t scroll_cap) {
    channel_ = channel;
    scrollback_.set_capacity(scroll_cap);
    scrollback_.clear();
    screen_.init(cols, rows);
    screen_.set_scroll_sink([this](Line&& line) { scrollback_.push(std::move(line)); });
    epoch_ = 0;
    seen_ = 0;
    parsed_bytes_ = 0;
    dropped_ = false;
    scroll_offset_ = 0;
    carry_.clear();
    at_line_start_ = true;
    has_hit_ = false;
}

void Pane::resize(int cols, int rows) {
    screen_.resize(cols, rows);
    scroll_offset_ = std::min(scroll_offset_, max_scroll());
}

std::string Pane::strip_timestamps(const std::string& data) {
    std::string work = carry_;
    carry_.clear();
    work += data;

    std::string out;
    out.reserve(work.size());
    std::size_t i = 0;
    while (i < work.size()) {
        if (esc_state_ == 0) {
            if (at_line_start_) {
                std::size_t pl = ts_prefix(work, i);
                if (pl >= kTsLen) {
                    i += kTsLen;
                    continue;                  // 整枚时间戳丢掉，仍在行首
                }
                if (pl > 0 && i + pl == work.size()) {
                    carry_ = work.substr(i);   // 可能是时间戳开头，等下一块
                    break;
                }
            }
            char c = work[i++];
            out.push_back(c);
            if (c == 0x1b) {
                esc_state_ = 1;                // ANSI 序列：不影响"行首有没有可见字符"
            } else if (c == '\n') {
                at_line_start_ = true;
            } else if (c >= 0x20) {
                at_line_start_ = false;
            }
            continue;
        }
        if (esc_state_ == 1) {
            char c = work[i++];
            out.push_back(c);
            if (c == '[') esc_state_ = 2;
            else if (c == ']') esc_state_ = 3;
            else esc_state_ = 0;
            continue;
        }
        if (esc_state_ == 2) {
            char c = work[i++];
            out.push_back(c);
            if (c >= 0x40 && c <= 0x7e) esc_state_ = 0;
            continue;
        }
        if (esc_state_ == 3) {
            char c = work[i++];
            out.push_back(c);
            if (c == 0x07) esc_state_ = 0;
            else if (c == 0x1b) esc_state_ = 4;
            continue;
        }
        char c = work[i++];
        out.push_back(c);
        esc_state_ = (c == '\\') ? 0 : 3;
    }
    return out;
}

void Pane::feed_bytes(const std::string& clean) {
    if (clean.empty()) return;
    screen_.feed(clean);
    if (scroll_offset_ > max_scroll()) scroll_offset_ = max_scroll();
}

bool Pane::pull(IBridge* bridge) {
    if (bridge == nullptr) return false;
    std::string raw = bridge->read_since(&epoch_, &seen_);
    if (raw.empty()) return false;
    parsed_bytes_ += raw.size();
    feed_bytes(strip_timestamps(raw));
    if (screen_.title().empty()) {
        // 标题由 OSC 维护，这里不动
    }
    return true;
}

void Pane::replay(IBridge* bridge) {
    reset_view();
    if (bridge == nullptr) return;
    std::string raw = bridge->read_all(&epoch_, &seen_);
    parsed_bytes_ += raw.size();
    feed_bytes(strip_timestamps(raw));
    scroll_to_bottom();
}

void Pane::reset_view() {
    screen_.reset_screen();
    esc_state_ = 0;
    scrollback_.clear();
    scroll_offset_ = 0;
    parsed_bytes_ = 0;
    dropped_ = false;
    carry_.clear();
    at_line_start_ = true;
    has_hit_ = false;
}

std::uint64_t Pane::last_abs() const {
    if (total_lines() == 0) return 0;
    return base_abs() + total_lines() - 1;
}

const Line* Pane::line_at(std::uint64_t abs) const {
    if (abs < screen_base_abs()) return scrollback_.at(abs);
    std::uint64_t row = abs - screen_base_abs();
    if (row >= static_cast<std::uint64_t>(screen_.rows())) return nullptr;
    return &screen_.lines()[static_cast<std::size_t>(row)];
}

std::string Pane::text_at(std::uint64_t abs) const {
    const Line* line = line_at(abs);
    return line ? line_text(*line) : std::string();
}

std::uint64_t Pane::cursor_abs() const { return screen_base_abs() + static_cast<std::uint64_t>(screen_.cursor_row()); }

int Pane::max_scroll() const {
    std::size_t total = total_lines();
    std::size_t rows = static_cast<std::size_t>(screen_.rows());
    if (total <= rows) return 0;
    return static_cast<int>(total - rows);
}

void Pane::scroll_by(int delta) {
    int v = scroll_offset_ + delta;
    if (v < 0) v = 0;
    int max_v = max_scroll();
    if (v > max_v) v = max_v;
    scroll_offset_ = v;
}

void Pane::scroll_to(int offset) {
    if (offset < 0) offset = 0;
    int max_v = max_scroll();
    if (offset > max_v) offset = max_v;
    scroll_offset_ = offset;
}

bool Pane::find(const std::string& needle, bool forward, Hit* hit, bool restart) {
    if (needle.empty()) return false;
    // restart = 从视口当前位置重新找（搜索框逐字输入用），不沿用上一次命中
    if (restart) has_hit_ = false;
    std::uint64_t lo = base_abs();
    std::uint64_t hi = last_abs();
    std::uint64_t from = has_hit_ ? (forward ? hit_.abs + 1 : (hit_.abs > 0 ? hit_.abs - 1 : 0))
                                  : (scroll_offset_ > 0 ? (hi - static_cast<std::uint64_t>(max_scroll() - scroll_offset_)) : lo);
    if (from < lo) from = lo;
    if (from > hi) from = forward ? lo : hi;

    std::uint64_t i = from;
    for (;;) {
        std::string t = text_at(i);
        std::size_t pos = t.find(needle);
        if (pos != std::string::npos) {
            // 字节偏移 → 列
            int cells = 0;
            std::size_t k = 0;
            while (k < pos && k < t.size()) {
                unsigned char b = static_cast<unsigned char>(t[k]);
                std::size_t len = 1;
                char32_t cp = b;
                if ((b & 0xE0) == 0xC0) { len = 2; cp = b & 0x1F; }
                else if ((b & 0xF0) == 0xE0) { len = 3; cp = b & 0x0F; }
                else if ((b & 0xF8) == 0xF0) { len = 4; cp = b & 0x07; }
                for (std::size_t m = 1; m < len && k + m < t.size(); ++m) {
                    cp = (cp << 6) | (static_cast<unsigned char>(t[k + m]) & 0x3F);
                }
                k += len;
                cells += codepoint_wide(cp) ? 2 : 1;
            }
            hit_ = Hit{};
            hit_.abs = i;
            hit_.col = cells;
            int needle_cells = 0;
            for (unsigned char c : needle) needle_cells += ((c & 0xC0) != 0x80) ? 1 : 0;
            hit_.len = needle_cells;
            has_hit_ = true;
            if (hit) *hit = hit_;
            int total = static_cast<int>(total_lines());
            int rows = screen_.rows();
            int offset = total - rows - static_cast<int>(i - base_abs());
            scroll_offset_ = std::max(0, std::min(offset, max_scroll()));
            return true;
        }
        if (forward) {
            if (i >= hi) break;
            ++i;
        } else {
            if (i <= lo) break;
            --i;
        }
    }
    return false;
}

void Pane::clear_search() { has_hit_ = false; }

}  // namespace term
}  // namespace sb3
