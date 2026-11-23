// app_core.h — 装配层：配置 → 通道对象（串口 / SSH）→ 启停与状态汇总（S3）。
// 只有 app 能同时接 core 与 ui；ui 只通过本类拿"有哪些通道、各自什么状态"。

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <cjson/cJSON.h>

#include "core/bridge/bridge.h"
#include "core/config/config.h"

namespace sb3 {

class AppCore {
public:
    struct Entry {
        std::string name;
        std::string kind;        // "serial" / "ssh"
        std::string detail;      // "COM3 @115200" / "192.168.1.10:22"
        std::string state;       // "offline" / "ready" / "idle"
        std::string sig;         // 参数指纹：变了就热应用
        bool        started = false;
        std::shared_ptr<IBridge> bridge;   // 异步按钮执行期间也要保住对象，避免配置重载后悬空
        std::shared_ptr<std::atomic<bool>> wake;   // 合并 RX 唤醒，避免消息洪水
    };

    struct ReloadResult {
        int         added   = 0;
        int         removed = 0;
        int         changed = 0;
        int         kept    = 0;
        std::string error;
        bool ok() const { return error.empty(); }
    };

    // 载入配置；返回是否有可用配置文件（没有也按默认值继续）
    bool init(const std::string& project_root);

    // 启动 auto_open 的串口通道与全部 SSH 通道（失败只影响本通道）
    void start_auto();
    void stop_all();

    // 通道表快照：Web/Agent 线程与 UI 热应用并发时，不能把 entries_ 的引用交出去。
    std::size_t channel_count() const;
    std::vector<Entry> entries() const;

    // 把"有新数据"通知接到 UI：fn(idx) 只会被合并后调用
    void set_kick(std::function<void(std::size_t)> fn);
    void clear_pending(std::size_t idx);

    IBridge* find(const std::string& name);
    std::shared_ptr<IBridge> find_shared(const std::string& name);
    bool reconnect(const std::string& name);
    bool disconnect(const std::string& name);
    bool clear(const std::string& name);

    // S6 手术式热应用：只重建受影响通道，其它通道的 ring 与游标不动
    ReloadResult reload_config();

    // 表单保存：同名覆盖（old_name 非空表示改名 = 删旧 + 增新）→ 落盘 → 热应用
    bool upsert_serial(const BoardConfig& board, const std::string& old_name,
                       std::string* err);
    bool upsert_ssh(const SSHLinkConfig& link, const std::string& old_name,
                    std::string* err);
    bool delete_channel(const std::string& name, std::string* err);
    // 继电器设备增改删：同样以磁盘最新配置为基准，改名同步按钮引用
    bool upsert_relay(const RelayDeviceConfig& relay, const std::string& old_name,
                      std::string* err);
    bool delete_relay(const std::string& name, std::string* err);

    // 配置值快照：调用方拿到的是副本，不再跨线程持有 cfg_ 内部指针。
    bool board_of(const std::string& name, BoardConfig* out) const;
    bool link_of(const std::string& name, SSHLinkConfig* out) const;
    bool is_ssh_name(const std::string& name) const;
    // S8 入站 SSH：用户名=通道名；返回该通道对象与入站密码（空=允许 none）。
    bool resolve_inbound(const std::string& name, std::shared_ptr<IBridge>* bridge,
                         std::string* password, std::string* err) const;
    // 指定 COM 口是否正被本程序某个运行中的串口通道占用（板控按钮执行前检查）
    bool port_in_use(const std::string& port) const;

    // S12 板卡按钮：读取当前启用按钮；返回副本，避免与配置热应用竞态
    std::vector<BoardActionConfig> buttons_of(const std::string& name) const;
    bool set_buttons(const std::string& name, const std::vector<BoardActionConfig>& buttons,
                     std::string* err);

    // S13 底部快捷指令：按通道保存，不重建通道；返回副本
    std::vector<QuickCommandConfig> quick_commands_of(const std::string& name) const;
    bool set_quick_commands(const std::string& name,
                            const std::vector<QuickCommandConfig>& commands, std::string* err);

    // 该通道配置的行尾（none/cr/lf/crlf）；找不到返回 crlf
    std::string line_ending_for(const std::string& name) const;

    // 状态栏第一段：通道 3 · 在线 1
    std::string summary() const;
    // 完整状态快照（给 ui_state.json 与后续网页/Agent 复用）
    std::string state_json(bool window_visible) const;

    // 线程安全的配置快照：持锁复制，避免 UI/Web/Agent 读与配置事务并发时撕裂容器
    AppConfig config() const {
        std::lock_guard<std::mutex> g(config_mu_);
        return cfg_;
    }
    // 网页只读配置页：当前已知字段导出（不包含未知历史键）
    std::string config_json_text() const {
        std::lock_guard<std::mutex> g(config_mu_);
        return config_export_json(cfg_);
    }
    // S9 网页配置表单保存：校验 JSON、落盘到当前 abs_path；UI 线程随后 reload_config。
    bool save_config_json(const std::string& text, std::string* err);
    // 以磁盘最新配置为基准做读-改-写，串行化所有配置写，避免并发覆盖。
    bool patch_config_json(const std::function<bool(cJSON*)>& apply, std::string* err);
    // AppConfig 版配置事务：读磁盘最新、应用局部修改、落盘并同步内存。
    bool update_config(const std::function<bool(AppConfig&, std::string*)>& apply, std::string* err);
    // UI 分屏布局（窗格数/各画框通道/激活画框）落盘，不改服务配置、不重建通道
    bool save_ui_layout(int split, const std::vector<std::string>& pane_channels, int active_pane);
    // Web 分屏只读快照（锁内复制，避免与 UI 自动保存竞态）
    struct PaneSnapshot {
        int split = 1;
        int active = 0;
        std::vector<std::string> channels;
    };
    PaneSnapshot pane_snapshot() const;
    // Web 受保护激活：只允许切到已绑定且仍存在的通道；落盘后由 UI 线程联动
    bool set_active_pane(int index, std::string* err);

private:
    void build_entries();
    void rebind_kicks_locked();
    void start_entry(Entry& e, bool serial_auto);

    AppConfig cfg_;
    std::string project_root_;
    std::vector<Entry> entries_;
    std::function<void(std::size_t)> kick_fn_;
    mutable std::mutex config_mu_;   // 配置读-改-写事务锁
    mutable std::mutex entries_mu_;  // 通道表与 kick 回调锁；reload 时整体替换
};

}  // namespace sb3
