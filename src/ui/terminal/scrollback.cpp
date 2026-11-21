// scrollback.cpp — 回看缓冲实现（S4）。

#include "ui/terminal/scrollback.h"

namespace sb3 {
namespace term {
namespace {

// UTF-8 文本里的字节偏移 → 第几格（宽字符算两格）
int cells_before(const std::string& utf8, std::size_t byte_off) {
    int cells = 0;
    std::size_t i = 0;
    while (i < byte_off && i < utf8.size()) {
        unsigned char b = static_cast<unsigned char>(utf8[i]);
        char32_t cp = 0;
        std::size_t len = 1;
        if ((b & 0x80) == 0) {
            cp = b;
        } else if ((b & 0xE0) == 0xC0) {
            cp = b & 0x1F;
            len = 2;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0F;
            len = 3;
        } else if ((b & 0xF8) == 0xF0) {
            cp = b & 0x07;
            len = 4;
        } else {
            len = 1;
        }
        for (std::size_t k = 1; k < len && i + k < utf8.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        }
        i += len;
        cells += codepoint_wide(cp) ? 2 : 1;
    }
    return cells;
}

int text_cells(const std::string& utf8) { return cells_before(utf8, utf8.size()); }

}  // namespace

void Scrollback::set_capacity(std::size_t cap) {
    cap_ = cap < 8 ? 8 : (cap > 200000 ? 200000 : cap);
    while (lines_.size() > cap_) {
        lines_.pop_front();
        ++base_;
    }
}

void Scrollback::push(Line&& line) {
    lines_.push_back(std::move(line));
    while (lines_.size() > cap_) {
        lines_.pop_front();
        ++base_;
    }
}

void Scrollback::clear() {
    base_ += lines_.size();
    lines_.clear();
}

const Line* Scrollback::at(std::uint64_t abs) const {
    if (abs < base_ || abs >= end()) return nullptr;
    return &lines_[static_cast<std::size_t>(abs - base_)];
}

std::string Scrollback::text(std::uint64_t abs) const {
    const Line* line = at(abs);
    return line ? line_text(*line) : std::string();
}

bool Scrollback::find(const std::string& needle, std::uint64_t start, bool forward, Hit* hit) const {
    if (needle.empty() || lines_.empty()) return false;
    std::uint64_t lo = base_;
    std::uint64_t hi = end() - 1;
    std::uint64_t i = (start < lo) ? lo : (start > hi ? hi : start);

    for (;;) {
        const Line* line = at(i);
        if (line) {
            std::string t = line_text(*line);
            std::size_t pos = t.find(needle);
            if (pos != std::string::npos) {
                if (hit) {
                    hit->abs = i;
                    hit->col = cells_before(t, pos);
                    hit->len = text_cells(needle);
                }
                return true;
            }
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

}  // namespace term
}  // namespace sb3
