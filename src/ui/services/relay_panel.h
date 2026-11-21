// relay_panel.h — S12 继电器 / 板卡绑定 / 按钮脚本独立配置窗口。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/config/config.h"

namespace sb3 {

class AppCore;
class ServiceManager;

namespace relay_ui {

inline constexpr wchar_t kClass[] = L"SB3_RelayPanel";

enum Id {
    kIdTitle = 1600,
    kIdTab = 1601,
    kIdMsg = 1602,
    kIdClose = 1603,
    kIdGroupDev = 1604,
    kIdGroupStates = 1605,
    kIdGroupBind = 1606,
    kIdGroupScript = 1607,
    kIdGroupBindTarget = 1608,
    kIdGroupScriptTarget = 1609,

    kIdDevName = 1610,
    kIdDevPort = 1611,
    kIdDevLinux = 1612,
    kIdDevBaud = 1613,
    kIdDevChannels = 1614,
    kIdDevDelay = 1615,
    kIdDevIo = 1616,
    kIdDevPolarity = 1617,
    kIdDevEnable = 1618,
    kIdDevTestCh = 1619,
    kIdDevOn = 1620,
    kIdDevOff = 1621,
    kIdDevPulseHold = 1622,
    kIdDevPulse = 1623,
    kIdDevStatus = 1624,
    kIdDevInfo = 1625,
    kIdDevNew = 1626,
    kIdDevSave = 1627,
    kIdDevDelete = 1628,
    kIdDevRefresh = 1629,
    kIdDevList = 1630,
    kIdDevStates = 1631,

    kIdBindTarget = 1640,
    kIdBindList = 1641,
    kIdBindNew = 1642,
    kIdBindSave = 1643,
    kIdBindDelete = 1644,
    kIdBindTest = 1645,
    kIdBindId = 1646,
    kIdBindLabel = 1647,
    kIdBindRelay = 1648,
    kIdBindChannel = 1649,
    kIdBindMode = 1650,
    kIdBindHold = 1651,
    kIdBindPolarity = 1652,
    kIdBindEnable = 1653,
    kIdBindPresetOn = 1654,
    kIdBindPresetOff = 1655,
    kIdBindPresetReboot = 1656,
    kIdBindGenerate = 1657,
    kIdBindGenerateUpgrade = 1658,
    kIdBindInfo = 1659,
    kIdBindTargetInfo = 1660,

    kIdScriptTarget = 1670,
    kIdScriptList = 1671,
    kIdScriptNew = 1672,
    kIdScriptSave = 1673,
    kIdScriptDelete = 1674,
    kIdScriptTest = 1675,
    kIdScriptId = 1676,
    kIdScriptLabel = 1677,
    kIdScriptKind = 1678,
    kIdScriptBody = 1679,
    kIdScriptDelay = 1680,
    kIdScriptEnable = 1681,
    kIdScriptPresetLogin = 1682,
    kIdScriptPresetUboot = 1683,
    kIdScriptPresetUpgrade = 1684,
    kIdScriptPresetUpgradeMode = 1685,
    kIdScriptGenerateUpgrade = 1686,
    kIdScriptInfo = 1687,

    kIdLabelDevName = 1700,
    kIdLabelDevPort = 1701,
    kIdLabelDevLinux = 1702,
    kIdLabelDevBaud = 1703,
    kIdLabelDevChannels = 1704,
    kIdLabelDevDelay = 1705,
    kIdLabelBindTarget = 1706,
    kIdLabelBindList = 1707,
    kIdLabelBindId = 1708,
    kIdLabelBindLabel = 1709,
    kIdLabelBindRelay = 1710,
    kIdLabelBindChannel = 1711,
    kIdLabelBindMode = 1712,
    kIdLabelBindHold = 1713,
    kIdLabelScriptTarget = 1714,
    kIdLabelScriptList = 1715,
    kIdLabelScriptId = 1716,
    kIdLabelScriptLabel = 1717,
    kIdLabelScriptKind = 1718,
    kIdLabelScriptDelay = 1719,
    kIdLabelScriptBody = 1720,
    kIdLabelDevTestCh = 1721,
    kIdLabelDevPulseHold = 1722,
    kIdLabelDevList = 1723,
    kIdLabelDevStates = 1724,
    kIdLabelDevHint = 1725,
};
}  // namespace relay_ui

class RelayPanel {
public:
    bool open(HINSTANCE inst, HWND parent, AppCore* core, ServiceManager* mgr, HFONT font,
              int dpi, int page = 0);
    void close();
    bool is_open() const { return hwnd_ && IsWindow(hwnd_); }
    HWND hwnd() const { return hwnd_; }
    void refresh();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void build();
    void show_page(int page);
    void track(int id, int page);

    void fill_devices();
    void load_device(int index);
    void fill_ports(int id);
    void fill_number_combo(int id, const std::vector<std::wstring>& items);
    void fill_channels(int id, int count);
    void save_device();
    void delete_device();
    void test_device(const std::string& mode);
    RelayDeviceConfig current_device() const;
    int selected_device_index() const;

    void fill_targets();
    std::string bind_target() const;
    std::string script_target() const;
    void fill_bind_list(const std::string& prefer);
    void load_bind(int action_index);
    void save_bind();
    void delete_bind();
    void test_bind();
    void preset_bind(const std::string& id);
    void generate_standard();
    void generate_upgrade_script();

    void fill_script_list(const std::string& prefer);
    void load_script(int action_index);
    void save_script();
    void delete_script();
    void test_script();
    void preset_script(int preset);
    int action_index_from_combo(int id) const;
    std::string combo_text(int id) const;
    void set_combo_text(int id, const std::wstring& text);
    std::vector<BoardActionConfig> actions_of(const std::string& channel) const;
    bool save_actions(const std::string& channel, const std::vector<BoardActionConfig>& actions);
    int upsert_action(std::vector<BoardActionConfig>* actions, const BoardActionConfig& a);

    void update_runtime();
    void update_devices_list();
    void update_target_info();
    void note(const std::wstring& text, bool error = false);
    void set_text(int id, const std::wstring& text);
    std::wstring get_text(int id) const;
    bool port_in_use(const std::string& port) const;

    AppCore* core_ = nullptr;
    ServiceManager* mgr_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    HFONT font_ = nullptr;
    int dpi_ = 96;
    int min_w_ = 0;
    int min_h_ = 0;
    int page_ = 0;
    bool updating_ = false;
    int dev_index_ = -1;
    std::vector<int> page_ids_[3];
};

}  // namespace sb3
