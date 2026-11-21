/* win_ui_bridge.h — Win32 程序「AI 可读 + 可操作」桥接层。
 *
 * 默认在 AI 工具构建中定义 UI_BRIDGE_ENABLE 并编译 win_ui_bridge.c。
 * 精简包不定义该宏，所有调用退化为空操作，产物无 HTTP 端口。
 * HTTP 只监听 127.0.0.1。
 *
 * 接入：
 *   1. ui_bridge_start(hwnd, port)  // port=0 自动选端口
 *   2. 每个控件 ui_bridge_set_id(ctl, "btn.calc")
 *   3. WndProc 开头：if (ui_bridge_handle_message(...)) return 0;
 *   4. ui_bridge_set_state_provider(...)
 */
#ifndef WIN_UI_BRIDGE_H
#define WIN_UI_BRIDGE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_BRIDGE_WM_ACTION (WM_APP + 0x0501)
#define UI_BRIDGE_PROTOCOL  "1.1"

#ifndef UI_BRIDGE_ENABLE
#define ui_bridge_start(...)              (0)
#define ui_bridge_stop()                  ((void) 0)
#define ui_bridge_set_token(...)          ((void) 0)
#define ui_bridge_set_state_provider(...) ((void) 0)
#define ui_bridge_set_id(h, id)           ((BOOL) 0)
#define ui_bridge_add_virtual(...)        ((BOOL) 0)
#define ui_bridge_add_virtual_ex(...)     ((BOOL) 0)
#define ui_bridge_set_virtual_slider(...) ((void) 0)
#define ui_bridge_attach_window(...)      ((BOOL) 0)
#define ui_bridge_log(...)                ((void) 0)
#define ui_bridge_handle_message(...)     ((BOOL) 0)
#define ui_bridge_get_port()              (0)

#else

typedef void (*UI_STATE_CB)(char *out, int cap, void *user);
typedef void (*UI_VIRTUAL_CLICK)(void *user);
/* 虚拟控件通用动作：action/text/value 来自 HTTP；可写 result_json（小对象）。 */
typedef void (*UI_VIRTUAL_ACTION)(const char *action, const char *text, int value,
                                  char *result_json, int result_cap, void *user);
/* 虚拟滑条的当前值读取（在 UI 线程调用，负责把绝对位置回给 /ui-tree）。 */
typedef int (*UI_VIRTUAL_INT)(void *user);

int  ui_bridge_start(HWND hwnd_main, int port);
void ui_bridge_stop(void);
int  ui_bridge_get_port(void);

void ui_bridge_set_token(const char *token);
void ui_bridge_set_state_provider(UI_STATE_CB cb, void *user);

BOOL ui_bridge_set_id(HWND ctl, const char *id);

BOOL ui_bridge_add_virtual(const char *id, const char *role, const char *name,
                           RECT rc, BOOL enabled, const char *text,
                           UI_VIRTUAL_CLICK on_click, void *user);

BOOL ui_bridge_add_virtual_ex(const char *id, const char *role, const char *name,
                              RECT rc, BOOL enabled, const char *text,
                              UI_VIRTUAL_CLICK on_click,
                              UI_VIRTUAL_ACTION on_action, void *user);

/* 给 role=slider 的虚拟控件补 value/min/max，并挂上当前值读取回调。
   没有这个调用时虚拟滑条只能 set_value、读不到当前值（/ui-tree 缺 value）。 */
void ui_bridge_set_virtual_slider(const char *id, int vmin, int vmax,
                                  UI_VIRTUAL_INT on_value, void *user);

/* 登记附属对话框/弹出窗，纳入 /ui-tree 与动作定位。 */
BOOL ui_bridge_attach_window(HWND hwnd);

/* 摘除已销毁的附属窗口，避免反复开关对话框把 attach 表占满。 */
BOOL ui_bridge_detach_window(HWND hwnd);

void ui_bridge_log(const char *fmt, ...);
BOOL ui_bridge_handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

#endif /* UI_BRIDGE_ENABLE */

#ifdef __cplusplus
}
#endif
#endif /* WIN_UI_BRIDGE_H */
