// sftp_window.cpp — S7 SFTP 双栏面板：独立会话、队列、进度、取消、MD5 回执。
// UI 线程只碰控件与快照；SFTP 调用全部在 worker 线程，互不阻塞。

#include "ui/sftp/sftp_window.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>

#include <cjson/cJSON.h>

#include "core/util/util.h"
#include "ui/aibridge/bridge_host.h"
#include "ui/terminal/input.h"

namespace sb3 {
namespace {

constexpr int kEventRefresh = 1;
constexpr int kEventStatus  = 2;
constexpr UINT kMsgSftpEvent = WM_APP + 60;
std::atomic<std::uint64_t> g_speed_limit_bps{0};  // 进程内共享：0=不限速

std::wstring widen(const std::string& s) { return term::utf8_to_utf16(s); }
std::string  narrow(const std::wstring& s) { return term::utf16_to_utf8(s); }

const wchar_t* phase_text(int p) {
    switch (p) {
    case 1: return L"连接中";
    case 2: return L"列目录";
    case 3: return L"上传中";
    case 4: return L"下载中";
    case 5: return L"完成";
    case 6: return L"失败";
    case 7: return L"已取消";
    case 8: return L"操作中";
    default: return L"空闲";
    }
}

std::string base_name(const std::string& p, char sep) {
    std::size_t end = p.size();
    while (end > 1 && (p[end - 1] == sep || p[end - 1] == '/' || p[end - 1] == '\'')) --end;
    std::size_t pos = p.find_last_of(sep, end - 1);
    if (pos == std::string::npos || pos >= end) return p.substr(0, end);
    return p.substr(pos + 1, end - pos - 1);
}

std::string dir_name(const std::string& p, char sep) {
    std::size_t pos = p.find_last_of(sep);
    if (pos == std::string::npos) return p;
    std::string out = p.substr(0, pos);
    if (out.empty() || (out.size() == 2 && out[1] == ':')) out += sep;
    return out;
}

std::string join(const std::string& dir, const std::string& name, char sep) {
    if (dir.empty()) return name;
    if (dir.back() == sep) return dir + name;
    return dir + sep + name;
}

std::string human_size(std::uint64_t n) {
    char b[64];
    if (n < 1024) std::snprintf(b, sizeof(b), "%llu B", static_cast<unsigned long long>(n));
    else if (n < 1024ull * 1024) std::snprintf(b, sizeof(b), "%.1f KB", n / 1024.0);
    else if (n < 1024ull * 1024 * 1024) std::snprintf(b, sizeof(b), "%.1f MB", n / (1024.0 * 1024.0));
    else std::snprintf(b, sizeof(b), "%.2f GB", n / (1024.0 * 1024.0 * 1024.0));
    return b;
}

std::string human_speed(double bps) {
    char b[64];
    if (bps < 1024.0) std::snprintf(b, sizeof(b), "%.0f B/s", bps);
    else if (bps < 1024.0 * 1024) std::snprintf(b, sizeof(b), "%.1f KB/s", bps / 1024.0);
    else std::snprintf(b, sizeof(b), "%.2f MB/s", bps / (1024.0 * 1024.0));
    return b;
}

std::string elapsed_text(std::uint64_t ms) {
    char b[64];
    if (ms < 1000) std::snprintf(b, sizeof(b), "%llu ms", static_cast<unsigned long long>(ms));
    else if (ms < 60000) std::snprintf(b, sizeof(b), "%.2f s", ms / 1000.0);
    else std::snprintf(b, sizeof(b), "%llu:%02llu",
                       static_cast<unsigned long long>(ms / 60000),
                       static_cast<unsigned long long>((ms / 1000) % 60));
    return b;
}

std::string stamp_now() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char b[64];
    std::snprintf(b, sizeof(b), "%02d-%02d %02d:%02d:%02d",
                  st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

std::string speed_label(std::uint64_t bps) {
    if (bps == 0) return "不限";
    return human_speed(static_cast<double>(bps));
}


HWND make_static(HWND parent, const wchar_t* text, int id, HFONT font) {
    HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                             0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                             nullptr, nullptr);
    if (font) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

HWND make_button(HWND parent, const wchar_t* text, int id, HFONT font) {
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                             nullptr, nullptr);
    if (font) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

}  // namespace

bool SftpWindow::open(HINSTANCE inst, HWND parent, AppCore* core, HFONT font, int dpi,
                      const std::string& channel) {
    if (is_open()) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        return true;
    }
    inst_ = inst;
    parent_ = parent;
    core_ = core;
    font_ = font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    dpi_ = dpi > 0 ? dpi : 96;
    quit_ = false;
    phase_.store(0);
    cancel_.store(false);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = sftp_ui::kClass;
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }

    int w = MulDiv(760, dpi_, 96);
    int h = MulDiv(500, dpi_, 96);
    RECT adj{0, 0, w, h};
    AdjustWindowRectEx(&adj, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
    int ow = adj.right - adj.left;
    int oh = adj.bottom - adj.top;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (parent) {
        RECT pr{};
        int pw = pr.right - pr.left;
        (void)pw;
        if (GetWindowRect(parent, &pr)) {
            x = pr.left + ((pr.right - pr.left) - ow) / 2;
            y = pr.top + ((pr.bottom - pr.top) - oh) / 2;
        }
    }
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, sftp_ui::kClass, L"SFTP 文件传输",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, ow, oh,
                            parent, nullptr, inst, this);
    if (!hwnd_) return false;

    build(channel);
    ShowWindow(hwnd_, SW_SHOW);
    layout();
    SetForegroundWindow(hwnd_);
    SetTimer(hwnd_, 1, 150, nullptr);
    start_worker();
    return true;
}

void SftpWindow::close() {
    if (!is_open()) return;
    if (history_ && IsWindow(history_)) DestroyWindow(history_);
    if (settings_ && IsWindow(settings_)) DestroyWindow(settings_);
    history_ = nullptr;
    settings_ = nullptr;
    stop_worker();
    client_.close();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void SftpWindow::start_worker() {
    if (worker_.joinable()) return;
    quit_ = false;
    worker_ = std::thread(&SftpWindow::worker_loop, this);
}

void SftpWindow::stop_worker() {
    cancel_.store(true);
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        quit_ = true;
        queue_.clear();
        pending_ = 0;
    }
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void SftpWindow::enqueue(Task t) {
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        if (quit_) return;
        queue_.push_back(std::move(t));
        pending_ = static_cast<int>(queue_.size());
    }
    flash_.clear();
    flash_ms_ = 0;
    queue_cv_.notify_one();
    update_progress_ui();
}

void SftpWindow::post_event(int code) {
    if (hwnd_) PostMessageW(hwnd_, kMsgSftpEvent, static_cast<WPARAM>(code), 0);
}

void SftpWindow::append_history(bool upload, bool tree, const std::string& name,
                                std::uint64_t bytes, std::uint64_t total,
                                std::uint64_t elapsed_ms, bool ok, bool cancelled,
                                const std::string& detail, int retries) {
    TransferRecord r;
    r.time = stamp_now();
    r.dir = upload ? "上传" : "下载";
    r.name = name;
    r.result = ok ? "成功" : (cancelled ? "已取消" : "失败");
    r.detail = detail;
    r.bytes = bytes;
    r.total = total;
    r.elapsed_ms = elapsed_ms ? elapsed_ms : 1;
    r.speed_bps = bytes * 1000.0 / static_cast<double>(r.elapsed_ms);
    r.retries = retries;
    r.tree = tree;
    std::lock_guard<std::mutex> lk(snap_mu_);
    records_.push_back(std::move(r));
    if (records_.size() > 50) records_.erase(records_.begin());
    history_dirty_.store(true, std::memory_order_relaxed);
}

std::vector<SftpWindow::TransferRecord> SftpWindow::history_snapshot() const {
    std::lock_guard<std::mutex> lk(snap_mu_);
    return records_;
}


void SftpWindow::do_connect(const std::string& initial_path) {
    phase_.store(1);
    post_event(kEventStatus);
    std::string err;
    if (!client_.connect(target_, &err)) {
        {
            std::lock_guard<std::mutex> lk(snap_mu_);
            remote_error_ = err;
            note_.assign("连接失败：" + err);
        }
        phase_.store(6);
        post_event(kEventRefresh);
        return;
    }
    connected_ = true;
    home_ = client_.home();
    std::string path = initial_path.empty() ? home_ : initial_path;
    do_list(path);
}

void SftpWindow::do_list(const std::string& path, bool announce) {
    if (announce) {
        phase_.store(2);
        post_event(kEventStatus);
    }
    std::vector<SftpEntry> items;
    std::string err;
    bool ok = client_.list(path.empty() ? home_ : path, &items, &err);
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        remote_items_ = std::move(items);
        remote_error_ = ok ? std::string() : err;
        remote_path_ = path.empty() ? home_ : path;
        remote_ready_ = ok;
        if (announce) {
            if (!ok) note_.assign("读取远端目录失败：" + err);
            else note_.assign("目录：" + remote_path_ + " · " + std::to_string(remote_items_.size()) + " 项");
        } else if (!ok) {
            note_.assign("目录已刷新失败：" + err);
        }
    }
    if (announce) phase_.store(0);
    post_event(kEventRefresh);
}

void SftpWindow::do_transfer(const Task& t) {
    if (!connected_ && !client_.connected()) {
        do_connect(remote_dir());
        if (!client_.connected()) return;
    }
    bool upload = (t.kind == Task::Upload);
    cancel_.store(false);
    done_.store(0);
    total_.store(0);
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        active_kind_ = upload ? "upload" : "download";
        active_name_ = base_name(upload ? t.local : t.remote, upload ? '\\' : '/');
        active_plan_.clear();
        active_plan_pos_ = 0;
        note_.clear();
    }
    phase_.store(upload ? 3 : 4);
    post_event(kEventStatus);
    client_.set_speed_limit(g_speed_limit_bps.load(std::memory_order_relaxed));
    auto started = std::chrono::steady_clock::now();

    std::string err;
    std::string md5;
    bool ok = upload ? client_.upload(t.local, t.remote, &cancel_, &done_, &total_, &err, &md5)
                     : client_.download(t.remote, t.local, &cancel_, &done_, &total_, &err, &md5);
    bool cancelled = cancel_.load();
    auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        note_ = ok ? ((upload ? "上传完成：" : "下载完成：") + active_name_ + " · " + md5)
                   : (cancelled ? "已取消（断点已保留，可点“重试”续传）" : "失败：" + err);
    }
    append_history(upload, false, active_name_,
                   ok ? total_.load() : done_.load(), total_.load(), elapsed,
                   ok, cancelled, ok ? md5 : (cancelled ? "断点已保留" : err),
                   t.retry_count);
    if (ok && upload) do_list(remote_dir(), false);
    phase_.store(ok ? 5 : (cancelled ? 7 : 6));
    post_event(kEventRefresh);
}

void SftpWindow::do_transfer_tree(const Task& t, bool upload) {
    std::string root_name = base_name(upload ? t.local : t.remote, upload ? '\\' : '/');
    namespace fs = std::filesystem;
    cancel_.store(false);
    done_.store(0);
    total_.store(0);
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        active_kind_ = "tree";
        active_name_ = root_name;
        active_files_ = 0;
        active_total_files_ = 0;
        note_.clear();
    }
    phase_.store(2);  // 先枚举目录树
    post_event(kEventStatus);

    std::vector<std::string> dirs;              // 相对路径（/ 分隔）
    std::vector<std::pair<std::string, std::uint64_t>> files;
    std::string err;
    bool ok = true;

    if (upload) {
        fs::path root = fs::u8path(t.local);
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        if (ec) { ok = false; err = "读取本地目录失败：" + ec.message(); }
        for (; ok && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (cancel_.load()) break;
            if (ec) { ok = false; err = "遍历本地目录失败：" + ec.message(); break; }
            const fs::directory_entry& de = *it;
            if (de.is_symlink(ec)) { it.disable_recursion_pending(); continue; }
            std::string rel = fs::relative(de.path(), root, ec).generic_string();
            if (rel.empty() || rel == ".") continue;
            if (de.is_directory(ec)) {
                dirs.push_back(rel);
            } else if (de.is_regular_file(ec)) {
                std::uint64_t sz = de.file_size(ec);
                files.push_back({rel, ec ? 0 : sz});
            }
        }
    } else {
        std::function<bool(const std::string&, const std::string&)> walk =
            [&](const std::string& abs, const std::string& rel) -> bool {
            std::vector<SftpEntry> items;
            std::string e;
            if (!client_.list(abs, &items, &e)) { err = "读取远端目录失败：" + e; return false; }
            for (const SftpEntry& se : items) {
                if (cancel_.load()) return true;
                std::string childRel = rel.empty() ? se.name : rel + "/" + se.name;
                std::string childAbs = abs + "/" + se.name;
                if (se.dir && !se.link) {
                    dirs.push_back(childRel);
                    if (!walk(childAbs, childRel)) return false;
                } else if (!se.dir) {
                    files.push_back({childRel, se.size});
                }
            }
            return true;
        };
        if (!walk(t.remote, "")) ok = false;
    }

    if (cancel_.load() && ok) {
        {
            std::lock_guard<std::mutex> lk(snap_mu_);
            note_ = "已取消（尚未开始传输）";
        }
        append_history(upload, true, root_name, 0, 0, 1, false, true, "枚举阶段取消", t.retry_count);
        phase_.store(7);
        post_event(kEventRefresh);
        return;
    }

    std::uint64_t sum = 0;
    for (const auto& f : files) sum += f.second;
    total_.store(sum);
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        active_total_files_ = static_cast<int>(files.size());
        active_plan_.clear();
        active_plan_.reserve(files.size());
        for (const auto& f : files) active_plan_.push_back(f.first);
        active_plan_pos_ = 0;
    }
    phase_.store(upload ? 3 : 4);
    post_event(kEventStatus);

    auto ensure_remote_dir = [&](const std::string& path) {
        std::string e;
        client_.mkdir(path, &e);  // 已存在失败可忽略；后续上传会暴露真实问题
    };
    auto ensure_local_dir = [&](const std::string& path) {
        std::error_code ec;
        fs::create_directories(fs::u8path(path), ec);
        return !ec;
    };

    if (ok) {
        if (upload) {
            ensure_remote_dir(t.remote);
            for (const std::string& d : dirs) ensure_remote_dir(t.remote + "/" + d);
        } else if (!ensure_local_dir(t.local)) {
            ok = false;
            err = "创建本地目录失败：" + t.local;
        } else {
            for (const std::string& d : dirs) {
                if (!ensure_local_dir(t.local + "/" + d)) {
                    ok = false;
                    err = "创建本地目录失败：" + d;
                    break;
                }
            }
        }
    }

    int done_files = 0;
    bool cancelled = false;
    std::uint64_t moved = 0;
    client_.set_speed_limit(g_speed_limit_bps.load(std::memory_order_relaxed));
    auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; ok && i < files.size(); ++i) {
        if (cancel_.load()) { cancelled = true; break; }
        {
            std::lock_guard<std::mutex> lk(snap_mu_);
            active_files_ = static_cast<int>(i) + 1;
            active_name_ = files[i].first;
            active_plan_pos_ = static_cast<int>(i);
        }
        std::string one_err;
        std::string md5;
        std::string src = upload ? (t.local + "\\" + files[i].first) : (t.remote + "/" + files[i].first);
        std::string dst = upload ? (t.remote + "/" + files[i].first) : (t.local + "\\" + files[i].first);
        for (auto& c : src) if (c == '/') c = '\\';
        if (!upload) for (auto& c : src) if (c == '\\') c = '/';
        bool one = upload ? client_.upload(src, dst, &cancel_, &done_, &total_, &one_err, &md5)
                          : client_.download(src, dst, &cancel_, &done_, &total_, &one_err, &md5);
        if (!one) {
            moved += done_.load();
            if (cancel_.load()) { cancelled = true; }
            else { ok = false; err = files[i].first + "：" + one_err; }
            break;
        }
        moved += files[i].second;
        ++done_files;
    }

    auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        if (ok && !cancelled) {
            note_ = (upload ? "目录上传完成：" : "目录下载完成：") + root_name + " · " +
                    std::to_string(done_files) + " 个文件";
        } else if (cancelled) {
            note_ = "已取消（断点已保留，可点“重试”续传）";
        } else {
            note_ = "目录传输失败：" + err;
        }
    }
    append_history(upload, true, root_name,
                   ok && !cancelled ? sum : moved, sum, elapsed,
                   ok && !cancelled, cancelled,
                   ok && !cancelled ? (std::to_string(done_files) + " 个文件")
                                    : (cancelled ? "断点已保留" : err),
                   t.retry_count);
    phase_.store(ok && !cancelled ? 5 : (cancelled ? 7 : 6));
    if (ok && !cancelled && upload) do_list(remote_dir(), false);
    post_event(kEventRefresh);
}


void SftpWindow::do_remote_op(const Task& t) {
    if (!client_.connected()) {
        do_connect(remote_dir());
        if (!client_.connected()) return;
    }
    phase_.store(8);
    done_.store(0);
    total_.store(0);
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        active_kind_ = (t.kind == Task::Mkdir) ? "mkdir"
                     : (t.kind == Task::Rename) ? "rename" : "delete";
        active_name_ = base_name(t.remote, '/');
        note_.clear();
    }
    post_event(kEventStatus);

    std::string err;
    bool ok = false;
    if (t.kind == Task::Mkdir) {
        ok = client_.mkdir(t.remote, &err);
    } else if (t.kind == Task::Rename) {
        ok = client_.rename(t.remote, t.remote2, &err);
    } else {
        ok = (t.remote2 == "dir") ? client_.rmdir(t.remote, &err)
                                  : client_.unlink(t.remote, &err);
    }
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        if (ok && t.kind == Task::Mkdir) note_ = "已新建目录：" + base_name(t.remote, '/');
        else if (ok && t.kind == Task::Rename) note_ = "已重命名：" + base_name(t.remote, '/') + " → " + base_name(t.remote2, '/');
        else if (ok) note_ = std::string("已删除：") + base_name(t.remote, '/') + (t.remote2 == "dir" ? "（目录）" : "（文件）");
        else if (t.kind == Task::Mkdir) note_ = "新建失败：目录可能已存在或无权限（" + err + "）";
        else if (t.kind == Task::Rename) note_ = "重命名失败：目标可能已存在或无权限（" + err + "）";
        else if (t.kind == Task::Delete && t.remote2 == "dir") note_ = "删除失败：目录可能非空或无权限（" + err + "）";
        else note_ = "操作失败：" + err;
    }
    if (ok) do_list(remote_dir(), false);
    phase_.store(ok ? 0 : 6);  // 文件管理不占传输进度：成功回空闲，失败停在失败态
    post_event(kEventRefresh);
}

void SftpWindow::worker_loop() {
    while (true) {
        Task t;
        {
            std::unique_lock<std::mutex> lk(queue_mu_);
            queue_cv_.wait(lk, [this] { return quit_ || !queue_.empty(); });
            if (quit_) break;
            t = std::move(queue_.front());
            queue_.pop_front();
            pending_ = static_cast<int>(queue_.size());
            busy_ = 1;
        }
        if (t.kind == Task::Connect) {
            do_connect(t.remote);
        } else if (t.kind == Task::List) {
            if (!client_.connected()) do_connect(t.remote.empty() ? home_ : t.remote);
            else do_list(t.remote);
            if (client_.connected() && phase_.load() != 6) phase_.store(0);
        } else if (t.kind == Task::UploadTree || t.kind == Task::DownloadTree) {
            do_transfer_tree(t, t.kind == Task::UploadTree);
        } else if (t.kind == Task::Mkdir || t.kind == Task::Rename || t.kind == Task::Delete) {
            do_remote_op(t);
        } else {
            do_transfer(t);
        }
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            busy_ = 0;
        }
        if (t.kind == Task::Upload || t.kind == Task::Download ||
            t.kind == Task::UploadTree || t.kind == Task::DownloadTree) {
            int ph = phase_.load();
            std::lock_guard<std::mutex> lk(snap_mu_);
            if (ph == 5) {
                has_last_failed_ = false;
            } else if (ph != 0) {
                last_failed_ = t;
                has_last_failed_ = true;
            }
        }
        post_event(kEventRefresh);
    }
}

void SftpWindow::build(const std::string& channel) {
    HWND kind_label = make_static(hwnd_, L"通道", sftp_ui::kIdChannelLabel, font_);
    (void)kind_label;
    HWND ch = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0, 0, 200, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<UINT_PTR>(sftp_ui::kIdChannel)),
                              inst_, nullptr);
    SendMessageW(ch, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    int sel = 0, idx = 0;
    for (const AppCore::Entry& e : core_->entries()) {
        if (e.kind != "ssh") continue;
        SendMessageW(ch, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(e.name).c_str()));
        if (e.name == channel) sel = idx;
        ++idx;
    }
    if (idx > 0) SendMessageW(ch, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    make_button(hwnd_, L"连接", sftp_ui::kIdConnect, font_);

    make_static(hwnd_, L"本地", sftp_ui::kIdLocalLabel, font_);
    HWND lp = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              WS_VSCROLL | CBS_DROPDOWN, 0, 0, 0, 200, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<UINT_PTR>(sftp_ui::kIdLocalPath)),
                              inst_, nullptr);
    SendMessageW(lp, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    make_button(hwnd_, L"刷新", sftp_ui::kIdLocalRefresh, font_);
    make_button(hwnd_, L"上级", sftp_ui::kIdLocalUp, font_);
    HWND ll = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                  LBS_NOINTEGRALHEIGHT,
                              0, 0, 0, 0, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<UINT_PTR>(sftp_ui::kIdLocalList)),
                              inst_, nullptr);
    SendMessageW(ll, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

    make_static(hwnd_, L"远端", sftp_ui::kIdRemoteLabel, font_);
    HWND rp = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              WS_VSCROLL | CBS_DROPDOWN, 0, 0, 0, 200, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<UINT_PTR>(sftp_ui::kIdRemotePath)),
                              inst_, nullptr);
    SendMessageW(rp, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    make_button(hwnd_, L"刷新", sftp_ui::kIdRemoteRefresh, font_);
    make_button(hwnd_, L"上级", sftp_ui::kIdRemoteUp, font_);
    HWND rl = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                  LBS_NOINTEGRALHEIGHT,
                              0, 0, 0, 0, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<UINT_PTR>(sftp_ui::kIdRemoteList)),
                              inst_, nullptr);
    SendMessageW(rl, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    make_static(hwnd_, L"", sftp_ui::kIdLocalInfo, font_);
    make_static(hwnd_, L"", sftp_ui::kIdRemoteInfo, font_);

    make_static(hwnd_, L"名称", sftp_ui::kIdNameLabel, font_);
    HWND nm = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              WS_VSCROLL | CBS_DROPDOWN, 0, 0, 0, 200, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<UINT_PTR>(sftp_ui::kIdName)),
                              inst_, nullptr);
    SendMessageW(nm, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    make_button(hwnd_, L"新建目录", sftp_ui::kIdMkdir, font_);
    make_button(hwnd_, L"重命名", sftp_ui::kIdRename, font_);
    make_button(hwnd_, L"删除", sftp_ui::kIdDelete, font_);

    make_button(hwnd_, L"上传 →", sftp_ui::kIdUpload, font_);
    make_button(hwnd_, L"← 下载", sftp_ui::kIdDownload, font_);
    make_button(hwnd_, L"取消", sftp_ui::kIdCancel, font_);
    make_button(hwnd_, L"重试", sftp_ui::kIdRetry, font_);
    make_button(hwnd_, L"历史", sftp_ui::kIdHistoryOpen, font_);
    make_button(hwnd_, L"设置", sftp_ui::kIdSettingsOpen, font_);
    make_button(hwnd_, L"关闭", sftp_ui::kIdClose, font_);

    HWND pb = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                              0, 0, 0, 0, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<UINT_PTR>(sftp_ui::kIdProgress)),
                              inst_, nullptr);
    SendMessageW(pb, PBM_SETRANGE32, 0, 1000);
    make_static(hwnd_, L"", sftp_ui::kIdQueue, font_);
    make_static(hwnd_, L"", sftp_ui::kIdStatus, font_);

    ensure_target();
    local_ready_ = true;
    SetWindowTextW(lp, widen(current_local_dir()).c_str());
    refresh_local();
    std::string ch_name = channel.empty() ? channel_name() : channel;
    if (!ch_name.empty()) {
        Task t;
        t.kind = Task::Connect;
        t.remote.clear();
        enqueue(t);
    }
}

void SftpWindow::layout() {
    int pad = MulDiv(12, dpi_, 96);
    int gap = MulDiv(8, dpi_, 96);
    int row = MulDiv(26, dpi_, 96);
    int btn = MulDiv(26, dpi_, 96);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = rc.right, h = rc.bottom;
    auto mv = [&](int id, int x, int y, int cw, int ch) {
        if (HWND c = GetDlgItem(hwnd_, id)) MoveWindow(c, x, y, cw, ch, TRUE);
    };

    mv(sftp_ui::kIdChannelLabel, pad, pad, MulDiv(40, dpi_, 96), row);
    mv(sftp_ui::kIdChannel, pad + MulDiv(44, dpi_, 96), pad, MulDiv(220, dpi_, 96), row);
    mv(sftp_ui::kIdConnect, pad + MulDiv(272, dpi_, 96), pad, MulDiv(76, dpi_, 96), btn);

    int top = pad + row + pad;
    int bottom_zone = MulDiv(184, dpi_, 96);

    int half = (w - pad * 3) / 2;
    int lx = pad, rx = pad * 2 + half;

    mv(sftp_ui::kIdLocalLabel, lx, top, MulDiv(60, dpi_, 96), row - MulDiv(6, dpi_, 96));
    int py = top + row + MulDiv(2, dpi_, 96);
    mv(sftp_ui::kIdLocalPath, lx, py, half, row);
    mv(sftp_ui::kIdLocalRefresh, lx + half - MulDiv(144, dpi_, 96), top, MulDiv(64, dpi_, 96), row - MulDiv(6, dpi_, 96));
    mv(sftp_ui::kIdLocalUp, lx + half - MulDiv(76, dpi_, 96), top, MulDiv(64, dpi_, 96), row - MulDiv(6, dpi_, 96));
    mv(sftp_ui::kIdLocalList, lx, py + row + gap, half, h - (py + row + gap) - bottom_zone);

    mv(sftp_ui::kIdRemoteLabel, rx, top, MulDiv(60, dpi_, 96), row - MulDiv(6, dpi_, 96));
    mv(sftp_ui::kIdRemotePath, rx, py, half, row);
    mv(sftp_ui::kIdRemoteRefresh, rx + half - MulDiv(144, dpi_, 96), top, MulDiv(64, dpi_, 96), row - MulDiv(6, dpi_, 96));
    mv(sftp_ui::kIdRemoteUp, rx + half - MulDiv(76, dpi_, 96), top, MulDiv(64, dpi_, 96), row - MulDiv(6, dpi_, 96));
    mv(sftp_ui::kIdRemoteList, rx, py + row + gap, half, h - (py + row + gap) - bottom_zone);

    int by = h - pad - btn;
    int bw = MulDiv(88, dpi_, 96);
    mv(sftp_ui::kIdUpload, pad, by, bw, btn);
    mv(sftp_ui::kIdDownload, pad + bw + gap, by, bw, btn);
    mv(sftp_ui::kIdCancel, pad + (bw + gap) * 2, by, bw, btn);
    mv(sftp_ui::kIdRetry, pad + (bw + gap) * 3, by, bw, btn);
    mv(sftp_ui::kIdHistoryOpen, pad + (bw + gap) * 4, by, bw, btn);
    mv(sftp_ui::kIdSettingsOpen, pad + (bw + gap) * 5, by, bw, btn);
    mv(sftp_ui::kIdClose, w - pad - bw, by, bw, btn);
    mv(sftp_ui::kIdLocalInfo, lx, by - MulDiv(140, dpi_, 96), half, MulDiv(20, dpi_, 96));
    mv(sftp_ui::kIdRemoteInfo, rx, by - MulDiv(140, dpi_, 96), half, MulDiv(20, dpi_, 96));

    mv(sftp_ui::kIdProgress, pad, by - MulDiv(62, dpi_, 96), w - pad * 2, MulDiv(12, dpi_, 96));
    mv(sftp_ui::kIdQueue, pad, by - MulDiv(46, dpi_, 96), w - pad * 2, MulDiv(16, dpi_, 96));
    mv(sftp_ui::kIdStatus, pad, by - MulDiv(24, dpi_, 96), w - pad * 2, MulDiv(20, dpi_, 96));

    // 远端文件管理行：名称（可编辑下拉）+ 新建目录 / 重命名 / 删除
    int row2 = by - MulDiv(104, dpi_, 96);
    int del_w = MulDiv(76, dpi_, 96);
    int ren_w = MulDiv(84, dpi_, 96);
    int mk_w  = MulDiv(92, dpi_, 96);
    int del_x = w - pad - del_w;
    int ren_x = del_x - gap - ren_w;
    int mk_x  = ren_x - gap - mk_w;
    int name_x = pad + MulDiv(44, dpi_, 96);
    mv(sftp_ui::kIdNameLabel, pad, row2, MulDiv(40, dpi_, 96), row);
    mv(sftp_ui::kIdName, name_x, row2, mk_x - gap - name_x, row);
    mv(sftp_ui::kIdMkdir, mk_x, row2, mk_w, btn);
    mv(sftp_ui::kIdRename, ren_x, row2, ren_w, btn);
    mv(sftp_ui::kIdDelete, del_x, row2, del_w, btn);
}

void SftpWindow::ensure_target() {
    std::string name = channel_name();
    if (name.empty()) return;
    if (SSHLinkConfig link; core_->link_of(name, &link)) {
        target_.host = link.host;
        target_.port = link.port;
        target_.username = link.username;
        target_.password = link.password;
        target_.connect_timeout_ms = link.connect_timeout_ms;
        target_.keepalive_s = link.keepalive_s;
    }
}

std::string SftpWindow::channel_name() {
    if (HWND c = GetDlgItem(hwnd_, sftp_ui::kIdChannel)) {
        int idx = static_cast<int>(SendMessageW(c, CB_GETCURSEL, 0, 0));
        if (idx >= 0) {
            wchar_t buf[128] = L"";
            SendMessageW(c, CB_GETLBTEXT, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(buf));
            return narrow(std::wstring(buf));
        }
    }
    return std::string();
}

std::string SftpWindow::local_dir() const {
    if (HWND c = GetDlgItem(hwnd_, sftp_ui::kIdLocalPath)) {
        wchar_t buf[512] = L"";
        GetWindowTextW(c, buf, 512);
        std::string s = narrow(std::wstring(buf));
        if (!s.empty()) return s;
    }
    return current_local_dir();
}

std::string SftpWindow::remote_dir() {
    if (HWND c = GetDlgItem(hwnd_, sftp_ui::kIdRemotePath)) {
        wchar_t buf[512] = L"";
        GetWindowTextW(c, buf, 512);
        std::string s = narrow(std::wstring(buf));
        if (!s.empty()) return s;
    }
    std::lock_guard<std::mutex> lk(snap_mu_);
    return remote_path_;
}

std::string SftpWindow::current_local_dir() const {
    wchar_t buf[512] = L"";
    GetCurrentDirectoryW(512, buf);
    return narrow(std::wstring(buf));
}

bool SftpWindow::selected_local(std::string* path, bool* is_dir) const {
    HWND l = GetDlgItem(hwnd_, sftp_ui::kIdLocalList);
    int idx = l ? static_cast<int>(SendMessageW(l, LB_GETCURSEL, 0, 0)) : LB_ERR;
    if (idx == LB_ERR) return false;
    wchar_t buf[512] = L"";
    SendMessageW(l, LB_GETTEXT, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(buf));
    std::string label = narrow(std::wstring(buf));
    bool d = !label.empty() && label.back() == '\\';
    if (!label.empty() && label.back() == '\\') label.pop_back();
    if (label.empty()) return false;
    std::string full = join(local_dir(), label, '\\');
    if (is_dir) *is_dir = d;
    if (path) *path = full;
    return true;
}

const SftpEntry* SftpWindow::selected_remote_entry() const {
    HWND l = GetDlgItem(hwnd_, sftp_ui::kIdRemoteList);
    int idx = l ? static_cast<int>(SendMessageW(l, LB_GETCURSEL, 0, 0)) : LB_ERR;
    if (idx == LB_ERR) return nullptr;
    std::lock_guard<std::mutex> lk(snap_mu_);
    if (idx < 0 || static_cast<std::size_t>(idx) >= remote_items_.size()) return nullptr;
    return &remote_items_[static_cast<std::size_t>(idx)];
}

bool SftpWindow::selected_remote(std::string* name, bool* is_dir) const {
    const SftpEntry* e = selected_remote_entry();
    if (!e) return false;
    if (name) *name = e->name;
    if (is_dir) *is_dir = e->dir;
    return true;
}

void SftpWindow::update_local_info() {
    std::string path;
    bool is_dir = false;
    if (!selected_local(&path, &is_dir)) {
        SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdLocalInfo), L"选中本地文件查看属性");
        return;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &fad)) {
        SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdLocalInfo), L"无法读取属性");
        return;
    }
    std::wstring kind = is_dir ? L"目录" : L"文件";
    std::wstring size = is_dir ? std::wstring(L"—") : widen(human_size(fad.nFileSizeLow + (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32)));
    FILETIME ft = fad.ftLastWriteTime;
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&ft, &ft);
    FileTimeToSystemTime(&ft, &st);
    wchar_t when[64];
    swprintf_s(when, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    std::wstring out = kind + L" · " + size + L" · " + when;
    SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdLocalInfo), out.c_str());
}

void SftpWindow::update_remote_info() {
    const SftpEntry* e = selected_remote_entry();
    if (!e) {
        SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdRemoteInfo), L"选中远端文件查看权限 / 属主 / 大小 / 时间");
        return;
    }
    wchar_t perm[16];
    swprintf_s(perm, L"%03o", e->mode & 0777u);
    std::wstring kind = e->dir ? L"目录" : (e->link ? L"链接" : L"文件");
    std::wstring size = e->dir ? std::wstring(L"—") : widen(human_size(e->size));
    wchar_t owner[48];
    swprintf_s(owner, L"%u:%u", e->uid, e->gid);
    std::wstring when = L"—";
    if (e->mtime) {
        time_t tt = static_cast<time_t>(e->mtime);
        tm tmv{};
        localtime_s(&tmv, &tt);
        wchar_t buf[64];
        swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                   tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
        when = buf;
    }
    std::wstring out = kind + L" · " + perm + L" · " + owner + L" · " + size + L" · " + when;
    SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdRemoteInfo), out.c_str());
}

void SftpWindow::refresh_local() {
    std::string dir = local_dir();
    HWND l = GetDlgItem(hwnd_, sftp_ui::kIdLocalList);
    if (!l) return;
    SendMessageW(l, LB_RESETCONTENT, 0, 0);
    std::wstring pattern = widen(dir);
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L"\\";
    pattern += L"*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    std::vector<std::pair<std::string, bool>> items;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            items.push_back({ narrow(n), is_dir });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(items.begin(), items.end(), [](const std::pair<std::string, bool>& a,
                                             const std::pair<std::string, bool>& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    for (const auto& it : items) {
        std::string label = it.first + (it.second ? "\\" : "");
        SendMessageW(l, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(label).c_str()));
    }
    SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdLocalPath), widen(dir).c_str());
    update_local_info();
    update_progress_ui();
}

void SftpWindow::refresh_remote() {
    Task t;
    t.kind = Task::List;
    t.remote = remote_dir();
    enqueue(t);
}

void SftpWindow::apply_remote_snapshot() {
    std::vector<SftpEntry> items;
    std::string err;
    std::string path;
    bool ready = false;
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        items = remote_items_;
        err = remote_error_;
        path = remote_path_;
        ready = remote_ready_;
    }
    HWND l = GetDlgItem(hwnd_, sftp_ui::kIdRemoteList);
    if (l) {
        SendMessageW(l, LB_RESETCONTENT, 0, 0);
        for (const SftpEntry& e : items) {
            std::string label = e.name + (e.dir ? "/" : "");
            SendMessageW(l, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(label).c_str()));
        }
    }
    if (!path.empty()) SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdRemotePath), widen(path).c_str());
    connected_ = ready || client_.connected();
    update_remote_info();
    update_progress_ui();
}

void SftpWindow::update_progress_ui() {
    if (!hwnd_) return;
    int ph = phase_.load();
    std::uint64_t d = done_.load();
    std::uint64_t t = total_.load();
    int pct = t ? static_cast<int>(std::min<std::uint64_t>(1000, d * 1000 / t)) : 0;
    if (ph == 5) pct = 1000;
    if (ph == 0) pct = 0;
    if (HWND pb = GetDlgItem(hwnd_, sftp_ui::kIdProgress)) {
        SendMessageW(pb, PBM_SETPOS, static_cast<WPARAM>(pct), 0);
    }
    int busy = (ph == 1 || ph == 2 || ph == 3 || ph == 4 || ph == 8) ? 1 : 0;
    bool has_retry = false;
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        has_retry = has_last_failed_;
    }
    EnableWindow(GetDlgItem(hwnd_, sftp_ui::kIdUpload), !busy && connected_);
    EnableWindow(GetDlgItem(hwnd_, sftp_ui::kIdDownload), !busy && connected_);
    EnableWindow(GetDlgItem(hwnd_, sftp_ui::kIdCancel), busy);
    EnableWindow(GetDlgItem(hwnd_, sftp_ui::kIdRetry), !busy && has_retry);
    EnableWindow(GetDlgItem(hwnd_, sftp_ui::kIdRemoteRefresh), connected_);
    EnableWindow(GetDlgItem(hwnd_, sftp_ui::kIdRemoteUp), connected_);
    EnableWindow(GetDlgItem(hwnd_, sftp_ui::kIdConnect), !busy);
    EnableWindow(GetDlgItem(hwnd_, sftp_ui::kIdMkdir), !busy && connected_);
    EnableWindow(GetDlgItem(hwnd_, sftp_ui::kIdRename), !busy && connected_);
    EnableWindow(GetDlgItem(hwnd_, sftp_ui::kIdDelete), !busy && connected_);
    EnableWindow(GetDlgItem(hwnd_, sftp_ui::kIdName), !busy && connected_);

    std::string name;
    std::string kind;
    std::string note;
    int files = 0, total_files = 0;
    std::string next_name;
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        name = active_name_;
        kind = active_kind_;
        note = note_;
        files = active_files_;
        total_files = active_total_files_;
        int pos = active_plan_pos_;
        if (pos >= 0 && pos + 1 < static_cast<int>(active_plan_.size()))
            next_name = active_plan_[pos + 1];
    }
    wchar_t q[256] = L"";
    int waiting = 0;
    {
        std::lock_guard<std::mutex> qlk(queue_mu_);
        waiting = pending_;
    }
    if ((ph == 3 || ph == 4) && kind == "tree" && total_files > 0) {
        if (!next_name.empty()) {
            swprintf_s(q, L"%s目录：%d/%d 个 · %s（%d%%）· 下一个 %s · 队列 %d",
                       ph == 3 ? L"上传" : L"下载", files, total_files,
                       widen(name).c_str(), pct / 10, widen(next_name).c_str(), waiting);
        } else {
            swprintf_s(q, L"%s目录：%d/%d 个 · %s（%d%%）· 队列 %d",
                       ph == 3 ? L"上传" : L"下载", files, total_files,
                       widen(name).c_str(), pct / 10, waiting);
        }
    } else if (ph == 3 || ph == 4) {
        swprintf_s(q, L"%s：%s  %s / %s（%d%%）· 队列 %d",
                   ph == 3 ? L"上传中" : L"下载中", widen(name).c_str(),
                   widen(human_size(d)).c_str(), widen(human_size(t)).c_str(),
                   pct / 10, waiting);
    } else if (ph == 5 || ph == 6 || ph == 7) {
        swprintf_s(q, L"%s%s · 队列 %d", phase_text(ph),
                   (ph != 5 && has_retry) ? L"（可重试续传）" : L"", waiting);
    } else {
        swprintf_s(q, L"%s%s%s", connected_ ? L"已连接" : L"未连接",
                   waiting > 0 ? L" · 队列 " : L"", waiting > 0 ? widen(std::to_string(waiting)).c_str() : L"");
    }
    SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdQueue), q);
    std::wstring st = status_text();
    SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdStatus), st.c_str());
}

void SftpWindow::reset_delete_confirm() {
    delete_armed_ = false;
    delete_arm_ms_ = 0;
    if (HWND b = GetDlgItem(hwnd_, sftp_ui::kIdDelete)) SetWindowTextW(b, L"删除");
}

void SftpWindow::set_status(const std::wstring& text) {
    status_ = text;
    flash_ = text;
    flash_ms_ = GetTickCount();
    SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdStatus), text.c_str());
}

std::wstring SftpWindow::status_text() const {
    int ph = phase_.load();
    if (ph != 1 && ph != 2 && ph != 3 && ph != 4 && ph != 7 && ph != 8 && !flash_.empty() && GetTickCount() - flash_ms_ < 5000) return flash_;
    std::string note;
    std::string active_kind;
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        note = note_;
        active_kind = active_kind_;
    }
    std::wstring out = widen(note);
    if (out.empty()) {
        switch (ph) {
        case 1: out = L"正在连接…"; break;
        case 2: out = L"正在读取目录…"; break;
        case 3: out = active_kind == "tree" ? L"正在上传目录…（队列见上方）" : L"正在上传…（进度见上方进度条）"; break;
        case 4: out = active_kind == "tree" ? L"正在下载目录…（队列见上方）" : L"正在下载…（进度见上方进度条）"; break;
        case 7: out = L"已取消"; break;
        case 8: out = L"操作中…"; break;
        default: out = L"就绪：左侧选文件上传，右侧选文件下载"; break;
        }
    }
    return out;
}

void SftpWindow::on_command(int id, int code) {
    switch (id) {
    case sftp_ui::kIdConnect: {
        ensure_target();
        Task t;
        t.kind = Task::Connect;
        t.remote = remote_dir();
        enqueue(t);
        break;
    }
    case sftp_ui::kIdLocalRefresh: refresh_local(); break;
    case sftp_ui::kIdLocalUp: {
        std::string dir = local_dir();
        std::string up = dir_name(dir, '\\');
        SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdLocalPath), widen(up).c_str());
        refresh_local();
        break;
    }
    case sftp_ui::kIdRemoteRefresh: refresh_remote(); break;
    case sftp_ui::kIdRemoteUp: {
        std::string dir = remote_dir();
        std::string up = dir_name(dir, '/');
        if (up.empty()) up = "/";
        SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdRemotePath), widen(up).c_str());
        Task t;
        t.kind = Task::List;
        t.remote = up;
        enqueue(t);
        break;
    }
    case sftp_ui::kIdUpload: {
        std::string path;
        bool is_dir = false;
        if (!selected_local(&path, &is_dir)) { set_status(L"先在左侧选择文件或目录"); break; }
        Task t;
        t.kind = is_dir ? Task::UploadTree : Task::Upload;
        t.local = path;
        t.remote = join(remote_dir(), base_name(path, '\\'), '/');
        enqueue(t);
        break;
    }
    case sftp_ui::kIdDownload: {
        std::string name;
        bool is_dir = false;
        if (!selected_remote(&name, &is_dir)) { set_status(L"先在右侧选择文件或目录"); break; }
        Task t;
        t.kind = is_dir ? Task::DownloadTree : Task::Download;
        t.remote = join(remote_dir(), name, '/');
        t.local = join(local_dir(), name, '\\');
        enqueue(t);
        break;
    }
    case sftp_ui::kIdMkdir: {
        WCHAR buf[256] = L"";
        GetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdName), buf, 256);
        std::wstring wn(buf);
        while (!wn.empty() && iswspace(wn.front())) wn.erase(wn.begin());
        while (!wn.empty() && iswspace(wn.back())) wn.pop_back();
        std::string name = narrow(wn);
        if (name.empty()) { set_status(L"先在“名称”框输入目录名"); break; }
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            name == "." || name == "..") {
            set_status(L"名称不能包含路径分隔符，也不能是 . 或 .."); break;
        }
        if (HWND nm = GetDlgItem(hwnd_, sftp_ui::kIdName)) {
            if (SendMessageW(nm, CB_FINDSTRINGEXACT, 0, reinterpret_cast<LPARAM>(wn.c_str())) == CB_ERR)
                SendMessageW(nm, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wn.c_str()));
        }
        Task t;
        t.kind = Task::Mkdir;
        t.remote = join(remote_dir(), name, '/');
        enqueue(t);
        break;
    }
    case sftp_ui::kIdRename: {
        std::string old_name;
        bool is_dir = false;
        if (!selected_remote(&old_name, &is_dir)) { set_status(L"先在右侧选择要重命名的文件或目录"); break; }
        WCHAR buf[256] = L"";
        GetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdName), buf, 256);
        std::string name = narrow(std::wstring(buf));
        if (name.empty()) { set_status(L"先在“名称”框输入新名称"); break; }
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            name == "." || name == "..") {
            set_status(L"名称不能包含路径分隔符，也不能是 . 或 .."); break;
        }
        if (name == old_name) { set_status(L"新名称与原名相同，未执行"); break; }
        if (HWND nm = GetDlgItem(hwnd_, sftp_ui::kIdName)) {
            if (SendMessageW(nm, CB_FINDSTRINGEXACT, 0, reinterpret_cast<LPARAM>(widen(name).c_str())) == CB_ERR)
                SendMessageW(nm, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(name).c_str()));
        }
        Task t;
        t.kind = Task::Rename;
        t.remote = join(remote_dir(), old_name, '/');
        t.remote2 = join(remote_dir(), name, '/');
        enqueue(t);
        break;
    }
    case sftp_ui::kIdDelete: {
        std::string name;
        bool is_dir = false;
        if (!selected_remote(&name, &is_dir)) { set_status(L"先在右侧选择要删除的文件或目录"); break; }
        if (!delete_armed_) {
            delete_armed_ = true;
            delete_arm_ms_ = GetTickCount();
            if (HWND b = GetDlgItem(hwnd_, sftp_ui::kIdDelete)) SetWindowTextW(b, L"确认删除");
            set_status(L"再次点击“确认删除”即删除：" + widen(name) + (is_dir ? L"（目录）" : L"（文件）"));
            break;
        }
        reset_delete_confirm();
        SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdName), L"");  /* 删除后清空名称，避免误再建同名目录 */
        Task t;
        t.kind = Task::Delete;
        t.remote = join(remote_dir(), name, '/');
        t.remote2 = is_dir ? "dir" : "";
        enqueue(t);
        break;
    }
    case sftp_ui::kIdCancel:
        cancel_.store(true);
        set_status(L"正在取消…（断点会保留，可点“重试”续传）");
        break;
    case sftp_ui::kIdRetry: {
        Task t;
        bool ready = false;
        {
            std::lock_guard<std::mutex> lk(snap_mu_);
            if (has_last_failed_) { t = last_failed_; has_last_failed_ = false; ready = true; }
        }
        if (!ready) { set_status(L"没有可重试的传输"); break; }
        t.retry_count += 1;
        cancel_.store(false);
        set_status(L"重试：从已保留的断点继续…");
        enqueue(t);
        break;
    }
    case sftp_ui::kIdHistoryOpen:
        show_history();
        break;
    case sftp_ui::kIdSettingsOpen:
        show_settings();
        break;
    case sftp_ui::kIdClose:
        close();
        break;
    case sftp_ui::kIdLocalList:
        if (code == LBN_SELCHANGE) { update_local_info(); }
        if (code == LBN_DBLCLK) {
            std::string path;
            bool is_dir = false;
            if (selected_local(&path, &is_dir) && is_dir) {
                SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdLocalPath), widen(path).c_str());
                refresh_local();
            }
        }
        break;
    case sftp_ui::kIdRemoteList:
        if (code == LBN_SELCHANGE) {
            std::string name;
            bool is_dir = false;
            if (selected_remote(&name, &is_dir)) {
                SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdName), widen(name).c_str());  /* 目录名不带尾部斜杠 */
                update_remote_info();
            }
        }
        if (code == LBN_DBLCLK) {
            std::string name;
            bool is_dir = false;
            if (selected_remote(&name, &is_dir) && is_dir) {
                std::string dir = join(remote_dir(), name, '/');
                SetWindowTextW(GetDlgItem(hwnd_, sftp_ui::kIdRemotePath), widen(dir).c_str());
                Task t;
                t.kind = Task::List;
                t.remote = dir;
                enqueue(t);
            }
        }
        break;
    default: break;
    }
}

std::string SftpWindow::state_json() {
    const_cast<SftpWindow*>(this)->ensure_target();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "open", is_open());
    if (!is_open()) {
        char* s = cJSON_PrintUnformatted(root);
        std::string out = s ? s : "{}";
        cJSON_free(s);
        cJSON_Delete(root);
        return out;
    }
    cJSON_AddStringToObject(root, "channel", channel_name().c_str());
    cJSON_AddStringToObject(root, "kind", "sftp");
    cJSON_AddStringToObject(root, "local_path", local_dir().c_str());
    std::vector<SftpEntry> items;
    std::string rpath, err, note, active, akind;
    std::vector<std::string> plan;
    int plan_pos = 0, files_now = 0, files_total = 0;
    bool has_retry = false;
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        items = remote_items_;
        rpath = remote_path_;
        err = remote_error_;
        note = note_;
        active = active_name_;
        akind = active_kind_;
        plan = active_plan_;
        plan_pos = active_plan_pos_;
        files_now = active_files_;
        files_total = active_total_files_;
        has_retry = has_last_failed_;
    }
    cJSON_AddStringToObject(root, "remote_path", rpath.c_str());
    cJSON_AddStringToObject(root, "remote_error", err.c_str());

    auto add_list = [](cJSON* parent, const char* key, HWND list) {
        cJSON* arr = cJSON_AddArrayToObject(parent, key);
        if (!list) return;
        int n = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
        for (int i = 0; i < n; ++i) {
            wchar_t buf[256] = L"";
            SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(buf));
            cJSON_AddItemToArray(arr, cJSON_CreateString(narrow(std::wstring(buf)).c_str()));
        }
    };
    add_list(root, "local_items", GetDlgItem(hwnd_, sftp_ui::kIdLocalList));
    add_list(root, "remote_items", GetDlgItem(hwnd_, sftp_ui::kIdRemoteList));
    int lsel = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, sftp_ui::kIdLocalList), LB_GETCURSEL, 0, 0));
    int rsel = static_cast<int>(SendMessageW(GetDlgItem(hwnd_, sftp_ui::kIdRemoteList), LB_GETCURSEL, 0, 0));
    cJSON_AddNumberToObject(root, "local_sel", lsel);
    cJSON_AddNumberToObject(root, "remote_sel", rsel);

    WCHAR info[512] = L"";
    if (HWND h = GetDlgItem(hwnd_, sftp_ui::kIdLocalInfo)) GetWindowTextW(h, info, 512);
    cJSON_AddStringToObject(root, "local_info", narrow(std::wstring(info)).c_str());
    info[0] = 0;
    if (HWND h = GetDlgItem(hwnd_, sftp_ui::kIdRemoteInfo)) GetWindowTextW(h, info, 512);
    cJSON_AddStringToObject(root, "remote_info", narrow(std::wstring(info)).c_str());
    cJSON* entries = cJSON_AddArrayToObject(root, "remote_entries");
    for (const SftpEntry& e : items) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e.name.c_str());
        cJSON_AddBoolToObject(o, "dir", e.dir);
        cJSON_AddBoolToObject(o, "link", e.link);
        cJSON_AddNumberToObject(o, "size", static_cast<double>(e.size));
        cJSON_AddNumberToObject(o, "mode", e.mode & 0777u);
        cJSON_AddNumberToObject(o, "uid", e.uid);
        cJSON_AddNumberToObject(o, "gid", e.gid);
        cJSON_AddNumberToObject(o, "mtime", e.mtime);
        cJSON_AddItemToArray(entries, o);
    }

    int ph = phase_.load();
    cJSON* tr = cJSON_AddObjectToObject(root, "transfer");
    cJSON_AddStringToObject(tr, "phase", narrow(std::wstring(phase_text(ph))).c_str());
    cJSON_AddNumberToObject(tr, "phase_code", ph);
    cJSON_AddBoolToObject(tr, "busy", (ph >= 1 && ph <= 4) || ph == 8);
    cJSON_AddStringToObject(tr, "kind", akind.c_str());
    cJSON_AddStringToObject(tr, "name", active.c_str());
    cJSON_AddNumberToObject(tr, "files", files_now);
    cJSON_AddNumberToObject(tr, "total_files", files_total);
    cJSON_AddNumberToObject(tr, "plan_index", plan_pos + 1);
    cJSON_AddNumberToObject(tr, "plan_total", static_cast<int>(plan.size()));
    cJSON* plan_arr = cJSON_AddArrayToObject(tr, "plan");
    for (const std::string& p : plan) cJSON_AddItemToArray(plan_arr, cJSON_CreateString(p.c_str()));
    cJSON_AddBoolToObject(tr, "can_retry", has_retry);
    cJSON_AddNumberToObject(tr, "done", static_cast<double>(done_.load()));
    cJSON_AddNumberToObject(tr, "total", static_cast<double>(total_.load()));
    std::uint64_t d = done_.load(), t = total_.load();
    double pct = (ph == 5) ? 100.0 : (ph == 0 ? 0.0 : (t ? static_cast<double>(d * 100.0 / t) : 0.0));
    cJSON_AddNumberToObject(tr, "percent", pct);
    cJSON_AddStringToObject(tr, "note", note.c_str());
    cJSON_AddBoolToObject(tr, "md5_ok", note.find("MD5 一致") != std::string::npos);
    cJSON_AddBoolToObject(root, "connected", client_.connected());
    {
        std::lock_guard<std::mutex> qlk(queue_mu_);
        cJSON_AddNumberToObject(root, "queue_pending", pending_);
    }
    cJSON_AddStringToObject(root, "status", narrow(status_text()).c_str());
    cJSON_AddNumberToObject(root, "remote_count", static_cast<int>(items.size()));
    WCHAR nmbuf[256] = L"";
    if (HWND nm = GetDlgItem(hwnd_, sftp_ui::kIdName)) GetWindowTextW(nm, nmbuf, 256);
    cJSON_AddStringToObject(root, "name_text", narrow(std::wstring(nmbuf)).c_str());
    cJSON_AddBoolToObject(root, "delete_armed", delete_armed_);
    cJSON_AddBoolToObject(root, "history_open", history_ && IsWindow(history_));
    cJSON_AddBoolToObject(root, "settings_open", settings_ && IsWindow(settings_));
    std::uint64_t sl = g_speed_limit_bps.load(std::memory_order_relaxed);
    cJSON_AddNumberToObject(root, "speed_limit_kbps", static_cast<double>(sl / 1024));
    cJSON_AddStringToObject(root, "speed_limit_text", speed_label(sl).c_str());
    std::vector<TransferRecord> hist = history_snapshot();
    cJSON_AddNumberToObject(root, "history_count", static_cast<int>(hist.size()));
    cJSON* hist_arr = cJSON_AddArrayToObject(root, "history");
    int begin = static_cast<int>(hist.size()) - 20;
    if (begin < 0) begin = 0;
    for (int i = static_cast<int>(hist.size()) - 1; i >= begin; --i) {
        const TransferRecord& r = hist[static_cast<std::size_t>(i)];
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "time", r.time.c_str());
        cJSON_AddStringToObject(o, "dir", r.dir.c_str());
        cJSON_AddStringToObject(o, "name", r.name.c_str());
        cJSON_AddStringToObject(o, "result", r.result.c_str());
        cJSON_AddStringToObject(o, "detail", r.detail.c_str());
        cJSON_AddNumberToObject(o, "bytes", static_cast<double>(r.bytes));
        cJSON_AddNumberToObject(o, "total", static_cast<double>(r.total));
        cJSON_AddNumberToObject(o, "elapsed_ms", static_cast<double>(r.elapsed_ms));
        cJSON_AddNumberToObject(o, "speed_bps", r.speed_bps);
        cJSON_AddNumberToObject(o, "retries", r.retries);
        cJSON_AddBoolToObject(o, "tree", r.tree);
        cJSON_AddItemToArray(hist_arr, o);
    }

    char* s = cJSON_PrintUnformatted(root);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(root);
    return out;
}

void SftpWindow::show_history() {
    if (history_ && IsWindow(history_)) {
        ShowWindow(history_, SW_SHOW);
        SetForegroundWindow(history_);
        history_dirty_.store(true, std::memory_order_relaxed);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HistoryWndProc;
        wc.hInstance = inst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = sftp_ui::kHistoryClass;
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    int w = MulDiv(660, dpi_, 96);
    int h = MulDiv(320, dpi_, 96);
    RECT adj{0, 0, w, h};
    AdjustWindowRectEx(&adj, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    RECT pr{};
    if (GetWindowRect(hwnd_, &pr)) {
        x = pr.left + MulDiv(40, dpi_, 96);
        y = pr.top + MulDiv(50, dpi_, 96);
    }
    history_ = CreateWindowExW(WS_EX_TOOLWINDOW, sftp_ui::kHistoryClass, L"传输历史",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, adj.right - adj.left, adj.bottom - adj.top,
                               hwnd_, nullptr, inst_, this);
    if (!history_) return;

    make_static(history_, L"", sftp_ui::kIdHistSummary, font_);
    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
                                    LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                0, 0, 0, 0, history_,
                                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(sftp_ui::kIdHistList)),
                                inst_, nullptr);
    SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    HWND detail = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                  0, 0, 0, 0, history_,
                                  reinterpret_cast<HMENU>(static_cast<UINT_PTR>(sftp_ui::kIdHistDetail)),
                                  inst_, nullptr);
    SendMessageW(detail, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    make_button(history_, L"清空", sftp_ui::kIdHistClear, font_);
    make_button(history_, L"关闭", sftp_ui::kIdHistClose, font_);

    int pad = MulDiv(12, dpi_, 96), gap = MulDiv(8, dpi_, 96), btn = MulDiv(26, dpi_, 96);
    RECT rc{};
    GetClientRect(history_, &rc);
    int cw = rc.right, ch = rc.bottom;
    int by = ch - pad - btn;
    int detail_h = MulDiv(48, dpi_, 96);
    int detail_y = by - gap - detail_h;
    MoveWindow(GetDlgItem(history_, sftp_ui::kIdHistSummary), pad, pad, cw - pad * 2, MulDiv(20, dpi_, 96), TRUE);
    MoveWindow(list, pad, MulDiv(38, dpi_, 96), cw - pad * 2,
               detail_y - gap - MulDiv(38, dpi_, 96), TRUE);
    MoveWindow(detail, pad, detail_y, cw - pad * 2, detail_h, TRUE);
    int bw = MulDiv(88, dpi_, 96);
    MoveWindow(GetDlgItem(history_, sftp_ui::kIdHistClear), pad, by, bw, btn, TRUE);
    MoveWindow(GetDlgItem(history_, sftp_ui::kIdHistClose), cw - pad - bw, by, bw, btn, TRUE);

    aibridge::register_sftp_history(history_);
    ShowWindow(history_, SW_SHOW);
    SetForegroundWindow(history_);
    layout_secondary_windows();
    history_dirty_.store(true, std::memory_order_relaxed);
    refresh_history_list();
}

void SftpWindow::refresh_history_list() {
    if (!history_ || !IsWindow(history_)) return;
    std::vector<TransferRecord> h = history_snapshot();
    HWND list = GetDlgItem(history_, sftp_ui::kIdHistList);
    if (list) {
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (int i = static_cast<int>(h.size()) - 1; i >= 0; --i) {
            const TransferRecord& r = h[static_cast<std::size_t>(i)];
            std::string line = r.time + " | " + r.dir + " | " + r.name + " | " +
                               human_size(r.bytes) + " | " + elapsed_text(r.elapsed_ms) + " | " +
                               human_speed(r.speed_bps) + " | " + r.result + " | 重试 " +
                               std::to_string(r.retries) + (r.tree ? " | 目录" : "");
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(line).c_str()));
        }
        SendMessageW(list, LB_SETHORIZONTALEXTENT, static_cast<WPARAM>(MulDiv(1200, dpi_, 96)), 0);
    }
    int ok = 0, fail = 0, cancel = 0;
    for (const auto& r : h) {
        if (r.result == "成功") ++ok;
        else if (r.result == "失败") ++fail;
        else ++cancel;
    }
    std::string sum = "共 " + std::to_string(h.size()) + " 条 · 成功 " + std::to_string(ok) +
                      " · 失败 " + std::to_string(fail) + " · 取消 " + std::to_string(cancel) +
                      " · 限速 " + speed_label(g_speed_limit_bps.load(std::memory_order_relaxed));
    if (HWND s = GetDlgItem(history_, sftp_ui::kIdHistSummary))
        SetWindowTextW(s, widen(sum).c_str());
    if (HWND d = GetDlgItem(history_, sftp_ui::kIdHistDetail))
        SetWindowTextW(d, L"选中一条记录，这里显示完整失败原因 / 校验结果。");
}

void SftpWindow::clear_history() {
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        records_.clear();
    }
    history_dirty_.store(true, std::memory_order_relaxed);
    if (history_ && IsWindow(history_)) refresh_history_list();
}

void SftpWindow::show_settings() {
    if (settings_ && IsWindow(settings_)) {
        ShowWindow(settings_, SW_SHOW);
        SetForegroundWindow(settings_);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = inst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = sftp_ui::kSettingsClass;
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    int w = MulDiv(520, dpi_, 96);
    int h = MulDiv(190, dpi_, 96);
    RECT adj{0, 0, w, h};
    AdjustWindowRectEx(&adj, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    RECT pr{};
    if (GetWindowRect(hwnd_, &pr)) {
        x = pr.left + MulDiv(100, dpi_, 96);
        y = pr.top + MulDiv(90, dpi_, 96);
    }
    settings_ = CreateWindowExW(WS_EX_TOOLWINDOW, sftp_ui::kSettingsClass, L"传输设置",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                x, y, adj.right - adj.left, adj.bottom - adj.top,
                                hwnd_, nullptr, inst_, this);
    if (!settings_) return;

    make_static(settings_, L"限速（KB/s，0 = 不限）", sftp_ui::kIdSetLabel, font_);
    HWND speed = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                 WS_VSCROLL | CBS_DROPDOWN, 0, 0, 0, 200, settings_,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(sftp_ui::kIdSetSpeed)),
                                 inst_, nullptr);
    SendMessageW(speed, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    const wchar_t* presets[] = {L"0", L"100", L"500", L"1024", L"5120", L"10240"};
    for (const wchar_t* v : presets) SendMessageW(speed, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(v));
    WCHAR cur[64] = L"";
    swprintf_s(cur, 64, L"%llu", static_cast<unsigned long long>(g_speed_limit_bps.load() / 1024));
    SetWindowTextW(speed, cur);
    make_static(settings_, L"", sftp_ui::kIdSetNote, font_);
    make_button(settings_, L"保存", sftp_ui::kIdSetSave, font_);
    make_button(settings_, L"关闭", sftp_ui::kIdSetClose, font_);

    int pad = MulDiv(12, dpi_, 96), btn = MulDiv(26, dpi_, 96);
    RECT rc{};
    GetClientRect(settings_, &rc);
    int cw = rc.right, ch = rc.bottom;
    MoveWindow(GetDlgItem(settings_, sftp_ui::kIdSetLabel), pad, MulDiv(14, dpi_, 96),
               MulDiv(180, dpi_, 96), MulDiv(24, dpi_, 96), TRUE);
    MoveWindow(speed, pad + MulDiv(190, dpi_, 96), MulDiv(12, dpi_, 96),
               MulDiv(180, dpi_, 96), MulDiv(28, dpi_, 96), TRUE);
    MoveWindow(GetDlgItem(settings_, sftp_ui::kIdSetNote), pad, MulDiv(64, dpi_, 96),
               cw - pad * 2, MulDiv(80, dpi_, 96), TRUE);
    MoveWindow(GetDlgItem(settings_, sftp_ui::kIdSetSave), pad, ch - pad - btn,
               MulDiv(88, dpi_, 96), btn, TRUE);
    MoveWindow(GetDlgItem(settings_, sftp_ui::kIdSetClose), cw - pad - MulDiv(88, dpi_, 96),
               ch - pad - btn, MulDiv(88, dpi_, 96), btn, TRUE);
    update_settings_note();
    aibridge::register_sftp_settings(settings_);
    ShowWindow(settings_, SW_SHOW);
    SetForegroundWindow(settings_);
    layout_secondary_windows();
}

void SftpWindow::layout_secondary_windows() {
    if (!history_ || !IsWindow(history_) || !IsWindowVisible(history_)) return;
    if (!settings_ || !IsWindow(settings_) || !IsWindowVisible(settings_)) return;
    RECT hr{}, sr{};
    if (!GetWindowRect(history_, &hr) || !GetWindowRect(settings_, &sr)) return;
    int hw = hr.right - hr.left, hh = hr.bottom - hr.top;
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    int gap = MulDiv(8, dpi_, 96);
    RECT work{};
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
    if (IsRectEmpty(&work)) work = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};

    int total = hh + gap + sh;
    int top = hr.top;
    if (top + total > work.bottom) top = work.bottom - total;
    if (top < work.top) top = work.top;
    if (top + total <= work.bottom) {
        int x = hr.left;
        if (x + (std::max)(hw, sw) > work.right) x = work.right - (std::max)(hw, sw);
        if (x < work.left) x = work.left;
        if (hr.top != top || hr.left != x)
            MoveWindow(history_, x, top, hw, hh, TRUE);
        MoveWindow(settings_, x, top + hh + gap, sw, sh, TRUE);
        return;
    }
    int right_x = hr.right + gap;
    if (right_x + sw <= work.right) {
        MoveWindow(settings_, right_x, hr.top, sw, sh, TRUE);
        return;
    }
    int sx = work.right - sw;
    if (sx < work.left) sx = work.left;
    int sy = work.bottom - sh;
    if (sy < work.top) sy = work.top;
    MoveWindow(settings_, sx, sy, sw, sh, TRUE);
}

void SftpWindow::update_settings_note() {
    if (!settings_ || !IsWindow(settings_)) return;
    HWND speed = GetDlgItem(settings_, sftp_ui::kIdSetSpeed);
    WCHAR buf[64] = L"";
    if (speed) GetWindowTextW(speed, buf, 64);
    std::wstring typed(buf);
    while (!typed.empty() && (typed.front() == L' ' || typed.front() == L'\t')) typed.erase(typed.begin());
    while (!typed.empty() && (typed.back() == L' ' || typed.back() == L'\t')) typed.pop_back();
    bool digits = !typed.empty();
    for (wchar_t c : typed) {
        if (c < L'0' || c > L'9') { digits = false; break; }
    }
    std::uint64_t current_kb = g_speed_limit_bps.load() / 1024ull;
    std::string note;
    if (digits) {
        errno = 0;
        unsigned long long kb = wcstoull(typed.c_str(), nullptr, 10);
        if (errno == 0 && kb <= 1048576ull && kb != current_kb) {
            note = "当前：" + speed_label(g_speed_limit_bps.load()) + "；保存后将改为 " +
                   std::to_string(kb) + " KB/s。";
        } else {
            note = "当前：" + speed_label(g_speed_limit_bps.load()) + "（保存后生效）。";
        }
    } else {
        note = "当前：" + speed_label(g_speed_limit_bps.load()) + "（保存后生效）。";
    }
    note += "\n限速只作用于 SFTP 传输，不影响串口/SSH 回显与终端操作。";
    SetWindowTextW(GetDlgItem(settings_, sftp_ui::kIdSetNote), widen(note).c_str());
}

void SftpWindow::apply_settings() {
    if (!settings_ || !IsWindow(settings_)) return;
    HWND speed = GetDlgItem(settings_, sftp_ui::kIdSetSpeed);
    WCHAR buf[64] = L"";
    if (speed) GetWindowTextW(speed, buf, 64);
    std::wstring wtext(buf);
    while (!wtext.empty() && (wtext.front() == L' ' || wtext.front() == L'\t')) wtext.erase(wtext.begin());
    while (!wtext.empty() && (wtext.back() == L' ' || wtext.back() == L'\t')) wtext.pop_back();
    if (wtext.empty()) {
        SetWindowTextW(GetDlgItem(settings_, sftp_ui::kIdSetNote), L"请输入 0–1048576 的整数（KB/s）。");
        return;
    }
    for (wchar_t c : wtext) {
        if (c < L'0' || c > L'9') {
            SetWindowTextW(GetDlgItem(settings_, sftp_ui::kIdSetNote),
                           L"只能输入整数：0 = 不限；1024 = 1 MB/s。");
            return;
        }
    }
    int kb = _wtoi(wtext.c_str());
    if (kb < 0 || kb > 1048576) {
        SetWindowTextW(GetDlgItem(settings_, sftp_ui::kIdSetNote),
                       L"范围 0–1048576 KB/s（最大 1 GB/s）。");
        return;
    }
    std::uint64_t bps = static_cast<std::uint64_t>(kb) * 1024ull;
    g_speed_limit_bps.store(bps, std::memory_order_relaxed);
    set_status(L"限速已设为 " + widen(speed_label(bps)));
    history_dirty_.store(true, std::memory_order_relaxed);
    DestroyWindow(settings_);
    settings_ = nullptr;
}

LRESULT CALLBACK SftpWindow::HistoryWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    SftpWindow* self = reinterpret_cast<SftpWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    self->history_ = hwnd;
    if (msg == WM_NCDESTROY) {
        aibridge::unregister_sftp_dialog(hwnd);
        self->history_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->handle_history(msg, wp, lp);
}

LRESULT SftpWindow::handle_history(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == sftp_ui::kIdHistClose) { DestroyWindow(history_); return 0; }
        if (id == sftp_ui::kIdHistClear) { clear_history(); return 0; }
        if (id == sftp_ui::kIdHistList && code == LBN_SELCHANGE) {
            int idx = static_cast<int>(SendMessageW(GetDlgItem(history_, sftp_ui::kIdHistList), LB_GETCURSEL, 0, 0));
            auto h = history_snapshot();
            if (idx >= 0 && idx < static_cast<int>(h.size())) {
                const TransferRecord& r = h[h.size() - 1 - static_cast<std::size_t>(idx)];
                std::string detail = r.time + " · " + r.dir + " · " + r.name + "\r\n" +
                                     "结果：" + r.result + " · 大小 " + human_size(r.bytes) +
                                     (r.total && r.total != r.bytes ? " / " + human_size(r.total) : "") +
                                     " · 耗时 " + elapsed_text(r.elapsed_ms) +
                                     " · 均速 " + human_speed(r.speed_bps) +
                                     " · 重试 " + std::to_string(r.retries) + (r.tree ? " · 目录" : "") +
                                     "\r\n详情：" + (r.detail.empty() ? "—" : r.detail);
                SetWindowTextW(GetDlgItem(history_, sftp_ui::kIdHistDetail), widen(detail).c_str());
            }
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(history_);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(history_, msg, wp, lp);
}

LRESULT CALLBACK SftpWindow::SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    SftpWindow* self = reinterpret_cast<SftpWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    self->settings_ = hwnd;
    if (msg == WM_NCDESTROY) {
        aibridge::unregister_sftp_dialog(hwnd);
        self->settings_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->handle_settings(msg, wp, lp);
}

LRESULT SftpWindow::handle_settings(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == sftp_ui::kIdSetSave) { apply_settings(); return 0; }
        if (id == sftp_ui::kIdSetClose) { DestroyWindow(settings_); return 0; }
        if (id == sftp_ui::kIdSetSpeed && (code == CBN_EDITCHANGE || code == CBN_SELCHANGE)) {
            update_settings_note();
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(settings_);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(settings_, msg, wp, lp);
}

LRESULT CALLBACK SftpWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    SftpWindow* self = reinterpret_cast<SftpWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    self->hwnd_ = hwnd;
    return self->handle(msg, wp, lp);
}

LRESULT SftpWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_TIMER:
        if (delete_armed_ && GetTickCount() - delete_arm_ms_ > 3000) reset_delete_confirm();
        update_progress_ui();
        if (history_ && IsWindow(history_) && history_dirty_.exchange(false)) refresh_history_list();
        return 0;
    case kMsgSftpEvent:
        if (wp == kEventRefresh) apply_remote_snapshot();
        update_progress_ui();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        close();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, 1);
        if (history_ && IsWindow(history_)) DestroyWindow(history_);
        if (settings_ && IsWindow(settings_)) DestroyWindow(settings_);
        history_ = nullptr;
        settings_ = nullptr;
        hwnd_ = nullptr;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { close(); return 0; }
        break;
    default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace sb3
