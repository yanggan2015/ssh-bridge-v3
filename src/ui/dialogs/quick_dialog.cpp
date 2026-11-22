// quick_dialog.cpp — 内存对话框模板 + 动态控件，避免引入 .rc 资源。
// 桥接 id：edit.quick.name / edit.quick.command / btn.quick.save / btn.quick.cancel。

#include "ui/dialogs/quick_dialog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#include "third_party/win_ui_bridge.h"

namespace sb3 {
namespace quick_dlg {
namespace {

struct Ctx {
    HFONT font = nullptr;
    int dpi = 96;
    HWND owner = nullptr;
    QuickCommandConfig* value = nullptr;
    HWND name = nullptr;
    HWND command = nullptr;
    HWND hint = nullptr;
};

std::wstring get_text(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::wstring s(static_cast<std::size_t>(len) + 1, L'\0');
    if (len > 0) GetWindowTextW(h, s.data(), len + 1);
    s.resize(static_cast<std::size_t>(len));
    return s;
}

std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

std::wstring trim(const std::wstring& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return s.substr(b, e - b);
}

HWND add_ctl(HWND dlg, const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exstyle,
             int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(exstyle, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                             dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dlg, GWLP_HINSTANCE)),
                             nullptr);
    return c;
}

void layout(HWND dlg, Ctx* c) {
    RECT rc{};
    GetClientRect(dlg, &rc);
    int w = rc.right;
    int margin = MulDiv(20, c->dpi, 96);
    int gap = MulDiv(8, c->dpi, 96);
    int h = MulDiv(28, c->dpi, 96);
    int label_h = MulDiv(22, c->dpi, 96);
    int y = margin;

    SetWindowPos(GetDlgItem(dlg, kIdNameLabel), nullptr, margin, y,
                 w - margin * 2, label_h, SWP_NOZORDER);
    y += label_h + MulDiv(2, c->dpi, 96);
    SetWindowPos(c->name, nullptr, margin, y, w - margin * 2, h, SWP_NOZORDER);
    y += h + gap + MulDiv(2, c->dpi, 96);
    SetWindowPos(GetDlgItem(dlg, kIdCmdLabel), nullptr, margin, y,
                 w - margin * 2, label_h, SWP_NOZORDER);
    y += label_h + MulDiv(2, c->dpi, 96);
    SetWindowPos(c->command, nullptr, margin, y, w - margin * 2, h, SWP_NOZORDER);
    y += h + gap;
    SetWindowPos(c->hint, nullptr, margin, y, w - margin * 2, label_h, SWP_NOZORDER);

    int bw = MulDiv(104, c->dpi, 96);
    int bh = MulDiv(32, c->dpi, 96);
    int by = rc.bottom - margin - bh;
    int bx = w - margin - bw * 2 - gap;
    SetWindowPos(GetDlgItem(dlg, IDOK), nullptr, bx, by, bw, bh, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(dlg, IDCANCEL), nullptr, bx + bw + gap, by, bw, bh, SWP_NOZORDER);
}

INT_PTR CALLBACK proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    Ctx* c = reinterpret_cast<Ctx*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        c = reinterpret_cast<Ctx*>(lp);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(c));
        int margin = MulDiv(20, c->dpi, 96);
        int w = MulDiv(460, c->dpi, 96);
        int h = MulDiv(246, c->dpi, 96);
        RECT ownerRc{};
        if (GetWindowRect(c->owner, &ownerRc)) {
            int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - w) / 2;
            int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - h) / 2;
            SetWindowPos(dlg, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);
        } else {
            SetWindowPos(dlg, HWND_TOP, 0, 0, w, h, SWP_NOMOVE | SWP_SHOWWINDOW);
        }

        HWND nameLabel = add_ctl(dlg, L"STATIC", L"按钮名称", SS_LEFT | SS_CENTERIMAGE, 0,
                                 margin, 0, 0, 0, kIdNameLabel);
        HWND cmdLabel = add_ctl(dlg, L"STATIC", L"下发指令（点击按钮后立即发送并回车）",
                                SS_LEFT | SS_CENTERIMAGE, 0, margin, 0, 0, 0, kIdCmdLabel);
        c->name = add_ctl(dlg, L"EDIT", L"", ES_AUTOHSCROLL,
                          WS_EX_CLIENTEDGE, margin, 0, 0, 0, kIdName);
        c->command = add_ctl(dlg, L"EDIT", L"", ES_AUTOHSCROLL,
                             WS_EX_CLIENTEDGE, margin, 0, 0, 0, kIdCmd);
        c->hint = add_ctl(dlg, L"STATIC", L"绑定当前画框的通道；不同板子各自保存一套按钮。",
                          SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, 0, margin, 0, 0, 0, kIdHint);
        add_ctl(dlg, L"BUTTON", L"保存", BS_DEFPUSHBUTTON, 0, 0, 0, 0, 0, IDOK);
        add_ctl(dlg, L"BUTTON", L"取消", BS_PUSHBUTTON, 0, 0, 0, 0, 0, IDCANCEL);

        HFONT font = c->font ? c->font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (HWND each : {nameLabel, cmdLabel, c->name, c->command, c->hint,
                          GetDlgItem(dlg, IDOK), GetDlgItem(dlg, IDCANCEL)}) {
            SendMessageW(each, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        SendMessageW(c->name, EM_SETLIMITTEXT, 64, 0);
        SendMessageW(c->command, EM_SETLIMITTEXT, 1024, 0);
        SetWindowTextW(c->name, wide(c->value->label).c_str());
        SetWindowTextW(c->command, wide(c->value->command).c_str());
        layout(dlg, c);
        SetFocus(c->name);
        SendMessageW(c->name, EM_SETSEL, 0, -1);

        ui_bridge_attach_window(dlg);
        ui_bridge_set_id(GetDlgItem(dlg, kIdNameLabel), "label.quick.name");
        ui_bridge_set_id(c->name, "edit.quick.name");
        ui_bridge_set_id(GetDlgItem(dlg, kIdCmdLabel), "label.quick.command");
        ui_bridge_set_id(c->command, "edit.quick.command");
        ui_bridge_set_id(c->hint, "label.quick.hint");
        ui_bridge_set_id(GetDlgItem(dlg, IDOK), "btn.quick.save");
        ui_bridge_set_id(GetDlgItem(dlg, IDCANCEL), "btn.quick.cancel");
        return TRUE;
    }
    case WM_SIZE:
        if (c) layout(dlg, c);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK && c) {
            std::wstring label = trim(get_text(c->name));
            std::wstring cmd = trim(get_text(c->command));
            if (cmd.empty()) {
                SetWindowTextW(c->hint, L"指令不能为空：请输入要发送到通道的命令");
                SetFocus(c->command);
                return TRUE;
            }
            if (label.empty()) label = cmd;
            if (label.size() > 24) label.resize(24);
            c->value->label = utf8(label);
            c->value->command = utf8(cmd);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

#pragma pack(push, 1)
struct Template {
    DLGTEMPLATE dlg;
    WORD menu;
    WORD cls;
    wchar_t title[64];
};
#pragma pack(pop)

}  // namespace

bool show(HINSTANCE inst, HWND owner, HFONT font, int dpi,
          const std::wstring& title, QuickCommandConfig* value) {
    if (!value) return false;
    Ctx c;
    c.font = font;
    c.dpi = dpi > 0 ? dpi : 96;
    c.owner = owner;
    c.value = value;
    Template t{};
    t.dlg.style = DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t.dlg.cx = 300;
    t.dlg.cy = 160;
    std::wstring titleText = title;
    if (titleText.size() >= 63) titleText.resize(63);
    std::wcsncpy(t.title, titleText.c_str(), 63);
    // DS_SETFONT 模板必须紧跟字号与字体名
    struct FontBlock {
        WORD pointsize;
        wchar_t face[32];
    } fb{9, L"Microsoft YaHei UI"};
    std::vector<unsigned char> buf(sizeof(Template) + sizeof(FontBlock));
    std::memcpy(buf.data(), &t, sizeof(t));
    std::memcpy(buf.data() + sizeof(t), &fb, sizeof(fb));
    INT_PTR r = DialogBoxIndirectParamW(inst, reinterpret_cast<LPCDLGTEMPLATEW>(buf.data()),
                                       owner, proc, reinterpret_cast<LPARAM>(&c));
    if (r == -1) {
        char msg[96]{};
        std::snprintf(msg, sizeof(msg), "quick dialog failed err=%lu size=%zu\n",
                      static_cast<unsigned long>(GetLastError()), buf.size());
        std::FILE* f = std::fopen("output\\quick_dialog_error.txt", "a");
        if (f) { std::fputs(msg, f); std::fclose(f); }
    }
    return r == IDOK;
}

}  // namespace quick_dlg
}  // namespace sb3
