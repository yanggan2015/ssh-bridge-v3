#include "ui/services/file_service_panel.h"

#include "app/app_core.h"

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

void add_combo_strings(HWND combo, const std::vector<std::wstring>& items) {
    if (!combo) return;
    for (const auto& s : items)
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
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

bool FileServicePanel::open(HINSTANCE inst, HWND parent, AppCore* core, ServiceManager* mgr,
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

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = filesvc_ui::kClass;
    RegisterClassExW(&wc);

    RECT rc{0, 0, S(1120), S(552)};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    min_w_ = w;
    min_h_ = h;
    int x = 80, y = 40;
    if (parent) {
        RECT pr{};
        GetWindowRect(parent, &pr);
        x = pr.left + (pr.right - pr.left - w) / 2;
        y = pr.top + (pr.bottom - pr.top - h) / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
    }
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, filesvc_ui::kClass,
                            L"文件服务 — ssh-bridge v3", WS_OVERLAPPEDWINDOW, x, y, w, h,
                            parent, nullptr, inst_, this);
    if (!hwnd_) return false;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    return true;
}

void FileServicePanel::close() {
    if (is_open()) DestroyWindow(hwnd_);
}

LRESULT CALLBACK FileServicePanel::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    FileServicePanel* self = reinterpret_cast<FileServicePanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<FileServicePanel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(msg, wp, lp);
}

LRESULT FileServicePanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
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
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = min_w_ > 0 ? min_w_ : S(900);
        mmi->ptMinTrackSize.y = min_h_ > 0 ? min_h_ : S(540);
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

void FileServicePanel::build() {
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w,
                    int h, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w),
                                 S(h), hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 inst_, nullptr);
        if (c && font_) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return c;
    };
    auto label = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
        return make(L"STATIC", text, SS_LEFT | SS_CENTERIMAGE, x, y, w, h, id);
    };
    auto button = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
        return make(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, h, id);
    };
    auto check = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
        return make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, h, id);
    };
    auto combo = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
        return make(L"COMBOBOX", text, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, x, y, w, h, id);
    };
    auto edit = [&](const wchar_t* text, int x, int y, int w, int h, int id, bool read_only) {
        return make(L"EDIT", text,
                    WS_BORDER | ES_LEFT | ES_AUTOHSCROLL | (read_only ? ES_READONLY : 0), x, y,
                    w, h, id);
    };

    label(L"文件服务", 20, 10, 260, 24, filesvc_ui::kIdTitle);
    make(L"BUTTON", L"共用目录（TFTP / 共享 / NFS）", BS_GROUPBOX, 16, 40, 1088, 70,
         filesvc_ui::kIdGroupRoot);
    label(L"共用目录", 32, 64, 70, 24, filesvc_ui::kIdLabelRoot);
    combo(L"", 108, 60, 600, 200, filesvc_ui::kIdRoot);
    button(L"选择…", 720, 60, 76, 26, filesvc_ui::kIdPickRoot);
    button(L"创建板卡目录", 806, 60, 120, 26, filesvc_ui::kIdLayout);

    make(L"BUTTON", L"TFTP", BS_GROUPBOX, 16, 118, 1088, 80, filesvc_ui::kIdGroupTftp);
    label(L"绑定 IP", 32, 142, 60, 24, filesvc_ui::kIdLabelTftpIp);
    combo(L"", 100, 138, 190, 200, filesvc_ui::kIdTftpIp);
    label(L"端口", 308, 142, 44, 24, filesvc_ui::kIdLabelTftpPort);
    combo(L"69", 356, 138, 90, 200, filesvc_ui::kIdTftpPort);
    check(L"允许写入", 464, 142, 100, 24, filesvc_ui::kIdTftpWrite);
    button(L"启动 TFTP", 578, 137, 112, 28, filesvc_ui::kIdTftpBtn);
    label(L"", 704, 142, 384, 24, filesvc_ui::kIdTftpStatus);

    make(L"BUTTON", L"共享 HTTP", BS_GROUPBOX, 16, 204, 1088, 80, filesvc_ui::kIdGroupShare);
    label(L"端口", 32, 228, 44, 24, filesvc_ui::kIdLabelSharePort);
    combo(L"18082", 82, 224, 100, 200, filesvc_ui::kIdSharePort);
    button(L"启动共享", 198, 223, 112, 28, filesvc_ui::kIdShareBtn);
    button(L"打开浏览器", 322, 223, 120, 28, filesvc_ui::kIdShareOpen);
    label(L"", 456, 228, 628, 24, filesvc_ui::kIdShareStatus);

    make(L"BUTTON", L"NFS", BS_GROUPBOX, 16, 290, 1088, 80, filesvc_ui::kIdGroupNfs);
    label(L"别名", 32, 314, 40, 24, filesvc_ui::kIdLabelNfsAlias);
    combo(L"/share", 80, 310, 130, 200, filesvc_ui::kIdNfsAlias);
    label(L"UID", 224, 314, 34, 24, filesvc_ui::kIdLabelUid);
    combo(L"0", 262, 310, 78, 200, filesvc_ui::kIdNfsUid);
    label(L"GID", 354, 314, 34, 24, filesvc_ui::kIdLabelGid);
    combo(L"0", 392, 310, 78, 200, filesvc_ui::kIdNfsGid);
    button(L"启动 NFS", 486, 309, 112, 28, filesvc_ui::kIdNfsBtn);
    label(L"", 612, 314, 470, 24, filesvc_ui::kIdNfsStatus);

    button(L"保存服务配置", 20, 380, 132, 28, filesvc_ui::kIdSave);
    button(L"全部停止", 164, 380, 112, 28, filesvc_ui::kIdStopAll);
    make(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 16, 416, 1088, 72,
         filesvc_ui::kIdMsg);
    button(L"关闭", 1000, 506, 96, 30, filesvc_ui::kIdClose);

    add_combo_strings(GetDlgItem(hwnd_, filesvc_ui::kIdTftpPort), {L"69", L"6969", L"1069"});
    SendMessageW(GetDlgItem(hwnd_, filesvc_ui::kIdTftpPort), CB_SETCURSEL, 0, 0);
    add_combo_strings(GetDlgItem(hwnd_, filesvc_ui::kIdSharePort), {L"18082", L"18083", L"8080"});
    SendMessageW(GetDlgItem(hwnd_, filesvc_ui::kIdSharePort), CB_SETCURSEL, 0, 0);
    add_combo_strings(GetDlgItem(hwnd_, filesvc_ui::kIdNfsAlias), {L"/share", L"/mnt/share"});
    add_combo_strings(GetDlgItem(hwnd_, filesvc_ui::kIdNfsUid), {L"0", L"1000", L"1001"});
    add_combo_strings(GetDlgItem(hwnd_, filesvc_ui::kIdNfsGid), {L"0", L"1000", L"1001"});
    fill_ip_list();
    load_config();
    update_runtime();
}

void FileServicePanel::fill_ip_list() {
    HWND combo = GetDlgItem(hwnd_, filesvc_ui::kIdTftpIp);
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

void FileServicePanel::load_config() {
    if (!core_ || !mgr_) return;
    updating_ = true;
    const AppConfig& cfg = core_->config();
    set_text(filesvc_ui::kIdRoot, utf8_to_wide(mgr_->share_root()));
    set_text(filesvc_ui::kIdTftpIp, cfg.tftp.bind_ip.empty() ? L"自动（0.0.0.0）"
                                                            : utf8_to_wide(cfg.tftp.bind_ip));
    set_text(filesvc_ui::kIdTftpPort,
             std::to_wstring(cfg.tftp.port > 0 ? cfg.tftp.port : 69).c_str());
    SendMessageW(GetDlgItem(hwnd_, filesvc_ui::kIdTftpWrite), BM_SETCHECK,
                 cfg.tftp.allow_write ? BST_CHECKED : BST_UNCHECKED, 0);
    set_text(filesvc_ui::kIdSharePort,
             std::to_wstring(cfg.share.port > 0 ? cfg.share.port : 18082).c_str());
    set_text(filesvc_ui::kIdNfsAlias, utf8_to_wide(cfg.nfs.alias.empty() ? "/share" : cfg.nfs.alias));
    set_text(filesvc_ui::kIdNfsUid, std::to_wstring(cfg.nfs.uid).c_str());
    set_text(filesvc_ui::kIdNfsGid, std::to_wstring(cfg.nfs.gid).c_str());
    updating_ = false;
}

void FileServicePanel::set_text(int id, const std::wstring& text) {
    if (HWND h = GetDlgItem(hwnd_, id)) SetWindowTextW(h, text.c_str());
}

std::wstring FileServicePanel::get_text(int id) const {
    HWND h = GetDlgItem(hwnd_, id);
    if (!h) return L"";
    wchar_t buf[1024] = L"";
    GetWindowTextW(h, buf, 1024);
    return buf;
}

void FileServicePanel::note(const std::wstring& text, bool error) {
    set_text(filesvc_ui::kIdMsg, (error ? L"⚠ " : L"✓ ") + text);
}

void FileServicePanel::update_runtime() {
    if (!mgr_ || !hwnd_) return;
    updating_ = true;
    set_text(filesvc_ui::kIdTftpStatus, utf8_to_wide(mgr_->tftp_status()));
    set_text(filesvc_ui::kIdShareStatus, utf8_to_wide(mgr_->share_status()));
    set_text(filesvc_ui::kIdNfsStatus, utf8_to_wide(mgr_->nfs_status()));
    set_text(filesvc_ui::kIdTftpBtn, mgr_->tftp_running() ? L"停止 TFTP" : L"启动 TFTP");
    set_text(filesvc_ui::kIdShareBtn, mgr_->share_running() ? L"停止共享" : L"启动共享");
    set_text(filesvc_ui::kIdNfsBtn, mgr_->nfs_running() ? L"停止 NFS" : L"启动 NFS");
    updating_ = false;
}

void FileServicePanel::refresh() {
    fill_ip_list();
    load_config();
    update_runtime();
}

AppConfig FileServicePanel::collect_config() const {
    AppConfig cfg = core_ ? core_->config() : AppConfig{};
    cfg.tftp.root = wide_to_utf8(get_text(filesvc_ui::kIdRoot));
    std::wstring ip = get_text(filesvc_ui::kIdTftpIp);
    cfg.tftp.bind_ip = (ip == L"自动（0.0.0.0）") ? "" : wide_to_utf8(ip);
    cfg.tftp.port = _wtoi(get_text(filesvc_ui::kIdTftpPort).c_str());
    cfg.tftp.allow_write =
        SendMessageW(GetDlgItem(hwnd_, filesvc_ui::kIdTftpWrite), BM_GETCHECK, 0, 0) == BST_CHECKED;
    cfg.tftp.enable = mgr_ && mgr_->tftp_running();
    cfg.share.root = "";
    cfg.share.port = _wtoi(get_text(filesvc_ui::kIdSharePort).c_str());
    cfg.share.enable = mgr_ && mgr_->share_running();
    cfg.nfs.alias = wide_to_utf8(get_text(filesvc_ui::kIdNfsAlias));
    cfg.nfs.uid = _wtoi(get_text(filesvc_ui::kIdNfsUid).c_str());
    cfg.nfs.gid = _wtoi(get_text(filesvc_ui::kIdNfsGid).c_str());
    cfg.nfs.root = "";
    cfg.nfs.enable = mgr_ && mgr_->nfs_running();
    return cfg;
}

void FileServicePanel::save_config(bool) {
    if (!core_ || !mgr_) return;
    AppConfig cfg = collect_config();
    core_->set_service_config(cfg);
    if (config_save(cfg)) mgr_->apply_config(cfg);
}

void FileServicePanel::on_command(int id, int) {
    if (updating_) return;
    if (id == filesvc_ui::kIdPickRoot) {
        std::wstring path;
        if (browse_folder(hwnd_, L"选择 TFTP / 共享 / NFS 共用目录", &path)) {
            set_text(filesvc_ui::kIdRoot, path);
            note(L"目录已选择，保存后生效");
        }
        return;
    }
    if (id == filesvc_ui::kIdLayout) {
        if (mgr_) {
            save_config(false);
            mgr_->ensure_layout();
            note(L"已创建 common/ 与各板卡目录");
        }
        return;
    }
    if (id == filesvc_ui::kIdSave) {
        save_config(true);
        note(L"文件服务配置已保存");
        return;
    }
    if (id == filesvc_ui::kIdTftpBtn) {
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
    if (id == filesvc_ui::kIdShareBtn) {
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
    if (id == filesvc_ui::kIdNfsBtn) {
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
    if (id == filesvc_ui::kIdShareOpen) {
        std::wstring url = utf8_to_wide(mgr_ ? mgr_->share_url() : "");
        if (url.empty()) note(L"共享未启动，先点“启动共享”", true);
        else ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == filesvc_ui::kIdStopAll) {
        if (mgr_) mgr_->stop_all();
        save_config(false);
        note(L"全部服务已停止，端口已释放");
        update_runtime();
        return;
    }
    if (id == filesvc_ui::kIdClose) {
        DestroyWindow(hwnd_);
        return;
    }
}

}  // namespace sb3

