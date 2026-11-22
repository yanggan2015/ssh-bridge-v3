// file_browser.cpp — 左侧“文件”页实现。
// 串口通道浏览本机共享目录（tftp.root，空则 work/share）；SSH 通道转 SFTP 面板。

#include "ui/shell/file_browser.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>

#include "app/app_core.h"
#include "third_party/win_ui_bridge.h"
#include "ui/terminal/term_types.h"

namespace sb3 {
namespace {

const wchar_t kClass[] = L"SB3_FileBrowser";

constexpr int kIdPath    = 1401;
constexpr int kIdUp      = 1402;
constexpr int kIdRefresh = 1403;
constexpr int kIdList    = 1404;
constexpr int kIdNote    = 1405;
constexpr int kIdSftp    = 1406;

constexpr std::size_t kMaxEntries = 500;

bool hidden_or_system(const std::filesystem::path& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
}

bool noise_file(const std::wstring& name) {
    std::wstring ext = std::filesystem::path(name).extension().wstring();
    if (ext.empty()) return false;
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    static const wchar_t* const kNoise[] = {
        L".tmp", L".log", L".bak", L".swp", L".swo", L".dmp", L".obj",
        L".o", L".pdb", L".ilk", L".exp", L".lib", L".pyc", L".class",
    };
    for (const wchar_t* e : kNoise) {
        if (ext == e) return true;
    }
    return false;
}

std::wstring size_text(unsigned long long bytes) {
    wchar_t buf[48] = L"";
    if (bytes < 1024)
        swprintf_s(buf, 48, L"%llu B", bytes);
    else if (bytes < 1024ull * 1024ull)
        swprintf_s(buf, 48, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
    else if (bytes < 1024ull * 1024ull * 1024ull)
        swprintf_s(buf, 48, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        swprintf_s(buf, 48, L"%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return std::wstring(buf);
}

}  // namespace

bool FileBrowser::create(HINSTANCE inst, HWND parent, AppCore* core, HFONT font, int dpi,
                         std::function<void()> open_sftp) {
    inst_      = inst;
    parent_    = parent;
    core_      = core;
    font_      = font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    dpi_       = dpi > 0 ? dpi : 96;
    open_sftp_ = std::move(open_sftp);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &FileBrowser::WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    hwnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                            0, 0, 0, 0, parent, nullptr, inst, this);
    if (!hwnd_) return false;
    ui_bridge_set_id(hwnd_, "panel.file");

    build();
    update_view();
    return true;
}

void FileBrowser::build() {
    path_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
                            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE |
                                SS_PATHELLIPSIS | SS_NOPREFIX,
                            0, 0, 0, 0, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdPath)), inst_, nullptr);
    up_ = CreateWindowExW(0, L"BUTTON", L"上级", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          0, 0, 0, 0, hwnd_,
                          reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdUp)), inst_, nullptr);
    refresh_ = CreateWindowExW(0, L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdRefresh)), inst_, nullptr);
    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                            0, 0, 0, 0, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdList)), inst_, nullptr);
    note_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                            0, 0, 0, 0, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdNote)), inst_, nullptr);
    sftp_ = CreateWindowExW(0, L"BUTTON", L"打开 SFTP", WS_CHILD | BS_PUSHBUTTON,
                            0, 0, 0, 0, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdSftp)), inst_, nullptr);

    for (HWND h : {path_, up_, refresh_, list_, note_, sftp_})
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

    SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    const wchar_t* heads[3] = {L"名称", L"类型", L"大小"};
    int widths[3] = {130, 46, 58};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW col{};
        col.mask     = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText  = const_cast<LPWSTR>(heads[i]);
        col.cx       = MulDiv(widths[i], dpi_, 96);
        col.iSubItem = i;
        SendMessageW(list_, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i),
                     reinterpret_cast<LPARAM>(&col));
    }

    ui_bridge_set_id(path_, "label.file.path");
    ui_bridge_set_id(up_, "btn.file.up");
    ui_bridge_set_id(refresh_, "btn.file.refresh");
    ui_bridge_set_id(list_, "list.file");
    ui_bridge_set_id(note_, "label.file.note");
    ui_bridge_set_id(sftp_, "btn.file.sftp");
}

std::filesystem::path FileBrowser::share_root() const {
    const AppConfig& cfg = core_->config();
    std::filesystem::path project = std::filesystem::u8path(cfg.project_root);
    std::filesystem::path root;
    if (cfg.tftp.root.empty()) {
        root = project / "work" / "share";
    } else {
        root = std::filesystem::u8path(cfg.tftp.root);
        if (!root.is_absolute()) root = project / root;
    }
    return root.lexically_normal();
}

void FileBrowser::set_channel(const std::string& name) {
    channel_ = name;
    kind_ = Kind::None;
    if (!name.empty()) {
        if (core_->is_ssh_name(name)) {
            kind_ = Kind::Ssh;
        } else {
            kind_ = Kind::Unsupported;
            for (const AppCore::Entry& e : core_->entries()) {
                if (e.name != name) continue;
                if (e.kind == "serial") kind_ = Kind::Serial;
                break;
            }
        }
    }

    if (kind_ == Kind::Serial) {
        std::filesystem::path next_root = share_root();
        if (root_ != next_root) {
            root_ = next_root;
            dir_  = root_;
        }
        if (dir_.empty()) dir_ = root_;
        load_dir(dir_);
        return;
    }

    entries_.clear();
    dir_.clear();
    note_text_.clear();
    fill_list();
    update_view();
}

void FileBrowser::load_dir(const std::filesystem::path& dir) {
    entries_.clear();
    std::wstring err;
    std::error_code ec;
    if (dir.empty()) {
        set_note(L"共享目录不可用");
        update_view();
        return;
    }

    if (!std::filesystem::exists(dir, ec)) {
        if (dir == root_) std::filesystem::create_directories(dir, ec);
        if (ec || !std::filesystem::exists(dir, ec)) {
            err = L"目录不存在：" + dir.wstring();
        }
    }

    if (err.empty()) {
        std::filesystem::directory_iterator it(dir,
            std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::directory_iterator end;
        while (!ec && it != end) {
            const std::filesystem::directory_entry& de = *it;
            if (hidden_or_system(de.path())) {
                it.increment(ec);
                continue;
            }
            Entry e;
            e.name = de.path().filename().wstring();
            std::error_code type_ec;
            e.dir = de.is_directory(type_ec);
            if (!e.dir) {
                if (noise_file(e.name)) {
                    it.increment(ec);
                    continue;
                }
                std::error_code size_ec;
                auto sz = de.file_size(size_ec);
                if (!size_ec) e.size = static_cast<unsigned long long>(sz);
            }
            entries_.push_back(std::move(e));
            it.increment(ec);
        }
    }

    if (ec) err = L"读取目录失败：" + dir.wstring();

    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    bool truncated = false;
    if (entries_.size() > kMaxEntries) {
        entries_.resize(kMaxEntries);
        truncated = true;
    }

    dir_ = dir;
    fill_list();
    if (!err.empty()) {
        set_note(err);
    } else {
        std::wstring note = L"共享目录 · " + std::to_wstring(entries_.size()) + L" 项";
        if (truncated) note += L"（已截断）";
        set_note(note);
    }
    update_view();
}

void FileBrowser::fill_list() {
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        std::wstring type = e.dir ? L"目录" : L"文件";
        std::wstring size = e.dir ? L"" : size_text(e.size);

        LVITEMW item{};
        item.mask     = LVIF_TEXT | LVIF_PARAM;
        item.iItem    = static_cast<int>(i);
        item.pszText  = const_cast<LPWSTR>(e.name.c_str());
        item.lParam   = static_cast<LPARAM>(i);
        int row = static_cast<int>(SendMessageW(list_, LVM_INSERTITEMW, 0,
                                                reinterpret_cast<LPARAM>(&item)));
        if (row < 0) continue;

        LVITEMW sub{};
        sub.mask    = LVIF_TEXT;
        sub.iItem   = row;
        sub.iSubItem = 1;
        sub.pszText = const_cast<LPWSTR>(type.c_str());
        SendMessageW(list_, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&sub));
        sub.iSubItem = 2;
        sub.pszText  = const_cast<LPWSTR>(size.c_str());
        SendMessageW(list_, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&sub));
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
}

void FileBrowser::refresh() {
    if (kind_ == Kind::Serial) {
        load_dir(dir_.empty() ? root_ : dir_);
        return;
    }
    update_view();
}

void FileBrowser::go_up() {
    if (kind_ != Kind::Serial || dir_.empty() || dir_ == root_) return;
    std::filesystem::path parent = dir_.parent_path();
    if (parent.empty()) return;
    load_dir(parent);
}

void FileBrowser::open_entry(int index) {
    if (kind_ != Kind::Serial || index < 0 ||
        static_cast<std::size_t>(index) >= entries_.size())
        return;
    const Entry e = entries_[static_cast<std::size_t>(index)];
    std::filesystem::path full = dir_ / e.name;
    if (e.dir) {
        load_dir(full);
        return;
    }
    HINSTANCE r = ShellExecuteW(nullptr, L"open", full.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32)
        set_note(L"无法打开文件：" + e.name);
}

void FileBrowser::set_note(const std::wstring& text) {
    note_text_ = text;
    if (note_) SetWindowTextW(note_, note_text_.c_str());
}

void FileBrowser::update_view() {
    bool serial = (kind_ == Kind::Serial);
    bool ssh    = (kind_ == Kind::Ssh);

    ShowWindow(path_, serial ? SW_SHOWNA : SW_HIDE);
    ShowWindow(up_, serial ? SW_SHOWNA : SW_HIDE);
    ShowWindow(refresh_, serial ? SW_SHOWNA : SW_HIDE);
    ShowWindow(list_, serial ? SW_SHOWNA : SW_HIDE);
    ShowWindow(sftp_, ssh ? SW_SHOWNA : SW_HIDE);

    if (serial) {
        std::wstring shown;
        if (!dir_.empty()) {
            std::filesystem::path project = std::filesystem::u8path(core_->config().project_root);
            std::filesystem::path rel = dir_.lexically_relative(project);
            std::wstring rel_text = rel.wstring();
            if (!rel.empty() && rel_text.rfind(L"..", 0) != 0 &&
                rel_text != L".") {
                shown = rel_text;
            } else {
                shown = dir_.wstring();
            }
        }
        if (path_) SetWindowTextW(path_, shown.c_str());
        if (up_) EnableWindow(up_, dir_ != root_);
    } else if (ssh) {
        set_note(L"SSH 通道：使用 SFTP 面板浏览远端文件");
        if (sftp_) EnableWindow(sftp_, TRUE);
    } else if (kind_ == Kind::None) {
        set_note(channel_.empty() ? L"未选择通道" : L"当前通道不可用");
        if (sftp_) EnableWindow(sftp_, FALSE);
    } else {
        set_note(L"该通道类型的文件视图尚未接入");
        if (sftp_) EnableWindow(sftp_, FALSE);
    }
    layout_children();
}

void FileBrowser::layout_children() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = rc.right;
    int h = rc.bottom;
    int row = MulDiv(24, dpi_, 96);
    int gap = MulDiv(3, dpi_, 96);
    int bw  = MulDiv(48, dpi_, 96);
    int note_h = MulDiv(20, dpi_, 96);

    if (kind_ == Kind::Serial) {
        int path_w = std::max(0, w - bw * 2 - gap * 2);
        MoveWindow(path_, 0, 0, path_w, row, TRUE);
        MoveWindow(up_, path_w + gap, 0, bw, row, TRUE);
        MoveWindow(refresh_, path_w + gap + bw + gap, 0, bw, row, TRUE);
        int list_h = std::max(0, h - row - gap - note_h);
        MoveWindow(list_, 0, row + gap, w, list_h, TRUE);
        MoveWindow(note_, 0, h - note_h, w, note_h, TRUE);
    } else {
        int top = MulDiv(40, dpi_, 96);
        int note_h2 = std::max(0, h - top - (kind_ == Kind::Ssh ? row + gap : 0));
        MoveWindow(note_, 0, top, w, note_h2, TRUE);
        if (kind_ == Kind::Ssh) {
            int sw = MulDiv(92, dpi_, 96);
            MoveWindow(sftp_, (w - sw) / 2, top + note_h2 + gap, sw, row, TRUE);
        }
    }
}

LRESULT CALLBACK FileBrowser::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    FileBrowser* self = reinterpret_cast<FileBrowser*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    self->hwnd_ = hwnd;
    return self->handle(msg, wp, lp);
}

LRESULT FileBrowser::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        layout_children();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kIdRefresh:
            refresh();
            return 0;
        case kIdUp:
            go_up();
            return 0;
        case kIdSftp:
            if (open_sftp_) open_sftp_();
            return 0;
        default:
            break;
        }
        return 0;

    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (!hdr) return 0;
        if (hdr->idFrom == kIdList && hdr->code == NM_DBLCLK) {
            auto* nm = reinterpret_cast<NMITEMACTIVATE*>(lp);
            open_entry(nm->iItem);
        }
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void FileBrowser::layout(int x, int y, int w, int h) {
    if (!hwnd_) return;
    MoveWindow(hwnd_, x, y, std::max(0, w), std::max(0, h), TRUE);
    layout_children();
}

}  // namespace sb3

