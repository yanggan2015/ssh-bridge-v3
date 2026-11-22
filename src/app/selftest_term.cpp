// selftest_term.cpp — 终端模型（ANSI 解析 + 回看）无窗口自检。
// 覆盖：基础写屏/CR/换行、SGR 颜色、擦除、光标定位、UTF-8 宽字符、
// 滚屏进回看、回看容量淘汰、搜索、OSC 标题。

#include "app/selftest_term.h"

#include <cstdio>
#include <string>

#include "ui/terminal/ansi.h"
#include "ui/terminal/input.h"
#include "ui/terminal/scrollback.h"
#include "ui/terminal/term_types.h"

namespace sb3 {
namespace {

int g_fail = 0;

void report(const char* id, const char* what, bool ok, const std::string& detail = "") {
    std::printf("%s %s %s%s%s\n", ok ? "PASS" : "FAIL", id, what, detail.empty() ? "" : " : ",
                detail.c_str());
    if (!ok) ++g_fail;
}

std::string row_text(const term::AnsiScreen& s, int row) {
    if (row < 0 || row >= s.rows()) return std::string();
    return term::line_text(s.lines()[static_cast<std::size_t>(row)]);
}

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06x", v & term::kColorMask);
    return std::string(buf);
}

void test_basic() {
    term::AnsiScreen s;
    s.init(20, 5);
    s.feed("abc\r\ndef");
    bool ok = row_text(s, 0) == "abc" && row_text(s, 1) == "def" && s.cursor_row() == 1 &&
              s.cursor_col() == 3;
    report("T4U-1", "写屏 + CRLF 换行", ok,
           "r0='" + row_text(s, 0) + "' r1='" + row_text(s, 1) + "'");

    term::AnsiScreen c;
    c.init(10, 2);
    c.feed("abcdef\rxyz");
    bool cr_ok = row_text(c, 0) == "xyzdef";
    report("T4U-1b", "裸 CR 重写（不换行）", cr_ok, "r0='" + row_text(c, 0) + "'");
}

void test_sgr() {
    term::AnsiScreen s;
    s.init(20, 2);
    s.feed("\x1b[31mRED\x1b[0mN");
    bool ok = false;
    if (s.lines()[0].cells.size() >= 4) {
        const term::Cell& c0 = s.lines()[0].cells[0];
        const term::Cell& c3 = s.lines()[0].cells[3];
        ok = c0.fg == term::palette_color(1) && c3.fg == term::kDefaultColor &&
             term::line_text(s.lines()[0]) == "REDN";
    }
    report("T4U-2", "SGR 前景色与复位", ok, "fg0=" + hex(s.lines()[0].cells[0].fg) +
                                               " fg3=" + hex(s.lines()[0].cells[3].fg));

    term::AnsiScreen b;
    b.init(20, 2);
    b.feed("\x1b[1;7;44mX\x1b[0m");
    const term::Cell& c = b.lines()[0].cells[0];
    bool attr_ok = (c.attr & term::kAttrBold) && (c.attr & term::kAttrReverse) &&
                   c.bg == term::palette_color(4);
    report("T4U-2b", "SGR 粗体/反显/背景", attr_ok, "attr=" + std::to_string(c.attr) +
                                                     " bg=" + hex(c.bg));
}

void test_erase_and_cursor() {
    term::AnsiScreen s;
    s.init(20, 4);
    s.feed("abcdef\x1b[3D\x1b[K");
    bool k_ok = row_text(s, 0) == "abc";
    report("T4U-3", "CSI K 擦到行尾", k_ok, "r0='" + row_text(s, 0) + "'");

    term::AnsiScreen j;
    j.init(20, 4);
    j.feed("abc\x1b[2JZ");
    bool j_ok = row_text(j, 0) == "   Z" && j.cursor_col() == 4;
    report("T4U-3b", "CSI 2J 整屏擦除", j_ok, "r0='" + row_text(j, 0) + "'");

    term::AnsiScreen h;
    h.init(20, 4);
    h.feed("\x1b[2;3HX");
    bool h_ok = row_text(h, 1) == "  X" && h.cursor_row() == 1 && h.cursor_col() == 3;
    report("T4U-4", "CSI H 光标定位", h_ok, "r1='" + row_text(h, 1) + "'");
}

void test_utf8() {
    term::AnsiScreen s;
    s.init(10, 2);
    s.feed("中文");
    bool ok = false;
    if (s.lines()[0].cells.size() >= 4) {
        ok = s.lines()[0].cells[0].ch == U'\u4e2d' && s.lines()[0].cells[1].wide_cont &&
             s.lines()[0].cells[2].ch == U'\u6587' && term::line_text(s.lines()[0]) == "中文";
    }
    report("T4U-5", "UTF-8 中文宽字符", ok, "r0='" + row_text(s, 0) + "'");

    term::AnsiScreen e;
    e.init(10, 2);
    // 4 字节 UTF-8：👋（U+1F44B），按两格算
    e.feed("\xF0\x9F\x91\x8B");
    bool emoji_ok = false;
    if (e.lines()[0].cells.size() >= 2) {
        emoji_ok = e.lines()[0].cells[0].ch == U'\U0001F44B' && e.lines()[0].cells[1].wide_cont;
    }
    report("T4U-5b", "UTF-8 表情宽字符", emoji_ok, "r0='" + row_text(e, 0) + "'");

    term::AnsiScreen cut;
    cut.init(10, 2);
    cut.feed("\xE4\xB8");   // 半个汉字（被切成两段）
    bool half_ok = row_text(cut, 0).empty();
    cut.feed("\xAD\xE6\x96\x87");
    report("T4U-5c", "跨块 UTF-8 不丢字", half_ok && row_text(cut, 0) == "中文",
           "r0='" + row_text(cut, 0) + "'");
}

void test_scrollback() {
    term::Scrollback sb(3);
    term::AnsiScreen s;
    s.init(10, 3);
    s.set_scroll_sink([&sb](term::Line&& line) { sb.push(std::move(line)); });
    s.feed("L1\r\nL2\r\nL3\r\nL4\r\n");
    bool ok = sb.count() == 2 && sb.text(sb.base()) == "L1" &&
              sb.text(sb.end() - 1) == "L2";
    report("T4U-6", "滚屏把首行进回看", ok, "count=" + std::to_string(sb.count()));

    term::Scrollback small(8);
    for (int i = 0; i < 12; ++i) {
        term::Line l;
        l.cells.assign(4, term::Cell());
        l.cells[0].ch = static_cast<char32_t>(U'a' + i);
        small.push(std::move(l));
    }
    bool evict = small.count() == 8 && small.base() == 4 && small.text(4) == "e";
    report("T4U-6b", "回看容量淘汰最老行", evict,
           "count=" + std::to_string(small.count()) + " base=" + std::to_string(small.base()));

    term::Scrollback h(100);
    for (const char* t : {"hello world", "second line", "third"}) {
        term::Line l;
        l.cells.assign(20, term::Cell());
        int col = 0;
        for (const char* p = t; *p; ++p, ++col) l.cells[static_cast<std::size_t>(col)].ch = *p;
        h.push(std::move(l));
    }
    term::Scrollback::Hit hit;
    bool found = h.find("world", 0, true, &hit) && hit.col == 6 && hit.len == 5;
    report("T4U-7", "回看搜索命中位置", found,
           "col=" + std::to_string(hit.col) + " len=" + std::to_string(hit.len));

    bool misses = !h.find("absent", 0, true, &hit);
    report("T4U-7b", "搜索无命中有确定返回", misses);
}

void test_autowrap() {
    term::AnsiScreen s;
    s.init(10, 3);
    s.feed("0123456789ABCDEF");
    bool ok = row_text(s, 0) == "0123456789" && row_text(s, 1) == "ABCDEF" &&
              s.cursor_row() == 1 && s.cursor_col() == 6;
    report("T4U-11", "满最后一列后延迟换行不覆盖", ok,
           "r0='" + row_text(s, 0) + "' r1='" + row_text(s, 1) + "' cursor=" +
               std::to_string(s.cursor_row()) + ":" + std::to_string(s.cursor_col()));
}

void test_osc_and_control() {
    term::AnsiScreen s;
    s.init(20, 3);
    s.feed("\x1b]0;root@board: ~\x07$ ");
    bool ok = s.title() == "root@board: ~" && row_text(s, 0) == "$";
    report("T4U-8", "OSC 标题 / 序列不外显", ok, "title='" + s.title() + "'");

    term::AnsiScreen t;
    t.init(20, 3);
    t.feed("ab\tc");
    bool tab_ok = row_text(t, 0) == "ab      c";
    report("T4U-9", "TAB 按 8 列对齐", tab_ok, "r0='" + row_text(t, 0) + "'");

    term::AnsiScreen n;
    n.init(8, 2);
    n.feed("\x1b[?25lX");
    bool hide_ok = !n.cursor_visible() && row_text(n, 0) == "X";
    report("T4U-10", "光标显隐 CSI ?25", hide_ok);
}

void test_keys() {
    using term::encode_key;
    bool ok = encode_key(VK_UP, false, false, false) == "\x1b[A" &&
              encode_key(VK_UP, true, false, false) == "\x1b[1;5A" &&
              encode_key(VK_UP, false, true, false) == "\x1b[1;3A" &&
              encode_key(VK_UP, false, false, true) == "\x1b[1;2A" &&
              encode_key(VK_TAB, false, false, false) == "\t" &&
              encode_key(VK_TAB, false, false, true) == "\x1b[Z" &&
              encode_key(VK_BACK, false, false, false) == "\x7f" &&
              encode_key(VK_ESCAPE, false, false, false) == "\x1b" &&
              encode_key(VK_DELETE, false, false, false) == "\x1b[3~" &&
              encode_key(VK_DELETE, true, false, false) == "\x1b[3;5~" &&
              encode_key(VK_F1, false, false, false) == "\x1b[11~" &&
              encode_key(VK_F1, false, false, true) == "\x1b[11;2~" &&
              term::encode_char('c', true, false) == "\x03" &&
              term::encode_char('[', true, false) == "\x1b";
    report("T4U-12", "控制键/方向键/功能键序列", ok,
           "up/ctrl/shift/alt/tab/back/esc/delete/F1");
}

}  // namespace

int term_selftest_main(const std::vector<std::string>& args) {
    (void)args;
    test_basic();
    test_sgr();
    test_erase_and_cursor();
    test_utf8();
    test_scrollback();
    test_autowrap();
    test_osc_and_control();
    test_keys();
    std::printf("RESULT: %s\n", g_fail == 0 ? "ALL PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}

}  // namespace sb3
