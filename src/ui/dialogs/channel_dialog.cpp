// channel_dialog.cpp — S6 通道表单：字段联动、就地校验、测试打开、保存即热应用。

#include <winsock2.h>
#include <ws2tcpip.h>
#include <commctrl.h>
#include <windowsx.h>

#include "ui/dialogs/channel_dialog.h"


#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/util/util.h"
#include "ui/terminal/input.h"

namespace sb3 {
namespace {

std::wstring widen(const std::string& s) { return term::utf8_to_utf16(s); }

void add_combo_items(HWND combo, const wchar_t* const* items, int n, int sel) {
    for (int i = 0; i < n; ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i]));
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
}

const wchar_t* kKinds[]      = {L"串口", L"SSH"};
const wchar_t* kDatabits[]   = {L"5", L"6", L"7", L"8"};
const wchar_t* kParities[]   = {L"无 (N)", L"偶 (E)", L"奇 (O)", L"标记 (M)", L"空 (S)"};
const wchar_t* kStopbits[]   = {L"1", L"1.5", L"2"};
const wchar_t* kFlows[]      = {L"无", L"RTS/CTS", L"XON/XOFF"};
const wchar_t* kLineEnds[]   = {L"CRLF", L"LF", L"CR", L"无"};
const wchar_t* kEncodings[]  = {L"UTF-8", L"GBK"};

// ---- 选择优先（Dropdown-First）：能用下拉就不让用户盲输；必须自由输入的用可编辑下拉 ----
std::vector<std::wstring> serial_port_items() {
    std::vector<std::wstring> out;
    wchar_t target[512] = L"";
    for (int i = 1; i <= 256; ++i) {
        wchar_t name[16] = L"";
        swprintf_s(name, L"COM%d", i);
        if (QueryDosDeviceW(name, target, 512)) out.push_back(name);
    }
    out.push_back(L"loop:ai");
    out.push_back(L"loop:demo");
    return out;
}

std::vector<std::wstring> number_items(std::initializer_list<int> nums) {
    std::vector<std::wstring> out;
    for (int n : nums) out.push_back(std::to_wstring(n));
    return out;
}

void add_wide_items(HWND combo, const std::vector<std::wstring>& items) {
    for (const std::wstring& s : items)
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
}

// 刷新端口下拉：保留用户已填内容，只换候选项（有真实串口就出现在列表里）
void fill_port_combo(HWND combo) {
    if (!combo) return;
    wchar_t cur[256] = L"";
    GetWindowTextW(combo, cur, 256);
    std::vector<std::wstring> items = serial_port_items();
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    add_wide_items(combo, items);
    SetWindowTextW(combo, cur);
    SendMessageW(combo, CB_SETMINVISIBLE, 8, 0);
}

}  // namespace

bool ChannelDialog::open(HINSTANCE inst, HWND parent, AppCore* core, HFONT font, int dpi,
                         const std::string& edit_name, bool force_ssh, AppliedFn on_applied) {
    if (is_open()) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        return true;
    }
    inst_       = inst;
    parent_     = parent;
    core_       = core;
    font_       = font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    dpi_        = dpi > 0 ? dpi : 96;
    editing_    = !edit_name.empty();
    edit_name_  = edit_name;
    on_applied_ = std::move(on_applied);
    delete_armed_ = false;
    hint_.clear();

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = chan_dlg::kClass;
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }

    int w = MulDiv(552, dpi_, 96);
    int h = MulDiv(300, dpi_, 96);
    RECT adj{0, 0, w, h};
    AdjustWindowRectEx(&adj, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
    int ow = adj.right - adj.left;
    int oh = adj.bottom - adj.top;
    RECT pr{};
    if (parent && GetWindowRect(parent, &pr)) {
        int x = pr.left + ((pr.right - pr.left) - ow) / 2;
        int y = pr.top + ((pr.bottom - pr.top) - oh) / 2;
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, chan_dlg::kClass,
                                editing_ ? L"编辑通道" : L"新建通道",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, ow, oh, parent,
                                nullptr, inst, this);
    } else {
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, chan_dlg::kClass,
                                editing_ ? L"编辑通道" : L"新建通道",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                                CW_USEDEFAULT, ow, oh, parent, nullptr, inst, this);
    }
    if (!hwnd_) return false;

    build(force_ssh);
    ShowWindow(hwnd_, SW_SHOW);
    layout();
    SetForegroundWindow(hwnd_);
    return true;
}

void ChannelDialog::close() {
    if (is_open()) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void ChannelDialog::build(bool force_ssh) {
    auto label = [&](const wchar_t* text, int id) {
        HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst_, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return h;
    };
    auto edit = [&](const wchar_t* text, int id, DWORD extra) {
        HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra,
                                 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst_, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return h;
    };
    auto combo = [&](int id) {
        HWND h = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                     CBS_DROPDOWNLIST, 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst_, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return h;
    };
    auto combo_edit = [&](const wchar_t* text, int id, const std::vector<std::wstring>& items) {
        HWND h = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                     CBS_DROPDOWN | CBS_AUTOHSCROLL, 0, 0, 0, 240, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst_, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        add_wide_items(h, items);
        SetWindowTextW(h, text);
        SendMessageW(h, CB_SETMINVISIBLE, 8, 0);
        return h;
    };
    auto check = [&](const wchar_t* text, int id) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst_, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return h;
    };
    auto button = [&](const wchar_t* text, int id, DWORD style) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                                 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst_, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return h;
    };

    label(L"名称", chan_dlg::kIdNameLabel);
    edit(L"", chan_dlg::kIdName, 0);

    label(L"类型", chan_dlg::kIdKindLabel);
    HWND kind = combo(chan_dlg::kIdKind);
    add_combo_items(kind, kKinds, 2, force_ssh ? 1 : 0);
    if (editing_) EnableWindow(kind, FALSE);   // 改类型 = 删除后新建，避免身份混淆

    label(L"串口参数", chan_dlg::kIdSerialCap);
    label(L"SSH 参数", chan_dlg::kIdSshCap);

    label(L"设备", chan_dlg::kIdDeviceLabel);
    HWND devc = combo_edit(L"", chan_dlg::kIdDevice, serial_port_items());
    fill_port_combo(devc);
    button(L"刷新", chan_dlg::kIdRefresh, BS_PUSHBUTTON);
    button(L"测试", chan_dlg::kIdTest, BS_PUSHBUTTON);

    label(L"波特率", chan_dlg::kIdBaudLabel);
    combo_edit(L"115200", chan_dlg::kIdBaud,
               number_items({9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600,
                             1500000, 2000000, 3000000}));
    label(L"数据位", chan_dlg::kIdDataLabel);
    HWND db = combo(chan_dlg::kIdDatabits);
    add_combo_items(db, kDatabits, 4, 3);
    label(L"校验", chan_dlg::kIdParityLabel);
    HWND parity = combo(chan_dlg::kIdParity);
    add_combo_items(parity, kParities, 5, 0);

    label(L"停止位", chan_dlg::kIdStopLabel);
    HWND stop = combo(chan_dlg::kIdStopbits);
    add_combo_items(stop, kStopbits, 3, 0);
    label(L"流控", chan_dlg::kIdFlowLabel);
    HWND flow = combo(chan_dlg::kIdFlow);
    add_combo_items(flow, kFlows, 3, 0);
    label(L"行尾", chan_dlg::kIdLineLabel);
    HWND le = combo(chan_dlg::kIdLineEnd);
    add_combo_items(le, kLineEnds, 4, 0);
    label(L"编码", chan_dlg::kIdEncLabel);
    HWND enc = combo(chan_dlg::kIdEncoding);
    add_combo_items(enc, kEncodings, 2, 0);

    check(L"自动打开", chan_dlg::kIdAutoOpen);
    check(L"DTR", chan_dlg::kIdDtr);
    check(L"RTS", chan_dlg::kIdRts);
    check(L"日志", chan_dlg::kIdLog);
    if (!editing_) {
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdAutoOpen), BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdDtr), BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdRts), BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdLog), BM_SETCHECK, BST_CHECKED, 0);
    }

    label(L"主机", chan_dlg::kIdHostLabel);
    edit(L"", chan_dlg::kIdHost, 0);
    label(L"端口", chan_dlg::kIdPortLabel);
    combo_edit(L"22", chan_dlg::kIdPort, number_items({22, 2022, 2222, 8022, 52222}));
    label(L"用户", chan_dlg::kIdUserLabel);
    edit(L"root", chan_dlg::kIdUser, 0);
    label(L"密码", chan_dlg::kIdPassLabel);
    edit(L"", chan_dlg::kIdPassword, ES_PASSWORD);
    label(L"term", chan_dlg::kIdTermLabel);
    combo_edit(L"xterm-256color", chan_dlg::kIdTerm,
               std::vector<std::wstring>{L"xterm-256color", L"xterm", L"vt100", L"ansi", L"linux"});
    label(L"超时(ms)", chan_dlg::kIdTimeoutLabel);
    combo_edit(L"5000", chan_dlg::kIdTimeout, number_items({1000, 3000, 5000, 10000, 30000}));
    label(L"列", chan_dlg::kIdWidthLabel);
    combo_edit(L"120", chan_dlg::kIdWidth, number_items({80, 100, 120, 160, 200}));
    label(L"行", chan_dlg::kIdHeightLabel);
    combo_edit(L"32", chan_dlg::kIdHeight, number_items({24, 30, 32, 40, 48}));
    label(L"保活(s)", chan_dlg::kIdKeepLabel);
    combo_edit(L"3", chan_dlg::kIdKeepalive, number_items({0, 3, 5, 10, 30}));

    label(L"", chan_dlg::kIdHint);
    button(L"删除", chan_dlg::kIdDelete, BS_PUSHBUTTON);
    button(L"取消", chan_dlg::kIdCancel, BS_PUSHBUTTON);
    button(L"保存", chan_dlg::kIdSave, BS_DEFPUSHBUTTON);

    // 分组只做视觉分界：分组框先建，子控件后建并压在它上面
    HWND serial_box = CreateWindowExW(0, L"BUTTON", L"串口参数",
                                      WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd_,
                                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(chan_dlg::kIdSerialBox)),
                                      inst_, nullptr);
    HWND ssh_box = CreateWindowExW(0, L"BUTTON", L"SSH 参数",
                                   WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<UINT_PTR>(chan_dlg::kIdSshBox)),
                                   inst_, nullptr);
    SendMessageW(serial_box, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(ssh_box,    WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    HWND serial_cap = GetDlgItem(hwnd_, chan_dlg::kIdSerialCap);
    HWND ssh_cap    = GetDlgItem(hwnd_, chan_dlg::kIdSshCap);
    SetWindowTextW(serial_cap, L"串口参数");
    SetWindowTextW(ssh_cap,    L"SSH 参数");
    ShowWindow(serial_box, SW_HIDE);
    ShowWindow(ssh_box, SW_HIDE);
    ShowWindow(serial_cap, SW_HIDE);
    ShowWindow(ssh_cap, SW_HIDE);

    if (editing_) load_from_config();

    apply_kind_visibility();
    layout();
    set_hint(L"", true);
}

void ChannelDialog::layout() {
    int pad = MulDiv(12, dpi_, 96);
    int row_h = MulDiv(26, dpi_, 96);
    int gap = MulDiv(8, dpi_, 96);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = rc.right;
    int h = rc.bottom;
    auto mv = [&](int id, int x, int y, int cw, int ch) {
        if (HWND c = GetDlgItem(hwnd_, id)) MoveWindow(c, x, y, cw, ch, TRUE);
    };

    // 第一行：名称 + 类型
    mv(chan_dlg::kIdNameLabel, pad, pad + MulDiv(4, dpi_, 96), MulDiv(56, dpi_, 96), row_h);
    mv(chan_dlg::kIdName, pad + MulDiv(60, dpi_, 96), pad,
       w - (pad + MulDiv(60, dpi_, 96)) - MulDiv(160, dpi_, 96), row_h);
    mv(chan_dlg::kIdKindLabel, w - MulDiv(152, dpi_, 96), pad + MulDiv(4, dpi_, 96),
       MulDiv(44, dpi_, 96), row_h);
    mv(chan_dlg::kIdKind, w - MulDiv(104, dpi_, 96), pad, MulDiv(92, dpi_, 96), row_h);

    int top = pad + row_h + pad;
    int box_h = MulDiv(162, dpi_, 96);
    int bw = w - pad * 2;
    bool ssh = is_ssh();

    mv(ssh ? chan_dlg::kIdSshBox : chan_dlg::kIdSerialBox, pad, top, bw, box_h);
    int ix = pad + MulDiv(10, dpi_, 96);
    int iw = bw - MulDiv(20, dpi_, 96);
    int y = top + MulDiv(24, dpi_, 96);

    if (ssh) {
        mv(chan_dlg::kIdHostLabel, ix, y + MulDiv(4, dpi_, 96), MulDiv(40, dpi_, 96), row_h);
        mv(chan_dlg::kIdHost, ix + MulDiv(44, dpi_, 96), y,
           iw - MulDiv(44, dpi_, 96) - MulDiv(150, dpi_, 96), row_h);
        mv(chan_dlg::kIdPortLabel, ix + iw - MulDiv(144, dpi_, 96), y + MulDiv(4, dpi_, 96),
           MulDiv(36, dpi_, 96), row_h);
        mv(chan_dlg::kIdPort, ix + iw - MulDiv(102, dpi_, 96), y, MulDiv(102, dpi_, 96), row_h);

        y += row_h + gap;
        mv(chan_dlg::kIdUserLabel, ix, y + MulDiv(4, dpi_, 96), MulDiv(40, dpi_, 96), row_h);
        mv(chan_dlg::kIdUser, ix + MulDiv(44, dpi_, 96), y, MulDiv(140, dpi_, 96), row_h);
        mv(chan_dlg::kIdPassLabel, ix + MulDiv(192, dpi_, 96), y + MulDiv(4, dpi_, 96),
           MulDiv(36, dpi_, 96), row_h);
        mv(chan_dlg::kIdPassword, ix + MulDiv(232, dpi_, 96), y, MulDiv(140, dpi_, 96), row_h);

        y += row_h + gap;
        mv(chan_dlg::kIdTermLabel, ix, y + MulDiv(4, dpi_, 96), MulDiv(40, dpi_, 96), row_h);
        mv(chan_dlg::kIdTerm, ix + MulDiv(44, dpi_, 96), y, MulDiv(100, dpi_, 96), row_h);
        mv(chan_dlg::kIdTimeoutLabel, ix + MulDiv(152, dpi_, 96), y + MulDiv(4, dpi_, 96),
           MulDiv(60, dpi_, 96), row_h);
        mv(chan_dlg::kIdTimeout, ix + MulDiv(216, dpi_, 96), y, MulDiv(60, dpi_, 96), row_h);
        mv(chan_dlg::kIdWidthLabel, ix + MulDiv(284, dpi_, 96), y + MulDiv(4, dpi_, 96),
           MulDiv(24, dpi_, 96), row_h);
        mv(chan_dlg::kIdWidth, ix + MulDiv(310, dpi_, 96), y, MulDiv(46, dpi_, 96), row_h);
        mv(chan_dlg::kIdHeightLabel, ix + MulDiv(364, dpi_, 96), y + MulDiv(4, dpi_, 96),
           MulDiv(24, dpi_, 96), row_h);
        mv(chan_dlg::kIdHeight, ix + MulDiv(390, dpi_, 96), y, MulDiv(46, dpi_, 96), row_h);

        y += row_h + gap;
        mv(chan_dlg::kIdKeepLabel, ix, y + MulDiv(4, dpi_, 96), MulDiv(80, dpi_, 96), row_h);
        mv(chan_dlg::kIdKeepalive, ix + MulDiv(84, dpi_, 96), y, MulDiv(60, dpi_, 96), row_h);
        mv(chan_dlg::kIdLog, ix + MulDiv(160, dpi_, 96), y, MulDiv(80, dpi_, 96), row_h);
    } else {
        mv(chan_dlg::kIdDeviceLabel, ix, y + MulDiv(4, dpi_, 96), MulDiv(40, dpi_, 96), row_h);
        mv(chan_dlg::kIdDevice, ix + MulDiv(44, dpi_, 96), y,
           iw - MulDiv(44, dpi_, 96) - MulDiv(72, dpi_, 96), row_h);
        mv(chan_dlg::kIdRefresh, ix + iw - MulDiv(64, dpi_, 96), y, MulDiv(64, dpi_, 96), row_h);

        y += row_h + gap;
        mv(chan_dlg::kIdBaudLabel, ix, y + MulDiv(4, dpi_, 96), MulDiv(48, dpi_, 96), row_h);
        mv(chan_dlg::kIdBaud, ix + MulDiv(52, dpi_, 96), y, MulDiv(96, dpi_, 96), row_h);
        mv(chan_dlg::kIdDataLabel, ix + MulDiv(158, dpi_, 96), y + MulDiv(4, dpi_, 96),
           MulDiv(48, dpi_, 96), row_h);
        mv(chan_dlg::kIdDatabits, ix + MulDiv(206, dpi_, 96), y, MulDiv(56, dpi_, 96), row_h);
        mv(chan_dlg::kIdParityLabel, ix + MulDiv(270, dpi_, 96), y + MulDiv(4, dpi_, 96),
           MulDiv(40, dpi_, 96), row_h);
        mv(chan_dlg::kIdParity, ix + MulDiv(310, dpi_, 96), y, MulDiv(84, dpi_, 96), row_h);

        y += row_h + gap;
        mv(chan_dlg::kIdStopLabel, ix, y + MulDiv(4, dpi_, 96), MulDiv(48, dpi_, 96), row_h);
        mv(chan_dlg::kIdStopbits, ix + MulDiv(52, dpi_, 96), y, MulDiv(56, dpi_, 96), row_h);
        mv(chan_dlg::kIdFlowLabel, ix + MulDiv(116, dpi_, 96), y + MulDiv(4, dpi_, 96),
           MulDiv(36, dpi_, 96), row_h);
        mv(chan_dlg::kIdFlow, ix + MulDiv(156, dpi_, 96), y, MulDiv(84, dpi_, 96), row_h);
        mv(chan_dlg::kIdLineLabel, ix + MulDiv(248, dpi_, 96), y + MulDiv(4, dpi_, 96),
           MulDiv(36, dpi_, 96), row_h);
        mv(chan_dlg::kIdLineEnd, ix + MulDiv(284, dpi_, 96), y, MulDiv(72, dpi_, 96), row_h);
        mv(chan_dlg::kIdEncLabel, ix + MulDiv(364, dpi_, 96), y + MulDiv(4, dpi_, 96),
           MulDiv(36, dpi_, 96), row_h);
        mv(chan_dlg::kIdEncoding, ix + MulDiv(400, dpi_, 96), y, MulDiv(76, dpi_, 96), row_h);

        y += row_h + gap;
        mv(chan_dlg::kIdAutoOpen, ix, y, MulDiv(100, dpi_, 96), row_h);
        mv(chan_dlg::kIdDtr, ix + MulDiv(108, dpi_, 96), y, MulDiv(60, dpi_, 96), row_h);
        mv(chan_dlg::kIdRts, ix + MulDiv(174, dpi_, 96), y, MulDiv(60, dpi_, 96), row_h);
        mv(chan_dlg::kIdLog, ix + MulDiv(244, dpi_, 96), y, MulDiv(80, dpi_, 96), row_h);
    }

    int btn_h = MulDiv(30, dpi_, 96);
    int btn_w = MulDiv(84, dpi_, 96);
    int by = h - pad - btn_h;
    mv(chan_dlg::kIdSave,   w - pad - btn_w, by, btn_w, btn_h);
    mv(chan_dlg::kIdCancel, w - pad - btn_w * 2 - gap, by, btn_w, btn_h);
    mv(chan_dlg::kIdDelete, w - pad - btn_w * 3 - gap * 2, by, btn_w, btn_h);
    mv(chan_dlg::kIdTest,   pad, by, btn_w, btn_h);
    mv(chan_dlg::kIdHint, pad, by - MulDiv(24, dpi_, 96), w - pad * 2, MulDiv(20, dpi_, 96));
    ShowWindow(GetDlgItem(hwnd_, chan_dlg::kIdDelete), editing_ ? SW_SHOWNA : SW_HIDE);
}

void ChannelDialog::apply_kind_visibility() {
    bool ssh = is_ssh();
    struct Pair { int ctl; bool serial_only; };
    const Pair set[] = {
        {chan_dlg::kIdDevice, true}, {chan_dlg::kIdBaud, true}, {chan_dlg::kIdDatabits, true},
        {chan_dlg::kIdParity, true}, {chan_dlg::kIdStopbits, true}, {chan_dlg::kIdFlow, true},
        {chan_dlg::kIdLineEnd, true}, {chan_dlg::kIdEncoding, true}, {chan_dlg::kIdAutoOpen, true},
        {chan_dlg::kIdDtr, true}, {chan_dlg::kIdRts, true}, {chan_dlg::kIdTest, false},
        {chan_dlg::kIdHost, false}, {chan_dlg::kIdPort, false}, {chan_dlg::kIdUser, false},
        {chan_dlg::kIdPassword, false}, {chan_dlg::kIdTerm, false}, {chan_dlg::kIdTimeout, false},
        {chan_dlg::kIdWidth, false}, {chan_dlg::kIdHeight, false}, {chan_dlg::kIdKeepalive, false},
    };
    for (const Pair& p : set) {
        HWND h = GetDlgItem(hwnd_, p.ctl);
        if (!h) continue;
        bool show = p.serial_only ? !ssh : ssh;
        if (p.ctl == chan_dlg::kIdTest) show = true;
        ShowWindow(h, show ? SW_SHOWNA : SW_HIDE);
    }
    // 串口专用 label / 分组框，成对显示
    const int serial_labels[] = {3102, 3104, 3105, 3106, 3116, 3117, 3118, 3119, 3103};
    for (int id : serial_labels) {
        if (HWND h = GetDlgItem(hwnd_, id)) ShowWindow(h, ssh ? SW_HIDE : SW_SHOWNA);
    }
    const int ssh_labels[] = {3107, 3108, 3109, 3110, 3111, 3112, 3113, 3114, 3115};
    for (int id : ssh_labels) {
        if (HWND h = GetDlgItem(hwnd_, id)) ShowWindow(h, ssh ? SW_SHOWNA : SW_HIDE);
    }
    if (HWND h = GetDlgItem(hwnd_, chan_dlg::kIdSerialCap)) ShowWindow(h, SW_HIDE);
    if (HWND h = GetDlgItem(hwnd_, chan_dlg::kIdSshCap))    ShowWindow(h, SW_HIDE);
    ShowWindow(GetDlgItem(hwnd_, chan_dlg::kIdSerialBox), ssh ? SW_HIDE : SW_SHOWNA);
    ShowWindow(GetDlgItem(hwnd_, chan_dlg::kIdSshBox),    ssh ? SW_SHOWNA : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd_, chan_dlg::kIdLog), SW_SHOWNA);   // 两种类型都记录日志
}

bool ChannelDialog::is_ssh() const {
    HWND kind = GetDlgItem(hwnd_, chan_dlg::kIdKind);
    return kind && SendMessageW(kind, CB_GETCURSEL, 0, 0) == 1;
}

void ChannelDialog::load_from_config() {
    if (!core_) return;
    set_text(chan_dlg::kIdName, widen(edit_name_));
    if (SSHLinkConfig s; core_->link_of(edit_name_, &s)) {
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdKind), CB_SETCURSEL, 1, 0);
        set_text(chan_dlg::kIdHost, widen(s.host));
        set_text(chan_dlg::kIdPort, std::to_wstring(s.port));
        set_text(chan_dlg::kIdUser, widen(s.username));
        set_text(chan_dlg::kIdPassword, widen(s.password));
        set_text(chan_dlg::kIdTerm, widen(s.term));
        set_text(chan_dlg::kIdTimeout, std::to_wstring(s.connect_timeout_ms));
        set_text(chan_dlg::kIdWidth, std::to_wstring(s.width));
        set_text(chan_dlg::kIdHeight, std::to_wstring(s.height));
        set_text(chan_dlg::kIdKeepalive, std::to_wstring(s.keepalive_s));
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdLog), BM_SETCHECK,
                     s.log_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    } else if (BoardConfig b; core_->board_of(edit_name_, &b)) {
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdKind), CB_SETCURSEL, 0, 0);
        set_text(chan_dlg::kIdDevice, widen(b.serial_device));
        set_text(chan_dlg::kIdBaud, std::to_wstring(b.baudrate));
        HWND db = GetDlgItem(hwnd_, chan_dlg::kIdDatabits);
        SendMessageW(db, CB_SETCURSEL,
                     static_cast<WPARAM>(std::max(0, std::min(3, b.databits - 5))), 0);
        int pidx = 0;
        if (b.parity == "E") pidx = 1;
        else if (b.parity == "O") pidx = 2;
        else if (b.parity == "M") pidx = 3;
        else if (b.parity == "S") pidx = 4;
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdParity), CB_SETCURSEL, static_cast<WPARAM>(pidx), 0);
        int sidx = b.stopbits_x10 == 15 ? 1 : (b.stopbits_x10 == 20 ? 2 : 0);
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdStopbits), CB_SETCURSEL, static_cast<WPARAM>(sidx), 0);
        int fidx = b.flowcontrol == "rtscts" ? 1 : (b.flowcontrol == "xonxoff" ? 2 : 0);
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdFlow), CB_SETCURSEL, static_cast<WPARAM>(fidx), 0);
        int lidx = b.line_ending == "lf" ? 1 : (b.line_ending == "cr" ? 2 :
                   (b.line_ending == "none" ? 3 : 0));
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdLineEnd), CB_SETCURSEL, static_cast<WPARAM>(lidx), 0);
        int eidx = b.encoding == "gbk" ? 1 : 0;
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdEncoding), CB_SETCURSEL, static_cast<WPARAM>(eidx), 0);
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdAutoOpen), BM_SETCHECK,
                     b.auto_open ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdDtr), BM_SETCHECK,
                     b.dtr ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdRts), BM_SETCHECK,
                     b.rts ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdLog), BM_SETCHECK,
                     b.log_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void ChannelDialog::on_kind_change() {
    apply_kind_visibility();
    layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

std::wstring ChannelDialog::text_of(int id) const {
    HWND h = GetDlgItem(hwnd_, id);
    if (!h) return std::wstring();
    wchar_t buf[512] = L"";
    GetWindowTextW(h, buf, 512);
    return std::wstring(buf);
}

void ChannelDialog::set_text(int id, const std::wstring& s) {
    if (HWND h = GetDlgItem(hwnd_, id)) SetWindowTextW(h, s.c_str());
}

int ChannelDialog::number_of(int id, int def) const {
    std::wstring s = text_of(id);
    if (s.empty()) return def;
    try {
        return std::stoi(s);
    } catch (...) {
        return def;
    }
}

void ChannelDialog::set_hint(const std::wstring& text, bool ok) {
    hint_ = text;
    hint_ok_ = ok;
    HWND h = GetDlgItem(hwnd_, chan_dlg::kIdHint);
    if (!h) return;
    SetWindowTextW(h, text.c_str());
    InvalidateRect(h, nullptr, TRUE);
}

BoardConfig ChannelDialog::collect_board() const {
    BoardConfig b;
    b.name = term::utf16_to_utf8(text_of(chan_dlg::kIdName));
    b.name = util::trim(b.name);
    b.serial_device = util::trim(term::utf16_to_utf8(text_of(chan_dlg::kIdDevice)));
    b.baudrate = number_of(chan_dlg::kIdBaud, 115200);
    int dbidx = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdDatabits),
                                              CB_GETCURSEL, 0, 0));
    b.databits = (dbidx >= 0 && dbidx <= 3) ? 5 + dbidx : 8;
    int pidx = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdParity), CB_GETCURSEL, 0, 0));
    static const char* kP[] = {"N", "E", "O", "M", "S"};
    b.parity = kP[pidx >= 0 && pidx < 5 ? pidx : 0];
    int sidx = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdStopbits), CB_GETCURSEL, 0, 0));
    b.stopbits_x10 = sidx == 1 ? 15 : (sidx == 2 ? 20 : 10);
    int fidx = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdFlow), CB_GETCURSEL, 0, 0));
    b.flowcontrol = fidx == 1 ? "rtscts" : (fidx == 2 ? "xonxoff" : "none");
    int lidx = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdLineEnd), CB_GETCURSEL, 0, 0));
    b.line_ending = lidx == 1 ? "lf" : (lidx == 2 ? "cr" : (lidx == 3 ? "none" : "crlf"));
    int eidx = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdEncoding), CB_GETCURSEL, 0, 0));
    b.encoding = eidx == 1 ? "gbk" : "utf-8";
    b.auto_open = SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdAutoOpen), BM_GETCHECK, 0, 0) == BST_CHECKED;
    b.dtr = SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdDtr), BM_GETCHECK, 0, 0) == BST_CHECKED;
    b.rts = SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdRts), BM_GETCHECK, 0, 0) == BST_CHECKED;
    b.log_enabled = SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdLog), BM_GETCHECK, 0, 0) == BST_CHECKED;
    b.timestamp = true;
    return b;
}

SSHLinkConfig ChannelDialog::collect_ssh() const {
    SSHLinkConfig s;
    s.name = util::trim(term::utf16_to_utf8(text_of(chan_dlg::kIdName)));
    s.host = util::trim(term::utf16_to_utf8(text_of(chan_dlg::kIdHost)));
    s.port = number_of(chan_dlg::kIdPort, 22);
    s.username = util::trim(term::utf16_to_utf8(text_of(chan_dlg::kIdUser)));
    s.password = term::utf16_to_utf8(text_of(chan_dlg::kIdPassword));
    s.password_present = true;
    s.term = util::trim(term::utf16_to_utf8(text_of(chan_dlg::kIdTerm)));
    s.connect_timeout_ms = number_of(chan_dlg::kIdTimeout, 5000);
    s.width = number_of(chan_dlg::kIdWidth, 120);
    s.height = number_of(chan_dlg::kIdHeight, 32);
    s.keepalive_s = number_of(chan_dlg::kIdKeepalive, 3);
    s.log_enabled = SendMessageW(GetDlgItem(hwnd_, chan_dlg::kIdLog), BM_GETCHECK, 0, 0) == BST_CHECKED;
    return s;
}

bool ChannelDialog::validate(std::wstring* err) const {
    std::wstring name = text_of(chan_dlg::kIdName);
    std::string name_u8 = util::trim(term::utf16_to_utf8(name));
    if (name_u8.empty()) { *err = L"通道名不能为空"; return false; }
    if (name_u8.size() > 40) { *err = L"通道名过长（≤40 字符）"; return false; }
    if (name_u8.find('|') != std::string::npos) { *err = L"通道名不能含 “|”"; return false; }
    for (const AppCore::Entry& e : core_->entries()) {
        if (e.name == name_u8 && (!editing_ || name_u8 != edit_name_)) {
            *err = L"通道名已存在：" + widen(name_u8);
            return false;
        }
    }
    if (is_ssh()) {
        std::string host = util::trim(term::utf16_to_utf8(text_of(chan_dlg::kIdHost)));
        if (host.empty()) { *err = L"主机不能为空"; return false; }
        int port = number_of(chan_dlg::kIdPort, 0);
        if (port < 1 || port > 65535) { *err = L"端口必须在 1–65535"; return false; }
        int to = number_of(chan_dlg::kIdTimeout, 0);
        if (to < 500 || to > 60000) { *err = L"超时必须在 500–60000 ms"; return false; }
        int w = number_of(chan_dlg::kIdWidth, 0);
        int h = number_of(chan_dlg::kIdHeight, 0);
        if (w < 20 || w > 500 || h < 10 || h > 300) { *err = L"终端尺寸超出范围（列 20–500 / 行 10–300）"; return false; }
        int ka = number_of(chan_dlg::kIdKeepalive, -1);
        if (ka < 0 || ka > 600) { *err = L"保活必须在 0–600 s"; return false; }
    } else {
        std::string dev = util::trim(term::utf16_to_utf8(text_of(chan_dlg::kIdDevice)));
        if (dev.empty()) { *err = L"串口设备不能为空（如 COM5 或 loop:ai）"; return false; }
        int baud = number_of(chan_dlg::kIdBaud, 0);
        if (baud < 300 || baud > 12000000) { *err = L"波特率必须在 300–12000000"; return false; }
    }
    return true;
}

void ChannelDialog::save() {
    std::wstring err;
    if (!validate(&err)) {
        set_hint(err, false);
        return;
    }
    std::uint64_t t0 = util::now_ms();
    bool ok = false;
    std::string err_u8;
    std::string name;
    if (is_ssh()) {
        SSHLinkConfig s = collect_ssh();
        name = s.name;
        ok = core_->upsert_ssh(s, editing_ ? edit_name_ : std::string(), &err_u8);
    } else {
        BoardConfig b = collect_board();
        name = b.name;
        ok = core_->upsert_serial(b, editing_ ? edit_name_ : std::string(), &err_u8);
    }
    int ms = static_cast<int>(util::now_ms() - t0);
    if (!ok) {
        set_hint(L"保存失败：" + widen(err_u8), false);
        return;
    }
    AppliedFn cb = on_applied_;
    HWND h = hwnd_;
    hwnd_ = nullptr;
    DestroyWindow(h);
    if (cb) cb(name, ms);
}

void ChannelDialog::test_open() {
    if (is_ssh()) {
        std::string host = util::trim(term::utf16_to_utf8(text_of(chan_dlg::kIdHost)));
        int port = number_of(chan_dlg::kIdPort, 0);
        if (host.empty() || port < 1 || port > 65535) {
            set_hint(L"先填合法的主机与端口", false);
            return;
        }
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            set_hint(L"网络初始化失败", false);
            return;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* addrs = nullptr;
        std::string port_s = std::to_string(port);
        int gai = getaddrinfo(host.c_str(), port_s.c_str(), &hints, &addrs);
        if (gai != 0 || !addrs) {
            WSACleanup();
            set_hint(L"主机名无法解析：" + widen(host), false);
            return;
        }
        SOCKET s = socket(addrs->ai_family, addrs->ai_socktype, addrs->ai_protocol);
        if (s == INVALID_SOCKET) {
            freeaddrinfo(addrs);
            WSACleanup();
            set_hint(L"创建套接字失败", false);
            return;
        }
        unsigned long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        connect(s, addrs->ai_addr, static_cast<int>(addrs->ai_addrlen));
        freeaddrinfo(addrs);
        fd_set wf, ef;
        FD_ZERO(&wf);
        FD_ZERO(&ef);
        FD_SET(s, &wf);
        FD_SET(s, &ef);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 900000;
        int sel = select(0, nullptr, &wf, &ef, &tv);
        bool ok = false;
        if (sel > 0 && FD_ISSET(s, &wf)) {
            int err = 0;
            int len = sizeof(err);
            ok = getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) == 0 && err == 0;
        }
        closesocket(s);
        WSACleanup();
        set_hint(ok ? L"TCP 可连通（端口开放）" : L"连不上：超时、域名解析失败或连接被拒绝", ok);
        return;
    }
    std::string dev = util::trim(term::utf16_to_utf8(text_of(chan_dlg::kIdDevice)));
    int baud = number_of(chan_dlg::kIdBaud, 0);
    if (dev.rfind("loop:", 0) == 0) {
        set_hint(L"内置回环端点：无需占用真实串口，可直接用", true);
        return;
    }
    if (dev.empty()) {
        set_hint(L"先填串口设备（如 COM5）", false);
        return;
    }
    std::wstring path = L"\\\\.\\" + widen(dev);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        wchar_t buf[160] = L"";
        if (e == ERROR_ACCESS_DENIED) swprintf_s(buf, L"端口被占用（error 5）");
        else swprintf_s(buf, L"打不开：error %lu", static_cast<unsigned long>(e));
        set_hint(buf, false);
        return;
    }
    CloseHandle(h);
    wchar_t buf[160] = L"";
    swprintf_s(buf, L"端口可打开：%s @ %d", widen(dev).c_str(), baud);
    set_hint(buf, true);
}

void ChannelDialog::handle_command(int id, int code) {
    switch (id) {
    case chan_dlg::kIdKind:
        if (code == CBN_SELCHANGE) on_kind_change();
        break;
    case chan_dlg::kIdTest:
        test_open();
        break;
    case chan_dlg::kIdSave:
        save();
        break;
    case chan_dlg::kIdCancel:
        DestroyWindow(hwnd_);
        break;
    case chan_dlg::kIdDelete: {
        if (!editing_) break;
        if (!delete_armed_) {
            delete_armed_ = true;
            set_hint(L"再点一次“确认删除”才会真正删除该通道", false);
            SetWindowTextW(GetDlgItem(hwnd_, chan_dlg::kIdDelete), L"确认删除");
            break;
        }
        std::string err;
        std::uint64_t t0 = util::now_ms();
        std::string old = edit_name_;
        if (!core_->delete_channel(old, &err)) {
            set_hint(L"删除失败：" + widen(err), false);
            delete_armed_ = false;
            SetWindowTextW(GetDlgItem(hwnd_, chan_dlg::kIdDelete), L"删除");
            break;
        }
        int ms = static_cast<int>(util::now_ms() - t0);
        AppliedFn cb = on_applied_;
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
        if (cb) cb(old, ms);
        break;
    }
    default:
        // “刷新”按钮：重新枚举本机串口，刷新设备下拉（保留已填内容）
        if (id == chan_dlg::kIdRefresh) {
            HWND c = GetDlgItem(hwnd_, chan_dlg::kIdDevice);
            fill_port_combo(c);
            int n = c ? static_cast<int>(SendMessageW(c, CB_GETCOUNT, 0, 0)) - 2 : 0;
            wchar_t msg[160] = L"";
            swprintf_s(msg, L"已刷新：发现 %d 个串口（可从下拉选择，也可直接输入）", n < 0 ? 0 : n);
            set_hint(msg, true);
        }
        break;
    }
}

LRESULT CALLBACK ChannelDialog::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    ChannelDialog* self = reinterpret_cast<ChannelDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    self->hwnd_ = hwnd;
    return self->handle(msg, wp, lp);
}

LRESULT ChannelDialog::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        handle_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        hwnd_ = nullptr;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd_);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HWND who = reinterpret_cast<HWND>(lp);
        HDC dc = reinterpret_cast<HDC>(wp);
        if (who == GetDlgItem(hwnd_, chan_dlg::kIdHint)) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, hint_ok_ ? RGB(0, 110, 60) : RGB(180, 40, 40));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace sb3
