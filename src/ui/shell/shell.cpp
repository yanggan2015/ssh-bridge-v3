// shell.cpp — 主窗口壳实现（S3）。

#include "ui/shell/shell.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/util/util.h"
#include "services/board_actions/board_action_runner.h"
#include "services/service_manager.h"
#include "services/web/web_console.h"
#include "ui/services/file_service_panel.h"
#include "ui/services/relay_panel.h"
#include "ui/dialogs/quick_dialog.h"
#include "ui/ui_messages.h"
#include "ui/aibridge/bridge_host.h"
#include "third_party/win_ui_bridge.h"
#include <cstdint>

namespace sb3 {
namespace {
ChannelKind kind_of_entry(AppCore* core, const std::string& name) {
    const std::string k = core ? core->kind_of(name) : std::string();
    if (k == "ssh") return ChannelKind::Ssh;
    if (k == "powershell") return ChannelKind::PowerShell;
    if (k == "telnet") return ChannelKind::Telnet;
    return ChannelKind::Serial;
}
}  // namespace
namespace {

const wchar_t kClassName[] = L"SB3_MainWindow";
const wchar_t kTitle[]     = L"ssh-bridge v3 · 终端工作台";
const wchar_t kPrefs[]     = L"output\\ui_prefs.ini";
const wchar_t kUpdateManifest[] =
    L"https://raw.githubusercontent.com/yanggan2015/ssh-bridge-v3-releases/windows_x86_64/UPDATE.json";

std::wstring installed_version() {
    wchar_t exePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return L"3.0-dev";
    std::wstring path(exePath);
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"3.0-dev";
    path.resize(slash + 1);
    path += L"VERSION.txt";

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"3.0-dev";
    char buf[512]{};
    DWORD n = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(h);

    std::string text(buf, n);
    const std::string key = "version=";
    size_t pos = text.find(key);
    if (pos == std::string::npos) return L"3.0-dev";
    if (pos != 0 && text[pos - 1] != '\n') return L"3.0-dev";
    size_t end = text.find_first_of("\r\n", pos + key.size());
    std::string ver = text.substr(pos + key.size(), end == std::string::npos
                                  ? std::string::npos : end - pos - key.size());
    while (!ver.empty() && (ver.back() == ' ' || ver.back() == '\t')) ver.pop_back();
    if (ver.empty()) return L"3.0-dev";
    return std::wstring(ver.begin(), ver.end());
}
constexpr int kIdcTree      = 1000;
constexpr int kIdcStatus    = 1001;
constexpr int kIdcView        = 1002;
constexpr int kIdcSearch      = 1013;
constexpr int kIdcSearchLabel = 1015;
constexpr int kIdcKeyHint     = 1016;
constexpr int kIdcBoardScope  = 1021;
constexpr int kIdcQuickAdd     = 1023;
constexpr int kIdcQuickLabel   = 1024;
constexpr int kIdcQuickSep     = 1025;
constexpr int kIdcQuickBase    = 5400;   // 避开板控按钮 5100–5299 的 id 区间
constexpr int kIdmQuickEditBase   = 40120;
constexpr int kIdmQuickDeleteBase = 40300;
constexpr UINT kMsgQuickOpen = WM_APP + 61;
constexpr int kIdcLeftTabs    = 1019;

constexpr int kIdcBtnRefresh   = 1010;
constexpr int kIdcBtnReconnect = 1011;
constexpr int kIdcBtnClear     = 1012;
constexpr int kIdcBtnNew       = 1017;
constexpr int kIdcBtnDisconnect = 1018;
constexpr int kIdcTreeToggle   = 1022;
constexpr int kIdcBoardBase = 5100;

constexpr int kIdmExit       = 40001;
constexpr int kIdmAbout      = 40002;
constexpr int kIdmCheckUpdate = 40060;
constexpr int kIdmHide       = 40010;
constexpr int kIdmRefresh    = 40011;
constexpr int kIdmReconnect  = 40012;
constexpr int kIdmSplit1     = 40030;
constexpr int kIdmSplit2     = 40031;
constexpr int kIdmSplit4     = 40032;
constexpr int kIdmCopy       = 40033;
constexpr int kIdmPaste      = 40034;
constexpr int kIdmReplay     = 40035;
constexpr int kIdmClearView  = 40036;
constexpr int kIdmFindNext   = 40038;
constexpr int kIdmFocusSearch= 40039;
constexpr int kIdmTrayShow   = 40020;
constexpr int kIdmTrayHide   = 40021;
constexpr int kIdmTrayExit   = 40022;
constexpr int kIdmChanNewSerial = 40040;
constexpr int kIdmChanNewSsh    = 40041;
constexpr int kIdmChanEdit      = 40042;
constexpr int kIdmChanDelete    = 40043;
constexpr int kIdmChanReload    = 40044;
constexpr int kIdmSftp          = 40045;
constexpr int kIdmChanNewPowershell = 40055;
constexpr int kIdmChanNewTelnet     = 40056;
constexpr int kIdmRelayConfig   = 40050;
constexpr int kIdmFileService   = 40051;
constexpr int kIdmBoardButtons  = 40052;
constexpr int kIdmDisconnect    = 40047;
constexpr int kIdmStatusBar     = 40053;

// 关于窗口（S5）：改成非模态。模态 MessageBox 会让 UI 线程阻塞，
// 桥接 /action 在 8s 后 504，等于 AI 永远点不动这个控件。
const wchar_t kAboutClass[] = L"SB3_AboutBox";
constexpr int kIdcAboutText = 2001;
constexpr int kIdcAboutOk   = 2002;
constexpr int kIdcAboutVersion = 2003;
constexpr int kIdcAboutUpdate = 2004;
constexpr int kIdcAboutUpdateNote = 2005;

LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == kIdcAboutUpdate) {
            HWND owner = reinterpret_cast<HWND>(GetWindowLongPtrW(hwnd, GWLP_HWNDPARENT));
            if (owner) {
                PostMessageW(owner, WM_COMMAND, MAKEWPARAM(kIdmCheckUpdate, 0), 0);
            }
            return 0;
        }
        if (LOWORD(wp) == kIdcAboutOk) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    return out;
}


}  // namespace

void save_window_rect(HWND hwnd, AppCore* core) {
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return;
    if (IsIconic(hwnd) || IsZoomed(hwnd)) return;
    if (!core) return;
    char value[64];
    std::snprintf(value, sizeof(value), "%ld,%ld,%ld,%ld", rc.left, rc.top, rc.right, rc.bottom);
    core->save_window_rect(value);
}

RECT legacy_window_rect() {
    wchar_t buf[64] = L"";
    GetPrivateProfileStringW(L"window", L"rect", L"", buf, 64, kPrefs);
    RECT rc{};
    if (std::swscanf(buf, L"%ld,%ld,%ld,%ld", &rc.left, &rc.top, &rc.right, &rc.bottom) == 4 &&
        rc.right - rc.left > 400 && rc.bottom - rc.top > 300) {
        return rc;
    }
    rc.left = CW_USEDEFAULT;
    rc.top = CW_USEDEFAULT;
    rc.right = rc.left + 1100;
    rc.bottom = rc.top + 720;
    return rc;
}

RECT load_window_rect(const UiConfig& ui) {
    RECT rc{};
    if (std::sscanf(ui.window_rect.c_str(), "%ld,%ld,%ld,%ld",
                    &rc.left, &rc.top, &rc.right, &rc.bottom) == 4 &&
        rc.right - rc.left > 400 && rc.bottom - rc.top > 300) {
        return rc;
    }
    // 兼容旧 ui_prefs.ini；新数据只写 ssh_bridge_ui.json。
    return legacy_window_rect();
}

Shell::~Shell() {
    if (web_) {
        web_->stop();
        aibridge::set_web_console(nullptr);
        delete web_;
        web_ = nullptr;
    }
    if (services_) {
        services_->stop_all();
        delete services_;
        services_ = nullptr;
    }
    if (relay_panel_) { relay_panel_->close(); delete relay_panel_; relay_panel_ = nullptr; }
    if (file_panel_) { file_panel_->close(); delete file_panel_; file_panel_ = nullptr; }
}

bool Shell::create(HINSTANCE inst, int nShow, AppCore* core) {
    inst_ = inst;
    core_ = core;

    services_ = new ServiceManager(core_->config().project_root);
    services_->apply_config(core_->config());
    services_->set_inbound_resolver([this](const std::string& name, InboundTarget* target,
                                           std::string* err) {
        if (!target || !core_) {
            if (err) *err = "入站解析器未就绪";
            return false;
        }
        return core_->resolve_inbound(name, &target->bridge, &target->password, err);
    });
    aibridge::set_service_manager(services_);

    // 临时打点：窗口创建链路出问题时要能看出停在哪一步

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &Shell::WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    WNDCLASSEXW ac{};
    ac.cbSize        = sizeof(ac);
    ac.lpfnWndProc   = &AboutProc;
    ac.hInstance     = inst;
    ac.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    ac.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    ac.lpszClassName = kAboutClass;
    if (!RegisterClassExW(&ac)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    RECT rc = load_window_rect(core_->config().ui);
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int avail_w = wa.right - wa.left;
    int avail_h = wa.bottom - wa.top;
    // 首次启动按工作区的 90% 开窗：这是终端工具，会话区越大越好
    int w = (rc.left == CW_USEDEFAULT) ? static_cast<int>(avail_w * 0.90) : (rc.right - rc.left);
    int h = (rc.left == CW_USEDEFAULT) ? static_cast<int>(avail_h * 0.90) : (rc.bottom - rc.top);
    // 启动时把窗口收进工作区，避免高 DPI 下超出屏幕导致状态栏看不见
    if (avail_w > 400 && w > avail_w - 40) w = avail_w - 40;
    if (avail_h > 300 && h > avail_h - 60) h = avail_h - 60;
    int x = (rc.left == CW_USEDEFAULT) ? wa.left + (avail_w - w) / 2 : rc.left;
    int y = (rc.left == CW_USEDEFAULT) ? wa.top + (avail_h - h) / 2 : rc.top;
    HWND hwnd = CreateWindowExW(0, kClassName, kTitle, WS_OVERLAPPEDWINDOW,
                                x, y, w, h, nullptr, nullptr, inst, this);
    if (!hwnd) return false;
    hwnd_ = hwnd;

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    // AI 可控桥接（可选）：默认关闭；配置 agent.enable=true 才监听 127.0.0.1。
    const AgentConfig& ag = core_->config().agent;
    aibridge::Options bo;
    bo.enabled = ag.enabled;
    bo.port    = ag.port;
    bo.token   = ag.token;
    if (!aibridge::start(hwnd, core_, &view_, bo)) {
        OutputDebugStringW(L"[aibridge] start skipped/failed\n");
    }
    sync_inbound();
    sync_web();
    sync_s12();
    return true;
}

void Shell::sync_inbound() {
    if (!services_ || !core_) return;
    const InboundConfig& inb = core_->config().inbound;
    if (!inb.enabled) {
        if (services_->inbound_running()) {
            services_->stop_inbound();
            note_status(L"入站 SSH 已关闭");
        }
        return;
    }
    std::string err;
    if (!services_->start_inbound(inb, &err)) {
        note_status(L"入站 SSH 启动失败：" + widen(err));
        return;
    }
    note_status(widen(services_->inbound_status()));
}

void Shell::sync_web() {
    if (!services_ || !core_) return;
    const WebConfig& w = core_->config().web;
    if (!web_) web_ = new WebConsole();
    web_->set_notify(hwnd_);
    if (!w.enabled) {
        if (web_->running()) {
            web_->stop();
            note_status(L"Web 控制台已关闭");
        }
        web_->configure(w.port, w.token, w.require_token);
        aibridge::set_web_console(web_);
        return;
    }
    if (web_->running() && web_->port() == w.port && web_->token() == w.token &&
        web_->require_token() == w.require_token) {
        aibridge::set_web_console(web_);
        return;
    }
    if (web_->running()) web_->stop();
    web_->configure(w.port, w.token, w.require_token);
    std::string err;
    if (!web_->start(core_, services_, w.port, w.token, w.require_token, &err)) {
        note_status(L"Web 控制台启动失败：" + widen(err));
        aibridge::set_web_console(web_);
        return;
    }
    aibridge::set_web_console(web_);
    std::string url = "http://" + services_->local_ip() + ":" + std::to_string(w.port);
    std::wstring auth = !w.require_token ? L"（可写，未启用 token）"
                       : (w.token.empty() ? L"（只读，token 未配置）" : L"（写接口需 token）");
    note_status(L"Web 控制台：" + widen(url) + auth);
}

void Shell::sync_s12() {
    if (!services_ || !core_) return;
    const AppConfig& cfg = core_->config();

    if (!cfg.tcp_fwd.enable) {
        services_->stop_tcp_forward();
    } else {
        std::string err;
        if (!services_->start_tcp_forward(cfg.tcp_fwd, &err)) {
            note_status(L"TCP 转发启动失败：" + widen(err));
        }
    }

    if (!cfg.tunnel.enable) {
        services_->stop_tunnel();
    } else {
        std::string err;
        if (!services_->start_tunnel(cfg.tunnel, &err)) {
            note_status(L"隧道启动失败：" + widen(err));
        } else if (!err.empty()) {
            note_status(L"隧道部分失败：" + widen(err));
        }
    }

    if (!cfg.discovery.enable) {
        services_->stop_discovery();
    } else {
        std::string err;
        if (!services_->start_discovery(cfg.discovery, cfg, &err)) {
            note_status(L"UDP 发现启动失败：" + widen(err));
        }
    }
}

void Shell::build_menu() {
    HMENU menu = CreateMenu();

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, kIdmChanNewSerial, L"新建串口通道…(&N)\tCtrl+N");
    AppendMenuW(file, MF_STRING, kIdmChanNewSsh, L"新建 SSH 通道…(&S)");
    AppendMenuW(file, MF_STRING, kIdmChanNewPowershell, L"新建 PowerShell 通道…(&P)");
    AppendMenuW(file, MF_STRING, kIdmChanNewTelnet, L"新建 Telnet 通道…(&T)");
    AppendMenuW(file, MF_STRING, kIdmChanEdit, L"编辑当前通道…(&E)");
    AppendMenuW(file, MF_STRING, kIdmChanDelete, L"删除当前通道…(&D)");
    AppendMenuW(file, MF_STRING, kIdmChanReload, L"重新加载配置(&R)");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kIdmSftp, L"SFTP 文件传输…(&T)\tF7");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kIdmHide, L"隐藏到托盘(&H)\tEsc");
    AppendMenuW(file, MF_STRING, kIdmExit, L"退出(&X)\tAlt+F4");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件(&F)");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, kIdmRefresh, L"刷新状态(&R)\tF5");
    AppendMenuW(view, MF_STRING, kIdmReconnect, L"重连当前通道(&C)");
    AppendMenuW(view, MF_STRING, kIdmDisconnect, L"断开当前通道(&D)");
    AppendMenuW(view, MF_STRING, kIdmStatusBar, L"显示状态栏(&S)");
    if (core_ && core_->config().ui.show_statusbar)
        CheckMenuItem(view, kIdmStatusBar, MF_BYCOMMAND | MF_CHECKED);
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, kIdmSplit1, L"单窗格(&1)");
    AppendMenuW(view, MF_STRING, kIdmSplit2, L"左右两窗格(&2)");
    AppendMenuW(view, MF_STRING, kIdmSplit4, L"四窗格(&4)");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, kIdmCopy, L"复制选区(&C)\tCtrl+Shift+C");
    AppendMenuW(view, MF_STRING, kIdmPaste, L"粘贴(&V)\tCtrl+Shift+V");
    AppendMenuW(view, MF_STRING, kIdmReplay, L"加载历史会话(&L)");
    AppendMenuW(view, MF_STRING, kIdmClearView, L"清屏(&K)");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, kIdmFocusSearch, L"跳到搜索框(&F)\tCtrl+F");
    AppendMenuW(view, MF_STRING, kIdmFindNext, L"下一个命中(&N)\tF3");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"视图(&V)");

    // 常用功能直接放顶级菜单，不藏在“工具”二级菜单里
    AppendMenuW(menu, MF_STRING, kIdmRelayConfig, L"继电器(&R)");
    AppendMenuW(menu, MF_STRING, kIdmFileService, L"文件服务(&F)");
    AppendMenuW(menu, MF_STRING, kIdmBoardButtons, L"板控按钮(&B)");
    AppendMenuW(menu, MF_STRING, kIdmSftp, L"SFTP(&S)\tF7");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, kIdmCheckUpdate, L"检查更新…(&U)");
    AppendMenuW(help, MF_STRING, kIdmAbout, L"关于(&A)");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"帮助(&H)");

    SetMenu(hwnd_, menu);
}

void Shell::build_children() {

    HDC dc = GetDC(hwnd_);
    dpi_ = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(hwnd_, dc);
    if (dpi_ <= 0) dpi_ = 96;
    statusbar_visible_ = core_->config().ui.show_statusbar;
    font_ = CreateFontW(-MulDiv(9, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!font_) font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    const wchar_t* btn_text[6] = {L"新建", L"刷新", L"重连", L"断开", L"清屏", L"收起"};
    const int btn_ids[6] = {kIdcBtnNew, kIdcBtnRefresh, kIdcBtnReconnect,
                            kIdcBtnDisconnect, kIdcBtnClear, kIdcTreeToggle};
    for (int i = 0; i < 6; ++i) {
        buttons_[i] = CreateWindowExW(0, L"BUTTON", btn_text[i],
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      0, 0, 0, 0, hwnd_,
                                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(btn_ids[i])),
                                      inst_, nullptr);
        SendMessageW(buttons_[i], WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    // 第 6 个按钮：窄窗自动收起，宽窗自动展开；手动点击后保持到跨越宽度断点。
    ui_bridge_set_id(buttons_[5], "btn.toggleTree");

    // 左侧固定工作区：通道 / 文件选项卡。文件页本轮先给结构说明，内容按通道类型后续接入。
    left_tabs_ = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_HOTTRACK,
                                 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdcLeftTabs)),
                                 inst_, nullptr);
    SendMessageW(left_tabs_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    {
        const wchar_t* pages[2] = {L"通道", L"文件"};
        for (int i = 0; i < 2; ++i) {
            TCITEMW it{};
            it.mask = TCIF_TEXT;
            it.pszText = const_cast<LPWSTR>(pages[i]);
            SendMessageW(left_tabs_, TCM_INSERTITEMW, static_cast<WPARAM>(i),
                         reinterpret_cast<LPARAM>(&it));
        }
    }

    ui_bridge_set_id(left_tabs_, "tab.left");

    tree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES |
                                TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
                                TVS_NOHSCROLL,
                            0, 0, 0, 0, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdcTree)), inst_, nullptr);
    SendMessageW(tree_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

    if (!file_browser_.create(inst_, hwnd_, core_, font_, dpi_,
                              [this]() { PostMessageW(hwnd_, WM_COMMAND, kIdmSftp, 0); }))
        return;

    // 终端工具底部不需要拖拽手柄（SBARS_SIZEGRIP 会占用第 4 段并触发额外重绘）
    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                              WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdcStatus)), inst_,
                              nullptr);
    SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    int parts[4] = {220, 420, 640, -1};
    SendMessageW(status_, SB_SETPARTS, 4, reinterpret_cast<LPARAM>(parts));

    search_label_ = CreateWindowExW(0, L"STATIC", L"搜索",
                                    WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
                                    0, 0, 0, 0, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdcSearchLabel)),
                                    inst_, nullptr);
    SendMessageW(search_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    keyhint_ = CreateWindowExW(0, L"STATIC", L"Ctrl+N 新建 · F5 刷新 · Ctrl+F 搜索 · F3 下一个",
                               WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                               0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdcKeyHint)),
                               inst_, nullptr);
    SendMessageW(keyhint_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    board_scope_ = CreateWindowExW(0, L"STATIC", L"",
                                   WS_CHILD | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX,
                                   0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdcBoardScope)),
                                   inst_, nullptr);
    SendMessageW(board_scope_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    ui_bridge_set_id(board_scope_, "label.board.scope");
    ShowWindow(board_scope_, SW_HIDE);

    quick_sep_ = CreateWindowExW(0, L"STATIC", L"",
                                 WS_CHILD | SS_ETCHEDHORZ, 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdcQuickSep)),
                                 inst_, nullptr);
    ui_bridge_set_id(quick_sep_, "sep.quick");
    quick_label_ = CreateWindowExW(0, L"STATIC", L"快捷指令",
                                   WS_CHILD | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE |
                                       SS_ENDELLIPSIS | SS_NOPREFIX,
                                   0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdcQuickLabel)),
                                   inst_, nullptr);
    SendMessageW(quick_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    ui_bridge_set_id(quick_label_, "label.quick.channel");
    quick_add_ = CreateWindowExW(0, L"BUTTON", L"＋ 添加",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdcQuickAdd)),
                                 inst_, nullptr);
    SendMessageW(quick_add_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    ui_bridge_set_id(quick_add_, "btn.quick.add");
    ShowWindow(quick_label_, SW_HIDE);
    ShowWindow(quick_add_, SW_HIDE);
    ShowWindow(quick_sep_, SW_HIDE);
    search_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdcSearch)), inst_, nullptr);
    SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"回看关键词（Enter 定位，F3 下一个）"));
    SetWindowSubclass(search_, SearchEditProc, 1, reinterpret_cast<DWORD_PTR>(this));


    TerminalView::Options opt;
    int cfg_font = core_->config().ui.font_size;
    opt.font_px = (cfg_font >= 9 && cfg_font <= 40) ? cfg_font : load_terminal_font_px(15);
    opt.scrollback = static_cast<std::size_t>(std::max(100, core_->config().ui.scrollback_lines));
    opt.split = core_->config().ui.split;
    if (!view_.create(inst_, hwnd_, core_, opt, dpi_)) return;


    core_->set_kick([this](std::size_t idx) {
        if (hwnd_) PostMessageW(hwnd_, kMsgTermData, static_cast<WPARAM>(idx), 0);
    });


    layout_ready_ = false;
    restore_layout_from_config();
    fill_tree();

    tray_.create(hwnd_, L"ssh-bridge v3 · 双击恢复窗口");
    layout_ready_ = true;
}

void Shell::fill_tree() {
    SendMessageW(tree_, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(TVI_ROOT));
    channel_items_.clear();
    const auto& entries = core_->entries();

    struct Group {
        const wchar_t* text;
        const char* kind;
    };
    const Group groups[4] = {
        {L"SSH", "ssh"},
        {L"串口", "serial"},
        {L"Telnet", "telnet"},
        {L"PowerShell", "powershell"},
    };

    for (const Group& g : groups) {
        int count = 0;
        for (const AppCore::Entry& e : entries) {
            if (e.kind == g.kind) ++count;
        }
        std::wstring title = g.text + std::wstring(L" (") + std::to_wstring(count) + L")";
        TVINSERTSTRUCTW group{};
        group.hParent = TVI_ROOT;
        group.hInsertAfter = TVI_LAST;
        group.item.mask = TVIF_TEXT | TVIF_PARAM;
        group.item.pszText = const_cast<LPWSTR>(title.c_str());
        group.item.lParam = -1;
        HTREEITEM groupItem = reinterpret_cast<HTREEITEM>(
            SendMessageW(tree_, TVM_INSERTITEM, 0, reinterpret_cast<LPARAM>(&group)));
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const AppCore::Entry& e = entries[i];
            if (e.kind != g.kind) continue;
            std::string text = e.name;
            std::wstring wt = widen(text);
            TVINSERTSTRUCTW item{};
            item.hParent = groupItem;
            item.hInsertAfter = TVI_LAST;
            item.item.mask = TVIF_TEXT | TVIF_PARAM;
            item.item.pszText = const_cast<LPWSTR>(wt.c_str());
            item.item.lParam = static_cast<LPARAM>(i);
            HTREEITEM h = reinterpret_cast<HTREEITEM>(
                SendMessageW(tree_, TVM_INSERTITEM, 0, reinterpret_cast<LPARAM>(&item)));
            channel_items_.push_back(h);
        }
        if (count > 0) SendMessageW(tree_, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(groupItem));
    }
    std::string want = view_.active_channel();
    HTREEITEM initial = nullptr;
    if (!want.empty()) {
        const auto& channel_entries = core_->entries();
        for (std::size_t i = 0; i < channel_entries.size(); ++i) {
            if (channel_entries[i].name != want) continue;
            for (HTREEITEM h : channel_items_) {
                TVITEMW tv{};
                tv.mask = TVIF_PARAM | TVIF_HANDLE;
                tv.hItem = h;
                if (SendMessageW(tree_, TVM_GETITEM, 0, reinterpret_cast<LPARAM>(&tv)) &&
                    static_cast<std::size_t>(tv.lParam) == i) {
                    initial = h;
                    break;
                }
            }
            break;
        }
    }
    if (!initial && core_->config().ui.pane_channels.empty() && !channel_items_.empty())
        initial = channel_items_[0];
    if (initial) {
        SendMessageW(tree_, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(initial));
        on_tree_change();
    } else {
        SendMessageW(tree_, TVM_SELECTITEM, TVGN_CARET, 0);
        refresh_board_buttons();
        update_status();
    }
}

void Shell::note_status(const std::wstring& text) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t ts[16] = L"";
    swprintf_s(ts, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    last_note_ = text + L" · " + ts;
    update_status();
}

static std::wstring fit_status_text(HDC dc, const std::wstring& s, int max_px) {
    if (s.empty() || max_px <= 0) return L"";
    SIZE sz{};
    if (!GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz) || sz.cx <= max_px)
        return s;
    const std::wstring ell = L"…";
    for (int len = static_cast<int>(s.size()) - 1; len > 0; --len) {
        std::wstring cand = s.substr(0, static_cast<std::size_t>(len)) + ell;
        if (GetTextExtentPoint32W(dc, cand.c_str(), static_cast<int>(cand.size()), &sz) &&
            sz.cx <= max_px)
            return cand;
    }
    return ell;
}

void Shell::update_status() {
    std::wstring s0 = widen(core_->summary());
    std::wstring s1 = L"当前：无";
    std::string sel = current_channel();
    int pane_no = view_.active_index() + 1;
    if (!sel.empty()) {
        if (view_.split_count() > 1)
            s1 = L"当前：画框" + std::to_wstring(pane_no) + L" · " + widen(sel);
        else
            s1 = L"当前：" + widen(sel);
    } else if (view_.split_count() > 1) {
        s1 = L"当前：画框" + std::to_wstring(pane_no) + L" · 空";
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = rc.right > 0 ? rc.right : 1000;
    bool narrow = w < MulDiv(900, dpi_, 96);
    std::wstring s3;
    if (button_busy_) {
        double sec = button_start_ms_ ? static_cast<double>(util::now_ms() - button_start_ms_) / 1000.0 : 0.0;
        wchar_t buf[32] = L"";
        swprintf_s(buf, L"%.1fs", sec);
        if (narrow)
            s3 = active_button_id_ + L" " + buf;
        else
            s3 = L"按钮：" + active_button_id_ + L"  执行中 " + buf;
    } else if (button_has_run_) {
        s3 = std::wstring(button_last_ok_ ? L"✓ " : L"⚠ ");
        if (!narrow) s3 += L"按钮：";
        s3 += active_button_id_;
    } else {
        s3 = narrow ? L"未触发" : L"按钮：未触发";
    }
    std::wstring s4;
    if (!active_script_line_.empty()) {
        s4 = L"输出：" + active_script_line_;
    } else if (button_busy_) {
        s4 = L"输出：等待脚本打印…";
    } else {
        s4 = last_note_.empty() ? L"就绪" : last_note_;
    }
    // Four parts: channel summary, current channel, triggered button, live script output.
    int parts[4];
    if (narrow) {
        parts[0] = w * 18 / 100;
        parts[1] = w * 42 / 100;
        parts[2] = w * 69 / 100;
    } else {
        parts[0] = w * 12 / 100;
        parts[1] = w * 30 / 100;
        parts[2] = w * 52 / 100;
    }
    parts[3] = -1;
    SendMessageW(status_, SB_SETPARTS, 4, reinterpret_cast<LPARAM>(parts));
    HDC sdc = GetDC(status_);
    HFONT old_font = sdc ? static_cast<HFONT>(SelectObject(sdc, font_)) : nullptr;
    if (sdc) {
        s0 = fit_status_text(sdc, s0, parts[0] - 8);
        s1 = fit_status_text(sdc, s1, parts[1] - parts[0] - 8);
        s3 = fit_status_text(sdc, s3, parts[2] - parts[1] - 8);
        s4 = fit_status_text(sdc, s4, w - parts[2] - 8);
    }
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(s0.c_str()));
    SendMessageW(status_, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(s1.c_str()));
    SendMessageW(status_, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(s3.c_str()));
    SendMessageW(status_, SB_SETTEXTW, 3, reinterpret_cast<LPARAM>(s4.c_str()));
    if (sdc) {
        SelectObject(sdc, old_font);
        ReleaseDC(status_, sdc);
    }

    const auto& entries = core_->entries();
    for (HTREEITEM item : channel_items_) {
        if (!item || !IsWindow(tree_)) continue;
        TVITEMW tv{};
        tv.mask  = TVIF_PARAM | TVIF_HANDLE;
        tv.hItem = item;
        if (!SendMessageW(tree_, TVM_GETITEM, 0, reinterpret_cast<LPARAM>(&tv))) continue;
        std::size_t idx = static_cast<std::size_t>(tv.lParam);
        if (idx >= entries.size()) continue;
        const char* st = entries[idx].bridge && entries[idx].bridge->running() ? "在线" : "离线";
        std::string text = entries[idx].name + "  ·  " + st;
        std::wstring wt = widen(text);
        TVITEMW set{};
        set.mask    = TVIF_TEXT | TVIF_HANDLE;
        set.hItem   = item;
        set.pszText = const_cast<LPWSTR>(wt.c_str());
        SendMessageW(tree_, TVM_SETITEM, 0, reinterpret_cast<LPARAM>(&set));
    }
    dump_state();
}

std::string Shell::selected_channel() const {
    HTREEITEM item = TreeView_GetSelection(tree_);
    if (!item) return std::string();
    TVITEMW tv{};
    tv.mask  = TVIF_PARAM | TVIF_HANDLE;
    tv.hItem = item;
    if (!SendMessageW(tree_, TVM_GETITEM, 0, reinterpret_cast<LPARAM>(&tv))) return std::string();
    std::size_t idx = static_cast<std::size_t>(tv.lParam);
    if (idx >= core_->entries().size()) return std::string();
    return core_->entries()[idx].name;
}

std::string Shell::current_channel() const {
    if (view_.hwnd()) {
        std::string active = view_.active_channel();
        if (!active.empty()) return active;
        // 多画框时，空画框就是“没有当前板子”，不能回落到左树选择。
        if (view_.split_count() > 1) return std::string();
    }
    return selected_channel();
}

void Shell::layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    bool ultra  = rc.right < MulDiv(640, dpi_, 96);
    bool narrow = rc.right < MulDiv(900, dpi_, 96);
    int gap   = ultra ? MulDiv(4, dpi_, 96) : (narrow ? MulDiv(5, dpi_, 96) : MulDiv(8, dpi_, 96));
    int btn_h = MulDiv(26, dpi_, 96);
    int btn_w = ultra ? MulDiv(44, dpi_, 96) : (narrow ? MulDiv(56, dpi_, 96) : MulDiv(70, dpi_, 96));
    for (int i = 0; i < 6; ++i) {
        MoveWindow(buttons_[i], gap + i * (btn_w + gap), gap, btn_w, btn_h, TRUE);
    }

    // 窄窗自动收起左侧通道管理器；手动切换优先，跨越宽度断点后恢复自动。
    bool compact = rc.right < MulDiv(820, dpi_, 96);
    if (compact != last_compact_) {
        left_user_override_ = false;
        last_compact_ = compact;
    }
    if (!left_user_override_) {
        if (compact) {
            if (!left_collapsed_) note_status(L"已收起");
            left_collapsed_ = true;
            left_auto_collapsed_ = true;
        } else if (left_auto_collapsed_) {
            left_collapsed_ = false;
            left_auto_collapsed_ = false;
            note_status(L"已展开");
        }
    }
    if (buttons_[5]) SetWindowTextW(buttons_[5], left_collapsed_ ? L"展开" : L"收起");

    int sb_h = 0;
    if (statusbar_visible_) {
        ShowWindow(status_, SW_SHOWNA);
        SendMessageW(status_, WM_SIZE, 0, 0);
        RECT sr{};
        GetWindowRect(status_, &sr);
        sb_h = sr.bottom - sr.top;
        if (sb_h <= 0) sb_h = MulDiv(22, dpi_, 96);
        MoveWindow(status_, 0, rc.bottom - sb_h, rc.right, sb_h, TRUE);
    } else {
        ShowWindow(status_, SW_HIDE);
    }
    int top = btn_h + gap * 2;
    int body_h = rc.bottom - sb_h - top;
    if (body_h < MulDiv(120, dpi_, 96)) body_h = MulDiv(120, dpi_, 96);

    int left_w = MulDiv(246, dpi_, 96);
    int header = MulDiv(26, dpi_, 96);
    if (left_collapsed_) {
        ShowWindow(left_tabs_, SW_HIDE);
        ShowWindow(tree_, SW_HIDE);
        if (file_browser_.hwnd()) ShowWindow(file_browser_.hwnd(), SW_HIDE);
    } else {
        ShowWindow(left_tabs_, SW_SHOWNA);
        MoveWindow(left_tabs_, gap, top, left_w, body_h, TRUE);
        if (left_tabs_) {
            RECT tr{0, 0, left_w, body_h};
            if (SendMessageW(left_tabs_, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&tr)) &&
                tr.bottom > MulDiv(8, dpi_, 96))
                header = tr.bottom + MulDiv(2, dpi_, 96);
        }
        int content_x = gap + MulDiv(4, dpi_, 96);
        int content_y = top + header;
        int content_w = left_w - MulDiv(8, dpi_, 96);
        int content_h = body_h - header - MulDiv(4, dpi_, 96);
        if (content_h < 0) content_h = 0;
        MoveWindow(tree_, content_x, content_y, content_w, content_h, TRUE);
        file_browser_.layout(content_x, content_y, content_w, content_h);
        if (left_page_ == 0) {
            ShowWindow(tree_, SW_SHOWNA);
            ShowWindow(file_browser_.hwnd(), SW_HIDE);
        } else {
            ShowWindow(tree_, SW_HIDE);
            ShowWindow(file_browser_.hwnd(), SW_SHOWNA);
        }
    }

    int label_w  = ultra ? 0 : (narrow ? MulDiv(36, dpi_, 96) : MulDiv(42, dpi_, 96));
    int search_w = ultra ? MulDiv(110, dpi_, 96) : (narrow ? MulDiv(170, dpi_, 96) : MulDiv(240, dpi_, 96));
    int search_x = rc.right - (ultra ? gap : gap * 2) - search_w;
    int label_x  = search_x - label_w - MulDiv(6, dpi_, 96);
    if (ultra) {
        ShowWindow(search_label_, SW_HIDE);
    } else {
        MoveWindow(search_label_, label_x, gap, label_w, btn_h, TRUE);
        ShowWindow(search_label_, SW_SHOWNA);
    }
    MoveWindow(search_, search_x, gap, search_w, btn_h, TRUE);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(ultra ? L"搜索（Enter）" : L"回看关键词（Enter 定位，F3 下一个）"));
    if (keyhint_) {
        int hx = gap + 6 * (btn_w + gap) + MulDiv(14, dpi_, 96);
        int hw = label_x - hx - MulDiv(12, dpi_, 96);
        if (!narrow && hw >= MulDiv(210, dpi_, 96)) {
            MoveWindow(keyhint_, hx, gap, hw, btn_h, TRUE);
            ShowWindow(keyhint_, SW_SHOWNA);
        } else {
            ShowWindow(keyhint_, SW_HIDE);
        }
    }

    int term_x = gap + (left_collapsed_ ? 0 : left_w) + gap;
    int term_w = rc.right - term_x - gap;
    if (term_w < MulDiv(200, dpi_, 96)) term_w = MulDiv(200, dpi_, 96);
    board_strip_h_ = layout_board_buttons(term_x, top, term_w);
    int quick_h = layout_quick_buttons(term_x, 0, term_w, false);
    int term_y = top + board_strip_h_;
    int term_h = body_h - board_strip_h_ - quick_h;
    if (term_h < 0) term_h = 0;
    MoveWindow(view_.hwnd(), term_x, term_y, term_w, term_h, TRUE);
    if (quick_h > 0)
        layout_quick_buttons(term_x, top + body_h - quick_h, term_w, true);
    view_.invalidate();
}

int Shell::layout_board_buttons(int x, int y, int avail_w) {
    int gap = MulDiv(4, dpi_, 96);
    int row_h = MulDiv(30, dpi_, 96);
    int min_w = MulDiv(74, dpi_, 96);
    bool multi = view_.split_count() > 1;

    int scope_w = 0;
    if (multi && board_scope_ && IsWindow(board_scope_)) {
        int text_w = 0;
        int len = GetWindowTextLengthW(board_scope_);
        if (len > 0) {
            std::wstring text(static_cast<std::size_t>(len) + 1, L'\0');
            GetWindowTextW(board_scope_, text.data(), len + 1);
            text.resize(static_cast<std::size_t>(len));
            HDC dc = GetDC(board_scope_);
            if (dc) {
                HFONT old = static_cast<HFONT>(SelectObject(dc, font_));
                SIZE sz{};
                if (GetTextExtentPoint32W(dc, text.c_str(), len, &sz)) text_w = sz.cx;
                SelectObject(dc, old);
                ReleaseDC(board_scope_, dc);
            }
        }
        scope_w = text_w + MulDiv(18, dpi_, 96);
        int cap = avail_w / 2;
        if (scope_w > cap) scope_w = cap;
        if (scope_w < MulDiv(120, dpi_, 96)) scope_w = MulDiv(120, dpi_, 96);
        MoveWindow(board_scope_, x, y, scope_w, row_h, TRUE);
        ShowWindow(board_scope_, SW_SHOWNA);
        x += scope_w + gap;
        avail_w -= scope_w + gap;
    } else if (board_scope_ && IsWindow(board_scope_)) {
        ShowWindow(board_scope_, SW_HIDE);
    }

    if (board_buttons_.empty()) return multi ? row_h : 0;
    if (avail_w < min_w) avail_w = min_w;

    int n = static_cast<int>(board_buttons_.size());
    int rows = 1;
    while (rows < 4 && n * (min_w + gap) > avail_w * rows) ++rows;
    int per_row = (n + rows - 1) / rows;
    int total = rows * row_h + (rows - 1) * gap;
    for (int i = 0; i < n; ++i) {
        int r = i / per_row;
        int c = i % per_row;
        int in_row = n - r * per_row;
        if (in_row > per_row) in_row = per_row;
        int w = (avail_w - gap * (in_row - 1)) / in_row;
        if (w < min_w) w = min_w;
        int max_w = MulDiv(190, dpi_, 96);
        if (w > max_w) w = max_w;
        MoveWindow(board_buttons_[i], x + c * (w + gap), y + r * (row_h + gap), w, row_h, TRUE);
    }
    return total;
}

int Shell::layout_quick_buttons(int x, int y, int avail_w, bool place) {
    if (quick_channel_.empty() || !quick_label_ || !quick_add_) {
        if (place) {
            ShowWindow(quick_label_, SW_HIDE);
            ShowWindow(quick_add_, SW_HIDE);
            ShowWindow(quick_sep_, SW_HIDE);
            for (HWND h : quick_buttons_) ShowWindow(h, SW_HIDE);
        }
        quick_rows_ = 0;
        quick_height_ = 0;
        return 0;
    }

    int pad = MulDiv(4, dpi_, 96);
    int gap = MulDiv(4, dpi_, 96);
    int row_h = MulDiv(30, dpi_, 96);
    int label_w = MulDiv(132, dpi_, 96);
    int add_w = MulDiv(86, dpi_, 96);
    if (avail_w < MulDiv(400, dpi_, 96)) {
        label_w = 0;
        add_w = MulDiv(76, dpi_, 96);
    }
    int add_x = x + avail_w - add_w;
    int btn_x = x + (label_w > 0 ? label_w + gap : 0);
    int btn_right = add_x - gap;
    int btn_w_max = 0;
    if (btn_right > btn_x) btn_w_max = btn_right - btn_x;

    int text_w = 0;
    int len = GetWindowTextLengthW(quick_label_);
    if (len > 0 && label_w > 0) {
        std::wstring text(static_cast<std::size_t>(len) + 1, L'\0');
        GetWindowTextW(quick_label_, text.data(), len + 1);
        text.resize(static_cast<std::size_t>(len));
        HDC dc = GetDC(hwnd_);
        if (dc) {
            HFONT old = static_cast<HFONT>(SelectObject(dc, font_));
            SIZE sz{};
            if (GetTextExtentPoint32W(dc, text.c_str(), len, &sz)) text_w = sz.cx;
            SelectObject(dc, old);
            ReleaseDC(hwnd_, dc);
        }
    }
    if (label_w > 0) {
        label_w = text_w + MulDiv(16, dpi_, 96);
        if (label_w < MulDiv(110, dpi_, 96)) label_w = MulDiv(110, dpi_, 96);
        if (label_w > MulDiv(180, dpi_, 96)) label_w = MulDiv(180, dpi_, 96);
        add_x = x + avail_w - add_w;
        btn_x = x + label_w + gap;
        btn_right = add_x - gap;
        btn_w_max = btn_right > btn_x ? btn_right - btn_x : 0;
    }

    int row = 0;
    int used = 0;
    int n = static_cast<int>(quick_commands_.size());
    std::vector<int> widths(static_cast<std::size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        int tw = 0;
        HDC dc = GetDC(hwnd_);
        if (dc) {
            std::wstring text = widen(quick_commands_[static_cast<std::size_t>(i)].label);
            HFONT old = static_cast<HFONT>(SelectObject(dc, font_));
            SIZE sz{};
            if (GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz))
                tw = sz.cx;
            SelectObject(dc, old);
            ReleaseDC(hwnd_, dc);
        }
        int w = tw + MulDiv(20, dpi_, 96);
        if (w < MulDiv(64, dpi_, 96)) w = MulDiv(64, dpi_, 96);
        if (w > MulDiv(190, dpi_, 96)) w = MulDiv(190, dpi_, 96);
        if (btn_w_max > 0 && w > btn_w_max) w = btn_w_max;
        widths[static_cast<std::size_t>(i)] = w;
    }
    for (int i = 0; i < n; ++i) {
        int w = widths[static_cast<std::size_t>(i)];
        if (used > 0 && used + w > btn_w_max) {
            ++row;
            used = 0;
        }
        if (place) {
            MoveWindow(quick_buttons_[static_cast<std::size_t>(i)],
                       btn_x + used, y + pad + row * row_h, w, row_h, TRUE);
            ShowWindow(quick_buttons_[static_cast<std::size_t>(i)], SW_SHOWNA);
        }
        used += w + gap;
    }
    quick_rows_ = row + 1;
    quick_height_ = pad * 2 + quick_rows_ * row_h;
    if (place) {
        MoveWindow(quick_sep_, x, y, avail_w, MulDiv(2, dpi_, 96), TRUE);
        ShowWindow(quick_sep_, SW_SHOWNA);
        if (label_w > 0) {
            MoveWindow(quick_label_, x, y + pad, label_w, row_h, TRUE);
            ShowWindow(quick_label_, SW_SHOWNA);
        } else {
            ShowWindow(quick_label_, SW_HIDE);
        }
        MoveWindow(quick_add_, add_x, y + pad, add_w, row_h, TRUE);
        ShowWindow(quick_add_, SW_SHOWNA);
    }
    return quick_height_;
}

void Shell::refresh_quick_buttons() {
    for (HWND h : quick_buttons_) {
        if (h && IsWindow(h)) DestroyWindow(h);
    }
    quick_buttons_.clear();
    quick_commands_.clear();
    quick_channel_ = view_.active_channel();

    if (quick_channel_.empty()) {
        if (quick_label_) ShowWindow(quick_label_, SW_HIDE);
        if (quick_add_) ShowWindow(quick_add_, SW_HIDE);
        if (quick_sep_) ShowWindow(quick_sep_, SW_HIDE);
        return;
    }

    if (core_) quick_commands_ = core_->quick_commands_of(quick_channel_);
    std::wstring label = L"快捷指令 · " + widen(quick_channel_);
    SetWindowTextW(quick_label_, label.c_str());
    ShowWindow(quick_label_, SW_SHOWNA);
    ShowWindow(quick_add_, SW_SHOWNA);

    for (std::size_t i = 0; i < quick_commands_.size(); ++i) {
        const QuickCommandConfig& q = quick_commands_[i];
        HWND h = CreateWindowExW(0, L"BUTTON", widen(q.label).c_str(),
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdcQuickBase + i)),
                                 inst_, nullptr);
        if (!h) {
            quick_buttons_.push_back(nullptr);
            continue;
        }
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SetWindowSubclass(h, QuickBtnProc, 2, static_cast<DWORD_PTR>(i + 1));
        aibridge::register_quick_button(h, i);
        quick_buttons_.push_back(h);
    }
}

void Shell::refresh_board_buttons() {    for (HWND h : board_buttons_) {
        if (h && IsWindow(h)) DestroyWindow(h);
    }
    board_buttons_.clear();
    board_actions_.clear();

    const std::string sel = current_channel();
    const std::vector<BoardActionConfig> all = sel.empty() ? std::vector<BoardActionConfig>{} : core_->buttons_of(sel);
    if (!all.empty()) {
        for (const BoardActionConfig& a : all) {
            if (!a.enable || a.label.empty()) continue;
            board_actions_.push_back(a);
            int id = kIdcBoardBase + static_cast<int>(board_buttons_.size());
            HWND h = CreateWindowExW(0, L"BUTTON", widen(a.label).c_str(),
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 0, 0, hwnd_,
                                     reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst_, nullptr);
            if (!h) {
                board_buttons_.push_back(nullptr);
                continue;
            }
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            board_buttons_.push_back(h);
            aibridge::register_board_button(h, a.id);
        }
    }

    if (board_scope_ && IsWindow(board_scope_)) {
        if (view_.split_count() > 1) {
            std::wstring text = L"板控 · 画框" + std::to_wstring(view_.active_index() + 1) + L" · ";
            if (sel.empty()) {
                text += L"未绑定通道";
            } else {
                text += widen(sel);
                if (board_buttons_.empty()) text += L"（无按钮）";
            }
            SetWindowTextW(board_scope_, text.c_str());
        } else {
            SetWindowTextW(board_scope_, L"");
        }
    }
    refresh_quick_buttons();
    layout();
}

void Shell::on_board_button(std::size_t index) {
    if (index >= board_actions_.size()) return;
    const BoardActionConfig action = board_actions_[index];
    const std::string sel = current_channel();
    if (sel.empty()) {
        note_status(L"当前画框没有绑定通道，无法执行板控按钮");
        return;
    }
    active_button_id_ = widen(action.id);
    active_button_label_ = widen(action.label);
    active_script_line_.clear();
    button_start_ms_ = util::now_ms();
    button_busy_ = true;
    button_last_ok_ = true;
    button_has_run_ = true;
    note_status(L"已触发按钮：" + active_button_id_ +
                (active_button_label_.empty() ? L"" : L"（" + active_button_label_ + L"）"));
    AppCore* core = core_;
    std::shared_ptr<IBridge> bridge = core->find_shared(sel);
    PortInUseFn in_use = [core](const std::string& name) {
        return core->port_in_use(name);
    };
    run_board_action_async(core->config(), sel, action, bridge, in_use, hwnd_,
                           kMsgBoardActionResult, kMsgBoardActionProgress);
}

void Shell::on_quick_button(std::size_t index) {
    const std::string channel = view_.active_channel();
    if (channel.empty()) {
        note_status(L"当前画框没有绑定通道，无法发送快捷指令");
        return;
    }
    if (channel != quick_channel_ || index >= quick_commands_.size()) {
        refresh_quick_buttons();
        layout();
        return;
    }
    IBridge* bridge = core_->find(channel);
    if (!bridge || !bridge->running()) {
        note_status(L"通道未连接，快捷指令未发送：" + widen(channel));
        return;
    }
    const QuickCommandConfig cmd = quick_commands_[index];
    active_script_line_.clear();
    if (!view_.type_text(cmd.command, true)) {
        note_status(L"快捷指令发送失败：" + widen(cmd.label));
        return;
    }
    note_status(L"已发送快捷指令：" + widen(cmd.label));
}

void Shell::show_quick_dialog(std::size_t edit_index, bool editing) {
    // 模态编辑框不能在桥接 /action 的 SendMessage 栈里直接打开，
    // 否则 /action 会等对话框关闭而超时；先投递回主循环再弹。
    PostMessageW(hwnd_, kMsgQuickOpen, editing ? 1 : 0, static_cast<LPARAM>(edit_index));
}

void Shell::open_quick_dialog(std::size_t edit_index, bool editing) {
    const std::string channel = view_.active_channel();
    if (channel.empty()) {
        note_status(L"当前画框没有绑定通道，无法添加快捷指令");
        return;
    }
    BoardConfig board;
    SSHLinkConfig link;
    if (!core_->board_of(channel, &board) && !core_->link_of(channel, &link)) {
        note_status(L"当前通道不在配置中，无法添加快捷指令");
        return;
    }
    const std::vector<QuickCommandConfig> src = core_->quick_commands_of(channel);
    if (editing && edit_index >= src.size()) return;

    QuickCommandConfig value;
    if (editing) value = src[edit_index];
    std::wstring title = editing ? L"编辑快捷指令" : L"添加快捷指令";
    if (!quick_dlg::show(inst_, hwnd_, font_, dpi_, title, &value)) return;

    std::vector<QuickCommandConfig> next = src;
    if (editing) next[edit_index] = value;
    else next.push_back(value);
    std::string err;
    if (!core_->set_quick_commands(channel, next, &err)) {
        note_status(L"快捷指令保存失败：" + widen(err));
        return;
    }
    refresh_quick_buttons();
    layout();
    note_status((editing ? L"已更新快捷指令：" : L"已添加快捷指令：") + widen(value.label));
}

void Shell::delete_quick_button(std::size_t index) {
    const std::string channel = view_.active_channel();
    if (channel.empty() || channel != quick_channel_ || index >= quick_commands_.size()) return;
    std::vector<QuickCommandConfig> next = quick_commands_;
    std::wstring label = widen(next[index].label);
    next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
    std::string err;
    if (!core_->set_quick_commands(channel, next, &err)) {
        note_status(L"快捷指令删除失败：" + widen(err));
        return;
    }
    refresh_quick_buttons();
    layout();
    note_status(L"已删除快捷指令：" + label);
}

LRESULT CALLBACK Shell::QuickBtnProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR id, DWORD_PTR ref) {
    (void)wp;
    (void)id;
    if (msg == WM_RBUTTONUP || msg == WM_CONTEXTMENU) {
        const std::size_t index = static_cast<std::size_t>(ref) - 1;
        HMENU menu = CreatePopupMenu();
        if (menu) {
            AppendMenuW(menu, MF_STRING, kIdmQuickEditBase + static_cast<int>(index), L"编辑此按钮…");
            AppendMenuW(menu, MF_STRING, kIdmQuickDeleteBase + static_cast<int>(index), L"删除此按钮");
            POINT pt{};
            if (msg == WM_CONTEXTMENU && lp != 0 &&
                GET_X_LPARAM(lp) != -1 && GET_Y_LPARAM(lp) != -1) {
                pt.x = GET_X_LPARAM(lp);
                pt.y = GET_Y_LPARAM(lp);
            } else {
                GetCursorPos(&pt);
                RECT br{};
                if (GetWindowRect(hwnd, &br)) {
                    pt.x = br.left + (br.right - br.left) / 2;
                    pt.y = br.bottom;
                }
            }
            HWND owner = GetParent(hwnd);
            SetForegroundWindow(owner);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                     pt.x, pt.y, 0, owner, nullptr);
            DestroyMenu(menu);
            if (cmd) PostMessageW(owner, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
        }
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, QuickBtnProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void Shell::on_left_tab_change() {
    left_page_ = static_cast<int>(SendMessageW(left_tabs_, TCM_GETCURSEL, 0, 0));
    if (left_page_ < 0) left_page_ = 0;
    if (left_page_ == 1) file_browser_.set_channel(current_channel());
    layout();
}

void Shell::toggle_left_panel() {
    left_collapsed_ = !left_collapsed_;
    left_auto_collapsed_ = false;
    left_user_override_ = true;
    if (left_collapsed_) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        // 窄窗里手动收起，变宽时仍按自动策略恢复，不把窗口永久锁成单栏。
        if (rc.right < MulDiv(820, dpi_, 96)) left_auto_collapsed_ = true;
    }
    layout();
    note_status(left_collapsed_ ? L"已收起" : L"已展开");
}

void Shell::show_relay_panel(int page) {
    if (!services_) {
        services_ = new ServiceManager(core_->config().project_root);
        services_->apply_config(core_->config());
        aibridge::set_service_manager(services_);
    }
    if (!relay_panel_) relay_panel_ = new RelayPanel();
    if (!relay_panel_->open(inst_, hwnd_, core_, services_, font_, dpi_, page)) {
        note_status(L"继电器窗口打开失败");
        return;
    }
    aibridge::register_relay_panel(relay_panel_->hwnd(),
                                   GetDlgItem(relay_panel_->hwnd(), relay_ui::kIdTab));
    note_status(page == 1 ? L"已打开板控按钮配置" : L"已打开继电器配置");
}

void Shell::show_file_panel() {
    if (!services_) {
        services_ = new ServiceManager(core_->config().project_root);
        services_->apply_config(core_->config());
        aibridge::set_service_manager(services_);
    }
    if (!file_panel_) file_panel_ = new FileServicePanel();
    if (!file_panel_->open(inst_, hwnd_, core_, services_, font_, dpi_)) {
        note_status(L"文件服务窗口打开失败");
        return;
    }
    aibridge::register_file_service_panel(file_panel_->hwnd());
    note_status(L"已打开文件服务配置");
}

void Shell::show_self() {
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    dump_state();
}

void Shell::hide_self() {
    ShowWindow(hwnd_, SW_HIDE);
    tray_.balloon(L"ssh-bridge v3", L"仍在后台运行，双击托盘图标恢复窗口。");
    dump_state();
}

void Shell::next_hit() {
    if (!search_) return;
    wchar_t buf[128] = L"";
    GetWindowTextW(search_, buf, 128);
    if (buf[0] == L'\0') {
        note_status(L"请先在搜索框输入关键词");
        return;
    }
    int need = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) {
        note_status(L"请先在搜索框输入关键词");
        return;
    }
    std::string utf8(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), need, nullptr, nullptr);
    utf8.resize(static_cast<std::size_t>(need - 1));
    bool ok = view_.find(utf8, true, false);
    note_status(ok ? L"已跳到下一个命中" : L"没有更多命中");
}

void Shell::check_update() {
    wchar_t exePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        note_status(L"无法定位安装目录，升级未启动");
        return;
    }
    std::wstring full(exePath);
    const std::size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        note_status(L"无法定位安装目录，升级未启动");
        return;
    }
    const std::wstring install = full.substr(0, slash);
    const std::wstring updater = install + L"\\tools\\update.ps1";
    const DWORD attr = GetFileAttributesW(updater.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        note_status(L"升级器缺失：请使用带 tools\\update.ps1 的安装包");
        return;
    }

    const std::wstring logDir = install + L"\\work\\logs";
    CreateDirectoryW(logDir.c_str(), nullptr);
    const std::wstring log = logDir + L"\\update-run.log";
    DeleteFileW(log.c_str());
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"& '" +
                       updater + L"' -Manifest '" + kUpdateManifest + L"' *> '" + log + L"'\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, install.c_str(), &si, &pi)) {
        note_status(L"升级器启动失败，错误码 " + std::to_wstring(GetLastError()));
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    update_log_path_ = log;
    update_deadline_ms_ = GetTickCount64() + 60000;
    update_checking_ = true;
    note_status(L"正在检查官方发布仓库；有新版本会自动校验并重启");
    if (about_update_note_ && IsWindow(about_update_note_))
        SetWindowTextW(about_update_note_, L"正在检查官方发布仓库…");
}

void Shell::poll_update_check() {
    if (!update_checking_) return;
    if (GetTickCount64() >= update_deadline_ms_) {
        update_checking_ = false;
        note_status(L"检查更新超时：请查看 work\\logs\\update-run.log");
        if (about_update_note_ && IsWindow(about_update_note_))
            SetWindowTextW(about_update_note_, L"检查更新超时，请查看 update-run.log");
        return;
    }

    HANDLE h = CreateFileW(update_log_path_.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t buf[4096]{};
    DWORD n = 0;
    ReadFile(h, buf, sizeof(buf) - sizeof(wchar_t), &n, nullptr);
    CloseHandle(h);
    const std::wstring text(buf, n / sizeof(wchar_t));
    auto about_note = [this](const std::wstring& s) {
        if (about_update_note_ && IsWindow(about_update_note_))
            SetWindowTextW(about_update_note_, s.c_str());
    };

    const std::size_t fail = text.find(L"UPDATE: FAIL ");
    if (fail != std::wstring::npos) {
        std::size_t end = text.find_first_of(L"\r\n", fail);
        std::wstring detail = text.substr(fail + 13, end == std::wstring::npos
                                                     ? std::wstring::npos : end - fail - 13);
        if (detail.size() > 100) detail = detail.substr(0, 100) + L"…";
        note_status(L"检查更新失败：" + detail);
        about_note(L"检查更新失败：" + detail);
        update_checking_ = false;
        return;
    }
    if (text.find(L"UPDATE: OK already-up-to-date") != std::wstring::npos) {
        note_status(L"已是最新版本：" + installed_version());
        about_note(L"已是最新版本 · " + installed_version());
        update_checking_ = false;
        return;
    }
    if (text.find(L"UPDATE: OK installed=") != std::wstring::npos ||
        text.find(L"UPDATE: OK restarted") != std::wstring::npos) {
        note_status(L"更新已安装，正在重启应用…");
        about_note(L"更新已安装，正在重启应用…");
        update_checking_ = false;
        return;
    }
    if (text.find(L"UPDATE: OK update-available") != std::wstring::npos) {
        if (last_note_.find(L"正在下载") == std::wstring::npos) {
            note_status(L"发现新版本，正在下载并校验…");
            about_note(L"发现新版本，正在下载并校验…");
        }
        update_deadline_ms_ = GetTickCount64() + 300000;   // 下载大包时放宽等待
    }
}

void Shell::show_about() {
    if (about_ && IsWindow(about_)) {
        ShowWindow(about_, SW_SHOW);
        SetForegroundWindow(about_);
        return;
    }
    RECT mr{};
    GetWindowRect(hwnd_, &mr);
    const int aw = MulDiv(460, dpi_, 96);
    const int ah = MulDiv(270, dpi_, 96);
    int ax = mr.left + ((mr.right - mr.left) - aw) / 2;
    int ay = mr.top + ((mr.bottom - mr.top) - ah) / 2;
    const std::wstring version = installed_version();
    const std::wstring aboutTitle = L"关于 · ssh-bridge v3 " + version;
    about_ = CreateWindowExW(WS_EX_TOOLWINDOW, kAboutClass, aboutTitle.c_str(),
                             WS_POPUP | WS_CAPTION | WS_SYSMENU, ax, ay, aw, ah, hwnd_,
                             nullptr, inst_, nullptr);
    if (!about_) return;
    int pad = MulDiv(20, dpi_, 96);
    HWND ver = CreateWindowW(L"STATIC", (L"版本 " + version).c_str(),
                  WS_CHILD | WS_VISIBLE | SS_LEFT, pad, MulDiv(14, dpi_, 96),
                  aw - pad * 2, MulDiv(24, dpi_, 96), about_,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdcAboutVersion)), inst_, nullptr);
    HWND txt = CreateWindowW(L"STATIC",
                  L"ssh-bridge v3 · 原生 Win32 单进程\r\n"
                  L"串口 / SSH / 日志共用一个 ring\r\n\r\n"
                  L"官方发布仓库：\r\n"
                  L"github.com/yanggan2015/ssh-bridge-v3-releases\r\n"
                  L"AI 桥接：http://127.0.0.1:18096",
                  WS_CHILD | WS_VISIBLE | SS_LEFT, pad, MulDiv(44, dpi_, 96),
                  aw - pad * 2, MulDiv(112, dpi_, 96), about_,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdcAboutText)), inst_, nullptr);
    HWND updNote = CreateWindowW(L"STATIC",
                  (update_checking_ && !last_note_.empty()
                       ? last_note_
                       : std::wstring(L"检查到新版本后自动下载、校验 SHA-256 并重启")).c_str(),
                  WS_CHILD | WS_VISIBLE | SS_LEFT, pad, MulDiv(156, dpi_, 96),
                  aw - pad * 2, MulDiv(22, dpi_, 96), about_,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdcAboutUpdateNote)), inst_, nullptr);
    about_update_note_ = updNote;
    const int btnW = MulDiv(120, dpi_, 96);
    const int btnH = MulDiv(32, dpi_, 96);
    const int gap = MulDiv(16, dpi_, 96);
    const int btnY = ah - MulDiv(58, dpi_, 96);
    HWND upd = CreateWindowW(L"BUTTON", L"检查更新", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  (aw - btnW * 2 - gap) / 2, btnY, btnW, btnH, about_,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdcAboutUpdate)), inst_, nullptr);
    HWND ok = CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                  (aw - btnW * 2 - gap) / 2 + btnW + gap, btnY, btnW, btnH, about_,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdcAboutOk)), inst_, nullptr);
    if (font_) {
        SendMessageW(ver, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(txt, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(updNote, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(upd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(ok,  WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    ui_bridge_set_id(ver, "label.about.version");
    ui_bridge_set_id(txt, "label.about.body");
    ui_bridge_set_id(updNote, "label.about.update");
    ui_bridge_set_id(upd, "btn.about.update");
    ShowWindow(about_, SW_SHOW);
    SetForegroundWindow(about_);
    aibridge::register_about(about_);
    dump_state();
}

void Shell::show_channel_dialog(const std::string& edit_name, ChannelKind kind) {
    auto self = this;
    bool ok = dialog_.open(inst_, hwnd_, core_, font_, dpi_, edit_name, kind,
                           [self](const std::string& name, int ms) {
                               self->on_channel_applied(name, ms);
                           });
    if (!ok) {
        note_status(L"通道窗口打开失败");
        return;
    }
    aibridge::register_channel_dialog(dialog_.hwnd());
    if (edit_name.empty()) {
        const wchar_t* label = kind == ChannelKind::Ssh ? L"SSH" :
                               kind == ChannelKind::PowerShell ? L"PowerShell" :
                               kind == ChannelKind::Telnet ? L"Telnet" : L"串口";
        note_status(L"新建 " + std::wstring(label) + L" 通道：填好保存即生效");
    } else {
        note_status(L"编辑通道：" + widen(edit_name));
    }
    dump_state();
}

void Shell::refresh_channels(const std::string& prefer) {
    std::vector<std::string> names;
    for (const AppCore::Entry& e : core_->entries()) names.push_back(e.name);
    view_.prune_channels(names);
    fill_tree();   // 内部会选中第一条并切换终端
    if (!prefer.empty()) {
        const auto& entries = core_->entries();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].name == prefer) {
                select_channel_index(i);
                break;
            }
        }
    }
    update_status();
    if (file_browser_.hwnd()) file_browser_.set_channel(current_channel());
    save_layout_state();
}

void Shell::on_channel_applied(const std::string& name, int ms) {
    bool exists = false;
    for (const AppCore::Entry& e : core_->entries()) {
        if (e.name == name) { exists = true; break; }
    }
    if (exists) {
        refresh_channels(name);
        note_status(L"通道已生效：" + widen(name) + L" · " + std::to_wstring(ms) + L" ms");
    } else {
        // 删除当前通道后优先切到第一条剩余通道，避免留下空终端。
        std::string next;
        for (const AppCore::Entry& e : core_->entries()) {
            if (!e.name.empty()) { next = e.name; break; }
        }
        refresh_channels(next);
        note_status(L"已删除通道：" + widen(name) + L" · " + std::to_wstring(ms) + L" ms");
    }
    dump_state();
}

void Shell::dump_state() {
    util::ensure_dir("output");
    std::string json = core_->state_json(IsWindowVisible(hwnd_) != 0);
    std::FILE* f = std::fopen("output\\ui_state.json", "wb");
    if (!f) return;
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);

    std::string term = view_.state_json();
    std::FILE* tf = std::fopen("output\\ui_terminal.json", "wb");
    if (tf) {
        std::fwrite(term.data(), 1, term.size(), tf);
        std::fclose(tf);
    }
    aibridge::notify_term_changed();
}

void Shell::on_command(int id) {
    if (id >= kIdcQuickBase && id < kIdcQuickBase + 1000) {
        on_quick_button(static_cast<std::size_t>(id - kIdcQuickBase));
        return;
    }
    if (id >= kIdmQuickEditBase && id < kIdmQuickDeleteBase) {
        show_quick_dialog(static_cast<std::size_t>(id - kIdmQuickEditBase), true);
        return;
    }
    if (id >= kIdmQuickDeleteBase && id < kIdmQuickDeleteBase + 1000) {
        delete_quick_button(static_cast<std::size_t>(id - kIdmQuickDeleteBase));
        return;
    }
    switch (id) {
    case kIdcQuickAdd:
        show_quick_dialog(0, false);
        break;
    case kIdmHide:
        hide_self();
        break;
    case kIdmExit:
        DestroyWindow(hwnd_);
        break;
    case kIdmAbout:
        show_about();
        break;
    case kIdmCheckUpdate:
        check_update();
        break;
    case kIdcBtnNew:
    case kIdmChanNewSerial:
        show_channel_dialog(std::string(), ChannelKind::Serial);
        break;
    case kIdmChanNewSsh:
        show_channel_dialog(std::string(), ChannelKind::Ssh);
        break;
    case kIdmChanNewPowershell:
        show_channel_dialog(std::string(), ChannelKind::PowerShell);
        break;
    case kIdmChanNewTelnet:
        show_channel_dialog(std::string(), ChannelKind::Telnet);
        break;
    case kIdmChanEdit:
        if (selected_channel().empty()) note_status(L"编辑：未选通道");
        else show_channel_dialog(selected_channel(), kind_of_entry(core_, selected_channel()));
        break;
    case kIdmSftp: {
        std::string ch = selected_channel();
        if (ch.empty() || !core_->is_ssh_name(ch)) {
            ch.clear();
            for (const AppCore::Entry& e : core_->entries()) {
                if (core_->is_ssh_name(e.name)) { ch = e.name; break; }
            }
        }
        if (ch.empty()) { note_status(L"SFTP：没有可用的 SSH 通道，请先新建"); break; }
        if (!sftp_.open(inst_, hwnd_, core_, font_, dpi_, ch)) {
            note_status(L"SFTP 面板打开失败");
            break;
        }
        aibridge::register_sftp(sftp_.hwnd(), &sftp_);
        note_status(L"SFTP：已打开 " + widen(ch));
        dump_state();
        break;
    }
    case kIdmRelayConfig:
        show_relay_panel(0);
        dump_state();
        break;
    case kIdmBoardButtons:
        // 顶级菜单“板控按钮”应直接进入按钮脚本页，而不是板卡绑定页
        show_relay_panel(2);
        dump_state();
        break;
    case kIdmFileService:
        show_file_panel();
        dump_state();
        break;
    case kIdmChanDelete:
        if (selected_channel().empty()) note_status(L"删除：未选通道");
        else show_channel_dialog(selected_channel(), kind_of_entry(core_, selected_channel()));
        break;
    case kIdmChanReload: {
        std::uint64_t t0 = util::now_ms();
        AppCore::ReloadResult r = core_->reload_config();
        int ms = static_cast<int>(util::now_ms() - t0);
        if (!r.ok()) {
            note_status(L"重载失败：" + widen(r.error));
            break;
        }
        refresh_channels(selected_channel());
        services_->apply_config(core_->config());
        sync_inbound();
        sync_web();
        sync_s12();
        note_status(L"已重载配置（新增 " + std::to_wstring(r.added) + L" / 删除 " +
                    std::to_wstring(r.removed) + L" / 变更 " + std::to_wstring(r.changed) +
                    L"）· " + std::to_wstring(ms) + L" ms");
        break;
    }
    case kIdmTrayShow:
        show_self();
        break;
    case kIdmTrayHide:
        hide_self();
        break;
    case kIdmTrayExit:
        DestroyWindow(hwnd_);
        break;
    case kIdmRefresh:
    case kIdcBtnRefresh:
        view_.pull_visible();
        if (left_page_ == 1 && file_browser_.hwnd()) file_browser_.refresh();
        note_status(L"已刷新");
        break;
    case kIdmFocusSearch:
        if (search_) {
            SetFocus(search_);
            SendMessageW(search_, EM_SETSEL, 0, -1);
            note_status(L"已跳到搜索框");
        }
        break;
    case kIdmFindNext:
        next_hit();
        break;
    case kIdmReconnect:
    case kIdcBtnReconnect: {
        std::string name = current_channel();
        if (!name.empty()) core_->reconnect(name);
        note_status(name.empty() ? L"重连：当前画框未绑定通道" : L"已重连 " + widen(name));
        break;
    }
    case kIdmDisconnect:
    case kIdcBtnDisconnect: {
        std::string name = current_channel();
        if (name.empty()) {
            note_status(L"断开：当前画框未绑定通道");
        } else if (core_->disconnect(name)) {
            note_status(L"已断开 " + widen(name));
        } else {
            note_status(L"断开失败：" + widen(name));
        }
        break;
    }
    case kIdmStatusBar: {
        statusbar_visible_ = !statusbar_visible_;
        core_->save_statusbar_visible(statusbar_visible_);
        HMENU menu = GetMenu(hwnd_);
        if (menu) {
            HMENU view = GetSubMenu(menu, 1);
            if (view) CheckMenuItem(view, kIdmStatusBar, MF_BYCOMMAND |
                                    (statusbar_visible_ ? MF_CHECKED : MF_UNCHECKED));
        }
        layout();
        note_status(statusbar_visible_ ? L"已显示状态栏" : L"已隐藏状态栏");
        break;
    }
    case kIdcBtnClear:
    case kIdmClearView:
        view_.clear_active();
        note_status(L"已清屏");
        break;
    case kIdcTreeToggle:
        toggle_left_panel();
        break;
    case kIdmSplit1:
        view_.split(1);
        save_layout_state();
        note_status(L"已切换 1 窗格");
        break;
    case kIdmSplit2:
        view_.split(2);
        save_layout_state();
        note_status(L"已切换 2 窗格");
        break;
    case kIdmSplit4:
        view_.split(4);
        save_layout_state();
        note_status(L"已切换 4 窗格");
        break;
    case kIdmCopy:
        if (!view_.has_selection()) {
            note_status(L"请先用鼠标选择要复制的文本");
        } else if (view_.copy_selection()) {
            note_status(L"已复制选区");
        } else {
            note_status(L"复制失败：剪贴板暂不可用");
        }
        break;
    case kIdmPaste:
        if (view_.paste_clipboard()) {
            note_status(L"已粘贴到终端");
        } else {
            note_status(L"粘贴失败：剪贴板为空或当前通道不可用");
        }
        break;
    case kIdmReplay:
        view_.replay_active();
        note_status(L"已加载历史会话");
        break;
    default:
        break;
    }
}

void Shell::show_tray_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kIdmTrayShow, L"显示窗口");
    AppendMenuW(menu, MF_STRING, kIdmTrayHide, L"隐藏窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdmTrayExit, L"退出");
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    int cmd = static_cast<int>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                              pt.x, pt.y, 0, hwnd_, nullptr));
    DestroyMenu(menu);
    if (cmd == kIdmTrayShow) PostMessageW(hwnd_, kMsgTrayShow, 0, 0);
    if (cmd == kIdmTrayHide) PostMessageW(hwnd_, kMsgTrayHide, 0, 0);
    if (cmd == kIdmTrayExit) PostMessageW(hwnd_, kMsgTrayExit, 0, 0);
}

bool Shell::select_channel_index(std::size_t idx) {
    HTREEITEM item = nullptr;
    for (HTREEITEM h : channel_items_) {
        TVITEMW tv{};
        tv.mask = TVIF_PARAM | TVIF_HANDLE;
        tv.hItem = h;
        if (SendMessageW(tree_, TVM_GETITEM, 0, reinterpret_cast<LPARAM>(&tv)) &&
            static_cast<std::size_t>(tv.lParam) == idx) {
            item = h;
            break;
        }
    }
    if (!item) return false;
    SendMessageW(tree_, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(item));
    std::string name = selected_channel();
    if (!name.empty()) view_.set_channel(name);
    refresh_board_buttons();
    update_status();
    save_layout_state();
    return true;
}

bool Shell::sync_tree_to_channel(const std::string& name) {
    if (name.empty() || !tree_) return false;
    const auto& entries = core_->entries();
    HTREEITEM item = nullptr;
    for (HTREEITEM h : channel_items_) {
        TVITEMW tv{};
        tv.mask = TVIF_PARAM | TVIF_HANDLE;
        tv.hItem = h;
        if (!SendMessageW(tree_, TVM_GETITEM, 0, reinterpret_cast<LPARAM>(&tv))) continue;
        std::size_t idx = static_cast<std::size_t>(tv.lParam);
        if (idx < entries.size() && entries[idx].name == name) {
            item = h;
            break;
        }
    }
    if (!item) return false;
    if (TreeView_GetSelection(tree_) == item) return true;
    sync_tree_binding_ = true;
    SendMessageW(tree_, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(item));
    sync_tree_binding_ = false;
    return true;
}

void Shell::on_active_pane_changed() {
    std::string name = view_.active_channel();
    if (!name.empty()) {
        sync_tree_to_channel(name);
    } else {
        sync_tree_binding_ = true;
        SendMessageW(tree_, TVM_SELECTITEM, TVGN_CARET, 0);
        sync_tree_binding_ = false;
    }
    refresh_board_buttons();
    update_status();
    if (file_browser_.hwnd()) file_browser_.set_channel(name);
    save_layout_state();
}

void Shell::show_tree_context_menu(HTREEITEM item) {
    int idx = -1;
    if (item) {
        TVITEMW tv{};
        tv.mask = TVIF_PARAM | TVIF_HANDLE;
        tv.hItem = item;
        if (SendMessageW(tree_, TVM_GETITEM, 0, reinterpret_cast<LPARAM>(&tv))) {
            idx = static_cast<int>(tv.lParam);
        }
    }
    const auto entries = core_->entries();
    const bool is_group = idx < 0 || static_cast<std::size_t>(idx) >= entries.size();
    const AppCore::Entry* entry = is_group ? nullptr : &entries[static_cast<std::size_t>(idx)];
    const bool online = entry && entry->bridge && entry->bridge->running();
    const bool is_ssh = entry && core_->is_ssh_name(entry->name);

    HMENU menu = CreatePopupMenu();
    if (!is_group) {
        AppendMenuW(menu, MF_STRING, kIdmReconnect, online ? L"重连" : L"打开");
        AppendMenuW(menu, MF_STRING | (online ? 0u : MF_GRAYED), kIdmDisconnect, L"断开");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kIdmChanEdit, L"编辑配置");
        AppendMenuW(menu, MF_STRING, kIdmChanDelete, L"删除通道");
        if (is_ssh) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kIdmSftp, L"打开 SFTP");
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, kIdmChanNewSerial, L"新建串口通道");
    AppendMenuW(menu, MF_STRING, kIdmChanNewSsh, L"新建 SSH 通道");
    AppendMenuW(menu, MF_STRING, kIdmChanNewPowershell, L"新建 PowerShell 通道");
    AppendMenuW(menu, MF_STRING, kIdmChanNewTelnet, L"新建 Telnet 通道");
    AppendMenuW(menu, MF_STRING, kIdmChanReload, L"重载配置");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    int cmd = static_cast<int>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                              pt.x, pt.y, 0, hwnd_, nullptr));
    DestroyMenu(menu);
    if (cmd) on_command(cmd);
}

void Shell::on_tree_change() {
    if (sync_tree_binding_) {
        sync_tree_binding_ = false;
        return;
    }
    std::string name = selected_channel();
    if (!name.empty()) view_.set_channel(name);
    refresh_board_buttons();
    update_status();
    if (file_browser_.hwnd()) file_browser_.set_channel(name);
    save_layout_state();
}

bool Shell::channel_exists(const std::string& name) const {
    if (name.empty()) return false;
    for (const AppCore::Entry& e : core_->entries()) {
        if (e.name == name) return true;
    }
    return false;
}

void Shell::restore_layout_from_config() {
    const UiConfig& ui = core_->config().ui;
    if (ui.pane_channels.empty()) return;
    std::vector<std::string> channels = ui.pane_channels;
    for (std::string& name : channels) {
        if (!channel_exists(name)) name.clear();
    }
    view_.restore_layout(channels, ui.active_pane);
}

void Shell::save_layout_state() {
    if (!layout_ready_ || !layout_user_input_ || !core_) return;
    core_->save_ui_layout(view_.split_count(), view_.slot_channels(), view_.active_index());
}

void Shell::find_typed() {
    if (!search_) return;
    wchar_t buf[128] = L"";
    GetWindowTextW(search_, buf, 128);
    if (buf[0] == L'\0') {
        view_.clear_search();
        return;
    }
    int need = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) {
        view_.clear_search();
        return;
    }
    std::string utf8(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), need, nullptr, nullptr);
    utf8.resize(static_cast<std::size_t>(need - 1));
    // 逐字输入时每次都是新搜索，不能在上一处命中之后继续找
    bool ok = view_.find(utf8, true, true);
    note_status(ok ? L"搜索命中：" + std::wstring(buf) : L"搜索无命中：" + std::wstring(buf));
}

void Shell::on_tray_event(LPARAM lp) {
    switch (LOWORD(lp)) {
    case WM_LBUTTONDBLCLK:
        show_self();
        break;
    case WM_RBUTTONUP:
        show_tray_menu();
        break;
    default:
        break;
    }
}

LRESULT CALLBACK Shell::SearchEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                       UINT_PTR id, DWORD_PTR ref) {
    Shell* self = reinterpret_cast<Shell*>(ref);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (self) self->find_typed();
        return 0;
    }
    if (msg == WM_CHAR && wp == L'\r') return 0;  // 回车已在 KEYDOWN 处理，避免提示音
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, SearchEditProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Shell::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    if (aibridge::handle_message(hwnd, msg, wp, lp)) return 0;
    Shell* self = reinterpret_cast<Shell*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    self->hwnd_ = hwnd;
    return self->handle(msg, wp, lp);
}

LRESULT Shell::handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LBUTTONDOWN || msg == WM_KEYDOWN || msg == WM_COMMAND) {
        layout_user_input_ = true;
    }
    switch (msg) {
    case kMsgWebReload:
        // 网页配置页保存后，由 Web 线程 PostMessage 回来，在 UI 线程做手术式重载
        on_command(kIdmChanReload);
        return 0;
    case WM_CREATE:

        build_menu();
        build_children();
        {
            ACCEL a[6] = {
                {static_cast<BYTE>(FCONTROL | FVIRTKEY), 'N', kIdmChanNewSerial},
                {FVIRTKEY, static_cast<WORD>(VK_F5), kIdmRefresh},
                {FVIRTKEY, static_cast<WORD>(VK_F3), kIdmFindNext},
                {static_cast<BYTE>(FCONTROL | FVIRTKEY), 'F', kIdmFocusSearch},
                {FVIRTKEY, static_cast<WORD>(VK_F7), kIdmSftp},
                {static_cast<BYTE>(FCONTROL | FVIRTKEY), 'B', kIdcTreeToggle},
            };
            accel_ = CreateAcceleratorTableW(a, 6);
        }
        SetTimer(hwnd_, 1, 1000, nullptr);
        // 首帧布局/状态栏文本必须等窗口真正有尺寸后再做：
        // WM_CREATE 时客户区还是 0，此时给状态栏发 SB_SETPARTS 会卡死。
        PostMessageW(hwnd_, kMsgRefresh, 0, 0);
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    case WM_GETMINMAXINFO: {
        // 最小窗口：低于这个尺寸终端会被工具栏/状态栏挤没
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = MulDiv(480, dpi_, 96);
        mmi->ptMinTrackSize.y = MulDiv(480, dpi_, 96);
        return 0;
    }

    case WM_TIMER:
        core_->poll_channel_retry();              // 托盘隐藏时也守护串口/PowerShell，避免掉线后永不恢复
        if (!IsWindowVisible(hwnd_)) return 0;   // 隐藏时不刷状态、不拉数据
        poll_update_check();
        view_.pull_visible();
        update_status();
        return 0;

    case WM_SETFOCUS:
        SetFocus(tree_);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == kIdcSearch && HIWORD(wp) == EN_CHANGE) {
            find_typed();
            return 0;
        }
        {
            int id = LOWORD(wp);
            if (id >= kIdcBoardBase && id < kIdcBoardBase + 200) {
                on_board_button(static_cast<std::size_t>(id - kIdcBoardBase));
                return 0;
            }
        }
        on_command(LOWORD(wp));
        return 0;

    case WM_NOTIFY: {
        LPNMHDR hdr = reinterpret_cast<LPNMHDR>(lp);
        if (hdr && hdr->idFrom == kIdcTree && hdr->code == TVN_SELCHANGEDW) {
            on_tree_change();
        }
        if (hdr && hdr->idFrom == kIdcTree && hdr->code == NM_RCLICK) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(tree_, &pt);
            TVHITTESTINFO ht{};
            ht.pt = pt;
            HTREEITEM item = reinterpret_cast<HTREEITEM>(
                SendMessageW(tree_, TVM_HITTEST, 0, reinterpret_cast<LPARAM>(&ht)));
            if (item) {
                SendMessageW(tree_, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(item));
                show_tree_context_menu(item);
            }
        }
        if (hdr && hdr->idFrom == kIdcLeftTabs && hdr->code == TCN_SELCHANGE) {
            on_left_tab_change();
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        // 快捷键提示做成次要信息（灰字），不和搜索/按钮抢视觉
        HDC hdc = reinterpret_cast<HDC>(wp);
        HWND who = reinterpret_cast<HWND>(lp);
        if (who == keyhint_) {
            SetTextColor(hdc, RGB(128, 128, 128));
            SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        if (who == board_scope_) {
            SetTextColor(hdc, RGB(0x28, 0x5F, 0x8F));
            SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        break;
    }

    case kMsgSelect:
        select_channel_index(static_cast<std::size_t>(wp));
        return 0;

    case kMsgActivePaneChanged:
        on_active_pane_changed();
        return 0;

    case kMsgActivatePane:
        layout_user_input_ = true;
        if (view_.set_active(static_cast<int>(wp))) {
            on_active_pane_changed();
            dump_state();
        }
        return 0;

    case kMsgPaneLayoutChanged:
        // 桥接切分屏/激活画框没有鼠标输入，需要显式允许把布局落盘。
        layout_user_input_ = true;
        on_active_pane_changed();
        return 0;

    case kMsgTermData:
        view_.pull_visible();
        if (core_) core_->clear_pending(static_cast<std::size_t>(wp));
        return 0;

    case kMsgQuickOpen:
        open_quick_dialog(static_cast<std::size_t>(lp), wp != 0);
        return 0;

    case WM_CLOSE:
        // ui.close_to_tray=false 时关闭即退出；托盘退出仍会走 stop_all。
        if (core_ && !core_->config().ui.close_to_tray) {
            DestroyWindow(hwnd_);
            return 0;
        }
        hide_self();
        return 0;

    case kMsgTrayShow:
        show_self();
        return 0;

    case kMsgBoardButtonsChanged:
        refresh_board_buttons();
        update_status();
        return 0;

    case kMsgBoardActionProgress: {
        AsyncBoardProgress* p = reinterpret_cast<AsyncBoardProgress*>(lp);
        if (p) {
            std::wstring line = widen(p->line);
            if (line.size() > 512) line.resize(512);
            active_script_line_ = line;
            if (!button_busy_ && !p->action_id.empty()) active_button_id_ = widen(p->action_id);
            update_status();
            delete p;
        }
        return 0;
    }

    case kMsgBoardActionResult: {
        AsyncBoardResult* r = reinterpret_cast<AsyncBoardResult*>(lp);
        if (r) {
            button_busy_ = false;
            button_last_ok_ = r->ok;
            button_has_run_ = true;
            if (!r->action_id.empty()) active_button_id_ = widen(r->action_id);
            if (!r->action_label.empty()) active_button_label_ = widen(r->action_label);
            // Keep the last printed script line visible after the run finishes.
            if (r->relay && services_) {
                services_->relay_note_state(r->relay_port, r->relay_channel, r->relay_on,
                                            r->relay_mode, r->ok ? std::string() : r->message);
            }
            std::wstring text = (r->ok ? L"✓ " : L"⚠ ") + widen(r->action_label) + L"：" +
                                widen(r->message.empty() ? std::string("完成") : r->message);
            if (!r->output.empty()) {
                text += L" · 输出 " + std::to_wstring(r->output.size()) + L" 字节";
            }
            note_status(text);
            delete r;
        }
        return 0;
    }

    case kMsgTrayHide:
        hide_self();
        return 0;

    case kMsgTrayExit:
        DestroyWindow(hwnd_);
        return 0;

    case kMsgRefresh:
        layout();
        update_status();
        return 0;

    case kMsgStateDump:
        dump_state();
        return 0;

    case kMsgTrayEvent:
        on_tray_event(lp);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd_, 1);
        sftp_.close();
        dialog_.close();
        if (relay_panel_) relay_panel_->close();
        if (file_panel_) file_panel_->close();
        aibridge::stop();
        save_window_rect(hwnd_, core_);
        tray_.destroy();
        if (core_) core_->stop_all();
        util::ensure_dir("output");
        {
            std::string json = core_ ? core_->state_json(false) : std::string("{}");
            std::FILE* f = std::fopen("output\\ui_state.json", "wb");
            if (f) {
                std::fwrite(json.data(), 1, json.size(), f);
                std::fclose(f);
            }
        }
        if (font_) DeleteObject(font_);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace sb3
