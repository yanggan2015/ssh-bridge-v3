#include "ui/services/service_panel.h"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/util/util.h"
#include "services/service_manager.h"
#include "ui/aibridge/bridge_host.h"

namespace sb3 {
namespace {

int g_dpi = 96;
int S(int v) { return MulDiv(v, g_dpi, 96); }

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
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
    for (const auto& s : items) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
    }
}

bool browse_folder(HWND owner, const std::wstring& title, std::wstring* out) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
    if (!idl) return false;
    wchar_t path[MAX_PATH * 2] = L"";
    bool ok = SHGetPathFromIDListW(idl, path) == TRUE;
    CoTaskMemFree(idl);
    if (ok && out) *out = path;
    return ok;
}

}  // namespace

bool ServicePanel::open(HINSTANCE inst, HWND parent, AppCore* core, ServiceManager* mgr,
                        HFONT font, int dpi) {
    if (is_open()) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
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
    aibridge::set_service_manager(mgr_);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = svc_ui::kClass;
    RegisterClassExW(&wc);

    RECT rc{0, 0, S(1120), S(632)};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = 80, y = 40;
    if (parent) {
        RECT pr{};
        GetWindowRect(parent, &pr);
        x = pr.left + (pr.right - pr.left - w) / 2;
        y = pr.top + (pr.bottom - pr.top - h) / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
    }
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, svc_ui::kClass, L"服务与继电器 — ssh-bridge v3",
                            WS_OVERLAPPEDWINDOW, x, y, w, h, parent, nullptr, inst_, this);
    if (!hwnd_) return false;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    return true;
}

void ServicePanel::close() {
    if (is_open()) DestroyWindow(hwnd_);
}

LRESULT CALLBACK ServicePanel::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ServicePanel* self = reinterpret_cast<ServicePanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<ServicePanel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(msg, wp, lp);
}

LRESULT ServicePanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        build();
        SetTimer(hwnd_, 1, 250, nullptr);
        return 0;
    case WM_TIMER:
        if (wp == 1 && !updating_) update_runtime();
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == CBN_SELCHANGE)
            on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
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

void ServicePanel::build() {
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w,
                    int h, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w),
                                 S(h), hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 inst_, nullptr);
        if (c && font_) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return c;
    };
    auto label = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
        return make(L"STATIC", text, SS_LEFT, x, y, w, h, id);
    };
    auto button = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
        return make(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, h, id);
    };
    auto check = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
        return make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, h, id);
    };
    auto combo = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
        return make(L"COMBOBOX", text,
                    CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, x, y, w, h, id);
    };
    auto edit = [&](const wchar_t* text, int x, int y, int w, int h, int id, bool read_only) {
        return make(L"EDIT", text,
                    WS_BORDER | ES_LEFT | ES_AUTOHSCROLL | (read_only ? ES_READONLY : 0),
                    x, y, w, h, id);
    };

    label(L"服务与继电器", 24, 14, 260, 26, svc_ui::kIdTitle);
    make(L"BUTTON", L"继电器", BS_GROUPBOX, 20, 44, 1060, 226, svc_ui::kIdGroupRelay);
    make(L"BUTTON", L"文件服务（TFTP / 共享目录 / NFS 同一份文件）", BS_GROUPBOX, 20, 282, 1060,
         300, svc_ui::kIdGroupFiles);

    label(L"设备", 36, 76, 56, 22, svc_ui::kIdLabelRelayDevice);
    combo(L"", 110, 72, 220, 200, svc_ui::kIdRelayDevice);
    label(L"串口", 346, 76, 40, 22, svc_ui::kIdLabelRelayPort);
    combo(L"", 392, 72, 96, 200, svc_ui::kIdRelayPort);
    label(L"波特率", 500, 76, 50, 22, svc_ui::kIdLabelBaud);
    combo(L"9600", 554, 72, 96, 200, svc_ui::kIdRelayBaud);
    button(L"刷新", 660, 72, 60, 26, svc_ui::kIdRelayRefresh);
    check(L"启用该设备", 736, 76, 104, 24, svc_ui::kIdRelayEnable);
    button(L"保存继电器配置", 848, 72, 138, 27, svc_ui::kIdRelaySave);

    button(L"通道 1：吸合", 110, 112, 188, 30, svc_ui::kIdRelayCh1);
    button(L"通道 2：吸合", 310, 112, 188, 30, svc_ui::kIdRelayCh2);
    label(L"脉冲时长", 516, 116, 62, 22, svc_ui::kIdLabelHold);
    combo(L"1 秒", 580, 112, 80, 200, svc_ui::kIdRelayHold);
    button(L"脉冲 1", 670, 112, 100, 30, svc_ui::kIdRelayPulse1);
    button(L"脉冲 2", 780, 112, 100, 30, svc_ui::kIdRelayPulse2);
    check(L"IO 低电平有效", 900, 116, 144, 24, svc_ui::kIdRelayInv);

    label(L"", 36, 156, 1020, 24, svc_ui::kIdRelayInfo);
    edit(L"", 36, 186, 1020, 58, svc_ui::kIdRelayStatus, true);

    label(L"共用目录", 36, 312, 70, 22, svc_ui::kIdLabelRoot);
    combo(L"", 110, 308, 600, 200, svc_ui::kIdRoot);
    button(L"选择…", 722, 308, 80, 26, svc_ui::kIdPickRoot);
    button(L"创建板卡目录", 814, 308, 120, 26, svc_ui::kIdLayout);

    label(L"TFTP", 36, 350, 60, 22, svc_ui::kIdLabelTftp);
    combo(L"", 110, 346, 150, 200, svc_ui::kIdTftpIp);
    label(L"端口", 272, 350, 42, 22, svc_ui::kIdLabelTftpPort);
    combo(L"69", 318, 346, 84, 200, svc_ui::kIdTftpPort);
    check(L"允许写入", 414, 350, 96, 24, svc_ui::kIdTftpWrite);
    button(L"启动 TFTP", 522, 345, 110, 28, svc_ui::kIdTftpBtn);
    label(L"", 648, 350, 420, 22, svc_ui::kIdTftpStatus);

    label(L"共享 HTTP", 36, 390, 76, 22, svc_ui::kIdLabelShare);
    label(L"端口", 114, 390, 42, 22, svc_ui::kIdLabelSharePort);
    combo(L"18082", 160, 386, 84, 200, svc_ui::kIdSharePort);
    button(L"启动共享", 256, 385, 110, 28, svc_ui::kIdShareBtn);
    button(L"打开浏览器", 378, 385, 120, 28, svc_ui::kIdShareOpen);
    label(L"", 514, 390, 554, 22, svc_ui::kIdShareStatus);

    label(L"NFS", 36, 430, 40, 22, svc_ui::kIdLabelNfs);
    combo(L"/share", 82, 426, 130, 200, svc_ui::kIdNfsAlias);
    label(L"UID", 228, 430, 34, 22, svc_ui::kIdLabelUid);
    combo(L"0", 264, 426, 72, 200, svc_ui::kIdNfsUid);
    label(L"GID", 350, 430, 34, 22, svc_ui::kIdLabelGid);
    combo(L"0", 386, 426, 72, 200, svc_ui::kIdNfsGid);
    button(L"启动 NFS", 470, 425, 110, 28, svc_ui::kIdNfsBtn);
    label(L"", 596, 430, 472, 22, svc_ui::kIdNfsStatus);

    button(L"保存服务配置", 110, 456, 130, 28, svc_ui::kIdSave);
    button(L"全部停止", 250, 456, 110, 28, svc_ui::kIdStopAll);
    edit(L"", 36, 490, 1020, 84, svc_ui::kIdMsg, true);

    button(L"关闭", 980, 600, 100, 30, svc_ui::kIdClose);

    add_combo_strings(GetDlgItem(hwnd_, svc_ui::kIdRelayHold),
                      {L"0.5 秒", L"1 秒", L"2 秒", L"5 秒"});
    SendMessageW(GetDlgItem(hwnd_, svc_ui::kIdRelayHold), CB_SETCURSEL, 1, 0);
    add_combo_strings(GetDlgItem(hwnd_, svc_ui::kIdRelayBaud),
                      {L"9600", L"19200", L"38400", L"57600", L"115200"});
    add_combo_strings(GetDlgItem(hwnd_, svc_ui::kIdTftpPort), {L"69", L"6969", L"1069"});
    SendMessageW(GetDlgItem(hwnd_, svc_ui::kIdTftpPort), CB_SETCURSEL, 0, 0);
    add_combo_strings(GetDlgItem(hwnd_, svc_ui::kIdSharePort), {L"18082", L"18083", L"8080"});
    SendMessageW(GetDlgItem(hwnd_, svc_ui::kIdSharePort), CB_SETCURSEL, 0, 0);
    add_combo_strings(GetDlgItem(hwnd_, svc_ui::kIdNfsAlias), {L"/share", L"/mnt/share"});
    add_combo_strings(GetDlgItem(hwnd_, svc_ui::kIdNfsUid), {L"0", L"1000", L"1001"});
    add_combo_strings(GetDlgItem(hwnd_, svc_ui::kIdNfsGid), {L"0", L"1000", L"1001"});

    fill_ports();
    fill_ip_list();
    load_config();
    update_runtime();
}

void ServicePanel::fill_ports() {
    HWND combo = GetDlgItem(hwnd_, svc_ui::kIdRelayPort);
    if (!combo) return;
    wchar_t cur[128] = L"";
    GetWindowTextW(combo, cur, 128);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    auto ports = serial_ports();
    if (ports.empty()) ports = {L"COM3", L"COM5"};
    add_combo_strings(combo, ports);
    if (cur[0]) SetWindowTextW(combo, cur);
    else SetWindowTextW(combo, ports[0].c_str());
}

void ServicePanel::fill_ip_list() {
    HWND combo = GetDlgItem(hwnd_, svc_ui::kIdTftpIp);
    if (!combo) return;
    wchar_t cur[128] = L"";
    GetWindowTextW(combo, cur, 128);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    std::vector<std::wstring> items = {L"自动（0.0.0.0）", L"127.0.0.1"};
    if (mgr_) {
        std::string ip = mgr_->local_ip();
        if (!ip.empty()) items.push_back(utf8_to_wide(ip));
    }
    add_combo_strings(combo, items);
    if (cur[0]) SetWindowTextW(combo, cur);
    else SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

void ServicePanel::load_config() {
    if (!core_ || !mgr_) return;
    updating_ = true;
    const AppConfig& cfg = core_->config();

    set_text(svc_ui::kIdRoot, utf8_to_wide(mgr_->share_root()));
    set_text(svc_ui::kIdTftpIp, cfg.tftp.bind_ip.empty() ? L"自动（0.0.0.0）"
                                                        : utf8_to_wide(cfg.tftp.bind_ip));
    set_text(svc_ui::kIdTftpPort, std::to_wstring(cfg.tftp.port > 0 ? cfg.tftp.port : 69).c_str());
    SendMessageW(GetDlgItem(hwnd_, svc_ui::kIdTftpWrite), BM_SETCHECK,
                 cfg.tftp.allow_write ? BST_CHECKED : BST_UNCHECKED, 0);
    set_text(svc_ui::kIdSharePort,
             std::to_wstring(cfg.share.port > 0 ? cfg.share.port : 18082).c_str());
    set_text(svc_ui::kIdNfsAlias, utf8_to_wide(cfg.nfs.alias.empty() ? "/share" : cfg.nfs.alias));
    set_text(svc_ui::kIdNfsUid, std::to_wstring(cfg.nfs.uid).c_str());
    set_text(svc_ui::kIdNfsGid, std::to_wstring(cfg.nfs.gid).c_str());

    fill_relay_devices();
    load_relay_fields();
    updating_ = false;
}

void ServicePanel::fill_relay_devices() {
    HWND combo = GetDlgItem(hwnd_, svc_ui::kIdRelayDevice);
    if (!combo) return;
    wchar_t keep[128] = L"";
    GetWindowTextW(combo, keep, 128);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    std::vector<std::wstring> names;
    if (core_) {
        for (const RelayDeviceConfig& r : core_->config().relays) {
            if (!r.name.empty()) names.push_back(utf8_to_wide(r.name));
        }
    }
    if (names.empty()) names.push_back(L"relay1");
    add_combo_strings(combo, names);
    int sel = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == keep) sel = static_cast<int>(i);
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    if (keep[0] && sel < static_cast<int>(names.size()))
        SetWindowTextW(combo, names[static_cast<size_t>(sel)].c_str());
}

void ServicePanel::load_relay_fields() {
    wchar_t name[128] = L"";
    GetWindowTextW(GetDlgItem(hwnd_, svc_ui::kIdRelayDevice), name, 128);
    const RelayDeviceConfig* dev = nullptr;
    if (core_) {
        for (const RelayDeviceConfig& r : core_->config().relays) {
            if (utf8_to_wide(r.name) == name) dev = &r;
        }
    }
    if (dev) {
        set_text(svc_ui::kIdRelayPort, utf8_to_wide(dev->windows_port));
        set_text(svc_ui::kIdRelayBaud, std::to_wstring(dev->baudrate).c_str());
        SendMessageW(GetDlgItem(hwnd_, svc_ui::kIdRelayEnable), BM_SETCHECK,
                     dev->enable ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(hwnd_, svc_ui::kIdRelayInv), BM_SETCHECK,
                     dev->io_inverted ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
        set_text(svc_ui::kIdRelayPort, L"COM3");
        set_text(svc_ui::kIdRelayBaud, L"9600");
        SendMessageW(GetDlgItem(hwnd_, svc_ui::kIdRelayEnable), BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(hwnd_, svc_ui::kIdRelayInv), BM_SETCHECK, BST_UNCHECKED, 0);
    }
}

RelayDeviceConfig ServicePanel::current_relay() const {
    RelayDeviceConfig dev;
    dev.name = wide_to_utf8(get_text(svc_ui::kIdRelayDevice));
    dev.windows_port = wide_to_utf8(get_text(svc_ui::kIdRelayPort));
    dev.enable = SendMessageW(GetDlgItem(hwnd_, svc_ui::kIdRelayEnable), BM_GETCHECK, 0, 0) ==
                 BST_CHECKED;
    dev.channels = 2;
    dev.baudrate = 9600;
    std::wstring baud = get_text(svc_ui::kIdRelayBaud);
    if (!baud.empty()) dev.baudrate = _wtoi(baud.c_str());
    if (dev.baudrate <= 0) dev.baudrate = 9600;
    dev.io_inverted = SendMessageW(GetDlgItem(hwnd_, svc_ui::kIdRelayInv), BM_GETCHECK, 0, 0) ==
                      BST_CHECKED;
    dev.post_write_delay_ms = 100;
    dev.polarity_inverted = false;
    return dev;
}

int ServicePanel::selected_relay_index() const {
    std::wstring name = get_text(svc_ui::kIdRelayDevice);
    if (core_) {
        const auto& relays = core_->config().relays;
        for (size_t i = 0; i < relays.size(); ++i) {
            if (utf8_to_wide(relays[i].name) == name) return static_cast<int>(i);
        }
    }
    return -1;
}

void ServicePanel::set_text(int id, const std::wstring& text) {
    if (HWND h = GetDlgItem(hwnd_, id)) SetWindowTextW(h, text.c_str());
}

std::wstring ServicePanel::get_text(int id) const {
    HWND h = GetDlgItem(hwnd_, id);
    if (!h) return L"";
    wchar_t buf[1024] = L"";
    GetWindowTextW(h, buf, 1024);
    return buf;
}

void ServicePanel::set_status(int id, const std::wstring& text, COLORREF) { set_text(id, text); }

void ServicePanel::note(const std::wstring& text, bool error) {
    set_text(svc_ui::kIdMsg, (error ? L"⚠ " : L"✓ ") + text);
}

void ServicePanel::update_runtime() {
    if (!mgr_ || !hwnd_) return;
    updating_ = true;
    std::wstring tftp = utf8_to_wide(mgr_->tftp_status());
    std::wstring share = utf8_to_wide(mgr_->share_status());
    std::wstring nfs = utf8_to_wide(mgr_->nfs_status());
    set_status(svc_ui::kIdTftpStatus, tftp, RGB(0, 0, 0));
    set_status(svc_ui::kIdShareStatus, share, RGB(0, 0, 0));
    set_status(svc_ui::kIdNfsStatus, nfs, RGB(0, 0, 0));
    set_text(svc_ui::kIdTftpBtn, mgr_->tftp_running() ? L"停止 TFTP" : L"启动 TFTP");
    set_text(svc_ui::kIdShareBtn, mgr_->share_running() ? L"停止共享" : L"启动共享");
    set_text(svc_ui::kIdNfsBtn, mgr_->nfs_running() ? L"停止 NFS" : L"启动 NFS");

    RelayDeviceConfig dev = current_relay();
    auto st1 = mgr_->relay_state(dev.windows_port, 1);
    auto st2 = mgr_->relay_state(dev.windows_port, 2);
    std::wstring info = std::wstring(L"CH1：") + (st1.on ? L"已吸合" : L"已断开") +
                        L"    CH2：" + (st2.on ? L"已吸合" : L"已断开") +
                        (mgr_->relay_busy() ? L"    （脉冲执行中…）" : L"");
    set_text(svc_ui::kIdRelayInfo, info);
    std::wstring err = utf8_to_wide(st1.last_error);
    if (err.empty()) err = utf8_to_wide(st2.last_error);
    set_status(svc_ui::kIdRelayStatus, err.empty() ? L"状态正常" : L"错误：" + err,
               RGB(180, 35, 24));
    set_text(svc_ui::kIdRelayCh1,
             st1.on ? L"通道 1：已吸合（点此断开）" : L"通道 1：已断开（点此吸合）");
    set_text(svc_ui::kIdRelayCh2,
             st2.on ? L"通道 2：已吸合（点此断开）" : L"通道 2：已断开（点此吸合）");
    updating_ = false;
}

void ServicePanel::refresh() {
    fill_ports();
    fill_ip_list();
    load_config();
    update_runtime();
}

void ServicePanel::on_command(int id, int code) {
    if (updating_) return;
    if (id == svc_ui::kIdRelayDevice && code == CBN_SELCHANGE) {
        load_relay_fields();
        update_runtime();
        return;
    }
    if (id == svc_ui::kIdRelayRefresh) {
        fill_ports();
        note(L"串口列表已刷新");
        return;
    }
    if (id == svc_ui::kIdPickRoot) {
        std::wstring path;
        if (browse_folder(hwnd_, L"选择 TFTP / 共享 / NFS 共用目录", &path)) {
            set_text(svc_ui::kIdRoot, path);
            note(L"目录已选择，保存后生效");
        }
        return;
    }
    if (id == svc_ui::kIdLayout) {
        if (mgr_) {
            save_config(false);
            mgr_->ensure_layout();
            note(L"已创建 common/ 与各板卡目录");
        }
        return;
    }
    if (id == svc_ui::kIdRelaySave) {
        save_config(true);
        note(L"继电器配置已保存");
        return;
    }
    if (id == svc_ui::kIdSave) {
        save_config(true);
        note(L"服务配置已保存");
        return;
    }
    if (id == svc_ui::kIdRelayCh1 || id == svc_ui::kIdRelayCh2) {
        int ch = id == svc_ui::kIdRelayCh1 ? 1 : 2;
        RelayDeviceConfig dev = current_relay();
        std::string holder;
        if (core_) {
            for (const AppCore::Entry& e : core_->entries()) {
                if (e.bridge && e.bridge->running() && e.kind == "serial" &&
                    e.detail.find(dev.windows_port) != std::string::npos) {
                    holder = e.name;
                    break;
                }
            }
        }
        if (!holder.empty()) {
            note(L"串口 " + utf8_to_wide(dev.windows_port) + L" 正被通道 " +
                     utf8_to_wide(holder) + L" 占用：请先停止该通道或改用其它串口",
                 true);
            return;
        }
        bool on = mgr_ ? mgr_->relay_state(dev.windows_port, ch).on : false;
        std::string err;
        if (mgr_ && mgr_->relay_set(dev, ch, !on, &err)) {
            note(L"通道 " + std::to_wstring(ch) + (on ? L" 已断开" : L" 已吸合"));
        } else {
            note(L"通道 " + std::to_wstring(ch) + L" 操作失败：" + utf8_to_wide(err), true);
        }
        update_runtime();
        return;
    }
    if (id == svc_ui::kIdRelayPulse1 || id == svc_ui::kIdRelayPulse2) {
        int ch = id == svc_ui::kIdRelayPulse1 ? 1 : 2;
        double hold = 1.0;
        std::wstring hs = get_text(svc_ui::kIdRelayHold);
        if (hs.find(L"0.5") != std::wstring::npos) hold = 0.5;
        else if (hs.find(L"2") != std::wstring::npos) hold = 2.0;
        else if (hs.find(L"5") != std::wstring::npos) hold = 5.0;
        RelayDeviceConfig dev = current_relay();
        std::string holder;
        if (core_) {
            for (const AppCore::Entry& e : core_->entries()) {
                if (e.bridge && e.bridge->running() && e.kind == "serial" &&
                    e.detail.find(dev.windows_port) != std::string::npos) {
                    holder = e.name;
                    break;
                }
            }
        }
        if (!holder.empty()) {
            note(L"串口 " + utf8_to_wide(dev.windows_port) + L" 正被通道 " +
                     utf8_to_wide(holder) + L" 占用：请先停止该通道或改用其它串口",
                 true);
            return;
        }
        if (mgr_ && mgr_->relay_pulse_async(dev, ch, hold)) note(L"脉冲已下发，后台执行");
        else note(L"脉冲失败：上一路脉冲还在执行", true);
        update_runtime();
        return;
    }
    if (id == svc_ui::kIdTftpBtn) {
        AppConfig cfg = collect_config();
        save_config(false);
        if (mgr_->tftp_running()) {
            mgr_->stop_tftp();
            note(L"TFTP 已停止");
        } else {
            std::string err;
            if (mgr_->start_tftp(cfg.tftp, &err)) note(L"TFTP 已启动");
            else note(L"TFTP 启动失败：" + utf8_to_wide(err), true);
        }
        save_config(false);
        update_runtime();
        return;
    }
    if (id == svc_ui::kIdShareBtn) {
        AppConfig cfg = collect_config();
        save_config(false);
        if (mgr_->share_running()) {
            mgr_->stop_share();
            note(L"共享 HTTP 已停止");
        } else {
            std::string err;
            if (mgr_->start_share(cfg.share, &err)) note(L"共享 HTTP 已启动");
            else note(L"共享启动失败：" + utf8_to_wide(err), true);
        }
        save_config(false);
        update_runtime();
        return;
    }
    if (id == svc_ui::kIdNfsBtn) {
        AppConfig cfg = collect_config();
        save_config(false);
        if (mgr_->nfs_running()) {
            mgr_->stop_nfs();
            note(L"NFS 已停止，111/1058/2049 已释放");
        } else {
            std::string err;
            if (mgr_->start_nfs(cfg.nfs, &err)) note(L"NFS 已启动");
            else note(L"NFS 启动失败：" + utf8_to_wide(err), true);
        }
        save_config(false);
        update_runtime();
        return;
    }
    if (id == svc_ui::kIdShareOpen) {
        std::wstring url = utf8_to_wide(mgr_ ? mgr_->share_url() : "");
        if (url.empty()) note(L"共享未启动，先点“启动共享”", true);
        else ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == svc_ui::kIdStopAll) {
        if (mgr_) mgr_->stop_all();
        save_config(false);
        note(L"全部服务已停止，端口已释放");
        update_runtime();
        return;
    }
    if (id == svc_ui::kIdClose) {
        DestroyWindow(hwnd_);
        return;
    }
}

AppConfig ServicePanel::panel_config() const { return core_ ? core_->config() : AppConfig{}; }

AppConfig ServicePanel::collect_config() const {
    AppConfig cfg = panel_config();
    cfg.tftp.root = wide_to_utf8(get_text(svc_ui::kIdRoot));
    std::wstring ip = get_text(svc_ui::kIdTftpIp);
    cfg.tftp.bind_ip = (ip == L"自动（0.0.0.0）") ? "" : wide_to_utf8(ip);
    cfg.tftp.port = _wtoi(get_text(svc_ui::kIdTftpPort).c_str());
    cfg.tftp.allow_write =
        SendMessageW(GetDlgItem(hwnd_, svc_ui::kIdTftpWrite), BM_GETCHECK, 0, 0) == BST_CHECKED;
    cfg.tftp.enable = mgr_ && mgr_->tftp_running();
    cfg.share.root = "";
    cfg.share.port = _wtoi(get_text(svc_ui::kIdSharePort).c_str());
    cfg.share.enable = mgr_ && mgr_->share_running();
    cfg.nfs.alias = wide_to_utf8(get_text(svc_ui::kIdNfsAlias));
    cfg.nfs.uid = _wtoi(get_text(svc_ui::kIdNfsUid).c_str());
    cfg.nfs.gid = _wtoi(get_text(svc_ui::kIdNfsGid).c_str());
    cfg.nfs.root = "";
    cfg.nfs.enable = mgr_ && mgr_->nfs_running();

    RelayDeviceConfig dev = current_relay();
    int idx = selected_relay_index();
    if (idx >= 0) {
        cfg.relays[static_cast<size_t>(idx)] = dev;
    } else if (!dev.name.empty()) {
        cfg.relays.push_back(dev);
    }
    return cfg;
}

void ServicePanel::save_config(bool) {
    if (!core_ || !mgr_) return;
    AppConfig cfg = collect_config();
    core_->set_service_config(cfg);
    if (config_save(cfg)) mgr_->apply_config(cfg);
}

}  // namespace sb3


