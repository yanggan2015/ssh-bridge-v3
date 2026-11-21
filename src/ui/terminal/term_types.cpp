// term_types.cpp — 行文本 / 颜色表 / 宽度判定（纯函数）。

#include "ui/terminal/term_types.h"

namespace sb3 {
namespace term {
namespace {

const char* base16[16] = {
    "000000", "cd0000", "00cd00", "cdcd00", "0000ee", "cd00cd", "00cdcd", "e5e5e5",
    "7f7f7f", "ff0000", "00ff00", "ffff00", "5c5cff", "ff00ff", "00ffff", "ffffff",
};

std::uint32_t from_hex(const char* hex) {
    std::uint32_t v = 0;
    for (int i = 0; i < 6; ++i) {
        char c = hex[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
    }
    return v;
}

std::uint32_t cube_level(int v) {
    if (v == 0) return 0;
    return static_cast<std::uint32_t>(55 + 40 * v);
}

}  // namespace

std::uint32_t palette_color(int index) {
    if (index < 0) return kDefaultColor;
    if (index < 16) return from_hex(base16[index]);
    if (index < 232) {
        int i = index - 16;
        std::uint32_t r = cube_level(i / 36);
        std::uint32_t g = cube_level((i % 36) / 6);
        std::uint32_t b = cube_level(i % 6);
        return (r << 16) | (g << 8) | b;
    }
    if (index < 256) {
        std::uint32_t v = static_cast<std::uint32_t>(8 + (index - 232) * 10);
        return (v << 16) | (v << 8) | v;
    }
    return kDefaultColor;
}

bool codepoint_wide(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
           (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
           (cp >= 0xA960 && cp <= 0xA97F) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE19) ||
           (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
           (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

std::string line_text(const Line& line, bool keep_trailing) {
    int last = static_cast<int>(line.cells.size()) - 1;
    if (!keep_trailing) {
        while (last >= 0) {
            const Cell& c = line.cells[static_cast<std::size_t>(last)];
            if (c.wide_cont) {
                --last;
                continue;
            }
            if (c.ch != U' ' || c.bg != kDefaultColor) break;
            --last;
        }
    }
    std::string out;
    char32_t pending_high = 0;
    for (int i = 0; i <= last; ++i) {
        char32_t cp = line.cells[static_cast<std::size_t>(i)].ch;
        if (cp == 0) continue;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            pending_high = cp;
            continue;
        }
        if (cp >= 0xDC00 && cp <= 0xDFFF && pending_high) {
            cp = 0x10000 + ((pending_high - 0xD800) << 10) + (cp - 0xDC00);
            pending_high = 0;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

int line_cells(const Line& line) {
    return static_cast<int>(line.cells.size());
}

}  // namespace term
}  // namespace sb3
