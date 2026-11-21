// file_service_panel.h — S12 文件服务独立窗口（TFTP / 共享 HTTP / NFS）。
#pragma once

#include <windows.h>

#include <string>

#include "core/config/config.h"

namespace sb3 {

class AppCore;
class ServiceManager;

namespace filesvc_ui {

inline constexpr wchar_t kClass[] = L"SB3_FileServicePanel";

enum Id {
    kIdTitle = 1500,
    kIdGroupRoot = 1501,
    kIdRoot = 1502,
    kIdPickRoot = 1503,
    kIdLayout = 1504,
    kIdGroupTftp = 1510,
    kIdTftpIp = 1511,
    kIdTftpPort = 1512,
    kIdTftpWrite = 1513,
    kIdTftpBtn = 1514,
    kIdTftpStatus = 1515,
    kIdGroupShare = 1520,
    kIdSharePort = 1521,
    kIdShareBtn = 1522,
    kIdShareOpen = 1523,
    kIdShareStatus = 1524,
    kIdGroupNfs = 1530,
    kIdNfsAlias = 1531,
    kIdNfsUid = 1532,
    kIdNfsGid = 1533,
    kIdNfsBtn = 1534,
    kIdNfsStatus = 1535,
    kIdSave = 1540,
    kIdStopAll = 1541,
    kIdClose = 1542,
    kIdMsg = 1543,
    kIdLabelRoot = 1550,
    kIdLabelTftpIp = 1551,
    kIdLabelTftpPort = 1552,
    kIdLabelSharePort = 1553,
    kIdLabelNfsAlias = 1554,
    kIdLabelUid = 1555,
    kIdLabelGid = 1556,
};
}  // namespace filesvc_ui

class FileServicePanel {
public:
    bool open(HINSTANCE inst, HWND parent, AppCore* core, ServiceManager* mgr, HFONT font,
              int dpi);
    void close();
    bool is_open() const { return hwnd_ && IsWindow(hwnd_); }
    HWND hwnd() const { return hwnd_; }
    void refresh();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void build();
    void on_command(int id, int code);
    void load_config();
    AppConfig collect_config() const;
    void fill_ip_list();
    void update_runtime();
    void save_config(bool show_note);
    void set_text(int id, const std::wstring& text);
    std::wstring get_text(int id) const;
    void note(const std::wstring& text, bool error = false);

    AppCore* core_ = nullptr;
    ServiceManager* mgr_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    HFONT font_ = nullptr;
    int dpi_ = 96;
    int min_w_ = 0;
    int min_h_ = 0;
    bool updating_ = false;
};

}  // namespace sb3
