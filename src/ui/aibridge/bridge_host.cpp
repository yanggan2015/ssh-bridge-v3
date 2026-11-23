// bridge_host.cpp — 把本体窗口接到 win_ui_bridge：控件 id、菜单/终端虚拟动作、
// 业务状态快照。HTTP 线程只投递消息，所有控件操作与状态生成都在 UI 线程完成。

#include "ui/aibridge/bridge_host.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <string>

#include <cjson/cJSON.h>

#include "app/app_core.h"
#include "third_party/win_ui_bridge.h"
#include "ui/dialogs/channel_dialog.h"
#include "ui/services/file_service_panel.h"
#include "ui/services/relay_panel.h"
#include "ui/services/service_panel.h"
#include "ui/sftp/sftp_window.h"
#include "ui/terminal/input.h"
#include "ui/terminal/view.h"
#include "ui/ui_messages.h"
#include "services/service_manager.h"
#include "services/web/web_console.h"

namespace sb3 {
namespace aibridge {
namespace {

HWND                  g_main  = nullptr;
AppCore*              g_core  = nullptr;
TerminalView*         g_view  = nullptr;
std::string           g_last_search;
HWND                  g_dlg   = nullptr;
HWND                  g_sftp_hwnd = nullptr;
SftpWindow*           g_sftp  = nullptr;
ServiceManager*       g_services = nullptr;
WebConsole*           g_web = nullptr;

// 本体控件 id（S3 定下的资源号，桥接不改它们）
constexpr int kIdTree     = 1000;
constexpr int kIdStatus   = 1001;
constexpr int kIdRefresh  = 1010;
constexpr int kIdReconn   = 1011;
constexpr int kIdClear    = 1012;
constexpr int kIdSearch   = 1013;
constexpr int kIdBtnNew   = 1017;

// 关于窗口（非模态；模态 MessageBox 会让桥接在 8s 内 504，等于 AI 点不动）
const wchar_t kAboutClass[] = L"SB3_AboutBox";
constexpr int kIdcAboutText = 2001;
constexpr int kIdcAboutOk   = 2002;
constexpr int kIdcAboutUpdate = 2004;
constexpr int kIdcAboutUpdateNote = 2005;

int num_of(const cJSON* o, const char* key, int def) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(it) ? it->valueint : def;
}

// ---------- 菜单命令：虚拟控件 click → WM_COMMAND ----------
void menu_click(void* user) {
    int cmd = static_cast<int>(reinterpret_cast<intptr_t>(user));
    // 虚拟点击可能来自 HTTP 线程；PostMessage 只投递、不阻塞桥接线程。
    if (g_main) PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

void add_menu(const char* id, const char* name, int cmd) {
    RECT rc{};
    ui_bridge_add_virtual(id, "custom", name, rc, TRUE, name, menu_click,
                          reinterpret_cast<void*>(static_cast<intptr_t>(cmd)));
}

// ---------- 终端虚拟动作 ----------
enum TermOp { kOpClear, kOpCopy, kOpPaste, kOpReload, kOpFocus };

int term_active_value(void*);

void term_click(void* user) {
    if (!g_view) return;
    switch (static_cast<TermOp>(reinterpret_cast<intptr_t>(user))) {
    case kOpClear:  g_view->clear_active();   break;
    case kOpCopy:   g_view->copy_selection(); break;
    case kOpPaste:  g_view->paste_clipboard(); break;
    case kOpReload: g_view->replay_active();  break;
    case kOpFocus:  g_view->focus_active();   break;
    }
}

void term_split_click(void* user) {
    if (!g_view) return;
    g_view->split(static_cast<int>(reinterpret_cast<intptr_t>(user)));
    if (g_main) PostMessageW(g_main, kMsgPaneLayoutChanged, 0, 0);
    ui_bridge_set_virtual_slider("term.pane", 0, std::max(0, g_view->split_count() - 1),
                                 term_active_value, nullptr);
}

int term_active_value(void*) { return g_view ? g_view->active_index() : 0; }

void term_pane_action(const char* action, const char*, int value, char* result, int cap, void*) {
    int cur = 0;
    if (g_view) {
        if (std::strcmp(action, "set_value") == 0) {
            if (g_view->set_active(value) && g_main) PostMessageW(g_main, kMsgPaneLayoutChanged, 0, 0);
        }
        cur = g_view->active_index();
        ui_bridge_set_virtual_slider("term.pane", 0, std::max(0, g_view->split_count() - 1),
                                     term_active_value, nullptr);
    }
    std::snprintf(result, static_cast<std::size_t>(cap), "{\"active\":%d}", cur);
}

void term_input_common(const char* action, const char* text, char* result, int cap,
                       bool submit) {
    if (!g_view) {
        std::snprintf(result, static_cast<std::size_t>(cap), "{\"sent\":0}");
        return;
    }
    if (std::strcmp(action, "set_text") == 0) {
        bool ok = g_view->type_text(text ? text : "", submit);
        std::snprintf(result, static_cast<std::size_t>(cap), "{\"sent\":%d}", ok ? 1 : 0);
        return;
    }
    if (std::strcmp(action, "clear") == 0) {
        std::snprintf(result, static_cast<std::size_t>(cap), "{\"sent\":0}");
        return;
    }
    std::snprintf(result, static_cast<std::size_t>(cap), "{\"text\":\"\"}");
}

void term_input_action(const char* action, const char* text, int, char* result, int cap,
                       void*) {
    term_input_common(action, text, result, cap, true);
}

void term_send_action(const char* action, const char* text, int, char* result, int cap,
                      void*) {
    if (!g_view) {
        std::snprintf(result, static_cast<std::size_t>(cap), "{\"sent\":0}");
        return;
    }
    if (std::strcmp(action, "set_text") == 0) {
        bool ok = g_view->send_raw(text ? text : "");
        std::snprintf(result, static_cast<std::size_t>(cap), "{\"sent\":%d}", ok ? 1 : 0);
        return;
    }
    if (std::strcmp(action, "clear") == 0) {
        std::snprintf(result, static_cast<std::size_t>(cap), "{\"sent\":0}");
        return;
    }
    std::snprintf(result, static_cast<std::size_t>(cap), "{\"text\":\"\"}");
}

void term_paste_text_action(const char* action, const char* text, int, char* result, int cap,
                            void*) {
    if (g_view && std::strcmp(action, "set_text") == 0) {
        bool ok = g_view->paste_text(text ? text : "");
        std::snprintf(result, static_cast<std::size_t>(cap), "{\"pasted\":%d}", ok ? 1 : 0);
        return;
    }
    std::snprintf(result, static_cast<std::size_t>(cap), "{\"pasted\":0}");
}

void term_mouse_action(const char* action, const char* text, int, char* result, int cap,
                       void*) {
    if (!g_view || std::strcmp(action, "set_text") != 0) {
        std::snprintf(result, static_cast<std::size_t>(cap), "{\"sent\":0}");
        return;
    }
    int col = 0, row = 0, button = 0, release = 0, motion = 0;
    unsigned mods = 0;
    int n = std::sscanf(text ? text : "", "%d,%d,%d,%d,%d,%u", &col, &row, &button,
                        &release, &motion, &mods);
    if (n < 3) {
        std::snprintf(result, static_cast<std::size_t>(cap),
                      "{\"sent\":0,\"error\":\"格式 col,row,button[,release[,motion[,mods]]]\"}");
        return;
    }
    bool ok = g_view->mouse_event(col, row, button, release != 0, motion != 0, mods);
    std::snprintf(result, static_cast<std::size_t>(cap), "{\"sent\":%d}", ok ? 1 : 0);
}

void term_search_action(const char* action, const char* text, int, char* result, int cap,
                        void*) {
    if (!g_view) return;
    if (std::strcmp(action, "set_text") == 0) {
        g_last_search = text ? text : "";
        bool found = g_last_search.empty() ? (g_view->clear_search(), false)
                                          : g_view->find(g_last_search, true, true);
        std::snprintf(result, static_cast<std::size_t>(cap), "{\"found\":%d}", found ? 1 : 0);
        return;
    }
    if (std::strcmp(action, "clear") == 0) {
        g_last_search.clear();
        g_view->clear_search();
        return;
    }
    std::snprintf(result, static_cast<std::size_t>(cap), "{\"text\":\"%s\"}",
                  g_last_search.c_str());
}

void sync_scroll_range();

void term_scroll_action(const char* action, const char*, int value, char* result, int cap,
                        void*) {
    if (!g_view) return;
    if (std::strcmp(action, "set_value") == 0) g_view->scroll_active_to(value);
    sync_scroll_range();
    std::snprintf(result, static_cast<std::size_t>(cap), "{\"value\":%d}",
                  g_view->active_offset());
}

int term_scroll_value(void*) { return g_view ? g_view->active_offset() : 0; }

// 回看上限随内容变化，滑条的 max 必须跟着走，否则 AI 会以为有 1000 行可滚
void sync_scroll_range() {
    if (!g_view) return;
    ui_bridge_set_virtual_slider("term.scroll", 0, g_view->active_max_scroll(),
                                 term_scroll_value, nullptr);
}

void add_term_virtuals() {
    if (!g_view) return;
    RECT rc{};
    rc.right = 10;
    rc.bottom = 10;
    ui_bridge_add_virtual_ex("term.input", "textbox", "向活动窗格输入并回车", rc, TRUE, "",
                             nullptr, term_input_action, nullptr);
    ui_bridge_add_virtual_ex("term.send", "textbox", "向活动窗格发送原始按键", rc, TRUE, "",
                             nullptr, term_send_action, nullptr);
    ui_bridge_add_virtual_ex("term.paste_text", "textbox", "向活动窗格粘贴文本（bracketed 自动包装）",
                             rc, TRUE, "", nullptr, term_paste_text_action, nullptr);
    ui_bridge_add_virtual_ex("term.mouse", "textbox",
                             "鼠标事件：列,行,按键[,释放[,移动[,修饰]]]（应用开启上报时生效）",
                             rc, TRUE, "", nullptr, term_mouse_action, nullptr);
    ui_bridge_add_virtual_ex("term.search", "textbox", "在回看里搜索", rc, TRUE, "",
                             nullptr, term_search_action, nullptr);
    ui_bridge_add_virtual_ex("term.scroll", "slider", "回看滚动（值=距底部行数）", rc, TRUE, "",
                             nullptr, term_scroll_action, nullptr);
    // 虚拟滑条默认读不到 value：补 min/max + 当前 offset 回调
    sync_scroll_range();

    ui_bridge_add_virtual("term.clear", "custom", "清空活动窗格", rc, TRUE, "",
                          term_click, reinterpret_cast<void*>(static_cast<intptr_t>(kOpClear)));
    ui_bridge_add_virtual("term.copy", "custom", "复制选区", rc, TRUE, "",
                          term_click, reinterpret_cast<void*>(static_cast<intptr_t>(kOpCopy)));
    ui_bridge_add_virtual("term.paste", "custom", "粘贴", rc, TRUE, "",
                          term_click, reinterpret_cast<void*>(static_cast<intptr_t>(kOpPaste)));
    ui_bridge_add_virtual("term.reload", "custom", "加载历史会话", rc, TRUE, "",
                          term_click, reinterpret_cast<void*>(static_cast<intptr_t>(kOpReload)));
    ui_bridge_add_virtual("term.focus", "custom", "聚焦终端", rc, TRUE, "",
                          term_click, reinterpret_cast<void*>(static_cast<intptr_t>(kOpFocus)));
    ui_bridge_add_virtual("term.split1", "custom", "单窗格", rc, TRUE, "",
                          term_split_click, reinterpret_cast<void*>(static_cast<intptr_t>(1)));
    ui_bridge_add_virtual("term.split2", "custom", "左右两窗格", rc, TRUE, "",
                          term_split_click, reinterpret_cast<void*>(static_cast<intptr_t>(2)));
    ui_bridge_add_virtual("term.split4", "custom", "四窗格", rc, TRUE, "",
                          term_split_click, reinterpret_cast<void*>(static_cast<intptr_t>(4)));
    ui_bridge_add_virtual_ex("term.pane", "slider", "活动窗格（值=序号）", rc, TRUE, "",
                             nullptr, term_pane_action, nullptr);
    ui_bridge_set_virtual_slider("term.pane", 0, std::max(0, g_view->split_count() - 1),
                                 term_active_value, nullptr);
}

// ---------- 通道管理虚拟动作：新开表单 / 编辑 / 删除 / 重载 ----------
void add_channel_virtuals() {
    RECT rc{};
    rc.right = 10;
    rc.bottom = 10;
    struct Item { const char* id; const char* name; int cmd; };
    const Item items[] = {
        {"chan.newSerial",    "新建串口通道（Ctrl+N）", 40040},
        {"chan.newSsh",       "新建 SSH 通道",          40041},
        {"chan.newPowershell","新建 PowerShell 通道",   40055},
        {"chan.newTelnet",    "新建 Telnet 通道",       40056},
        {"chan.editCurrent",  "编辑当前通道",           40042},
        {"chan.deleteCurrent","删除当前通道（两步确认）", 40043},
        {"chan.reload",       "重新加载配置文件",       40044},
    };
    for (const Item& it : items) {
        ui_bridge_add_virtual(it.id, "custom", it.name, rc, TRUE, it.name, menu_click,
                              reinterpret_cast<void*>(static_cast<intptr_t>(it.cmd)));
    }
}

// ---------- /state（UI 线程生成，HTTP 线程通过 SendMessage 取） ----------
std::string g_state_cache;

std::string dlg_text(int id) {
    HWND h = g_dlg ? GetDlgItem(g_dlg, id) : nullptr;
    if (!h) return std::string();
    WCHAR buf[512] = L"";
    GetWindowTextW(h, buf, 512);
    return term::utf16_to_utf8(std::wstring(buf));
}

std::string dlg_combo(int id) {
    HWND h = g_dlg ? GetDlgItem(g_dlg, id) : nullptr;
    if (!h) return std::string();
    int idx = static_cast<int>(SendMessageW(h, CB_GETCURSEL, 0, 0));
    if (idx < 0) {
        // 可编辑下拉（CBS_DROPDOWN）：用户/AI 直接填的自定义值没有选中项，读编辑区文字
        WCHAR typed[256] = L"";
        GetWindowTextW(h, typed, 256);
        return term::utf16_to_utf8(std::wstring(typed));
    }
    WCHAR buf[128] = L"";
    SendMessageW(h, CB_GETLBTEXT, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(buf));
    return term::utf16_to_utf8(std::wstring(buf));
}

bool dlg_checked(int id) {
    HWND h = g_dlg ? GetDlgItem(g_dlg, id) : nullptr;
    return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void add_dialog_block(cJSON* ui) {
    bool open = g_dlg && IsWindow(g_dlg);
    cJSON_AddBoolToObject(ui, "channel_dialog_open", open);
    if (!open) return;
    cJSON* d = cJSON_AddObjectToObject(ui, "dialog");
    WCHAR title[64] = L"";
    GetWindowTextW(g_dlg, title, 64);
    cJSON_AddStringToObject(d, "title", term::utf16_to_utf8(std::wstring(title)).c_str());
    cJSON_AddStringToObject(d, "kind", dlg_combo(3002).c_str());
    cJSON_AddStringToObject(d, "name", dlg_text(3001).c_str());
    cJSON_AddStringToObject(d, "device", dlg_text(3010).c_str());
    cJSON_AddNumberToObject(d, "baud", atoi(dlg_text(3011).c_str()));
    cJSON_AddStringToObject(d, "parity", dlg_combo(3013).c_str());
    cJSON_AddStringToObject(d, "stopbits", dlg_combo(3014).c_str());
    cJSON_AddStringToObject(d, "flow", dlg_combo(3015).c_str());
    cJSON_AddStringToObject(d, "line_ending", dlg_combo(3016).c_str());
    cJSON_AddStringToObject(d, "encoding", dlg_combo(3017).c_str());
    cJSON_AddBoolToObject(d, "auto_open", dlg_checked(3018));
    cJSON_AddBoolToObject(d, "dtr", dlg_checked(3019));
    cJSON_AddBoolToObject(d, "rts", dlg_checked(3020));
    cJSON_AddBoolToObject(d, "log_enabled", dlg_checked(3021));
    cJSON_AddStringToObject(d, "host", dlg_text(3030).c_str());
    cJSON_AddNumberToObject(d, "port", atoi(dlg_text(3031).c_str()));
    cJSON_AddStringToObject(d, "user", dlg_text(3032).c_str());
    cJSON_AddNumberToObject(d, "password_len",
                            static_cast<int>(dlg_text(3033).size()));
    cJSON_AddStringToObject(d, "term", dlg_text(3034).c_str());
    cJSON_AddNumberToObject(d, "timeout_ms", atoi(dlg_text(3035).c_str()));
    cJSON_AddNumberToObject(d, "cols", atoi(dlg_text(3036).c_str()));
    cJSON_AddNumberToObject(d, "rows", atoi(dlg_text(3037).c_str()));
    cJSON_AddNumberToObject(d, "keepalive_s", atoi(dlg_text(3038).c_str()));
    cJSON_AddStringToObject(d, "hint", dlg_text(3050).c_str());
}

void add_terminal_block(cJSON* root) {
    if (!g_view) return;
    cJSON* t = cJSON_Parse(g_view->state_json().c_str());
    if (!cJSON_IsObject(t)) {
        cJSON_Delete(t);
        return;
    }
    cJSON* out = cJSON_AddObjectToObject(root, "terminal");
    cJSON_AddNumberToObject(out, "split", num_of(t, "split", 1));
    cJSON_AddNumberToObject(out, "active", num_of(t, "active", 0));
    cJSON_AddNumberToObject(out, "font_px", num_of(t, "font_px", 15));

    const cJSON* panes = cJSON_GetObjectItemCaseSensitive(t, "panes");
    cJSON* arr = cJSON_AddArrayToObject(out, "panes");
    const cJSON* p = nullptr;
    int pane_index = 0;
    cJSON_ArrayForEach(p, panes) {
        if (!cJSON_IsObject(p)) continue;
        cJSON* o = cJSON_CreateObject();
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(p, "channel");
        cJSON_AddStringToObject(o, "channel", cJSON_IsString(name) ? name->valuestring : "");
        const cJSON* hint = cJSON_GetObjectItemCaseSensitive(p, "empty_hint");
        if (cJSON_IsString(hint)) cJSON_AddStringToObject(o, "empty_hint", hint->valuestring);
        cJSON_AddNumberToObject(o, "cols", num_of(p, "cols", 0));
        cJSON_AddNumberToObject(o, "rows", num_of(p, "rows", 0));
        cJSON_AddNumberToObject(o, "mouse_mode", num_of(p, "mouse_mode", 0));
        const cJSON* msgr = cJSON_GetObjectItemCaseSensitive(p, "mouse_sgr");
        cJSON_AddBoolToObject(o, "mouse_sgr", cJSON_IsTrue(msgr) ? 1 : 0);
        cJSON_AddNumberToObject(o, "total", num_of(p, "total", 0));
        cJSON_AddNumberToObject(o, "offset", num_of(p, "offset", 0));
        cJSON_AddNumberToObject(o, "bytes", num_of(p, "bytes", 0));
        const cJSON* sh = cJSON_GetObjectItemCaseSensitive(p, "search_hit");
        cJSON_AddBoolToObject(o, "search_hit", cJSON_IsTrue(sh) ? 1 : 0);
        cJSON_AddBoolToObject(o, "active", pane_index == num_of(t, "active", 0) ? 1 : 0);

        // 尾部 2 行：让 Agent 不截图也能读到终端最后输出
        const cJSON* lines = cJSON_GetObjectItemCaseSensitive(p, "lines");
        cJSON* tail = cJSON_AddArrayToObject(o, "tail");
        int n = cJSON_GetArraySize(lines);
        for (int i = n - 1; i >= 0 && cJSON_GetArraySize(tail) < 2; --i) {
            const cJSON* ln = cJSON_GetArrayItem(lines, i);
            if (cJSON_IsString(ln) && ln->valuestring[0] != '\0')
                cJSON_AddItemToArray(tail, cJSON_CreateString(ln->valuestring));
        }
        cJSON_AddItemToArray(arr, o);
        ++pane_index;
    }
    cJSON_Delete(t);
}

void state_provider(char* out, int cap, void*) {
    g_state_cache.clear();
    if (g_main) {
        // 不能用无限 SendMessage：关窗时 UI 线程正在 ui_bridge_stop 等 HTTP 线程退出，
        // 两边互相等待会死锁。这里最多等 1 秒，超时返回空状态。
        DWORD_PTR result = 0;
        SendMessageTimeoutW(g_main, kMsgBridgeState, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result);
    }
    std::snprintf(out, static_cast<std::size_t>(cap), "%s", g_state_cache.c_str());
}

}  // namespace

void notify_term_changed() { sync_scroll_range(); }

std::string build_state_json() {
    std::string out = "{}";
    if (!g_core) return out;
    cJSON* root = cJSON_Parse(g_core->state_json(IsWindowVisible(g_main) != FALSE).c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    cJSON_AddNumberToObject(root, "ts", static_cast<double>(GetTickCount()));
    add_terminal_block(root);
    {
        cJSON* quick = cJSON_AddObjectToObject(root, "quick");
        const std::string channel = g_view ? g_view->active_channel() : std::string();
        cJSON_AddStringToObject(quick, "channel", channel.c_str());
        cJSON* arr = cJSON_AddArrayToObject(quick, "buttons");
        if (g_core && !channel.empty()) {
            const std::vector<QuickCommandConfig> cmds = g_core->quick_commands_of(channel);
            for (const QuickCommandConfig& q : cmds) {
                cJSON* b = cJSON_CreateObject();
                cJSON_AddStringToObject(b, "label", q.label.c_str());
                cJSON_AddStringToObject(b, "command", q.command.c_str());
                cJSON_AddItemToArray(arr, b);
            }
        }
    }

    WCHAR w[128] = L"";
    if (HWND edit = g_main ? GetDlgItem(g_main, kIdSearch) : nullptr)
        GetWindowTextW(edit, w, 128);
    cJSON* ui = cJSON_AddObjectToObject(root, "ui");
    cJSON_AddStringToObject(ui, "search_text", term::utf16_to_utf8(std::wstring(w)).c_str());
    cJSON_AddBoolToObject(root, "about_open", FindWindowW(kAboutClass, nullptr) != nullptr);
    cJSON_AddBoolToObject(root, "tree_visible",
                          g_main ? IsWindowVisible(GetDlgItem(g_main, kIdTree)) != FALSE : FALSE);
    cJSON_AddBoolToObject(ui, "left_visible",
                          g_main ? IsWindowVisible(GetDlgItem(g_main, 1019)) != FALSE : FALSE);
    cJSON_AddBoolToObject(ui, "left_collapsed",
                          g_main ? IsWindowVisible(GetDlgItem(g_main, 1019)) == FALSE : FALSE);
    cJSON_AddNumberToObject(ui, "left_page",
                            g_main ? static_cast<double>(SendMessageW(GetDlgItem(g_main, 1019),
                                      TCM_GETCURSEL, 0, 0)) : 0.0);
    cJSON_AddBoolToObject(ui, "relay_open", FindWindowW(relay_ui::kClass, nullptr) != nullptr);
    cJSON_AddBoolToObject(ui, "filesvc_open", FindWindowW(filesvc_ui::kClass, nullptr) != nullptr);
    add_dialog_block(ui);
    {
        cJSON* sf = nullptr;
        if (g_sftp) sf = cJSON_Parse(g_sftp->state_json().c_str());
        cJSON_AddBoolToObject(ui, "sftp_open", g_sftp && g_sftp->is_open());
        if (cJSON_IsObject(sf)) cJSON_AddItemToObject(ui, "sftp", sf);
        else cJSON_Delete(sf);
    }
    if (g_services) {
        cJSON* svc = cJSON_Parse(g_services->state_json().c_str());
        if (cJSON_IsObject(svc)) cJSON_AddItemToObject(root, "services", svc);
        else cJSON_Delete(svc);
    }
    if (g_web) {
        cJSON* web = cJSON_Parse(g_web->status_json().c_str());
        if (cJSON_IsObject(web)) cJSON_AddItemToObject(root, "web", web);
        else cJSON_Delete(web);
    }

    // 状态栏四段原文：验收要判断“动作有没有回执”，必须能从 /state 读到
    cJSON* bar = cJSON_AddObjectToObject(root, "statusbar");
    const char* keys[4] = {"summary", "channel", "terminal", "note"};
    HWND status = g_main ? GetDlgItem(g_main, kIdStatus) : nullptr;
    for (int i = 0; i < 4; ++i) {
        WCHAR buf[512] = L"";
        if (status) {
            int len = static_cast<int>(SendMessageW(status, SB_GETTEXTLENGTHW, i, 0));
            if (len > 0 && len < 500) {
                SendMessageW(status, SB_GETTEXTW, i, reinterpret_cast<LPARAM>(buf));
            }
        }
        cJSON_AddStringToObject(bar, keys[i], term::utf16_to_utf8(std::wstring(buf)).c_str());
    }

    char* text = cJSON_PrintUnformatted(root);
    if (text) out = text;
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

bool start(HWND main, AppCore* core, TerminalView* view, const Options& opt) {
    if (!opt.enabled || !main) return false;
    g_main = main;
    g_core = core;
    g_view = view;
    if (!opt.token.empty()) ui_bridge_set_token(opt.token.c_str());

    // 真实控件：稳定 id（不允许 auto/*）
    struct Pair { int id; const char* name; };
    const Pair pairs[] = {
        {kIdTree, "tree.channels"}, {kIdStatus, "label.status"},
        {kIdRefresh, "btn.refresh"}, {kIdReconn, "btn.reconnect"}, {kIdClear, "btn.clear"},
        {1018, "btn.disconnect"},
        {kIdSearch, "edit.search"}, {kIdBtnNew, "btn.newChannel"},
        {1015, "label.searchHint"}, {1016, "label.keyHint"},
        {1019, "tab.left"},
    };
    for (const Pair& p : pairs) {
        HWND h = GetDlgItem(main, p.id);
        if (h) ui_bridge_set_id(h, p.name);
    }
    if (view && view->hwnd()) ui_bridge_set_id(view->hwnd(), "term.view");

    add_menu("menu.file.exit", "退出", 40001);
    add_menu("menu.help.about", "关于", 40002);
    add_menu("menu.help.checkUpdate", "检查更新", 40060);
    add_menu("menu.file.newSerial", "新建串口通道", 40040);
    add_menu("menu.file.newSsh", "新建 SSH 通道", 40041);
    add_menu("menu.file.newPowershell", "新建 PowerShell 通道", 40055);
    add_menu("menu.file.newTelnet", "新建 Telnet 通道", 40056);
    add_menu("menu.file.editChannel", "编辑当前通道", 40042);
    add_menu("menu.file.deleteChannel", "删除当前通道", 40043);
    add_menu("menu.file.reloadConfig", "重新加载配置", 40044);
    add_menu("menu.file.sftp", "SFTP 文件传输", 40045);
    add_menu("menu.relay.config", "继电器配置", 40050);
    add_menu("menu.fileService.config", "文件服务配置", 40051);
    add_menu("menu.boardButtons.config", "板控按钮配置", 40052);
    add_menu("menu.view.hide", "隐藏窗口", 40010);
    add_menu("menu.view.refresh", "刷新", 40011);
    add_menu("menu.view.reconnect", "重连", 40012);
    add_menu("menu.view.disconnect", "断开当前通道", 40047);
    add_menu("menu.view.split1", "单窗格", 40030);
    add_menu("menu.view.split2", "左右两窗格", 40031);
    add_menu("menu.view.split4", "四窗格", 40032);
    add_menu("menu.view.copy", "复制选区", 40033);
    add_menu("menu.view.paste", "粘贴", 40034);
    add_menu("menu.view.reload", "加载历史会话", 40035);
    add_menu("menu.view.clear", "清屏", 40036);
    add_menu("menu.view.findnext", "下一个命中", 40038);
    add_menu("menu.view.focussearch", "跳到搜索框", 40039);
    add_menu("menu.tray.show", "显示窗口", 40020);
    add_menu("menu.tray.hide", "隐藏到托盘", 40021);
    add_menu("menu.tray.exit", "托盘退出", 40022);
    add_term_virtuals();
    add_channel_virtuals();

    ui_bridge_set_state_provider(state_provider, nullptr);
    if (!ui_bridge_start(main, opt.port)) {
        ui_bridge_stop();
        return false;
    }
    return true;
}

void register_about(HWND box) {
    if (!box) return;
    // WS_POPUP 的 hwndParent 是 owner 不是父窗，EnumChildWindows(main) 收不到；
    // 用 attach_window 把这个附属窗口纳入 /ui-tree 与动作定位。
    ui_bridge_attach_window(box);
    if (HWND t = GetDlgItem(box, kIdcAboutText)) ui_bridge_set_id(t, "about.text");
    if (HWND b = GetDlgItem(box, kIdcAboutOk)) ui_bridge_set_id(b, "btn.about.ok");
    if (HWND u = GetDlgItem(box, kIdcAboutUpdate)) ui_bridge_set_id(u, "btn.about.update");
    if (HWND n = GetDlgItem(box, kIdcAboutUpdateNote)) ui_bridge_set_id(n, "label.about.update");
}

void register_channel_dialog(HWND dlg) {
    if (!dlg) return;
    if (g_dlg && g_dlg != dlg && IsWindow(g_dlg)) ui_bridge_detach_window(g_dlg);
    g_dlg = dlg;
    ui_bridge_attach_window(dlg);

    struct Pair { int id; const char* name; };
    const Pair pairs[] = {
        {chan_dlg::kIdNameLabel, "dlg.label.name"},
        {chan_dlg::kIdKindLabel, "dlg.label.kind"},
        {chan_dlg::kIdDeviceLabel, "dlg.label.device"},
        {chan_dlg::kIdRefresh, "dlg.channel.refresh"},
        {chan_dlg::kIdBaudLabel, "dlg.label.baud"},
        {chan_dlg::kIdDataLabel, "dlg.label.databits"},
        {chan_dlg::kIdParityLabel, "dlg.label.parity"},
        {chan_dlg::kIdHostLabel, "dlg.label.host"},
        {chan_dlg::kIdPortLabel, "dlg.label.port"},
        {chan_dlg::kIdUserLabel, "dlg.label.user"},
        {chan_dlg::kIdPassLabel, "dlg.label.password"},
        {chan_dlg::kIdTermLabel, "dlg.label.term"},
        {chan_dlg::kIdTimeoutLabel, "dlg.label.timeout"},
        {chan_dlg::kIdWidthLabel, "dlg.label.cols"},
        {chan_dlg::kIdHeightLabel, "dlg.label.rows"},
        {chan_dlg::kIdKeepLabel, "dlg.label.keepalive"},
        {chan_dlg::kIdStopLabel, "dlg.label.stopbits"},
        {chan_dlg::kIdFlowLabel, "dlg.label.flow"},
        {chan_dlg::kIdLineLabel, "dlg.label.lineending"},
        {chan_dlg::kIdEncLabel, "dlg.label.encoding"},
        {chan_dlg::kIdShellLabel, "dlg.label.shell"},
        {chan_dlg::kIdArgsLabel, "dlg.label.args"},
        {chan_dlg::kIdCwdLabel, "dlg.label.cwd"},
        {chan_dlg::kIdName, "dlg.channel.name"},
        {chan_dlg::kIdKind, "dlg.channel.kind"},
        {chan_dlg::kIdDevice, "dlg.channel.device"},
        {chan_dlg::kIdBaud, "dlg.channel.baud"},
        {chan_dlg::kIdDatabits, "dlg.channel.databits"},
        {chan_dlg::kIdParity, "dlg.channel.parity"},
        {chan_dlg::kIdStopbits, "dlg.channel.stopbits"},
        {chan_dlg::kIdFlow, "dlg.channel.flow"},
        {chan_dlg::kIdLineEnd, "dlg.channel.lineending"},
        {chan_dlg::kIdEncoding, "dlg.channel.encoding"},
        {chan_dlg::kIdAutoOpen, "dlg.channel.autoopen"},
        {chan_dlg::kIdDtr, "dlg.channel.dtr"},
        {chan_dlg::kIdRts, "dlg.channel.rts"},
        {chan_dlg::kIdLog, "dlg.channel.log_enabled"},
        {chan_dlg::kIdHost, "dlg.channel.host"},
        {chan_dlg::kIdPort, "dlg.channel.port"},
        {chan_dlg::kIdUser, "dlg.channel.user"},
        {chan_dlg::kIdPassword, "dlg.channel.password"},
        {chan_dlg::kIdTerm, "dlg.channel.term"},
        {chan_dlg::kIdTimeout, "dlg.channel.timeout"},
        {chan_dlg::kIdWidth, "dlg.channel.cols"},
        {chan_dlg::kIdHeight, "dlg.channel.rows"},
        {chan_dlg::kIdKeepalive, "dlg.channel.keepalive"},
        {chan_dlg::kIdShell, "dlg.channel.shell"},
        {chan_dlg::kIdArgs, "dlg.channel.args"},
        {chan_dlg::kIdCwd, "dlg.channel.cwd"},
        {chan_dlg::kIdSerialBox, "dlg.group.serial"},
        {chan_dlg::kIdSshBox, "dlg.group.ssh"},
        {chan_dlg::kIdPsBox, "dlg.group.powershell"},
        {chan_dlg::kIdHint, "dlg.channel.hint"},
        {chan_dlg::kIdSave, "dlg.channel.save"},
        {chan_dlg::kIdCancel, "dlg.channel.cancel"},
        {chan_dlg::kIdDelete, "dlg.channel.delete"},
        {chan_dlg::kIdTest, "dlg.channel.test"},
    };
    for (const Pair& p : pairs) {
        if (HWND h = GetDlgItem(dlg, p.id)) ui_bridge_set_id(h, p.name);
    }
}

void register_sftp(HWND wnd, SftpWindow* panel) {
    if (!wnd) return;
    g_sftp_hwnd = wnd;
    g_sftp = panel;
    ui_bridge_attach_window(wnd);
    struct Pair { int id; const char* name; };
    const Pair pairs[] = {
        {sftp_ui::kIdChannel,       "sftp.channel"},
        {sftp_ui::kIdConnect,       "sftp.connect"},
        {sftp_ui::kIdLocalPath,     "sftp.local.path"},
        {sftp_ui::kIdLocalList,     "sftp.local.list"},
        {sftp_ui::kIdLocalRefresh,  "sftp.local.refresh"},
        {sftp_ui::kIdLocalUp,       "sftp.local.up"},
        {sftp_ui::kIdRemotePath,    "sftp.remote.path"},
        {sftp_ui::kIdRemoteList,    "sftp.remote.list"},
        {sftp_ui::kIdRemoteRefresh, "sftp.remote.refresh"},
        {sftp_ui::kIdRemoteUp,      "sftp.remote.up"},
        {sftp_ui::kIdUpload,        "sftp.upload"},
        {sftp_ui::kIdDownload,      "sftp.download"},
        {sftp_ui::kIdCancel,        "sftp.cancel"},
        {sftp_ui::kIdRetry,         "sftp.retry"},
        {sftp_ui::kIdClose,         "sftp.close"},
        {sftp_ui::kIdProgress,      "sftp.progress"},
        {sftp_ui::kIdStatus,        "sftp.status"},
        {sftp_ui::kIdQueue,         "sftp.queue"},
        {sftp_ui::kIdLocalLabel,    "sftp.label.local"},
        {sftp_ui::kIdRemoteLabel,   "sftp.label.remote"},
        {sftp_ui::kIdChannelLabel,  "sftp.label.channel"},
        {sftp_ui::kIdNameLabel,     "sftp.label.name"},
        {sftp_ui::kIdName,          "sftp.name"},
        {sftp_ui::kIdMkdir,         "sftp.mkdir"},
        {sftp_ui::kIdRename,        "sftp.rename"},
        {sftp_ui::kIdDelete,        "sftp.delete"},
        {sftp_ui::kIdLocalInfo,     "sftp.local.info"},
        {sftp_ui::kIdRemoteInfo,    "sftp.remote.info"},
        {sftp_ui::kIdHistoryOpen,   "sftp.history.open"},
        {sftp_ui::kIdSettingsOpen,  "sftp.settings.open"},
    };
    for (const Pair& p : pairs) {
        if (HWND h = GetDlgItem(wnd, p.id)) ui_bridge_set_id(h, p.name);
    }
}

void register_sftp_history(HWND wnd) {
    if (!wnd) return;
    ui_bridge_attach_window(wnd);
    struct Pair { int id; const char* name; };
    const Pair pairs[] = {
        {sftp_ui::kIdHistSummary, "sftp.history.summary"},
        {sftp_ui::kIdHistList,    "sftp.history.list"},
        {sftp_ui::kIdHistDetail,  "sftp.history.detail"},
        {sftp_ui::kIdHistClear,   "sftp.history.clear"},
        {sftp_ui::kIdHistClose,   "sftp.history.close"},
    };
    for (const Pair& p : pairs) {
        if (HWND h = GetDlgItem(wnd, p.id)) ui_bridge_set_id(h, p.name);
    }
}

void register_sftp_settings(HWND wnd) {
    if (!wnd) return;
    ui_bridge_attach_window(wnd);
    struct Pair { int id; const char* name; };
    const Pair pairs[] = {
        {sftp_ui::kIdSetSpeed, "sftp.settings.speed"},
        {sftp_ui::kIdSetNote,  "sftp.settings.note"},
        {sftp_ui::kIdSetSave,  "sftp.settings.save"},
        {sftp_ui::kIdSetClose, "sftp.settings.close"},
        {sftp_ui::kIdSetLabel, "sftp.settings.label.speed"},
    };
    for (const Pair& p : pairs) {
        if (HWND h = GetDlgItem(wnd, p.id)) ui_bridge_set_id(h, p.name);
    }
}

void unregister_sftp_dialog(HWND wnd) {
    if (wnd) ui_bridge_detach_window(wnd);
}

void set_service_manager(ServiceManager* mgr) { g_services = mgr; }
void set_web_console(WebConsole* web) { g_web = web; }

void register_service_panel(HWND wnd) {
    if (!wnd) return;
    ui_bridge_attach_window(wnd);
    struct Pair { int id; const char* name; };
    const Pair pairs[] = {
        {svc_ui::kIdTitle,             "svc.title"},
        {svc_ui::kIdGroupRelay,        "svc.group.relay"},
        {svc_ui::kIdGroupFiles,        "svc.group.files"},
        {svc_ui::kIdRelayDevice,       "svc.relay.device"},
        {svc_ui::kIdRelayPort,         "svc.relay.port"},
        {svc_ui::kIdRelayBaud,         "svc.relay.baud"},
        {svc_ui::kIdRelayRefresh,      "svc.relay.refresh"},
        {svc_ui::kIdRelayEnable,       "svc.relay.enable"},
        {svc_ui::kIdRelaySave,         "svc.relay.save"},
        {svc_ui::kIdRelayStatus,       "svc.relay.status"},
        {svc_ui::kIdRelayCh1,          "svc.relay.ch1"},
        {svc_ui::kIdRelayCh2,          "svc.relay.ch2"},
        {svc_ui::kIdRelayPulse1,       "svc.relay.pulse1"},
        {svc_ui::kIdRelayPulse2,       "svc.relay.pulse2"},
        {svc_ui::kIdRelayHold,         "svc.relay.hold"},
        {svc_ui::kIdRelayInv,          "svc.relay.inverted"},
        {svc_ui::kIdRelayInfo,         "svc.relay.info"},
        {svc_ui::kIdRoot,              "svc.root"},
        {svc_ui::kIdPickRoot,          "svc.root.pick"},
        {svc_ui::kIdLayout,            "svc.root.layout"},
        {svc_ui::kIdTftpIp,            "svc.tftp.ip"},
        {svc_ui::kIdTftpPort,          "svc.tftp.port"},
        {svc_ui::kIdTftpWrite,         "svc.tftp.write"},
        {svc_ui::kIdTftpBtn,           "svc.tftp.toggle"},
        {svc_ui::kIdTftpStatus,        "svc.tftp.status"},
        {svc_ui::kIdSharePort,         "svc.share.port"},
        {svc_ui::kIdShareBtn,          "svc.share.toggle"},
        {svc_ui::kIdShareOpen,         "svc.share.open"},
        {svc_ui::kIdShareStatus,       "svc.share.status"},
        {svc_ui::kIdNfsAlias,          "svc.nfs.alias"},
        {svc_ui::kIdNfsUid,            "svc.nfs.uid"},
        {svc_ui::kIdNfsGid,            "svc.nfs.gid"},
        {svc_ui::kIdNfsBtn,            "svc.nfs.toggle"},
        {svc_ui::kIdNfsStatus,         "svc.nfs.status"},
        {svc_ui::kIdSave,              "svc.save"},
        {svc_ui::kIdStopAll,           "svc.stopall"},
        {svc_ui::kIdClose,             "svc.close"},
        {svc_ui::kIdMsg,               "svc.message"},
        {svc_ui::kIdLabelRelayDevice,  "svc.label.device"},
        {svc_ui::kIdLabelRelayPort,    "svc.label.port"},
        {svc_ui::kIdLabelBaud,         "svc.label.baud"},
        {svc_ui::kIdLabelHold,         "svc.label.hold"},
        {svc_ui::kIdLabelRoot,         "svc.label.root"},
        {svc_ui::kIdLabelTftp,         "svc.label.tftp"},
        {svc_ui::kIdLabelShare,        "svc.label.share"},
        {svc_ui::kIdLabelNfs,         "svc.label.nfs"},
        {svc_ui::kIdLabelTftpPort,     "svc.label.tftp.port"},
        {svc_ui::kIdLabelSharePort,     "svc.label.share.port"},
        {svc_ui::kIdLabelUid,          "svc.label.uid"},
        {svc_ui::kIdLabelGid,          "svc.label.gid"},
    };
    for (const Pair& p : pairs) {
        if (HWND h = GetDlgItem(wnd, p.id)) ui_bridge_set_id(h, p.name);
    }
}

void register_relay_panel(HWND wnd, HWND tabs) {
    if (!wnd) return;
    ui_bridge_attach_window(wnd);
    struct Pair { int id; const char* name; };
    const Pair pairs[] = {
        {relay_ui::kIdTitle, "relay.title"},
        {relay_ui::kIdTab, "relay.tabs"},
        {relay_ui::kIdMsg, "relay.message"},
        {relay_ui::kIdClose, "relay.close"},
        {relay_ui::kIdGroupDev, "relay.group.dev"},
        {relay_ui::kIdGroupStates, "relay.group.states"},
        {relay_ui::kIdGroupBind, "relay.group.bind"},
        {relay_ui::kIdGroupScript, "relay.group.script"},
        {relay_ui::kIdGroupBindTarget, "relay.group.bindTarget"},
        {relay_ui::kIdGroupScriptTarget, "relay.group.scriptTarget"},

        {relay_ui::kIdDevName, "relay.dev.name"},
        {relay_ui::kIdDevPort, "relay.dev.port"},
        {relay_ui::kIdDevLinux, "relay.dev.linux"},
        {relay_ui::kIdDevBaud, "relay.dev.baud"},
        {relay_ui::kIdDevChannels, "relay.dev.channels"},
        {relay_ui::kIdDevDelay, "relay.dev.delay"},
        {relay_ui::kIdDevIo, "relay.dev.inverted"},
        {relay_ui::kIdDevPolarity, "relay.dev.polarity"},
        {relay_ui::kIdDevEnable, "relay.dev.enable"},
        {relay_ui::kIdDevTestCh, "relay.dev.testChannel"},
        {relay_ui::kIdDevOn, "relay.dev.on"},
        {relay_ui::kIdDevOff, "relay.dev.off"},
        {relay_ui::kIdDevPulseHold, "relay.dev.pulseHold"},
        {relay_ui::kIdDevPulse, "relay.dev.pulse"},
        {relay_ui::kIdDevStatus, "relay.dev.status"},
        {relay_ui::kIdDevInfo, "relay.dev.info"},
        {relay_ui::kIdDevNew, "relay.dev.new"},
        {relay_ui::kIdDevSave, "relay.dev.save"},
        {relay_ui::kIdDevDelete, "relay.dev.delete"},
        {relay_ui::kIdDevRefresh, "relay.dev.refresh"},
        {relay_ui::kIdDevList, "relay.dev.list"},
        {relay_ui::kIdDevStates, "relay.dev.states"},

        {relay_ui::kIdBindTarget, "relay.bind.target"},
        {relay_ui::kIdBindList, "relay.bind.list"},
        {relay_ui::kIdBindNew, "relay.bind.new"},
        {relay_ui::kIdBindSave, "relay.bind.save"},
        {relay_ui::kIdBindDelete, "relay.bind.delete"},
        {relay_ui::kIdBindTest, "relay.bind.test"},
        {relay_ui::kIdBindId, "relay.bind.id"},
        {relay_ui::kIdBindLabel, "relay.bind.label"},
        {relay_ui::kIdBindRelay, "relay.bind.relay"},
        {relay_ui::kIdBindChannel, "relay.bind.channel"},
        {relay_ui::kIdBindMode, "relay.bind.mode"},
        {relay_ui::kIdBindHold, "relay.bind.hold"},
        {relay_ui::kIdBindPolarity, "relay.bind.polarity"},
        {relay_ui::kIdBindEnable, "relay.bind.enable"},
        {relay_ui::kIdBindPresetOn, "relay.bind.presetOn"},
        {relay_ui::kIdBindPresetOff, "relay.bind.presetOff"},
        {relay_ui::kIdBindPresetReboot, "relay.bind.presetReboot"},
        {relay_ui::kIdBindGenerate, "relay.bind.generate"},
        {relay_ui::kIdBindGenerateUpgrade, "relay.bind.generateUpgrade"},
        {relay_ui::kIdBindInfo, "relay.bind.info"},
        {relay_ui::kIdBindTargetInfo, "relay.bind.targetInfo"},

        {relay_ui::kIdScriptTarget, "relay.script.target"},
        {relay_ui::kIdScriptList, "relay.script.list"},
        {relay_ui::kIdScriptNew, "relay.script.new"},
        {relay_ui::kIdScriptSave, "relay.script.save"},
        {relay_ui::kIdScriptDelete, "relay.script.delete"},
        {relay_ui::kIdScriptTest, "relay.script.test"},
        {relay_ui::kIdScriptId, "relay.script.id"},
        {relay_ui::kIdScriptLabel, "relay.script.label"},
        {relay_ui::kIdScriptKind, "relay.script.kind"},
        {relay_ui::kIdScriptBody, "relay.script.body"},
        {relay_ui::kIdScriptDelay, "relay.script.delay"},
        {relay_ui::kIdScriptEnable, "relay.script.enable"},
        {relay_ui::kIdScriptPresetLogin, "relay.script.presetLogin"},
        {relay_ui::kIdScriptPresetUboot, "relay.script.presetUboot"},
        {relay_ui::kIdScriptPresetUpgrade, "relay.script.presetUpgrade"},
        {relay_ui::kIdScriptPresetUpgradeMode, "relay.script.presetUpgradeMode"},
        {relay_ui::kIdScriptGenerateUpgrade, "relay.script.generateUpgrade"},
        {relay_ui::kIdScriptInfo, "relay.script.info"},

        {relay_ui::kIdLabelDevName, "relay.label.devName"},
        {relay_ui::kIdLabelDevPort, "relay.label.devPort"},
        {relay_ui::kIdLabelDevLinux, "relay.label.devLinux"},
        {relay_ui::kIdLabelDevBaud, "relay.label.devBaud"},
        {relay_ui::kIdLabelDevChannels, "relay.label.devChannels"},
        {relay_ui::kIdLabelDevDelay, "relay.label.devDelay"},
        {relay_ui::kIdLabelBindTarget, "relay.label.bindTarget"},
        {relay_ui::kIdLabelBindList, "relay.label.bindList"},
        {relay_ui::kIdLabelBindId, "relay.label.bindId"},
        {relay_ui::kIdLabelBindLabel, "relay.label.bindLabel"},
        {relay_ui::kIdLabelBindRelay, "relay.label.bindRelay"},
        {relay_ui::kIdLabelBindChannel, "relay.label.bindChannel"},
        {relay_ui::kIdLabelBindMode, "relay.label.bindMode"},
        {relay_ui::kIdLabelBindHold, "relay.label.bindHold"},
        {relay_ui::kIdLabelScriptTarget, "relay.label.scriptTarget"},
        {relay_ui::kIdLabelScriptList, "relay.label.scriptList"},
        {relay_ui::kIdLabelScriptId, "relay.label.scriptId"},
        {relay_ui::kIdLabelScriptLabel, "relay.label.scriptLabel"},
        {relay_ui::kIdLabelScriptKind, "relay.label.scriptKind"},
        {relay_ui::kIdLabelScriptDelay, "relay.label.scriptDelay"},
        {relay_ui::kIdLabelScriptBody, "relay.label.scriptBody"},
        {relay_ui::kIdLabelDevTestCh, "relay.label.devTestCh"},
        {relay_ui::kIdLabelDevPulseHold, "relay.label.devPulseHold"},
        {relay_ui::kIdLabelDevList, "relay.label.devList"},
        {relay_ui::kIdLabelDevStates, "relay.label.devStates"},
        {relay_ui::kIdLabelDevHint, "relay.label.devHint"},
    };
    for (const Pair& p : pairs) {
        if (HWND h = GetDlgItem(wnd, p.id)) ui_bridge_set_id(h, p.name);
    }
    if (tabs) ui_bridge_set_id(tabs, "relay.tabs");
}

void register_file_service_panel(HWND wnd) {
    if (!wnd) return;
    ui_bridge_attach_window(wnd);
    struct Pair { int id; const char* name; };
    const Pair pairs[] = {
        {filesvc_ui::kIdTitle, "filesvc.title"},
        {filesvc_ui::kIdGroupRoot, "filesvc.group.root"},
        {filesvc_ui::kIdRoot, "filesvc.root"},
        {filesvc_ui::kIdPickRoot, "filesvc.root.pick"},
        {filesvc_ui::kIdLayout, "filesvc.root.layout"},
        {filesvc_ui::kIdGroupTftp, "filesvc.group.tftp"},
        {filesvc_ui::kIdTftpIp, "filesvc.tftp.ip"},
        {filesvc_ui::kIdTftpPort, "filesvc.tftp.port"},
        {filesvc_ui::kIdTftpWrite, "filesvc.tftp.write"},
        {filesvc_ui::kIdTftpBtn, "filesvc.tftp.toggle"},
        {filesvc_ui::kIdTftpStatus, "filesvc.tftp.status"},
        {filesvc_ui::kIdGroupShare, "filesvc.group.share"},
        {filesvc_ui::kIdSharePort, "filesvc.share.port"},
        {filesvc_ui::kIdShareBtn, "filesvc.share.toggle"},
        {filesvc_ui::kIdShareOpen, "filesvc.share.open"},
        {filesvc_ui::kIdShareStatus, "filesvc.share.status"},
        {filesvc_ui::kIdGroupNfs, "filesvc.group.nfs"},
        {filesvc_ui::kIdNfsAlias, "filesvc.nfs.alias"},
        {filesvc_ui::kIdNfsUid, "filesvc.nfs.uid"},
        {filesvc_ui::kIdNfsGid, "filesvc.nfs.gid"},
        {filesvc_ui::kIdNfsBtn, "filesvc.nfs.toggle"},
        {filesvc_ui::kIdNfsStatus, "filesvc.nfs.status"},
        {filesvc_ui::kIdSave, "filesvc.save"},
        {filesvc_ui::kIdStopAll, "filesvc.stopall"},
        {filesvc_ui::kIdClose, "filesvc.close"},
        {filesvc_ui::kIdMsg, "filesvc.message"},
        {filesvc_ui::kIdLabelRoot, "filesvc.label.root"},
        {filesvc_ui::kIdLabelTftpIp, "filesvc.label.tftpIp"},
        {filesvc_ui::kIdLabelTftpPort, "filesvc.label.tftpPort"},
        {filesvc_ui::kIdLabelSharePort, "filesvc.label.sharePort"},
        {filesvc_ui::kIdLabelNfsAlias, "filesvc.label.nfsAlias"},
        {filesvc_ui::kIdLabelUid, "filesvc.label.uid"},
        {filesvc_ui::kIdLabelGid, "filesvc.label.gid"},
    };
    for (const Pair& p : pairs) {
        if (HWND h = GetDlgItem(wnd, p.id)) ui_bridge_set_id(h, p.name);
    }
}

void register_board_button(HWND btn, const std::string& action_id) {
    if (!btn || action_id.empty()) return;
    // ui_bridge_set_id 只保存字符串指针，不复制；动态 id 必须活得比按钮久。
    static std::deque<std::string> keep;
    keep.push_back("board.btn." + action_id);
    ui_bridge_set_id(btn, keep.back().c_str());
}

void register_quick_button(HWND btn, std::size_t index) {
    if (!btn) return;
    static std::deque<std::string> keep;
    keep.push_back("quick.btn." + std::to_string(index));
    ui_bridge_set_id(btn, keep.back().c_str());
}

void stop() { ui_bridge_stop(); }
bool handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kMsgBridgeState) {
        g_state_cache = build_state_json();
        return true;
    }
    return ui_bridge_handle_message(hwnd, msg, wp, lp) != FALSE;
}

bool running() { return ui_bridge_get_port() > 0; }
int  port()    { return ui_bridge_get_port(); }

}  // namespace aibridge
}  // namespace sb3
