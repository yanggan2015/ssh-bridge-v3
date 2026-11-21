// tray.cpp — 托盘图标实现（S3）。

#include "ui/tray/tray.h"

#include <cstring>

#include "ui/ui_messages.h"

namespace sb3 {

TrayIcon::~TrayIcon() { destroy(); }

bool TrayIcon::create(HWND owner, const std::wstring& tip) {
    if (alive_) return true;
    std::memset(&data_, 0, sizeof(data_));
    data_.cbSize           = sizeof(data_);
    data_.hWnd             = owner;
    data_.uID              = 1;
    data_.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data_.uCallbackMessage = kMsgTrayEvent;
    data_.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    std::wstring t = tip.substr(0, 127);
    std::wmemcpy(data_.szTip, t.c_str(), t.size());
    data_.szTip[t.size()]  = L'\0';
    alive_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    return alive_;
}

void TrayIcon::destroy() {
    if (!alive_) return;
    Shell_NotifyIconW(NIM_DELETE, &data_);
    alive_ = false;
}

void TrayIcon::balloon(const std::wstring& title, const std::wstring& text, DWORD flags) {
    if (!alive_) return;
    data_.uFlags = NIF_INFO;
    std::wstring t = title.substr(0, 63);
    std::wstring m = text.substr(0, 255);
    std::wmemcpy(data_.szInfoTitle, t.c_str(), t.size());
    data_.szInfoTitle[t.size()] = L'\0';
    std::wmemcpy(data_.szInfo, m.c_str(), m.size());
    data_.szInfo[m.size()] = L'\0';
    data_.dwInfoFlags = flags;
    Shell_NotifyIconW(NIM_MODIFY, &data_);
}

}  // namespace sb3
