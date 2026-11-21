// service_panel.h — S11 服务与继电器面板（独立顶层窗口，所有控件可桥接）。
#pragma once

#include <windows.h>

#include <string>

#include "app/app_core.h"

namespace sb3 {

class ServiceManager;

namespace svc_ui {

inline constexpr wchar_t kClass[] = L"SB3_ServicePanel";

enum Id {
    kIdRelayDevice = 1300,
    kIdRelayPort = 1301,
    kIdRelayBaud = 1302,
    kIdRelayRefresh = 1303,
    kIdRelayEnable = 1304,
    kIdRelaySave = 1305,
    kIdRelayStatus = 1306,
    kIdRelayCh1 = 1310,
    kIdRelayCh2 = 1311,
    kIdRelayPulse1 = 1312,
    kIdRelayPulse2 = 1313,
    kIdRelayHold = 1314,
    kIdRelayInv = 1315,
    kIdRelayInfo = 1316,
    kIdRoot = 1330,
    kIdPickRoot = 1331,
    kIdLayout = 1332,
    kIdTftpIp = 1333,
    kIdTftpPort = 1334,
    kIdTftpWrite = 1335,
    kIdTftpBtn = 1336,
    kIdTftpStatus = 1337,
    kIdSharePort = 1340,
    kIdShareBtn = 1341,
    kIdShareOpen = 1342,
    kIdShareStatus = 1343,
    kIdNfsAlias = 1350,
    kIdNfsUid = 1351,
    kIdNfsGid = 1352,
    kIdNfsBtn = 1353,
    kIdNfsStatus = 1354,
    kIdSave = 1360,
    kIdStopAll = 1361,
    kIdClose = 1362,
    kIdMsg = 1363,
    kIdTitle = 1364,
    kIdGroupRelay = 1365,
    kIdGroupFiles = 1366,
    kIdLabelRelayDevice = 1367,
    kIdLabelRelayPort = 1368,
    kIdLabelHold = 1369,
    kIdLabelRoot = 1370,
    kIdLabelTftp = 1371,
    kIdLabelShare = 1372,
    kIdLabelNfs = 1373,
    kIdLabelTftpPort = 1374,
    kIdLabelSharePort = 1375,
    kIdLabelUid = 1376,
    kIdLabelGid = 1377,
    kIdLabelBaud = 1378,
};
}  // namespace svc_ui

class ServicePanel {
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
    void layout();
    void on_command(int id, int code);
    void load_config();
    void load_relay_fields();
    AppConfig collect_config() const;
    void fill_relay_devices();
    void fill_ports();
    void fill_ip_list();
    void update_runtime();
    void save_config(bool show_note);
    RelayDeviceConfig current_relay() const;
    int selected_relay_index() const;
    AppConfig panel_config() const;
    void set_text(int id, const std::wstring& text);
    std::wstring get_text(int id) const;
    void set_status(int id, const std::wstring& text, COLORREF color);
    void note(const std::wstring& text, bool error = false);

    AppCore* core_ = nullptr;
    ServiceManager* mgr_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    HFONT font_ = nullptr;
    int dpi_ = 96;
    bool updating_ = false;
};

}  // namespace sb3
