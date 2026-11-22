// view.cpp — 终端窗格视图实现（S4，第一部分：窗口/字体/布局/同步）。

#include "ui/terminal/view.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <cjson/cJSON.h>

#include "core/util/util.h"
#include "ui/terminal/input.h"
#include "ui/terminal/term_types.h"
#include "ui/ui_messages.h"

namespace sb3 {
namespace {

const wchar_t kViewClass[] = L"SB3_TermView";
const wchar_t kPrefsPath[] = L"output\\ui_prefs.ini";

COLORREF rgb(std::uint32_t c) {
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

COLORREF fg_color(std::uint32_t c) { return c == term::kDefaultColor ? RGB(0xD4, 0xD4, 0xD4) : rgb(c); }
COLORREF bg_color(std::uint32_t c) { return c == term::kDefaultColor ? RGB(0x12, 0x12, 0x12) : rgb(c); }

}  // namespace

int load_terminal_font_px(int fallback) {
    wchar_t buf[16] = L"";
    GetPrivateProfileStringW(L"terminal", L"font", L"", buf, 16, kPrefsPath);
    int v = _wtoi(buf);
    if (v >= 8 && v <= 40) return v;
    return fallback;
}

void save_terminal_font_px(int px) {
    util::ensure_dir("output");
    WritePrivateProfileStringW(L"terminal", L"font", std::to_wstring(px).c_str(), kPrefsPath);
}

bool TerminalView::create(HINSTANCE inst, HWND parent, AppCore* core, const Options& opt, int dpi) {
    inst_ = inst;
    core_ = core;
    opt_ = opt;
    dpi_ = dpi > 0 ? dpi : 96;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &TerminalView::WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_IBEAM);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kViewClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    brush_bg_     = CreateSolidBrush(RGB(0x12, 0x12, 0x12));
    brush_header_ = CreateSolidBrush(RGB(0x1E, 0x1E, 0x1E));
    brush_active_ = CreateSolidBrush(RGB(0x3A, 0x8D, 0xDE));
    brush_sel_    = CreateSolidBrush(RGB(0x26, 0x4F, 0x78));
    brush_hit_    = CreateSolidBrush(RGB(0x8B, 0x6A, 0x00));

    int split_n = (opt_.split == 4) ? 4 : (opt_.split == 2 ? 2 : 1);
    slots_.resize(static_cast<std::size_t>(split_n));
    active_ = 0;

    hwnd_ = CreateWindowExW(0, kViewClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                            0, 0, 0, 0, parent, nullptr, inst, this);
    if (!hwnd_) return false;

    ensure_font();
    HDC dc = GetDC(hwnd_);
    measure(dc);
    ReleaseDC(hwnd_, dc);
    return true;
}

void TerminalView::ensure_font() {
    int px = std::max(9, std::min(40, opt_.font_px));
    opt_.font_px = px;
    if (font_) DeleteObject(font_);
    font_ = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!font_) font_ = static_cast<HFONT>(GetStockObject(ANSI_FIXED_FONT));
}

void TerminalView::measure(HDC dc) {
    if (!dc || !font_) return;
    HGDIOBJ old = SelectObject(dc, font_);
    SIZE sz{};
    GetTextExtentPoint32W(dc, L"M", 1, &sz);
    cell_w_ = std::max(4, static_cast<int>(sz.cx));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    cell_h_ = tm.tmHeight + 2;
    header_h_ = tm.tmHeight + MulDiv(6, dpi_, 96);
    SelectObject(dc, old);
}

RECT TerminalView::slot_rect(int idx, const RECT& client) const {
    int gap = 4;
    int x0 = client.left + gap;
    int y0 = client.top + gap;
    int w = client.right - client.left - gap * 2;
    int h = client.bottom - client.top - gap * 2;
    int n = static_cast<int>(slots_.size());
    RECT rc{};
    if (n <= 1) {
        rc = RECT{x0, y0, x0 + w, y0 + h};
    } else if (n == 2) {
        int half = (w - gap) / 2;
        rc = (idx == 0) ? RECT{x0, y0, x0 + half, y0 + h}
                        : RECT{x0 + half + gap, y0, x0 + w, y0 + h};
    } else {
        int half_w = (w - gap) / 2;
        int half_h = (h - gap) / 2;
        int col = idx % 2;
        int row = idx / 2;
        rc.left = x0 + col * (half_w + gap);
        rc.top = y0 + row * (half_h + gap);
        rc.right = rc.left + half_w;
        rc.bottom = rc.top + half_h;
    }
    return rc;
}

int TerminalView::slot_at(POINT pt) const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        RECT rc = slot_rect(static_cast<int>(i), client);
        if (PtInRect(&rc, pt)) return static_cast<int>(i);
    }
    return -1;
}

void TerminalView::sync_slot(Slot& slot, const RECT& rc) {
    if (slot.channel.empty()) return;
    RECT body{rc.left + 1, rc.top + header_h_, rc.right - 1, rc.bottom - 1};
    int cols = std::max(8, static_cast<int>(body.right - body.left) / std::max(1, cell_w_));
    int rows = std::max(2, static_cast<int>(body.bottom - body.top) / std::max(1, cell_h_));
    int old_cols = slot.pane ? slot.pane->screen().cols() : 0;
    int old_rows = slot.pane ? slot.pane->screen().rows() : 0;
    IBridge* b = core_ ? core_->find(slot.channel) : nullptr;
    if (!slot.pane) {
        slot.pty_cols = 0;
        slot.pty_rows = 0;
        slot.pane.reset(new term::Pane());
        slot.pane->init(slot.channel, cols, rows, opt_.scrollback);
        if (b) slot.pane->replay(b);       // 首次打开显示尾部历史
    } else {
        slot.pane->resize(cols, rows);
    }
    if (old_cols != cols || old_rows != rows) sync_pty(slot);
}

void TerminalView::sync_pty(Slot& slot) {
    if (!slot.pane || slot.channel.empty() || !core_) return;
    IBridge* b = core_->find(slot.channel);
    if (!b || !b->running()) return;       // 等通道 ready 后再补发，避免首次 resize 被丢
    int cols = slot.pane->screen().cols();
    int rows = slot.pane->screen().rows();
    if (cols == slot.pty_cols && rows == slot.pty_rows) return;
    if (b->resize_terminal(cols, rows)) {
        slot.pty_cols = cols;
        slot.pty_rows = rows;
    }
}

void TerminalView::set_channel(const std::string& name) {
    if (slots_.empty()) return;
    Slot& slot = slots_[static_cast<std::size_t>(active_)];
    if (slot.channel == name && slot.pane) {
        slot.pane->scroll_to_bottom();
        slot.pane->clear_search();
        invalidate();
        return;
    }
    set_slot_channel(active_, name);
}

void TerminalView::set_slot_channel(int idx, const std::string& name) {
    if (idx < 0 || idx >= static_cast<int>(slots_.size())) return;
    Slot& slot = slots_[static_cast<std::size_t>(idx)];
    if (slot.channel == name && slot.pane) {
        slot.pane->scroll_to_bottom();
        slot.pane->clear_search();
        return;
    }
    slot.channel = name;
    slot.pane.reset();                     // 重建：独立游标，从尾部历史开始
    RECT client{};
    GetClientRect(hwnd_, &client);
    ensure_font();
    HDC dc = GetDC(hwnd_);
    if (dc) {
        measure(dc);
        sync_slot(slot, slot_rect(idx, client));
        ReleaseDC(hwnd_, dc);
    }
    invalidate();
}

void TerminalView::restore_layout(const std::vector<std::string>& channels, int active) {
    if (slots_.empty()) return;
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        const std::string name = static_cast<std::size_t>(i) < channels.size()
                                ? channels[static_cast<std::size_t>(i)] : std::string();
        set_slot_channel(i, name);
    }
    active_ = std::max(0, std::min(active, static_cast<int>(slots_.size()) - 1));
    invalidate();
}

std::vector<std::string> TerminalView::slot_channels() const {
    std::vector<std::string> out;
    out.reserve(slots_.size());
    for (const Slot& slot : slots_) out.push_back(slot.channel);
    return out;
}

std::string TerminalView::active_channel() const {
    if (slots_.empty()) return std::string();
    return slots_[static_cast<std::size_t>(active_)].channel;
}

bool TerminalView::set_active(int idx) {
    if (slots_.empty()) return false;
    int want = std::max(0, std::min(idx, static_cast<int>(slots_.size()) - 1));
    if (want == active_) return false;
    active_ = want;
    invalidate();
    notify_active_changed();
    return true;
}

void TerminalView::notify_active_changed() {
    if (!hwnd_) return;
    HWND parent = GetParent(hwnd_);
    if (parent && IsWindow(parent))
        PostMessageW(parent, kMsgActivePaneChanged, static_cast<WPARAM>(active_), 0);
}

void TerminalView::normalize_active() {
    if (active_ < 0) active_ = 0;
    if (active_ >= static_cast<int>(slots_.size())) active_ = static_cast<int>(slots_.size()) - 1;
}

void TerminalView::split(int n) {
    int want = (n == 4) ? 4 : (n == 2 ? 2 : 1);
    int old_count = static_cast<int>(slots_.size());
    if (want == old_count) return;
    std::vector<std::string> old_channels = slot_channels();
    int old_active = active_;
    slots_.resize(static_cast<std::size_t>(want));
    active_ = (old_active < want) ? old_active : 0;
    for (int i = 0; i < want; ++i) {
        Slot& slot = slots_[static_cast<std::size_t>(i)];
        slot.channel.clear();
        slot.pane.reset();
    }
    for (int i = 0; i < want && i < old_count; ++i) {
        if (!old_channels[static_cast<std::size_t>(i)].empty())
            set_slot_channel(i, old_channels[static_cast<std::size_t>(i)]);
    }
    invalidate();
    notify_active_changed();
}

bool TerminalView::pull_visible() {
    if (!hwnd_ || !IsWindowVisible(hwnd_)) return false;
    RECT client{};
    GetClientRect(hwnd_, &client);
    bool any = false;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (!slot.pane || slot.channel.empty()) continue;
        RECT rc = slot_rect(static_cast<int>(i), client);
        if (rc.bottom - rc.top < cell_h_ * 2) continue;
        sync_slot(slot, rc);
        sync_pty(slot);            // 通道 ready 后补发首次 PTY 尺寸
        IBridge* b = core_ ? core_->find(slot.channel) : nullptr;
        if (b && slot.pane->pull(b)) any = true;
        slot.pane->resize(std::max(8, static_cast<int>(rc.right - rc.left - 2) / std::max(1, cell_w_)),
                          std::max(2, static_cast<int>(rc.bottom - rc.top - header_h_ - 2) / std::max(1, cell_h_)));
    }
    if (any) invalidate();
    return any;
}

void TerminalView::prune_channels(const std::vector<std::string>& names) {
    bool changed = false;
    for (Slot& slot : slots_) {
        if (slot.channel.empty()) continue;
        bool alive = false;
        for (const std::string& n : names) {
            if (n == slot.channel) { alive = true; break; }
        }
        if (!alive) {
            slot.channel.clear();
            slot.pane.reset();
            changed = true;
        }
    }
    if (changed) invalidate();
}

void TerminalView::clear_active() {
    if (slots_.empty()) return;
    Slot& slot = slots_[static_cast<std::size_t>(active_)];
    if (slot.pane) slot.pane->reset_view();
    sel_.has = false;
    invalidate();
}

void TerminalView::replay_active() {
    if (slots_.empty()) return;
    Slot& slot = slots_[static_cast<std::size_t>(active_)];
    IBridge* b = core_ ? core_->find(slot.channel) : nullptr;
    if (slot.pane && b) {
        slot.pane->replay(b);
        slot.pane->scroll_to_bottom();
        invalidate();
    }
}

std::string TerminalView::info() const {
    if (slots_.empty()) return std::string();
    const Slot& slot = slots_[static_cast<std::size_t>(active_)];
    if (!slot.pane) return std::string();
    // RX 计数外显：串口没接板子/波特率不对时，这里会一直是 0，用户不必猜
    std::uint64_t rx = slot.pane->rx_bytes();
    char rxs[24];
    if (rx >= 1024)
        std::snprintf(rxs, sizeof(rxs), "%.1fKB", static_cast<double>(rx) / 1024.0);
    else
        std::snprintf(rxs, sizeof(rxs), "%lluB", static_cast<unsigned long long>(rx));
    char buf[192];
    IBridge* bridge = core_ ? core_->find(slot.channel) : nullptr;
    std::uint64_t latency = bridge ? bridge->echo_latency_us() : 0;
    if (latency > 0)
        std::snprintf(buf, sizeof(buf), "%s · %dx%d · 回看 %d 行 · RX %s · 回显 %.2fms",
                      slot.channel.c_str(), slot.pane->screen().cols(),
                      slot.pane->screen().rows(),
                      static_cast<int>(slot.pane->scrollback().count()), rxs,
                      static_cast<double>(latency) / 1000.0);
    else
        std::snprintf(buf, sizeof(buf), "%s · %dx%d · 回看 %d 行 · RX %s", slot.channel.c_str(),
                      slot.pane->screen().cols(), slot.pane->screen().rows(),
                      static_cast<int>(slot.pane->scrollback().count()), rxs);
    return std::string(buf);
}

std::string TerminalView::state_json() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "split", static_cast<int>(slots_.size()));
    cJSON_AddNumberToObject(root, "active", active_);
    cJSON_AddNumberToObject(root, "font_px", opt_.font_px);
    cJSON* arr = cJSON_AddArrayToObject(root, "panes");
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const Slot& slot = slots_[i];
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "channel", slot.channel.c_str());
        cJSON_AddBoolToObject(o, "active", static_cast<int>(i) == active_);
        const bool has_channels = core_ && !core_->entries().empty();
        if (slot.channel.empty()) {
            cJSON_AddStringToObject(o, "empty_hint",
                                    has_channels ? "在左侧通道树选择一条通道"
                                                 : "没有通道，点“新建”创建一条");
        }
        if (slot.pane) {
            const term::Pane& p = *slot.pane;
            cJSON_AddNumberToObject(o, "cols", p.screen().cols());
            cJSON_AddNumberToObject(o, "rows", p.screen().rows());
            cJSON_AddNumberToObject(o, "scrollback", static_cast<int>(p.scrollback().count()));
            cJSON_AddNumberToObject(o, "total", static_cast<int>(p.total_lines()));
            cJSON_AddNumberToObject(o, "offset", p.scroll_offset());
            cJSON_AddNumberToObject(o, "cursor_row", p.screen().cursor_row());
            cJSON_AddNumberToObject(o, "cursor_col", p.screen().cursor_col());
            cJSON_AddStringToObject(o, "title", p.screen().title().c_str());
            cJSON_AddNumberToObject(o, "bytes", static_cast<double>(p.rx_bytes()));
            cJSON_AddBoolToObject(o, "search_hit", p.has_search());
            if (p.has_search()) {
                cJSON* h = cJSON_AddObjectToObject(o, "hit");
                cJSON_AddNumberToObject(h, "abs", static_cast<double>(p.search_hit().abs));
                cJSON_AddNumberToObject(h, "col", p.search_hit().col);
                cJSON_AddNumberToObject(h, "len", p.search_hit().len);
            }
            std::size_t total = p.total_lines();
            std::size_t from = total > 40 ? total - 40 : 0;
            cJSON* fg = cJSON_AddArrayToObject(o, "fg_sample");
            int seen = 0;
            for (std::size_t k = from; k < total && seen < 12; ++k) {
                const term::Line* ln = p.line_at(p.base_abs() + k);
                if (!ln) continue;
                for (const term::Cell& cell : ln->cells) {
                    if (cell.fg == term::kDefaultColor) continue;
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "%06x", static_cast<unsigned>(cell.fg & 0xFFFFFF));
                    cJSON_AddItemToArray(fg, cJSON_CreateString(hex));
                    if (++seen >= 12) break;
                }
            }
            cJSON* lines = cJSON_AddArrayToObject(o, "lines");
            for (std::size_t k = from; k < total; ++k) {
                std::uint64_t abs = p.base_abs() + k;
                std::string text = p.text_at(abs);
                if (text.empty() && k + 1 == total) continue;   // 末尾空行不塞
                cJSON_AddItemToArray(lines, cJSON_CreateString(text.c_str()));
            }
            cJSON_AddItemToArray(lines, cJSON_CreateString(""));
        }
        cJSON_AddItemToArray(arr, o);
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

void TerminalView::focus_active() { if (hwnd_) SetFocus(hwnd_); }

void TerminalView::invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

int TerminalView::cell_col_at(const term::Pane& pane, int x, RECT body) const {
    int col = (x - body.left) / std::max(1, cell_w_);
    if (col < 0) col = 0;
    if (col >= pane.screen().cols()) col = pane.screen().cols() - 1;
    return col;
}

std::uint64_t TerminalView::cell_row_at(const term::Pane& pane, int y, RECT body) const {
    int rows = pane.screen().rows();
    int row_in_view = (y - body.top) / std::max(1, cell_h_);
    if (row_in_view < 0) row_in_view = 0;
    if (row_in_view >= rows) row_in_view = rows - 1;
    int offset = pane.scroll_offset();
    std::uint64_t bottom = pane.last_abs();
    int bottom_row = rows - 1 - offset;
    std::int64_t target = static_cast<std::int64_t>(bottom) - (bottom_row - row_in_view);
    if (target < 0) return 0;
    return static_cast<std::uint64_t>(target);
}

void TerminalView::begin_selection(int slot, POINT pt) {
    if (slot < 0) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    RECT rc = slot_rect(slot, client);
    Slot& s = slots_[static_cast<std::size_t>(slot)];
    if (!s.pane) return;
    RECT body{rc.left + 1, rc.top + header_h_, rc.right - 1, rc.bottom - 1};
    sel_.has = true;
    sel_.slot = slot;
    sel_.anchor = cell_row_at(*s.pane, pt.y, body);
    sel_.anchor_col = cell_col_at(*s.pane, pt.x, body);
    sel_.cur = sel_.anchor;
    sel_.cur_col = sel_.anchor_col;
}

void TerminalView::extend_selection(POINT pt) {
    if (!sel_.has || sel_.slot != active_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    RECT rc = slot_rect(sel_.slot, client);
    Slot& s = slots_[static_cast<std::size_t>(sel_.slot)];
    if (!s.pane) return;
    RECT body{rc.left + 1, rc.top + header_h_, rc.right - 1, rc.bottom - 1};
    sel_.cur = cell_row_at(*s.pane, pt.y, body);
    sel_.cur_col = cell_col_at(*s.pane, pt.x, body);
}

namespace {

void append_cells(const term::Line& line, int from_col, int to_col, std::string* out) {
    int end = to_col < 0 ? static_cast<int>(line.cells.size()) - 1 : to_col;
    if (from_col < 0) from_col = 0;
    for (int c = from_col; c <= end && c < static_cast<int>(line.cells.size()); ++c) {
        const term::Cell& cell = line.cells[static_cast<std::size_t>(c)];
        if (cell.wide_cont) continue;
        if (cell.ch < 0x80) {
            out->push_back(static_cast<char>(cell.ch));
        } else if (cell.ch < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cell.ch >> 6)));
            out->push_back(static_cast<char>(0x80 | (cell.ch & 0x3F)));
        } else if (cell.ch < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cell.ch >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cell.ch >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cell.ch & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cell.ch >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cell.ch >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cell.ch >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cell.ch & 0x3F)));
        }
    }
}

struct SelView {
    bool          has = false;
    std::uint64_t anchor = 0;
    std::uint64_t cur = 0;
    int           anchor_col = 0;
    int           cur_col = 0;
};

bool in_selection(const SelView& sel, std::uint64_t abs, int col) {
    if (!sel.has) return false;
    std::uint64_t a = std::min(sel.anchor, sel.cur);
    std::uint64_t b = std::max(sel.anchor, sel.cur);
    if (abs < a || abs > b) return false;
    if (a == b) return col >= std::min(sel.anchor_col, sel.cur_col) &&
                         col <= std::max(sel.anchor_col, sel.cur_col);
    if (abs == a) return col >= (sel.anchor < sel.cur ? sel.anchor_col : sel.cur_col);
    if (abs == b) return col <= (sel.anchor < sel.cur ? sel.cur_col : sel.anchor_col);
    return true;
}

}  // namespace

std::string TerminalView::selection_text() const {
    if (!sel_.has || sel_.slot < 0 || sel_.slot >= static_cast<int>(slots_.size())) return std::string();
    const Slot& s = slots_[static_cast<std::size_t>(sel_.slot)];
    if (!s.pane) return std::string();
    std::uint64_t a = std::min(sel_.anchor, sel_.cur);
    std::uint64_t b = std::max(sel_.anchor, sel_.cur);
    int a_col = (sel_.anchor <= sel_.cur) ? sel_.anchor_col : sel_.cur_col;
    int b_col = (sel_.anchor <= sel_.cur) ? sel_.cur_col : sel_.anchor_col;

    std::string out;
    for (std::uint64_t abs = a; abs <= b; ++abs) {
        const term::Line* line = s.pane->line_at(abs);
        if (!line) continue;
        if (abs == a && abs == b) append_cells(*line, a_col, b_col, &out);
        else if (abs == a) append_cells(*line, a_col, -1, &out);
        else if (abs == b) append_cells(*line, 0, b_col, &out);
        else append_cells(*line, 0, -1, &out);
        if (abs != b) out += "\r\n";
    }
    return out;
}

bool TerminalView::copy_to_clipboard(const std::string& utf8) {
    if (utf8.empty()) return false;
    std::wstring wide = term::utf8_to_utf16(utf8);
    if (!OpenClipboard(hwnd_)) return false;
    EmptyClipboard();
    bool ok = false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
    if (mem) {
        void* dst = GlobalLock(mem);
        if (dst) {
            std::memcpy(dst, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            if (!ok) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    return ok;
}

bool TerminalView::has_selection() const {
    return !selection_text().empty();
}

bool TerminalView::copy_selection() {
    std::string text = selection_text();
    if (text.empty()) return false;
    return copy_to_clipboard(text);
}

bool TerminalView::paste_clipboard() {
    if (!OpenClipboard(hwnd_)) return false;
    std::string utf8;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
        if (p) {
            utf8 = term::utf16_to_utf8(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (utf8.empty()) return false;
    return paste_text(utf8);
}

bool TerminalView::paste_text(const std::string& utf8) {
    if (utf8.empty() || slots_.empty()) return false;
    const Slot& slot = slots_[static_cast<std::size_t>(active_)];
    if (slot.channel.empty()) return false;
    if (!core_ || !core_->find(slot.channel)) return false;
    const bool bracketed = slot.pane && slot.pane->screen().bracketed_paste();
    send_active(term::encode_paste(utf8, bracketed));
    return true;
}

void TerminalView::send_active(const std::string& bytes) {
    if (bytes.empty() || slots_.empty()) return;
    Slot& slot = slots_[static_cast<std::size_t>(active_)];
    if (slot.channel.empty()) return;
    IBridge* b = core_ ? core_->find(slot.channel) : nullptr;
    if (!b) return;
    b->write(bytes.data(), bytes.size());
}

bool TerminalView::type_text(const std::string& utf8, bool submit) {
    if (slots_.empty()) return false;
    std::string bytes;
    for (std::size_t i = 0; i < utf8.size();) {
        unsigned char b = static_cast<unsigned char>(utf8[i]);
        std::size_t len = 1;
        unsigned int cp = b;
        if ((b & 0xE0) == 0xC0) { len = 2; cp = b & 0x1F; }
        else if ((b & 0xF0) == 0xE0) { len = 3; cp = b & 0x0F; }
        else if ((b & 0xF8) == 0xF0) { len = 4; cp = b & 0x07; }
        for (std::size_t k = 1; k < len && i + k < utf8.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        i += len;
        bytes += term::encode_codepoint(cp);
    }
    if (submit) bytes += line_ending_bytes();
    if (bytes.empty()) return false;
    send_active(bytes);
    invalidate();
    return true;
}

bool TerminalView::send_raw(const std::string& bytes) {
    if (slots_.empty() || bytes.empty()) return false;
    send_active(bytes);
    invalidate();
    return true;
}

std::string TerminalView::line_ending_bytes() const {
    std::string mode = "crlf";
    std::string channel;
    if (core_ && !slots_.empty()) {
        channel = slots_[static_cast<std::size_t>(active_)].channel;
        mode = core_->line_ending_for(channel);
    }
    if (mode == "none") return std::string();
    // SSH PTY 是 Unix 行规程：CR 与 LF 都会各自提交一行，发 CRLF 会多出一个空行/空提示符。
    // 因此 SSH 下 CRLF 归一为单个 CR；显式 CR / LF 仍按配置发一个字节。
    if (core_ && !channel.empty() && core_->is_ssh_name(channel)) {
        if (mode == "lf") return "\n";
        return "\r";
    }
    if (mode == "cr") return "\r";
    if (mode == "lf") return "\n";
    return "\r\n";
}

void TerminalView::scroll_active(int lines) {
    if (slots_.empty() || lines == 0) return;
    Slot& slot = slots_[static_cast<std::size_t>(active_)];
    if (!slot.pane) return;
    slot.pane->scroll_by(lines);
    invalidate();
}

void TerminalView::scroll_active_to(int offset) {
    if (slots_.empty()) return;
    Slot& slot = slots_[static_cast<std::size_t>(active_)];
    if (!slot.pane) return;
    slot.pane->scroll_to(offset);
    invalidate();
}

int TerminalView::active_offset() const {
    if (slots_.empty()) return 0;
    const Slot& slot = slots_[static_cast<std::size_t>(active_)];
    return slot.pane ? slot.pane->scroll_offset() : 0;
}

int TerminalView::active_max_scroll() const {
    if (slots_.empty()) return 0;
    const Slot& slot = slots_[static_cast<std::size_t>(active_)];
    return slot.pane ? slot.pane->max_scroll() : 0;
}

bool TerminalView::find(const std::string& needle, bool forward, bool restart) {
    if (slots_.empty()) return false;
    Slot& slot = slots_[static_cast<std::size_t>(active_)];
    if (!slot.pane) return false;
    term::Pane::Hit hit;
    bool ok = slot.pane->find(needle, forward, &hit, restart);
    invalidate();
    return ok;
}

void TerminalView::clear_search() {
    if (slots_.empty()) return;
    Slot& slot = slots_[static_cast<std::size_t>(active_)];
    if (!slot.pane) return;
    slot.pane->clear_search();
    invalidate();
}

void TerminalView::zoom(int delta) {
    int px = std::max(9, std::min(40, opt_.font_px + delta));
    if (px == opt_.font_px) return;
    opt_.font_px = px;
    save_terminal_font_px(px);
    ensure_font();
    HDC dc = GetDC(hwnd_);
    measure(dc);
    ReleaseDC(hwnd_, dc);
    RECT client{};
    GetClientRect(hwnd_, &client);
    for (std::size_t i = 0; i < slots_.size(); ++i) sync_slot(slots_[i], slot_rect(static_cast<int>(i), client));
    invalidate();
}

void TerminalView::draw_run(HDC dc, int x, int y, const std::wstring& text, int run_cols,
                            std::uint32_t fg, std::uint32_t bg, std::uint8_t attr, bool selected) {
    int w = run_cols * cell_w_;
    bool reverse = (attr & term::kAttrReverse) != 0;
    COLORREF back = selected ? RGB(0x26, 0x4F, 0x78) : bg_color(bg);
    COLORREF fore = selected ? RGB(0xFF, 0xFF, 0xFF) : fg_color(fg);
    if (reverse) std::swap(back, fore);
    if (bg != term::kDefaultColor || selected) {
        RECT r{x, y, x + w, y + cell_h_};
        SetDCBrushColor(dc, back);
        FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    SetTextColor(dc, fore);
    SetBkMode(dc, TRANSPARENT);
    if (!text.empty()) TextOutW(dc, x, y, text.c_str(), static_cast<int>(text.size()));
}

void TerminalView::draw_all(HDC dc) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRect(dc, &client, brush_bg_);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        draw_slot(dc, static_cast<int>(i), slot_rect(static_cast<int>(i), client),
                  static_cast<int>(i) == active_);
    }
}

void TerminalView::draw_slot(HDC dc, int idx, RECT rc, bool active) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    Slot& slot = slots_[static_cast<std::size_t>(idx)];

    SetDCBrushColor(dc, active ? RGB(0x3A, 0x8D, 0xDE) : RGB(0x2C, 0x2C, 0x2C));
    FrameRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

    RECT head{rc.left + 1, rc.top + 1, rc.right - 1, rc.top + header_h_};
    FillRect(dc, &head, brush_header_);
    SetTextColor(dc, active ? RGB(0xDC, 0xE8, 0xF6) : RGB(0x9A, 0x9A, 0x9A));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font_);

    std::wstring title = L"（空）";
    const bool has_channels = core_ && !core_->entries().empty();
    if (!has_channels) title = L"（无通道）";
    if (!slot.channel.empty()) {
        std::string info = slot.channel;
        if (slot.pane) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "  ·  %dx%d  ·  回看 %d", slot.pane->screen().cols(),
                          slot.pane->screen().rows(),
                          static_cast<int>(slot.pane->scrollback().count()));
            info += buf;
        }
        IBridge* b = core_ ? core_->find(slot.channel) : nullptr;
        info += (b && b->running()) ? "  ·  在线" : "  ·  离线";
        title = term::utf8_to_utf16(info);
    }
    RECT head_text{head.left + 6, head.top, head.right - 6, head.bottom};
    DrawTextW(dc, title.c_str(), -1, &head_text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT body{rc.left + 1, rc.top + header_h_, rc.right - 1, rc.bottom - 1};
    FillRect(dc, &body, brush_bg_);

    if (!slot.pane) {
        SetTextColor(dc, RGB(0x6A, 0x6A, 0x6A));
        std::wstring hint = has_channels ? L"在左侧通道树选择一条通道"
                                         : L"没有通道，点工具栏“新建”创建一条";
        DrawTextW(dc, hint.c_str(), -1, &body, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    term::Pane& pane = *slot.pane;
    int cols = pane.screen().cols();
    int rows = pane.screen().rows();
    int offset = pane.scroll_offset();
    std::int64_t abs0 = static_cast<std::int64_t>(pane.screen_base_abs()) - offset;

    SelView selv;
    selv.has = sel_.has && sel_.slot == idx;
    selv.anchor = sel_.anchor;
    selv.cur = sel_.cur;
    selv.anchor_col = sel_.anchor_col;
    selv.cur_col = sel_.cur_col;
    term::Pane::Hit hit = pane.search_hit();
    bool has_hit = pane.has_search();

    std::int64_t y = body.top;
    for (int r = 0; r < rows && y < body.bottom; ++r, y += cell_h_) {
        std::int64_t abs = abs0 + r;
        const term::Line* line = (abs >= 0) ? pane.line_at(static_cast<std::uint64_t>(abs)) : nullptr;
        int c = 0;
        while (c < cols) {
            const term::Cell* first = nullptr;
            if (line && c < static_cast<int>(line->cells.size())) first = &line->cells[static_cast<std::size_t>(c)];
            std::uint32_t fg = first ? first->fg : term::kDefaultColor;
            std::uint32_t bg = first ? first->bg : term::kDefaultColor;
            std::uint8_t attr = first ? first->attr : 0;
            bool selected = in_selection(selv, static_cast<std::uint64_t>(abs < 0 ? 0 : abs), c);
            if (has_hit && abs == static_cast<std::int64_t>(hit.abs) && c >= hit.col && c < hit.col + hit.len) {
                bg = 0x8B6A00;
                selected = false;
            }
            std::wstring text;
            int run_cols = 0;
            while (c < cols) {
                const term::Cell* cell = nullptr;
                if (line && c < static_cast<int>(line->cells.size())) {
                    cell = &line->cells[static_cast<std::size_t>(c)];
                }
                std::uint32_t cfg = cell ? cell->fg : term::kDefaultColor;
                std::uint32_t cbg = cell ? cell->bg : term::kDefaultColor;
                std::uint8_t cattr = cell ? cell->attr : 0;
                bool csel = in_selection(selv, static_cast<std::uint64_t>(abs < 0 ? 0 : abs), c);
                bool chit = has_hit && abs == static_cast<std::int64_t>(hit.abs) && c >= hit.col &&
                            c < hit.col + hit.len;
                std::uint32_t want_bg = chit ? 0x8B6A00 : cbg;
                bool want_sel = chit ? false : csel;
                if (cfg != fg || want_bg != bg || cattr != attr || want_sel != selected) break;
                if (cell && !cell->wide_cont && cell->ch != 0) {
                    if (cell->ch < 0x10000) {
                        text.push_back(static_cast<wchar_t>(cell->ch));
                    } else {
                        char32_t v = cell->ch - 0x10000;
                        text.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
                        text.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
                    }
                }
                ++c;
                ++run_cols;
            }
            if (run_cols == 0) {
                ++c;
                ++run_cols;
            }
            int x = body.left + (c - run_cols) * cell_w_;
            draw_run(dc, x, static_cast<int>(y), text, run_cols, fg, bg, attr, selected);
        }
    }

    if (offset == 0 && pane.cursor_visible()) {
        std::uint64_t cursor_abs = pane.cursor_abs();
        if (cursor_abs >= static_cast<std::uint64_t>(abs0) && cursor_abs < static_cast<std::uint64_t>(abs0 + rows)) {
            int row = static_cast<int>(cursor_abs - static_cast<std::uint64_t>(abs0));
            int x = body.left + pane.cursor_col() * cell_w_;
            int y = body.top + row * cell_h_;
            HBRUSH caret = CreateSolidBrush(active ? RGB(0xD4, 0xD4, 0xD4) : RGB(0x8A, 0x8A, 0x8A));
            RECT cr{x, y, x + 2, y + cell_h_};
            FillRect(dc, &cr, caret);
            DeleteObject(caret);
        }
    }

    int max_scroll = pane.max_scroll();
    if (max_scroll > 0) {
        RECT track{body.right - 6, body.top, body.right, body.bottom};
        FillRect(dc, &track, brush_header_);
        int h = body.bottom - body.top;
        int thumb_h = std::max(12, h * rows / static_cast<int>(pane.total_lines()));
        int travel = h - thumb_h;
        int pos = max_scroll > 0 ? (max_scroll - offset) * travel / max_scroll : 0;
        RECT thumb{body.right - 6, body.top + pos, body.right, body.top + pos + thumb_h};
        SetDCBrushColor(dc, RGB(0x55, 0x55, 0x55));
        FillRect(dc, &thumb, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
}

void TerminalView::on_wheel(POINT pt, int delta) {
    if (GetKeyState(VK_CONTROL) & 0x8000) {
        zoom(delta > 0 ? 1 : -1);
        return;
    }
    int slot = slot_at(pt);
    if (slot < 0) slot = active_;
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) return;
    Slot& s = slots_[static_cast<std::size_t>(slot)];
    if (!s.pane) return;
    s.pane->scroll_by(delta > 0 ? 3 : -3);
    invalidate();
}

void TerminalView::show_menu(POINT pt) {
    enum { kCopy = 5001, kPaste = 5002, kClear = 5003, kReplay = 5004,
           kSplit1 = 5010, kSplit2 = 5011, kSplit4 = 5012, kZoomIn = 5020, kZoomOut = 5021 };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCopy, L"复制\tCtrl+Shift+C");
    AppendMenuW(menu, MF_STRING, kPaste, L"粘贴\tCtrl+Shift+V");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kClear, L"清屏");
    AppendMenuW(menu, MF_STRING, kReplay, L"加载历史会话");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kSplit1, L"单窗格");
    AppendMenuW(menu, MF_STRING, kSplit2, L"左右两窗格");
    AppendMenuW(menu, MF_STRING, kSplit4, L"四窗格");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kZoomIn, L"放大字体\tCtrl+滚轮");
    AppendMenuW(menu, MF_STRING, kZoomOut, L"缩小字体");
    int cmd = static_cast<int>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                              hwnd_, nullptr));
    DestroyMenu(menu);
    switch (cmd) {
    case kCopy: copy_selection(); break;
    case kPaste: paste_clipboard(); break;
    case kClear: clear_active(); break;
    case kReplay: replay_active(); break;
    case kSplit1: split(1); break;
    case kSplit2: split(2); break;
    case kSplit4: split(4); break;
    case kZoomIn: zoom(1); break;
    case kZoomOut: zoom(-1); break;
    default: break;
    }
}

LRESULT CALLBACK TerminalView::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    TerminalView* self = reinterpret_cast<TerminalView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    self->hwnd_ = hwnd;
    return self->handle(msg, wp, lp);
}

LRESULT TerminalView::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd_, 1, 200, nullptr);
        return 0;

    case WM_SIZE: {
        HDC dc = GetDC(hwnd_);
        measure(dc);
        ReleaseDC(hwnd_, dc);
        RECT client{};
        GetClientRect(hwnd_, &client);
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            sync_slot(slots_[i], slot_rect(static_cast<int>(i), client));
        }
        InvalidateRect(hwnd_, nullptr, FALSE);   // 尺寸变化后整窗重绘，避免露出未画的旧区域
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        draw_all(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_TIMER:
        // 兜底：kick 消息丢了也要跟上数据；窗口不可见时直接跳过（不耗 CPU）
        if (!IsWindowVisible(hwnd_)) return 0;
        pull_visible();
        return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        invalidate();
        return 0;

    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (ctrl && (wp == 'C' || wp == 'V')) {
            if (wp == 'V') {
                paste_clipboard();
                swallow_char_ = true;
                return 0;
            }
            // 有选区才复制；没有选区时放行，让 Ctrl+C 作为 SIGINT 发给远端
            if (copy_selection()) {
                swallow_char_ = true;
                return 0;
            }
        }
        if (term::is_special_key(static_cast<UINT>(wp))) {
            send_active(term::encode_key(static_cast<UINT>(wp), ctrl, alt, shift));
            // Backspace / Tab 的真实按键同时会生成 WM_CHAR，必须吞掉，
            // 否则一次退格会发两个 DEL、一次 Tab 会发两个制表符。
            if (wp == VK_BACK || wp == VK_TAB) swallow_char_ = true;
            return 0;
        }
        return 0;
    }

    case WM_CHAR: {
        if (swallow_char_) {
            swallow_char_ = false;
            return 0;
        }
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (ctrl && (wp == 3 || wp == 22)) return 0;
        if (wp == L'\r' || wp == L'\n') {
            send_active(line_ending_bytes());
            return 0;
        }
        std::string bytes = term::encode_char(static_cast<unsigned int>(wp), ctrl, alt);
        if (alt && !ctrl && !bytes.empty()) bytes.insert(0, "\x1b");  // Alt+字符 → ESC 前缀
        send_active(bytes);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int slot = slot_at(pt);
        if (slot >= 0) {
            set_active(slot);
            SetFocus(hwnd_);
            begin_selection(slot, pt);
            SetCapture(hwnd_);
            invalidate();
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd_ && sel_.has) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            extend_selection(pt);
            invalidate();
        }
        return 0;

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd_) ReleaseCapture();
        return 0;

    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int slot = slot_at(pt);
        if (slot >= 0) set_active(slot);
        ClientToScreen(hwnd_, &pt);
        show_menu(pt);
        invalidate();
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        on_wheel(pt, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd_, 1);
        if (font_) DeleteObject(font_);
        if (brush_bg_) DeleteObject(brush_bg_);
        if (brush_header_) DeleteObject(brush_header_);
        if (brush_active_) DeleteObject(brush_active_);
        if (brush_sel_) DeleteObject(brush_sel_);
        if (brush_hit_) DeleteObject(brush_hit_);
        font_ = nullptr;
        brush_bg_ = brush_header_ = brush_active_ = brush_sel_ = brush_hit_ = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace sb3
