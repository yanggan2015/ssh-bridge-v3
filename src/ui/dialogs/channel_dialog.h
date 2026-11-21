// channel_dialog.h — S6 通道编辑表单（串口 / SSH 二选一，非模态）。
// 独立顶层窗口：桥接可 attach 后逐控件驱动；保存走 AppCore 手术式热应用。

#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "app/app_core.h"

namespace sb3 {

namespace chan_dlg {

inline constexpr wchar_t kClass[] = L"SB3_ChannelDialog";

// 资源号（桥接按这些号取 HWND；改动要同步 bridge_host）
enum Id {
    kIdNameLabel  = 3100,
    kIdKindLabel  = 3101,
    kIdDeviceLabel= 3102,
    kIdRefresh    = 3103,
    kIdBaudLabel  = 3104,
    kIdDataLabel  = 3105,
    kIdParityLabel= 3106,
    kIdHostLabel  = 3107,
    kIdPortLabel  = 3108,
    kIdUserLabel  = 3109,
    kIdPassLabel  = 3110,
    kIdTermLabel  = 3111,
    kIdTimeoutLabel = 3112,
    kIdWidthLabel = 3113,
    kIdHeightLabel= 3114,
    kIdKeepLabel  = 3115,
    kIdStopLabel  = 3116,
    kIdFlowLabel  = 3117,
    kIdLineLabel  = 3118,
    kIdEncLabel   = 3119,
    kIdName       = 3001,
    kIdKind       = 3002,
    kIdDevice     = 3010,
    kIdBaud       = 3011,
    kIdDatabits   = 3012,
    kIdParity     = 3013,
    kIdStopbits   = 3014,
    kIdFlow       = 3015,
    kIdLineEnd    = 3016,
    kIdEncoding   = 3017,
    kIdAutoOpen   = 3018,
    kIdDtr        = 3019,
    kIdRts        = 3020,
    kIdLog        = 3021,
    kIdHost       = 3030,
    kIdPort       = 3031,
    kIdUser       = 3032,
    kIdPassword   = 3033,
    kIdTerm       = 3034,
    kIdTimeout    = 3035,
    kIdWidth      = 3036,
    kIdHeight     = 3037,
    kIdKeepalive  = 3038,
    kIdSerialBox  = 3040,
    kIdSshBox     = 3041,
    kIdSerialCap  = 3042,
    kIdSshCap     = 3043,
    kIdHint       = 3050,
    kIdSave       = 3051,
    kIdCancel     = 3052,
    kIdDelete     = 3053,
    kIdTest       = 3054,
};

}  // namespace chan_dlg

class ChannelDialog {
public:
    using AppliedFn = std::function<void(const std::string& name, int elapsed_ms)>;

    // edit_name 非空 = 编辑；force_ssh 决定新建时的初始类型
    bool open(HINSTANCE inst, HWND parent, AppCore* core, HFONT font, int dpi,
              const std::string& edit_name, bool force_ssh, AppliedFn on_applied);
    void close();
    bool is_open() const { return hwnd_ && IsWindow(hwnd_); }
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void build(bool force_ssh);
    void layout();
    void apply_kind_visibility();
    void load_from_config();
    void save();
    void test_open();
    void on_kind_change();
    void handle_command(int id, int code);
    void set_hint(const std::wstring& text, bool ok);
    std::wstring text_of(int id) const;
    void set_text(int id, const std::wstring& s);
    int number_of(int id, int def) const;
    bool is_ssh() const;
    BoardConfig   collect_board() const;
    SSHLinkConfig collect_ssh() const;
    bool validate(std::wstring* err) const;

    AppCore*    core_    = nullptr;
    HINSTANCE   inst_    = nullptr;
    HWND        hwnd_    = nullptr;
    HWND        parent_  = nullptr;
    HFONT       font_    = nullptr;
    int         dpi_     = 96;
    bool        editing_ = false;
    bool        delete_armed_ = false;
    std::string edit_name_;
    std::wstring hint_;
    bool         hint_ok_ = true;
    AppliedFn   on_applied_;
};

}  // namespace sb3

