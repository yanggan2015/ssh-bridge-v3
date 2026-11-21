// tray.h — 托盘图标：创建/删除/气泡提示（S3）。
// 事件统一投递到 owner 窗口的 kMsgTrayEvent，菜单与动作由 shell 决定。

#pragma once

#include <windows.h>

#include <string>

namespace sb3 {

class TrayIcon {
public:
    ~TrayIcon();

    bool create(HWND owner, const std::wstring& tip);
    void destroy();
    void balloon(const std::wstring& title, const std::wstring& text, DWORD flags = NIIF_INFO);
    bool alive() const { return alive_; }

private:
    NOTIFYICONDATAW data_{};
    bool           alive_ = false;
};

}  // namespace sb3
