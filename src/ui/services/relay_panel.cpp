#include "ui/services/relay_panel.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/app_core.h"
#include "core/util/util.h"
#include "services/board_actions/board_action_runner.h"
#include "services/script/script_lines.h"
#include "services/service_manager.h"
#include "ui/aibridge/bridge_host.h"
#include "ui/ui_messages.h"

namespace sb3 {
namespace {

int g_dpi = 96;
int S(int v) { return MulDiv(v, g_dpi, 96); }

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

// Win32 多行 EDIT 只认 CRLF；脚本正文里常是 LF，直接塞进去会被显示成一整行。
std::wstring edit_multiline(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 16);
    for (wchar_t ch : s) {
        if (ch == L'\r') continue;
        if (ch == L'\n') {
            out += L"\r\n";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::wstring format_seconds(double v) {
    std::wstring text = std::to_wstring(v);
    std::size_t dot = text.find(L'.');
    if (dot != std::wstring::npos) {
        while (!text.empty() && text.back() == L'0') text.pop_back();
        if (!text.empty() && text.back() == L'.') text.pop_back();
    }
    return text;
}

std::vector<std::wstring> serial_ports() {
    std::vector<std::wstring> out;
    wchar_t target[512];
    for (int i = 1; i <= 256; ++i) {
        wchar_t name[16];
        std::swprintf(name, 16, L"COM%d", i);
        if (QueryDosDeviceW(name, target, 512)) out.push_back(name);
    }
    return out;
}

void add_combo_strings(HWND combo, const std::vector<std::wstring>& items) {
    if (!combo) return;
    for (const auto& s : items)
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) out += "\r\n";
    }
    return out;
}

bool valid_id(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

const RelayDeviceConfig* find_relay(const AppConfig& cfg, const std::string& name) {
    for (const RelayDeviceConfig& r : cfg.relays)
        if (r.name == name) return &r;
    return nullptr;
}

const BoardActionConfig* find_action(const std::vector<BoardActionConfig>& list,
                                     const std::string& id) {
    for (const BoardActionConfig& a : list)
        if (a.id == id) return &a;
    return nullptr;
}

std::string preset_script_body(int kind, const std::string& relay_name, int channels) {
    (void)relay_name;
    (void)channels;
    if (kind == 0) {
        return "--lua\r\n"
               "local m = channel.mark()\r\n"
               "channel.send(\"enter\")\r\n"
               "local out = channel.wait_mark(m, \"U-Boot\", 5)\r\n"
               "if not out then out = channel.wait_mark(m, \"=>\", 5) end\r\n"
               "if out then print(out) else print(\"未匹配 U-Boot 或 =>\") end\r\n";
    }
    if (kind == 1) {
        return "tftp -g -r update.img 192.168.1.100\r\n"
               "sync\r\n"
               "echo upgrade done\r\n";
    }
    return "ping -n 2 127.0.0.1\r\n";
}

std::string upgrade_mode_body(const std::string& relay_name, int channels) {
    if (relay_name.empty() || channels < 2) return std::string();
    std::string s = "--lua\r\n-- 进入升级模式：PWR off -> MASKROM on -> PWR on -> MASKROM off\r\n";
    s += "relay(\"" + relay_name + "\", 1, \"off\")\r\n";
    s += "msleep(200)\r\n";
    s += "relay(\"" + relay_name + "\", 2, \"on\")\r\n";
    s += "msleep(200)\r\n";
    s += "relay(\"" + relay_name + "\", 1, \"on\")\r\n";
    s += "msleep(500)\r\n";
    s += "relay(\"" + relay_name + "\", 2, \"off\")\r\n";
    s += "print(\"enter_upgrade_mode done\")\r\n";
    return s;
}

}  // namespace

bool RelayPanel::open(HINSTANCE inst, HWND parent, AppCore* core, ServiceManager* mgr,
                       HFONT font, int dpi, int page) {
    if (is_open()) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        page_ = (page >= 0 && page <= 2) ? page : page_;
        show_page(page_);
        refresh();
        return true;
    }
    inst_ = inst;
    parent_ = parent;
    core_ = core;
    mgr_ = mgr;
    font_ = font;
    dpi_ = dpi > 0 ? dpi : 96;
    g_dpi = dpi_;
    page_ = (page >= 0 && page <= 2) ? page : 0;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = relay_ui::kClass;
    RegisterClassExW(&wc);

    RECT rc{0, 0, S(1120), S(620)};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT wa0{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa0, 0);
    int avail_w = wa0.right - wa0.left;
    int avail_h = wa0.bottom - wa0.top;
    if (w > avail_w) w = avail_w;
    if (h > avail_h) h = avail_h;
    min_w_ = w;
    min_h_ = h;
    int x = 40, y = 20;
    if (parent) {
        RECT pr{};
        GetWindowRect(parent, &pr);
        x = pr.left + (pr.right - pr.left - w) / 2;
        y = pr.top + (pr.bottom - pr.top - h) / 2;
    }
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (x + w > wa.right) x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, relay_ui::kClass,
                            L"继电器与板控按钮 — ssh-bridge v3", WS_OVERLAPPEDWINDOW,
                            x, y, w, h, parent, nullptr, inst_, this);
    if (!hwnd_) return false;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    return true;
}

void RelayPanel::close() {
    if (is_open()) DestroyWindow(hwnd_);
}

void RelayPanel::track(int id, int page) {
    if (page >= 0 && page < 3) page_ids_[page].push_back(id);
}

void RelayPanel::show_page(int page) {
    if (page < 0 || page > 2) return;
    page_ = page;
    for (int p = 0; p < 3; ++p) {
        for (int id : page_ids_[p]) {
            if (HWND h = GetDlgItem(hwnd_, id)) ShowWindow(h, p == page ? SW_SHOWNA : SW_HIDE);
        }
    }
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdTab), TCM_SETCURSEL, page, 0);
}

LRESULT CALLBACK RelayPanel::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    RelayPanel* self = reinterpret_cast<RelayPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<RelayPanel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(msg, wp, lp);
}

LRESULT RelayPanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        build();
        SetTimer(hwnd_, 1, 250, nullptr);
        return 0;
    case WM_TIMER:
        if (wp == 1 && !updating_) update_runtime();
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == CBN_SELCHANGE ||
            HIWORD(wp) == LBN_SELCHANGE) {
            int id = LOWORD(wp);
            if (id == relay_ui::kIdBindTarget || id == relay_ui::kIdScriptTarget) {
                fill_bind_list(std::string());
                fill_script_list(std::string());
                update_target_info();
                return 0;
            }
            if (id == relay_ui::kIdBindList && HIWORD(wp) == CBN_SELCHANGE) {
                load_bind(action_index_from_combo(id));
                return 0;
            }
            if (id == relay_ui::kIdScriptList && HIWORD(wp) == CBN_SELCHANGE) {
                load_script(action_index_from_combo(id));
                return 0;
            }
            if (id == relay_ui::kIdDevList && HIWORD(wp) == LBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, id), LB_GETCURSEL, 0, 0));
                if (sel >= 0) load_device(sel);
                return 0;
            }
            if (id == relay_ui::kIdDevName && HIWORD(wp) == CBN_SELCHANGE) {
                std::wstring name = get_text(id);
                const auto& relays = core_ ? core_->config().relays : std::vector<RelayDeviceConfig>{};
                for (std::size_t i = 0; i < relays.size(); ++i) {
                    if (utf8_to_wide(relays[i].name) == name) {
                        load_device(static_cast<int>(i));
                        break;
                    }
                }
                return 0;
            }
            if (id == relay_ui::kIdBindRelay && HIWORD(wp) == CBN_SELCHANGE) {
                int idx = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, id), CB_GETCURSEL, 0, 0));
                int count = 2;
                if (core_ && idx >= 0 &&
                    static_cast<std::size_t>(idx) < core_->config().relays.size())
                    count = core_->config().relays[static_cast<std::size_t>(idx)].channels;
                fill_channels(relay_ui::kIdBindChannel, count);
                return 0;
            }
            {
                bool handled = false;
                switch (id) {
                case relay_ui::kIdDevNew: dev_index_ = -1; load_device(-1); handled = true; break;
                case relay_ui::kIdDevSave: save_device(); handled = true; break;
                case relay_ui::kIdDevDelete: delete_device(); handled = true; break;
                case relay_ui::kIdDevRefresh: fill_ports(relay_ui::kIdDevPort); note(L"串口列表已刷新"); handled = true; break;
                case relay_ui::kIdDevOn: test_device("on"); handled = true; break;
                case relay_ui::kIdDevOff: test_device("off"); handled = true; break;
                case relay_ui::kIdDevPulse: test_device("pulse"); handled = true; break;
                case relay_ui::kIdBindNew: load_bind(-1); note(L"已新建绑定草稿，保存后生效"); handled = true; break;
                case relay_ui::kIdBindSave: save_bind(); handled = true; break;
                case relay_ui::kIdBindDelete: delete_bind(); handled = true; break;
                case relay_ui::kIdBindTest: test_bind(); handled = true; break;
                case relay_ui::kIdBindPresetOn: preset_bind("power_on"); handled = true; break;
                case relay_ui::kIdBindPresetOff: preset_bind("power_off"); handled = true; break;
                case relay_ui::kIdBindPresetReboot: preset_bind("reboot"); handled = true; break;
                case relay_ui::kIdBindGenerate: generate_standard(); handled = true; break;
                case relay_ui::kIdBindGenerateUpgrade: generate_upgrade_script(); handled = true; break;
                case relay_ui::kIdScriptNew: load_script(-1); note(L"已新建脚本草稿，保存后生效"); handled = true; break;
                case relay_ui::kIdScriptSave: save_script(); handled = true; break;
                case relay_ui::kIdScriptDelete: delete_script(); handled = true; break;
                case relay_ui::kIdScriptTest: test_script(); handled = true; break;
                case relay_ui::kIdScriptPresetLogin: preset_script(0); handled = true; break;
                case relay_ui::kIdScriptPresetUboot: preset_script(1); handled = true; break;
                case relay_ui::kIdScriptPresetUpgrade: preset_script(2); handled = true; break;
                case relay_ui::kIdScriptPresetUpgradeMode: preset_script(3); handled = true; break;
                case relay_ui::kIdScriptGenerateUpgrade: generate_upgrade_script(); handled = true; break;
                case relay_ui::kIdClose: DestroyWindow(hwnd_); handled = true; break;
                default: break;
                }
                if (handled) return 0;
            }
        }
        return 0;
    case WM_NOTIFY: {
        LPNMHDR hdr = reinterpret_cast<LPNMHDR>(lp);
        if (hdr && hdr->idFrom == relay_ui::kIdTab && hdr->code == TCN_SELCHANGE) {
            int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdTab),
                                                   TCM_GETCURSEL, 0, 0));
            show_page(sel);
        }
        return 0;
    }
    case kMsgRelayActionResult: {
        auto* r = reinterpret_cast<AsyncBoardResult*>(lp);
        if (r) {
            if (r->relay && mgr_) {
                mgr_->relay_note_state(r->relay_port, r->relay_channel, r->relay_on, r->relay_mode,
                                       r->ok ? std::string() : r->message);
            }
            std::wstring msg = utf8_to_wide(r->message);
            if (!r->output.empty()) {
                std::size_t n = std::min<std::size_t>(r->output.size(), 600);
                msg += L"\n" + utf8_to_wide(r->output.substr(0, n));
            }
            note(msg, !r->ok);
            if (!r->output.empty()) util::ensure_dir("output");
            delete r;
        }
        update_runtime();
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = min_w_ > 0 ? min_w_ : S(1000);
        mmi->ptMinTrackSize.y = min_h_ > 0 ? min_h_ : S(700);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, 1);
        hwnd_ = nullptr;
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

void RelayPanel::build() {
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w,
                    int h, int id, int page, bool track_control = true) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w),
                                 S(h), hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 inst_, nullptr);
        if (c && font_) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        if (c && track_control) track(id, page);
        return c;
    };
    auto label = [&](const wchar_t* text, int x, int y, int w, int h, int id, int page) {
        return make(L"STATIC", text, SS_LEFT | SS_CENTERIMAGE, x, y, w, h, id, page);
    };
    auto info = [&](int x, int y, int w, int h, int id, int page) {
        return make(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, x, y, w, h,
                    id, page);
    };
    auto button = [&](const wchar_t* text, int x, int y, int w, int h, int id, int page) {
        return make(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, h, id, page);
    };
    auto check = [&](const wchar_t* text, int x, int y, int w, int h, int id, int page) {
        return make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, h, id, page);
    };
    auto combo = [&](const wchar_t* text, int x, int y, int w, int h, int id, int page,
                     bool list_only = false) {
        return make(L"COMBOBOX", text,
                    (list_only ? CBS_DROPDOWNLIST : CBS_DROPDOWN) | WS_VSCROLL | WS_TABSTOP,
                    x, y, w, h, id, page);
    };
    auto edit = [&](const wchar_t* text, int x, int y, int w, int h, int id, int page) {
        return make(L"EDIT", text, WS_BORDER | ES_LEFT | ES_AUTOHSCROLL, x, y, w, h, id, page);
    };
    auto group = [&](const wchar_t* text, int x, int y, int w, int h, int id, int page) {
        return make(L"BUTTON", text, BS_GROUPBOX, x, y, w, h, id, page, true);
    };

    // 公共
    label(L"继电器与板控按钮", 20, 10, 320, 24, relay_ui::kIdTitle, -1);
    {
        HWND tab = CreateWindowExW(WS_EX_CONTROLPARENT, WC_TABCONTROLW, L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(20), S(42), S(1080),
                                   S(520), hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(relay_ui::kIdTab)),
                                   inst_, nullptr);
        if (tab && font_) SendMessageW(tab, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        if (!tab) {
            std::FILE* f = std::fopen("relay_tab_error.txt", "wb");
            if (f) {
                std::fprintf(f, "CreateWindowEx failed: %lu\n", GetLastError());
                std::fclose(f);
            }
        }
    }
    info(20, 578, 920, 28, relay_ui::kIdMsg, -1);
    button(L"关闭", 1000, 578, 96, 30, relay_ui::kIdClose, -1);

    // 页 1：继电器设备
    group(L"设备参数", 16, 78, 1074, 250, relay_ui::kIdGroupDev, 0);
    label(L"设备名", 32, 108, 48, 24, relay_ui::kIdLabelDevName, 0);
    combo(L"", 84, 104, 200, 200, relay_ui::kIdDevName, 0);
    label(L"Windows 串口", 296, 108, 90, 24, relay_ui::kIdLabelDevPort, 0);
    combo(L"", 390, 104, 100, 200, relay_ui::kIdDevPort, 0);
    label(L"Linux 串口", 502, 108, 66, 24, relay_ui::kIdLabelDevLinux, 0);
    edit(L"", 572, 104, 120, 26, relay_ui::kIdDevLinux, 0);
    label(L"波特率", 706, 108, 44, 24, relay_ui::kIdLabelDevBaud, 0);
    combo(L"9600", 754, 104, 86, 200, relay_ui::kIdDevBaud, 0);
    label(L"通道数", 852, 108, 48, 24, relay_ui::kIdLabelDevChannels, 0);
    combo(L"2", 904, 104, 66, 200, relay_ui::kIdDevChannels, 0);
    button(L"刷新串口", 980, 103, 100, 28, relay_ui::kIdDevRefresh, 0);

    label(L"写后延时 ms", 32, 148, 78, 24, relay_ui::kIdLabelDevDelay, 0);
    combo(L"100", 114, 144, 84, 200, relay_ui::kIdDevDelay, 0);
    check(L"IO 低电平有效", 218, 144, 126, 24, relay_ui::kIdDevIo, 0);
    check(L"脉冲极性取反", 360, 144, 126, 24, relay_ui::kIdDevPolarity, 0);
    check(L"启用设备", 506, 144, 100, 24, relay_ui::kIdDevEnable, 0);

    label(L"测试通道", 32, 188, 66, 24, relay_ui::kIdLabelDevTestCh, 0);
    combo(L"", 102, 184, 66, 200, relay_ui::kIdDevTestCh, 0);
    button(L"测试 ON", 184, 182, 96, 30, relay_ui::kIdDevOn, 0);
    button(L"测试 OFF", 288, 182, 96, 30, relay_ui::kIdDevOff, 0);
    label(L"脉冲时长", 398, 188, 60, 24, relay_ui::kIdLabelDevPulseHold, 0);
    combo(L"1 秒", 462, 184, 80, 200, relay_ui::kIdDevPulseHold, 0);
    button(L"测试脉冲", 552, 182, 100, 30, relay_ui::kIdDevPulse, 0);
    label(L"", 668, 188, 400, 24, relay_ui::kIdDevStatus, 0);

    button(L"新增设备", 32, 222, 100, 28, relay_ui::kIdDevNew, 0);
    button(L"保存设备", 140, 222, 100, 28, relay_ui::kIdDevSave, 0);
    button(L"删除设备", 248, 222, 100, 28, relay_ui::kIdDevDelete, 0);
    label(L"", 32, 264, 1040, 24, relay_ui::kIdDevInfo, 0);

    group(L"设备与通道状态", 16, 340, 1074, 160, relay_ui::kIdGroupStates, 0);
    label(L"设备列表", 32, 362, 120, 24, relay_ui::kIdLabelDevList, 0);
    make(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, 32, 384, 280, 104,
         relay_ui::kIdDevList, 0);
    label(L"通道状态", 328, 362, 120, 24, relay_ui::kIdLabelDevStates, 0);
    info(328, 384, 744, 104, relay_ui::kIdDevStates, 0);
    label(L"提示：测试前请先断开占用同一 COM 的串口通道。设备名修改后，已有按钮绑定会自动同步。",
          32, 520, 1040, 24, relay_ui::kIdLabelDevHint, 0);

    // 页 2：板卡绑定
    group(L"目标通道与绑定列表", 16, 78, 1074, 116, relay_ui::kIdGroupBindTarget, 1);
    label(L"目标通道", 32, 104, 70, 24, relay_ui::kIdLabelBindTarget, 1);
    combo(L"", 106, 100, 280, 200, relay_ui::kIdBindTarget, 1, true);
    label(L"当前绑定", 402, 104, 66, 24, relay_ui::kIdLabelBindList, 1);
    combo(L"", 472, 100, 340, 200, relay_ui::kIdBindList, 1, true);
    button(L"新增绑定", 32, 138, 100, 28, relay_ui::kIdBindNew, 1);
    button(L"保存绑定", 140, 138, 100, 28, relay_ui::kIdBindSave, 1);
    button(L"删除绑定", 248, 138, 100, 28, relay_ui::kIdBindDelete, 1);
    button(L"测试执行", 356, 138, 100, 28, relay_ui::kIdBindTest, 1);
    button(L"生成上电/下电/重启", 464, 138, 188, 28, relay_ui::kIdBindGenerate, 1);
    label(L"", 668, 142, 400, 24, relay_ui::kIdBindTargetInfo, 1);

    group(L"绑定参数", 16, 206, 1074, 300, relay_ui::kIdGroupBind, 1);
    label(L"ID", 32, 234, 24, 24, relay_ui::kIdLabelBindId, 1);
    edit(L"", 60, 230, 180, 26, relay_ui::kIdBindId, 1);
    label(L"按钮文字", 252, 234, 60, 24, relay_ui::kIdLabelBindLabel, 1);
    edit(L"", 316, 230, 220, 26, relay_ui::kIdBindLabel, 1);
    check(L"启用", 548, 230, 70, 24, relay_ui::kIdBindEnable, 1);
    check(L"极性取反", 626, 230, 100, 24, relay_ui::kIdBindPolarity, 1);

    label(L"继电器", 32, 276, 50, 24, relay_ui::kIdLabelBindRelay, 1);
    combo(L"", 86, 272, 200, 200, relay_ui::kIdBindRelay, 1, true);
    label(L"继电器通道", 300, 276, 76, 24, relay_ui::kIdLabelBindChannel, 1);
    combo(L"1", 380, 272, 66, 200, relay_ui::kIdBindChannel, 1, true);
    label(L"模式", 460, 276, 36, 24, relay_ui::kIdLabelBindMode, 1);
    combo(L"", 500, 272, 130, 200, relay_ui::kIdBindMode, 1, true);
    label(L"保持/脉冲秒", 644, 276, 84, 24, relay_ui::kIdLabelBindHold, 1);
    combo(L"1.0", 732, 272, 80, 200, relay_ui::kIdBindHold, 1);

    button(L"预设：上电", 32, 312, 100, 28, relay_ui::kIdBindPresetOn, 1);
    button(L"预设：下电", 140, 312, 100, 28, relay_ui::kIdBindPresetOff, 1);
    button(L"预设：重启", 248, 312, 100, 28, relay_ui::kIdBindPresetReboot, 1);
    button(L"生成进升级模式按钮", 356, 312, 180, 28, relay_ui::kIdBindGenerateUpgrade, 1);
    label(L"", 32, 352, 1040, 24, relay_ui::kIdBindInfo, 1);

    // 页 3：按钮脚本
    group(L"目标通道与脚本列表", 16, 78, 1074, 90, relay_ui::kIdGroupScriptTarget, 2);
    label(L"目标通道", 32, 104, 70, 24, relay_ui::kIdLabelScriptTarget, 2);
    combo(L"", 106, 100, 280, 200, relay_ui::kIdScriptTarget, 2, true);
    label(L"当前按钮", 402, 104, 66, 24, relay_ui::kIdLabelScriptList, 2);
    combo(L"", 472, 100, 330, 200, relay_ui::kIdScriptList, 2, true);
    button(L"新增脚本", 812, 100, 100, 28, relay_ui::kIdScriptNew, 2);
    button(L"删除", 920, 100, 70, 28, relay_ui::kIdScriptDelete, 2);

    group(L"脚本参数", 16, 180, 1074, 330, relay_ui::kIdGroupScript, 2);
    label(L"ID", 32, 208, 24, 24, relay_ui::kIdLabelScriptId, 2);
    edit(L"", 60, 204, 180, 26, relay_ui::kIdScriptId, 2);
    label(L"按钮文字", 252, 208, 60, 24, relay_ui::kIdLabelScriptLabel, 2);
    edit(L"", 316, 204, 220, 26, relay_ui::kIdScriptLabel, 2);
    label(L"类型", 548, 208, 36, 24, relay_ui::kIdLabelScriptKind, 2);
    combo(L"", 590, 204, 140, 200, relay_ui::kIdScriptKind, 2, true);
    check(L"启用", 748, 204, 70, 24, relay_ui::kIdScriptEnable, 2);

    label(L"命令间隔 ms", 32, 248, 78, 24, relay_ui::kIdLabelScriptDelay, 2);
    combo(L"200", 116, 244, 80, 200, relay_ui::kIdScriptDelay, 2);
    button(L"预设：登录", 212, 242, 96, 28, relay_ui::kIdScriptPresetLogin, 2);
    button(L"预设：进 U-Boot", 316, 242, 116, 28, relay_ui::kIdScriptPresetUboot, 2);
    button(L"预设：升级命令", 440, 242, 116, 28, relay_ui::kIdScriptPresetUpgrade, 2);
    button(L"预设：进升级模式", 564, 242, 140, 28, relay_ui::kIdScriptPresetUpgradeMode, 2);
    button(L"生成并保存升级模式", 712, 242, 168, 28, relay_ui::kIdScriptGenerateUpgrade, 2);
    button(L"保存脚本", 888, 242, 100, 28, relay_ui::kIdScriptSave, 2);
    button(L"测试执行", 996, 242, 76, 28, relay_ui::kIdScriptTest, 2);

    label(L"Lua 脚本，或命令列表（一行一条）", 32, 286, 320, 24, relay_ui::kIdLabelScriptBody, 2);
    make(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
         32, 312, 1040, 154, relay_ui::kIdScriptBody, 2);
    label(L"", 32, 474, 1040, 24, relay_ui::kIdScriptInfo, 2);

    // Tab 页签
    HWND tab = GetDlgItem(hwnd_, relay_ui::kIdTab);
    if (tab) {
        const wchar_t* pages[3] = {L"继电器设备", L"板卡绑定", L"按钮脚本"};
        for (int i = 0; i < 3; ++i) {
            TCITEMW it{};
            it.mask = TCIF_TEXT;
            it.pszText = const_cast<LPWSTR>(pages[i]);
            SendMessageW(tab, TCM_INSERTITEMW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&it));
        }
    }

    // 下拉选项
    fill_ports(relay_ui::kIdDevPort);
    fill_number_combo(relay_ui::kIdDevBaud, {L"9600", L"19200", L"38400", L"57600", L"115200"});
    fill_number_combo(relay_ui::kIdDevChannels, {L"1", L"2", L"4", L"8"});
    fill_number_combo(relay_ui::kIdDevDelay, {L"20", L"50", L"100", L"200", L"500"});
    fill_number_combo(relay_ui::kIdDevPulseHold, {L"0.5 秒", L"1 秒", L"2 秒", L"5 秒"});
    add_combo_strings(GetDlgItem(hwnd_, relay_ui::kIdBindMode), {L"hold_on", L"hold_off", L"pulse"});
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdBindMode), CB_SETCURSEL, 0, 0);
    add_combo_strings(GetDlgItem(hwnd_, relay_ui::kIdScriptKind),
                      {L"Lua 脚本", L"板端命令", L"本机命令", L"简单脚本"});
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdScriptKind), CB_SETCURSEL, 0, 0);
    fill_number_combo(relay_ui::kIdScriptDelay, {L"100", L"200", L"500", L"1000"});

    fill_devices();
    fill_targets();
    // 首次打开时也把绑定/脚本列表和默认草稿装好，不能等用户先切换目标通道
    fill_bind_list(std::string());
    fill_script_list(std::string());
    show_page(page_);
    update_runtime();
}

void RelayPanel::fill_ports(int id) {
    HWND combo = GetDlgItem(hwnd_, id);
    if (!combo) return;
    wchar_t cur[128] = L"";
    GetWindowTextW(combo, cur, 128);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    std::vector<std::wstring> ports = serial_ports();
    if (ports.empty()) ports = {L"COM3", L"COM5"};
    add_combo_strings(combo, ports);
    if (cur[0]) SetWindowTextW(combo, cur);
    else SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

void RelayPanel::fill_number_combo(int id, const std::vector<std::wstring>& items) {
    HWND combo = GetDlgItem(hwnd_, id);
    if (!combo) return;
    wchar_t cur[128] = L"";
    GetWindowTextW(combo, cur, 128);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    add_combo_strings(combo, items);
    if (cur[0]) {
        int idx = static_cast<int>(SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                               reinterpret_cast<LPARAM>(cur)));
        if (idx >= 0) SendMessageW(combo, CB_SETCURSEL, idx, 0);
        else SetWindowTextW(combo, cur);
    } else if (!items.empty()) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }
}

void RelayPanel::fill_channels(int id, int count) {
    if (count < 1) count = 1;
    if (count > 32) count = 32;
    HWND combo = GetDlgItem(hwnd_, id);
    if (!combo) return;
    wchar_t cur[32] = L"";
    GetWindowTextW(combo, cur, 32);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (int i = 1; i <= count; ++i) {
        std::wstring s = std::to_wstring(i);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
    }
    if (cur[0]) SendMessageW(combo, CB_SETCURSEL, _wtoi(cur) - 1, 0);
    if (SendMessageW(combo, CB_GETCURSEL, 0, 0) < 0)
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

void RelayPanel::fill_devices() {
    if (!core_) return;
    updating_ = true;
    HWND combo = GetDlgItem(hwnd_, relay_ui::kIdDevName);
    HWND list = GetDlgItem(hwnd_, relay_ui::kIdDevList);
    std::wstring keep = get_text(relay_ui::kIdDevName);
    if (combo) SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    if (list) SendMessageW(list, LB_RESETCONTENT, 0, 0);
    const auto& relays = core_->config().relays;
    for (std::size_t i = 0; i < relays.size(); ++i) {
        std::wstring w = utf8_to_wide(relays[i].name);
        if (combo) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
        if (list) SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
    }
    int sel = 0;
    if (!keep.empty()) {
        for (std::size_t i = 0; i < relays.size(); ++i)
            if (utf8_to_wide(relays[i].name) == keep) sel = static_cast<int>(i);
    }
    if (relays.empty()) {
        dev_index_ = -1;
        load_device(-1);
    } else {
        sel = std::min<int>(sel, static_cast<int>(relays.size()) - 1);
        load_device(sel);
        if (combo) SendMessageW(combo, CB_SETCURSEL, sel, 0);
        if (list) SendMessageW(list, LB_SETCURSEL, sel, 0);
    }
    updating_ = false;
}

void RelayPanel::load_device(int index) {
    if (!core_) return;
    updating_ = true;
    const auto& relays = core_->config().relays;
    RelayDeviceConfig dev;
    if (index >= 0 && static_cast<std::size_t>(index) < relays.size()) {
        dev = relays[static_cast<std::size_t>(index)];
        dev_index_ = index;
    } else {
        dev_index_ = -1;
        std::string base = "relay";
        for (int n = 1; n < 100; ++n) {
            std::string name = base + std::to_string(n);
            bool used = false;
            for (const RelayDeviceConfig& r : relays)
                if (r.name == name) used = true;
            if (!used) { dev.name = name; break; }
        }
        dev.enable = true;
        dev.windows_port = "COM3";
        dev.channels = 2;
        dev.baudrate = 9600;
        dev.post_write_delay_ms = 100;
    }
    set_text(relay_ui::kIdDevName, utf8_to_wide(dev.name));
    fill_ports(relay_ui::kIdDevPort);
    set_text(relay_ui::kIdDevPort, utf8_to_wide(dev.windows_port));
    set_text(relay_ui::kIdDevLinux, utf8_to_wide(dev.linux_port));
    set_text(relay_ui::kIdDevBaud, std::to_wstring(dev.baudrate));
    set_text(relay_ui::kIdDevChannels, std::to_wstring(dev.channels));
    set_text(relay_ui::kIdDevDelay, std::to_wstring(dev.post_write_delay_ms));
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdDevIo),
                 BM_SETCHECK, dev.io_inverted ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdDevPolarity),
                 BM_SETCHECK, dev.polarity_inverted ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdDevEnable),
                 BM_SETCHECK, dev.enable ? BST_CHECKED : BST_UNCHECKED, 0);
    fill_channels(relay_ui::kIdDevTestCh, dev.channels);
    set_text(relay_ui::kIdDevInfo, utf8_to_wide("设备已加载：" + dev.name));
    updating_ = false;
}

RelayDeviceConfig RelayPanel::current_device() const {
    RelayDeviceConfig dev;
    dev.name = wide_to_utf8(get_text(relay_ui::kIdDevName));
    dev.windows_port = wide_to_utf8(get_text(relay_ui::kIdDevPort));
    dev.linux_port = wide_to_utf8(get_text(relay_ui::kIdDevLinux));
    dev.channels = _wtoi(get_text(relay_ui::kIdDevChannels).c_str());
    if (dev.channels < 1) dev.channels = 1;
    if (dev.channels > 32) dev.channels = 32;
    dev.baudrate = _wtoi(get_text(relay_ui::kIdDevBaud).c_str());
    if (dev.baudrate <= 0) dev.baudrate = 9600;
    dev.post_write_delay_ms = _wtoi(get_text(relay_ui::kIdDevDelay).c_str());
    if (dev.post_write_delay_ms < 0) dev.post_write_delay_ms = 0;
    dev.io_inverted = SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdDevIo), BM_GETCHECK, 0, 0) == BST_CHECKED;
    dev.polarity_inverted = SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdDevPolarity), BM_GETCHECK, 0, 0) == BST_CHECKED;
    dev.enable = SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdDevEnable), BM_GETCHECK, 0, 0) == BST_CHECKED;
    return dev;
}

int RelayPanel::selected_device_index() const {
    if (!core_) return -1;
    if (dev_index_ >= 0 && static_cast<std::size_t>(dev_index_) < core_->config().relays.size())
        return dev_index_;
    std::string name = wide_to_utf8(get_text(relay_ui::kIdDevName));
    const auto& relays = core_->config().relays;
    for (std::size_t i = 0; i < relays.size(); ++i)
        if (relays[i].name == name) return static_cast<int>(i);
    return -1;
}

void RelayPanel::save_device() {
    if (!core_ || !mgr_) return;
    RelayDeviceConfig dev = current_device();
    if (dev.name.empty()) { note(L"设备名不能为空", true); return; }
    if (dev.windows_port.empty()) { note(L"Windows 串口不能为空", true); return; }
    int idx = selected_device_index();
    AppConfig cfg = core_->config();
    std::string old_name;
    if (idx >= 0) {
        old_name = cfg.relays[static_cast<std::size_t>(idx)].name;
        cfg.relays[static_cast<std::size_t>(idx)] = dev;
    } else {
        for (const RelayDeviceConfig& r : cfg.relays) {
            if (r.name == dev.name) { note(L"设备名已存在：" + utf8_to_wide(dev.name), true); return; }
        }
        cfg.relays.push_back(dev);
    }
    if (!old_name.empty() && old_name != dev.name) {
        for (BoardConfig& b : cfg.boards) {
            for (BoardActionConfig& a : b.buttons)
                if (a.backend == "relay" && a.relay == old_name) a.relay = dev.name;
        }
        for (SSHLinkConfig& s : cfg.links) {
            for (BoardActionConfig& a : s.buttons)
                if (a.backend == "relay" && a.relay == old_name) a.relay = dev.name;
        }
    }
    if (!config_save(cfg)) { note(L"配置落盘失败：" + utf8_to_wide(cfg.abs_path), true); return; }
    core_->set_service_config(cfg);
    mgr_->apply_config(cfg);
    if (parent_) PostMessageW(parent_, kMsgBoardButtonsChanged, 0, 0);
    fill_devices();
    note(L"设备已保存：" + utf8_to_wide(dev.name));
}

void RelayPanel::delete_device() {
    if (!core_ || !mgr_) return;
    int idx = selected_device_index();
    if (idx < 0) { note(L"请先在列表中选择设备", true); return; }
    AppConfig cfg = core_->config();
    std::string name = cfg.relays[static_cast<std::size_t>(idx)].name;
    for (const BoardConfig& b : cfg.boards) {
        for (const BoardActionConfig& a : b.buttons)
            if (a.backend == "relay" && a.relay == name) {
                note(L"设备仍被通道 " + utf8_to_wide(b.name) + L" 的按钮绑定，不能删除", true);
                return;
            }
    }
    for (const SSHLinkConfig& s : cfg.links) {
        for (const BoardActionConfig& a : s.buttons)
            if (a.backend == "relay" && a.relay == name) {
                note(L"设备仍被通道 " + utf8_to_wide(s.name) + L" 的按钮绑定，不能删除", true);
                return;
            }
    }
    cfg.relays.erase(cfg.relays.begin() + idx);
    if (!config_save(cfg)) { note(L"配置落盘失败", true); return; }
    core_->set_service_config(cfg);
    mgr_->apply_config(cfg);
    if (parent_) PostMessageW(parent_, kMsgBoardButtonsChanged, 0, 0);
    dev_index_ = -1;
    fill_devices();
    note(L"设备已删除：" + utf8_to_wide(name));
}

void RelayPanel::test_device(const std::string& mode) {
    if (!mgr_) return;
    RelayDeviceConfig dev = current_device();
    if (!dev.enable) { note(L"设备未启用，先勾选“启用设备”", true); return; }
    if (dev.windows_port.empty()) { note(L"串口未选择", true); return; }
    if (port_in_use(dev.windows_port)) {
        note(L"串口 " + utf8_to_wide(dev.windows_port) + L" 正被运行中的通道占用，请先断开", true);
        return;
    }
    int ch = _wtoi(get_text(relay_ui::kIdDevTestCh).c_str());
    if (mode == "on" || mode == "off") {
        std::string err;
        if (mgr_->relay_set(dev, ch, mode == "on", &err))
            note(L"CH" + std::to_wstring(ch) + (mode == "on" ? L" 已吸合" : L" 已释放"));
        else
            note(L"操作失败：" + utf8_to_wide(err), true);
    } else {
        double hold = 1.0;
        std::wstring hs = get_text(relay_ui::kIdDevPulseHold);
        if (hs.find(L"0.5") != std::wstring::npos) hold = 0.5;
        else if (hs.find(L"2") != std::wstring::npos) hold = 2.0;
        else if (hs.find(L"5") != std::wstring::npos) hold = 5.0;
        if (mgr_->relay_run_mode_async(dev, ch, "pulse", hold, dev.polarity_inverted))
            note(L"脉冲已下发，后台执行");
        else
            note(L"脉冲未下发：上一路继电器动作还在执行", true);
    }
    update_runtime();
}

bool RelayPanel::port_in_use(const std::string& port) const {
    if (!core_ || port.empty()) return false;
    for (const AppCore::Entry& e : core_->entries()) {
        if (!e.bridge || !e.bridge->running() || e.kind != "serial") continue;
        if (e.detail.rfind(port + " @", 0) == 0 || e.detail == port) return true;
    }
    return false;
}

void RelayPanel::fill_targets() {
    if (!core_) return;
    const auto& entries = core_->entries();
    HWND bind = GetDlgItem(hwnd_, relay_ui::kIdBindTarget);
    HWND script = GetDlgItem(hwnd_, relay_ui::kIdScriptTarget);
    std::wstring keepBind = get_text(relay_ui::kIdBindTarget);
    std::wstring keepScript = get_text(relay_ui::kIdScriptTarget);
    if (bind) SendMessageW(bind, CB_RESETCONTENT, 0, 0);
    if (script) SendMessageW(script, CB_RESETCONTENT, 0, 0);
    for (const AppCore::Entry& e : entries) {
        std::wstring w = utf8_to_wide(e.name);
        if (bind) SendMessageW(bind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
        if (script) SendMessageW(script, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
    }
    int bi = 0, si = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (utf8_to_wide(entries[i].name) == keepBind) bi = static_cast<int>(i);
        if (utf8_to_wide(entries[i].name) == keepScript) si = static_cast<int>(i);
    }
    if (bind && !entries.empty()) SendMessageW(bind, CB_SETCURSEL, bi, 0);
    if (script && !entries.empty()) SendMessageW(script, CB_SETCURSEL, si, 0);
}

std::string RelayPanel::bind_target() const { return wide_to_utf8(get_text(relay_ui::kIdBindTarget)); }
std::string RelayPanel::script_target() const { return wide_to_utf8(get_text(relay_ui::kIdScriptTarget)); }

std::vector<BoardActionConfig> RelayPanel::actions_of(const std::string& channel) const {
    if (!core_) return {};
    const std::vector<BoardActionConfig>* p = core_->buttons_of(channel);
    return p ? *p : std::vector<BoardActionConfig>{};
}

bool RelayPanel::save_actions(const std::string& channel,
                              const std::vector<BoardActionConfig>& actions) {
    if (!core_ || channel.empty()) return false;
    std::string err;
    if (!core_->set_buttons(channel, actions, &err)) {
        note(L"保存失败：" + utf8_to_wide(err), true);
        return false;
    }
    if (parent_) PostMessageW(parent_, kMsgBoardButtonsChanged, 0, 0);
    return true;
}

int RelayPanel::upsert_action(std::vector<BoardActionConfig>* actions, const BoardActionConfig& a) {
    if (!actions) return -1;
    for (std::size_t i = 0; i < actions->size(); ++i) {
        if ((*actions)[i].id == a.id) {
            (*actions)[i] = a;
            return static_cast<int>(i);
        }
    }
    actions->push_back(a);
    return static_cast<int>(actions->size()) - 1;
}

void RelayPanel::fill_bind_list(const std::string& prefer) {
    if (!core_) return;
    HWND combo = GetDlgItem(hwnd_, relay_ui::kIdBindList);
    if (!combo) return;
    std::string channel = bind_target();
    if (channel.empty() && !core_->entries().empty()) channel = core_->entries()[0].name;
    std::vector<BoardActionConfig> list = actions_of(channel);
    std::wstring keep = prefer.empty() ? get_text(relay_ui::kIdBindList) : utf8_to_wide(prefer);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int sel = -1, first = -1;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].backend != "relay") continue;
        std::wstring text = utf8_to_wide(list[i].id + "  ·  " + list[i].label);
        int idx = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())));
        SendMessageW(combo, CB_SETITEMDATA, idx, static_cast<LPARAM>(i));
        if (first < 0) first = idx;
        if (!keep.empty() && (utf8_to_wide(list[i].id) == keep ||
                              text.find(keep) != std::wstring::npos))
            sel = idx;
    }
    if (sel < 0) sel = first;
    if (sel >= 0) {
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
        load_bind(static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, sel, 0)));
    } else {
        load_bind(-1);
    }
}

int RelayPanel::action_index_from_combo(int id) const {
    HWND combo = GetDlgItem(hwnd_, id);
    if (!combo) return -1;
    int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (sel < 0) return -1;
    return static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, sel, 0));
}

void RelayPanel::load_bind(int action_index) {
    if (!core_) return;
    updating_ = true;
    std::vector<BoardActionConfig> list = actions_of(bind_target());
    BoardActionConfig a;
    if (action_index >= 0 && static_cast<std::size_t>(action_index) < list.size()) {
        a = list[static_cast<std::size_t>(action_index)];
    } else {
        a.id = "power_on";
        a.label = "上电";
        for (int n = 1; n < 100; ++n) {
            bool used = find_action(list, a.id) != nullptr;
            if (!used) break;
            a.id = "power_on" + std::to_string(n);
        }
        a.backend = "relay";
        a.mode = "hold_on";
        a.channel = 1;
        a.hold_seconds = 1.0;
        a.enable = true;
        if (core_ && !core_->config().relays.empty()) a.relay = core_->config().relays[0].name;
    }
    set_text(relay_ui::kIdBindId, utf8_to_wide(a.id));
    set_text(relay_ui::kIdBindLabel, utf8_to_wide(a.label));
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdBindEnable), BM_SETCHECK, a.enable ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdBindPolarity), BM_SETCHECK, a.polarity_inverted ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND relay = GetDlgItem(hwnd_, relay_ui::kIdBindRelay);
    if (relay) {
        SendMessageW(relay, CB_RESETCONTENT, 0, 0);
        int sel = -1;
        for (std::size_t i = 0; i < core_->config().relays.size(); ++i) {
            std::wstring w = utf8_to_wide(core_->config().relays[i].name);
            int idx = static_cast<int>(SendMessageW(relay, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
            if (core_->config().relays[i].name == a.relay) sel = idx;
        }
        if (sel < 0 && SendMessageW(relay, CB_GETCOUNT, 0, 0) > 0) sel = 0;
        if (sel >= 0) SendMessageW(relay, CB_SETCURSEL, sel, 0);
    }
    int channels = 2;
    if (const RelayDeviceConfig* d = find_relay(core_->config(), a.relay)) channels = d->channels;
    fill_channels(relay_ui::kIdBindChannel, channels);
    if (a.channel >= 1 && a.channel <= channels)
        SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdBindChannel), CB_SETCURSEL, a.channel - 1, 0);
    int mode_idx = 0;
    if (a.mode == "hold_off") mode_idx = 1;
    else if (a.mode == "pulse") mode_idx = 2;
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdBindMode), CB_SETCURSEL, mode_idx, 0);
    set_text(relay_ui::kIdBindHold, format_seconds(a.hold_seconds));
    set_text(relay_ui::kIdBindInfo, utf8_to_wide(action_index >= 0 ? "已加载绑定：" + a.id
                                                                  : "新绑定草稿"));
    updating_ = false;
}

void RelayPanel::preset_bind(const std::string& id) {
    if (!core_) return;
    std::vector<BoardActionConfig> list = actions_of(bind_target());
    BoardActionConfig a;
    a.backend = "relay";
    a.enable = true;
    a.id = id;
    if (id == "power_on") { a.label = "上电"; a.mode = "hold_on"; a.channel = 1; a.hold_seconds = 1.0; }
    if (id == "power_off") { a.label = "下电"; a.mode = "hold_off"; a.channel = 1; a.hold_seconds = 1.0; }
    if (id == "reboot") { a.label = "重启"; a.mode = "pulse"; a.channel = 2; a.hold_seconds = 1.0; }
    if (!core_->config().relays.empty()) a.relay = core_->config().relays[0].name;
    if (const RelayDeviceConfig* d = find_relay(core_->config(), a.relay)) {
        a.polarity_inverted = d->polarity_inverted;
        if (a.channel > d->channels) a.channel = 1;
    }
    set_text(relay_ui::kIdBindId, utf8_to_wide(a.id));
    set_text(relay_ui::kIdBindLabel, utf8_to_wide(a.label));
    set_text(relay_ui::kIdBindHold, format_seconds(a.hold_seconds));
    int mode_idx = a.mode == "hold_off" ? 1 : (a.mode == "pulse" ? 2 : 0);
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdBindMode), CB_SETCURSEL, mode_idx, 0);
    fill_channels(relay_ui::kIdBindChannel, 2);
    if (a.channel >= 1 && a.channel <= 2)
        SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdBindChannel), CB_SETCURSEL, a.channel - 1, 0);
    note(L"预设已填入，保存后生效");
}

void RelayPanel::save_bind() {
    if (!core_) return;
    std::string channel = bind_target();
    if (channel.empty()) { note(L"目标通道为空", true); return; }
    BoardActionConfig a;
    a.id = wide_to_utf8(get_text(relay_ui::kIdBindId));
    a.label = wide_to_utf8(get_text(relay_ui::kIdBindLabel));
    a.enable = SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdBindEnable), BM_GETCHECK, 0, 0) == BST_CHECKED;
    a.polarity_inverted = SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdBindPolarity), BM_GETCHECK, 0, 0) == BST_CHECKED;
    a.backend = "relay";
    a.relay = wide_to_utf8(get_text(relay_ui::kIdBindRelay));
    a.channel = _wtoi(get_text(relay_ui::kIdBindChannel).c_str());
    int mi = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdBindMode), CB_GETCURSEL, 0, 0));
    a.mode = mi == 1 ? "hold_off" : (mi == 2 ? "pulse" : "hold_on");
    a.hold_seconds = _wtof(get_text(relay_ui::kIdBindHold).c_str());
    if (a.hold_seconds <= 0) a.hold_seconds = 1.0;
    if (!valid_id(a.id)) { note(L"ID 只能包含字母、数字、下划线和点", true); return; }
    if (a.label.empty()) { note(L"按钮文字不能为空", true); return; }
    const RelayDeviceConfig* dev = find_relay(core_->config(), a.relay);
    if (!dev) { note(L"请选择有效的继电器设备", true); return; }
    if (a.channel < 1 || a.channel > dev->channels) { note(L"继电器通道超出范围", true); return; }
    std::vector<BoardActionConfig> list = actions_of(channel);
    // 按 id 增量 upsert：从“上电”切到“下电”再保存，不能把上电覆盖掉
    upsert_action(&list, a);
    if (!save_actions(channel, list)) return;
    fill_bind_list(a.id);
    note(L"绑定已保存：" + utf8_to_wide(a.id));
}

void RelayPanel::delete_bind() {
    std::string channel = bind_target();
    std::vector<BoardActionConfig> list = actions_of(channel);
    int idx = action_index_from_combo(relay_ui::kIdBindList);
    if (idx < 0 || static_cast<std::size_t>(idx) >= list.size()) {
        note(L"请先选择绑定", true);
        return;
    }
    std::string id = list[static_cast<std::size_t>(idx)].id;
    list.erase(list.begin() + idx);
    if (!save_actions(channel, list)) return;
    fill_bind_list("");
    note(L"绑定已删除：" + utf8_to_wide(id));
}

void RelayPanel::test_bind() {
    if (!core_ || !mgr_) return;
    std::string channel = bind_target();
    if (channel.empty()) { note(L"目标通道为空", true); return; }
    BoardActionConfig a;
    a.backend = "relay";
    a.enable = true;
    a.id = wide_to_utf8(get_text(relay_ui::kIdBindId));
    a.label = wide_to_utf8(get_text(relay_ui::kIdBindLabel));
    a.relay = wide_to_utf8(get_text(relay_ui::kIdBindRelay));
    a.channel = _wtoi(get_text(relay_ui::kIdBindChannel).c_str());
    int mi = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdBindMode), CB_GETCURSEL, 0, 0));
    a.mode = mi == 1 ? "hold_off" : (mi == 2 ? "pulse" : "hold_on");
    a.hold_seconds = _wtof(get_text(relay_ui::kIdBindHold).c_str());
    a.polarity_inverted = SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdBindPolarity), BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (a.hold_seconds <= 0) a.hold_seconds = 1.0;
    AppCore* core = core_;
    PortInUseFn fn = [core](const std::string& port) {
        for (const AppCore::Entry& e : core->entries()) {
            if (!e.bridge || !e.bridge->running() || e.kind != "serial") continue;
            if (e.detail.rfind(port + " @", 0) == 0 || e.detail == port) return true;
        }
        return false;
    };
    run_board_action_async(core_->config(), channel, a, core_->find_shared(channel), fn, hwnd_,
                           kMsgRelayActionResult);
    note(L"测试已开始：" + utf8_to_wide(a.id));
}

void RelayPanel::generate_standard() {
    if (!core_) return;
    std::string channel = (page_ == 2 ? script_target() : bind_target());
    if (channel.empty()) { note(L"目标通道为空", true); return; }
    std::string relay = wide_to_utf8(get_text(relay_ui::kIdBindRelay));
    if (relay.empty() && !core_->config().relays.empty()) relay = core_->config().relays[0].name;
    const RelayDeviceConfig* dev = find_relay(core_->config(), relay);
    if (!dev) { note(L"请先选择继电器设备", true); return; }
    std::vector<BoardActionConfig> list = actions_of(channel);
    auto make = [&](const char* id, const char* label, const char* mode, int ch, double hold) {
        BoardActionConfig a;
        a.id = id;
        a.label = label;
        a.backend = "relay";
        a.relay = relay;
        a.channel = ch;
        a.mode = mode;
        a.hold_seconds = hold;
        a.polarity_inverted = dev->polarity_inverted;
        a.enable = true;
        upsert_action(&list, a);
    };
    make("power_on", "上电", "hold_on", 1, 1.0);
    make("power_off", "下电", "hold_off", 1, 1.0);
    make("reboot", "重启", "pulse", dev->channels >= 2 ? 2 : 1, 1.0);
    if (!save_actions(channel, list)) return;
    fill_bind_list("power_on");
    note(L"已生成标准按钮：上电 / 下电 / 重启");
}

void RelayPanel::generate_upgrade_script() {
    if (!core_) return;
    std::string channel = (page_ == 2 ? script_target() : bind_target());
    if (channel.empty()) { note(L"目标通道为空", true); return; }
    std::string relay = wide_to_utf8(get_text(relay_ui::kIdBindRelay));
    if (relay.empty() && !core_->config().relays.empty()) relay = core_->config().relays[0].name;
    const RelayDeviceConfig* dev = find_relay(core_->config(), relay);
    if (!dev) { note(L"请先选择继电器设备", true); return; }
    std::string body = upgrade_mode_body(relay, dev->channels);
    if (body.empty()) { note(L"进升级模式至少需要 2 路继电器", true); return; }
    std::vector<BoardActionConfig> list = actions_of(channel);
    BoardActionConfig a;
    a.id = "enter_upgrade_mode";
    a.label = "进升级模式";
    a.backend = "script";
    a.script = body;
    a.enable = true;
    upsert_action(&list, a);
    if (!save_actions(channel, list)) return;
    // 把脚本页目标同步到刚刚生成按钮的通道，避免两个页签目标不同导致看不到新按钮
    HWND scriptTarget = GetDlgItem(hwnd_, relay_ui::kIdScriptTarget);
    for (int i = 0; scriptTarget && i < static_cast<int>(SendMessageW(scriptTarget, CB_GETCOUNT, 0, 0)); ++i) {
        wchar_t buf[256] = L"";
        SendMessageW(scriptTarget, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(buf));
        if (wide_to_utf8(std::wstring(buf)) == channel) {
            SendMessageW(scriptTarget, CB_SETCURSEL, i, 0);
            break;
        }
    }
    fill_script_list("enter_upgrade_mode");
    show_page(2);
    note(L"已生成并保存按钮：进升级模式");
}

void RelayPanel::fill_script_list(const std::string& prefer) {
    if (!core_) return;
    HWND combo = GetDlgItem(hwnd_, relay_ui::kIdScriptList);
    if (!combo) return;
    std::string channel = script_target();
    if (channel.empty() && !core_->entries().empty()) channel = core_->entries()[0].name;
    std::vector<BoardActionConfig> list = actions_of(channel);
    std::wstring keep = prefer.empty() ? get_text(relay_ui::kIdScriptList) : utf8_to_wide(prefer);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int sel = -1, first = -1;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].backend == "relay") continue;
        std::string kind = list[i].backend == "script"
                               ? (script_is_lua_source(list[i].script) ? "Lua" : "简单")
                               : (list[i].run == "host" ? "本机" : "板端");
        std::wstring text = utf8_to_wide(list[i].id + "  ·  " + list[i].label + "  [" + kind + "]");
        int idx = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())));
        SendMessageW(combo, CB_SETITEMDATA, idx, static_cast<LPARAM>(i));
        if (first < 0) first = idx;
        if (!keep.empty() && (utf8_to_wide(list[i].id) == keep || text.find(keep) != std::wstring::npos))
            sel = idx;
    }
    if (sel < 0) sel = first;
    if (sel >= 0) {
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
        load_script(static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, sel, 0)));
    } else {
        load_script(-1);
    }
}

void RelayPanel::load_script(int action_index) {
    if (!core_) return;
    updating_ = true;
    std::vector<BoardActionConfig> list = actions_of(script_target());
    BoardActionConfig a;
    if (action_index >= 0 && static_cast<std::size_t>(action_index) < list.size()) {
        a = list[static_cast<std::size_t>(action_index)];
    } else {
        // 新脚本草稿：给一个可直接保存的通用按钮，避免出现“进升级模式”标题配登录脚本的错配
        a.id = "button1";
        for (int n = 1; n < 100; ++n) {
            if (!find_action(list, a.id)) break;
            a.id = "button" + std::to_string(n + 1);
        }
        a.label = "新按钮";
        a.backend = "script";
        a.script = "--lua\r\nprint('hello')\r\n";
        a.enable = true;
    }
    set_text(relay_ui::kIdScriptId, utf8_to_wide(a.id));
    set_text(relay_ui::kIdScriptLabel, utf8_to_wide(a.label));
    int kind = 0;
    if (a.backend == "commands") kind = (a.run == "host" ? 2 : 1);
    else if (a.backend == "script" && !script_is_lua_source(a.script)) kind = 3;
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdScriptKind), CB_SETCURSEL, kind, 0);
    set_text(relay_ui::kIdScriptDelay, std::to_wstring(a.delay_ms > 0 ? a.delay_ms : 200));
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdScriptEnable), BM_SETCHECK, a.enable ? BST_CHECKED : BST_UNCHECKED, 0);
    std::string body = a.backend == "commands" ? join_lines(a.commands) : a.script;
    set_text(relay_ui::kIdScriptBody, edit_multiline(utf8_to_wide(body)));
    set_text(relay_ui::kIdScriptInfo, utf8_to_wide(action_index >= 0 ? "已加载按钮：" + a.id
                                                                   : "新脚本草稿"));
    updating_ = false;
}

void RelayPanel::preset_script(int preset) {
    if (!core_) return;
    std::string relay;
    if (!core_->config().relays.empty()) relay = core_->config().relays[0].name;
    const RelayDeviceConfig* dev = find_relay(core_->config(), relay);
    int channels = dev ? dev->channels : 2;
    std::string id, label, body;
    int kind = 0;
    if (preset == 0) {
        id = "login"; label = "登录";
        body = "--lua\r\nlocal m = channel.mark()\r\nchannel.login()\r\nlocal out = channel.wait_idle(m, 300, 8)\r\nprint(out or \"登录已发送\")\r\n";
    } else if (preset == 1) {
        id = "enter_uboot"; label = "进 U-Boot";
        body = preset_script_body(0, relay, channels);
    } else if (preset == 2) {
        id = "upgrade"; label = "升级"; kind = 1;
        body = preset_script_body(1, relay, channels);
    } else {
        id = "enter_upgrade_mode"; label = "进升级模式";
        body = upgrade_mode_body(relay, channels);
        if (body.empty()) { note(L"进升级模式至少需要 2 路继电器", true); return; }
    }
    set_text(relay_ui::kIdScriptId, utf8_to_wide(id));
    set_text(relay_ui::kIdScriptLabel, utf8_to_wide(label));
    SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdScriptKind), CB_SETCURSEL, kind, 0);
    set_text(relay_ui::kIdScriptBody, edit_multiline(utf8_to_wide(body)));
    note(L"预设已填入，保存后生效");
}

void RelayPanel::save_script() {
    if (!core_) return;
    std::string channel = script_target();
    if (channel.empty()) { note(L"目标通道为空", true); return; }
    BoardActionConfig a;
    a.id = wide_to_utf8(get_text(relay_ui::kIdScriptId));
    a.label = wide_to_utf8(get_text(relay_ui::kIdScriptLabel));
    a.enable = SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdScriptEnable), BM_GETCHECK, 0, 0) == BST_CHECKED;
    int kind = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdScriptKind), CB_GETCURSEL, 0, 0));
    std::string body = wide_to_utf8(get_text(relay_ui::kIdScriptBody));
    if (!valid_id(a.id)) { note(L"ID 只能包含字母、数字、下划线和点", true); return; }
    if (a.label.empty()) { note(L"按钮文字不能为空", true); return; }
    if (body.empty()) { note(L"脚本或命令不能为空", true); return; }
    if (kind == 0) {
        a.backend = "script";
        a.script = script_is_lua_source(body) ? body : "--lua\r\n" + body;
    } else if (kind == 3) {
        if (script_is_lua_source(body)) {
            note(L"简单脚本首行不能是 --lua / #!lua；需要 Lua 请把类型改为“Lua 脚本”", true);
            return;
        }
        a.backend = "script";
        a.script = body;
    } else {
        a.backend = "commands";
        a.run = (kind == 2 ? "host" : "channel");
        a.commands = split_lines(body);
        a.delay_ms = _wtoi(get_text(relay_ui::kIdScriptDelay).c_str());
        if (a.commands.empty()) { note(L"命令列表为空", true); return; }
    }
    std::vector<BoardActionConfig> list = actions_of(channel);
    // 按 id 增量 upsert，避免“新建脚本”覆盖当前选中的旧脚本
    upsert_action(&list, a);
    if (!save_actions(channel, list)) return;
    fill_script_list(a.id);
    note(L"按钮已保存：" + utf8_to_wide(a.id));
}

void RelayPanel::delete_script() {
    std::string channel = script_target();
    std::vector<BoardActionConfig> list = actions_of(channel);
    int idx = action_index_from_combo(relay_ui::kIdScriptList);
    if (idx < 0 || static_cast<std::size_t>(idx) >= list.size()) {
        note(L"请先选择按钮", true);
        return;
    }
    std::string id = list[static_cast<std::size_t>(idx)].id;
    list.erase(list.begin() + idx);
    if (!save_actions(channel, list)) return;
    fill_script_list("");
    note(L"按钮已删除：" + utf8_to_wide(id));
}

void RelayPanel::test_script() {
    if (!core_) return;
    std::string channel = script_target();
    if (channel.empty()) { note(L"目标通道为空", true); return; }
    BoardActionConfig a;
    a.id = wide_to_utf8(get_text(relay_ui::kIdScriptId));
    a.label = wide_to_utf8(get_text(relay_ui::kIdScriptLabel));
    a.enable = true;
    int kind = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, relay_ui::kIdScriptKind), CB_GETCURSEL, 0, 0));
    std::string body = wide_to_utf8(get_text(relay_ui::kIdScriptBody));
    if (kind == 0) {
        a.backend = "script";
        a.script = script_is_lua_source(body) ? body : "--lua\r\n" + body;
    } else if (kind == 3) {
        if (script_is_lua_source(body)) {
            note(L"简单脚本首行不能是 --lua / #!lua", true);
            return;
        }
        a.backend = "script";
        a.script = body;
    } else {
        a.backend = "commands";
        a.run = (kind == 2 ? "host" : "channel");
        a.commands = split_lines(body);
    }
    AppCore* core = core_;
    PortInUseFn fn = [core](const std::string& port) {
        for (const AppCore::Entry& e : core->entries()) {
            if (!e.bridge || !e.bridge->running() || e.kind != "serial") continue;
            if (e.detail.rfind(port + " @", 0) == 0 || e.detail == port) return true;
        }
        return false;
    };
    run_board_action_async(core_->config(), channel, a, core_->find_shared(channel), fn, hwnd_,
                           kMsgRelayActionResult);
    note(L"测试已开始：" + utf8_to_wide(a.id));
}

void RelayPanel::update_target_info() {
    if (!core_) return;
    std::string channel = page_ == 1 ? bind_target() : script_target();
    std::wstring text;
    if (channel.empty()) {
        text = L"没有可用通道";
    } else {
        std::string state = "离线";
        for (const AppCore::Entry& e : core_->entries()) {
            if (e.name == channel) {
                state = (e.bridge && e.bridge->running()) ? "在线" : "离线";
                break;
            }
        }
        text = L"通道 " + utf8_to_wide(channel) + L" · " + utf8_to_wide(state);
    }
    set_text(relay_ui::kIdBindTargetInfo, text);
}

void RelayPanel::update_runtime() {
    if (!mgr_ || !hwnd_) return;
    int page = page_;
    if (page == 0) {
        RelayDeviceConfig dev = current_device();
        std::wstring info;
        if (dev.channels < 1) info = L"通道数无效";
        for (int i = 1; i <= std::min(dev.channels, 8); ++i) {
            auto st = mgr_->relay_state(dev.windows_port, i);
            if (!info.empty()) info += L"    ";
            info += L"CH" + std::to_wstring(i) + L"：" +
                    (st.on ? std::wstring(L"吸合") : std::wstring(L"断开")) +
                    (st.busy ? std::wstring(L"(忙)") : std::wstring());
            if (!st.last_error.empty()) info += L"（" + utf8_to_wide(st.last_error) + L"）";
        }
        set_text(relay_ui::kIdDevStates, info.empty() ? L"无状态" : info);
        set_text(relay_ui::kIdDevStatus, mgr_->relay_busy() ? L"继电器动作执行中…" : L"状态就绪");
    }
    update_target_info();
}

void RelayPanel::set_text(int id, const std::wstring& text) {
    if (HWND h = GetDlgItem(hwnd_, id)) SetWindowTextW(h, text.c_str());
}

std::wstring RelayPanel::get_text(int id) const {
    HWND h = GetDlgItem(hwnd_, id);
    if (!h) return L"";
    wchar_t buf[2048] = L"";
    GetWindowTextW(h, buf, 2048);
    return buf;
}

std::string RelayPanel::combo_text(int id) const {
    return wide_to_utf8(get_text(id));
}

void RelayPanel::set_combo_text(int id, const std::wstring& text) {
    set_text(id, text);
}

void RelayPanel::note(const std::wstring& text, bool error) {
    set_text(relay_ui::kIdMsg, (error ? L"⚠ " : L"✓ ") + text);
}

void RelayPanel::refresh() {
    fill_devices();
    fill_targets();
    fill_bind_list("");
    fill_script_list("");
    update_runtime();
}

}  // namespace sb3

