// app_core.cpp — 通道装配、状态汇总与手术式热应用（S3 / S6）。

#include "app/app_core.h"

#include <windows.h>

#include <cjson/cJSON.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "core/bridge/serial/serial_bridge.h"
#include "core/bridge/ssh/ssh_bridge.h"
#include "core/util/util.h"

namespace sb3 {
namespace {

std::string serial_sig(const BoardConfig& b) {
    std::string s;
    s += b.name;                    s += '|';
    s += b.serial_device;           s += '|';
    s += std::to_string(b.baudrate);s += '|';
    s += std::to_string(b.databits);s += '|';
    s += b.parity;                  s += '|';
    s += std::to_string(b.stopbits_x10); s += '|';
    s += b.flowcontrol;             s += '|';
    s += b.dtr ? "1" : "0";         s += '|';
    s += b.rts ? "1" : "0";         s += '|';
    s += b.line_ending;             s += '|';
    s += b.encoding;                s += '|';
    s += b.auto_open ? "1" : "0";   s += '|';
    s += b.timestamp ? "1" : "0";   s += '|';
    s += std::to_string(b.buffer_size); s += '|';
    s += b.log_enabled ? "1" : "0"; s += '|';
    s += b.log_dir;                 s += '|';
    s += b.username;                s += '|';
    s += b.password_present ? b.password : std::string("$");
    return s;
}

std::string ssh_sig(const SSHLinkConfig& s) {
    std::string v;
    v += s.name;                    v += '|';
    v += s.host;                    v += '|';
    v += std::to_string(s.port);    v += '|';
    v += s.username;                v += '|';
    v += s.password_present ? s.password : std::string("$"); v += '|';
    v += s.term;                    v += '|';
    v += std::to_string(s.width);   v += '|';
    v += std::to_string(s.height);  v += '|';
    v += std::to_string(s.connect_timeout_ms); v += '|';
    v += std::to_string(s.keepalive_s);        v += '|';
    v += std::to_string(s.buffer_size);        v += '|';
    v += s.log_enabled ? "1" : "0"; v += '|';
    v += s.log_dir;
    return v;
}

std::string serial_detail(const BoardConfig& b) {
    std::string d = b.serial_device;
    if (!d.empty()) d += " @ " + std::to_string(b.baudrate);
    return d;
}

std::string ssh_detail(const SSHLinkConfig& s) {
    return s.host + ":" + std::to_string(s.port);
}

template <typename T>
void erase_named(std::vector<T>* vec, const std::string& name) {
    vec->erase(std::remove_if(vec->begin(), vec->end(),
                              [&name](const T& x) { return x.name == name; }),
               vec->end());
}

}  // namespace

bool AppCore::init(const std::string& project_root) {
    project_root_ = project_root;
    config_load(cfg_, std::string());
    build_entries();
    return !cfg_.abs_path.empty();
}

void AppCore::build_entries() {
    entries_.clear();

    for (const BoardConfig& b : cfg_.boards) {
        Entry e;
        e.name = b.name.empty() ? std::string("board") : b.name;
        e.kind = "serial";
        e.detail = serial_detail(b);
        e.sig = serial_sig(b);
        e.bridge = std::make_shared<SerialBridge>(b, project_root_);
        e.wake = std::make_shared<std::atomic<bool>>(false);
        entries_.push_back(std::move(e));
    }

    for (const SSHLinkConfig& s : cfg_.links) {
        Entry e;
        e.name = s.name.empty() ? (s.host + ":" + std::to_string(s.port)) : s.name;
        e.kind = "ssh";
        e.detail = ssh_detail(s);
        e.sig = ssh_sig(s);
        e.bridge = std::make_shared<SshBridge>(s, project_root_);
        e.wake = std::make_shared<std::atomic<bool>>(false);
        entries_.push_back(std::move(e));
    }
}

void AppCore::rebind_kicks() {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        if (!e.bridge) continue;
        if (!e.wake) e.wake = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> wake = e.wake;
        std::size_t idx = i;
        std::function<void(std::size_t)> fn = kick_fn_;
        std::function<void()> cb = [fn, wake, idx]() {
            if (wake->exchange(true)) return;      // 已挂起：合并，不再发消息
            if (fn) fn(idx);
        };
        if (e.kind == "serial") {
            static_cast<SerialBridge*>(e.bridge.get())->set_kick(cb);
        } else {
            static_cast<SshBridge*>(e.bridge.get())->set_kick(cb);
        }
    }
}

void AppCore::set_kick(std::function<void(std::size_t)> fn) {
    kick_fn_ = std::move(fn);
    rebind_kicks();
}

void AppCore::clear_pending(std::size_t idx) {
    if (idx < entries_.size() && entries_[idx].wake) {
        entries_[idx].wake->store(false, std::memory_order_release);
    }
}

void AppCore::start_entry(Entry& e, bool serial_auto) {
    if (!e.bridge) return;
    if (e.bridge->running()) {
        e.started = true;
        return;
    }
    // 串口只在 auto_open 时自动开；SSH 一律尝试（失败后台重试）
    if (e.kind == "serial" && !serial_auto) return;
    e.started = e.bridge->start();
}

void AppCore::start_auto() {
    std::size_t idx = 0;
    for (const BoardConfig& b : cfg_.boards) {
        if (idx >= entries_.size()) break;
        start_entry(entries_[idx], b.auto_open);
        ++idx;
    }
    for (std::size_t i = cfg_.boards.size(); i < entries_.size(); ++i) {
        start_entry(entries_[i], true);
    }
}

void AppCore::stop_all() {
    for (Entry& e : entries_) {
        if (e.bridge) e.bridge->stop();
        e.started = false;
    }
}

AppCore::ReloadResult AppCore::reload_config() {
    ReloadResult r;

    AppConfig next;
    std::string path = cfg_.abs_path;
    config_load(next, path);
    next.project_root = project_root_;
    if (next.abs_path.empty()) {
        r.error = "no config file";
        return r;
    }
    if (next.boards.empty() && next.links.empty() && !next.abs_path.empty()) {
        // 允许“删到零通道”，这不是错误
    }

    std::vector<Entry> old = std::move(entries_);
    std::vector<bool> consumed(old.size(), false);
    entries_.clear();
    entries_.reserve(next.boards.size() + next.links.size());

    std::vector<std::size_t> changed_idx;
    std::vector<std::size_t> added_idx;

    for (const BoardConfig& b : next.boards) {
        std::size_t mi = old.size();
        for (std::size_t i = 0; i < old.size(); ++i) {
            if (!consumed[i] && old[i].kind == "serial" && old[i].name == b.name) {
                mi = i;
                break;
            }
        }
        if (mi < old.size()) {
            consumed[mi] = true;
            if (old[mi].sig == serial_sig(b)) {
                old[mi].detail = serial_detail(b);
                entries_.push_back(std::move(old[mi]));
                ++r.kept;
            } else {
                old[mi].detail = serial_detail(b);
                old[mi].sig = serial_sig(b);
                entries_.push_back(std::move(old[mi]));
                changed_idx.push_back(entries_.size() - 1);
                ++r.changed;
            }
        } else {
            Entry e;
            e.name = b.name;
            e.kind = "serial";
            e.detail = serial_detail(b);
            e.sig = serial_sig(b);
            e.bridge = std::make_shared<SerialBridge>(b, project_root_);
            e.wake = std::make_shared<std::atomic<bool>>(false);
            entries_.push_back(std::move(e));
            added_idx.push_back(entries_.size() - 1);
            ++r.added;
        }
    }

    for (const SSHLinkConfig& s : next.links) {
        std::size_t mi = old.size();
        for (std::size_t i = 0; i < old.size(); ++i) {
            if (!consumed[i] && old[i].kind == "ssh" && old[i].name == s.name) {
                mi = i;
                break;
            }
        }
        if (mi < old.size()) {
            consumed[mi] = true;
            if (old[mi].sig == ssh_sig(s)) {
                old[mi].detail = ssh_detail(s);
                entries_.push_back(std::move(old[mi]));
                ++r.kept;
            } else {
                old[mi].detail = ssh_detail(s);
                old[mi].sig = ssh_sig(s);
                entries_.push_back(std::move(old[mi]));
                changed_idx.push_back(entries_.size() - 1);
                ++r.changed;
            }
        } else {
            Entry e;
            e.name = s.name;
            e.kind = "ssh";
            e.detail = ssh_detail(s);
            e.sig = ssh_sig(s);
            e.bridge = std::make_shared<SshBridge>(s, project_root_);
            e.wake = std::make_shared<std::atomic<bool>>(false);
            entries_.push_back(std::move(e));
            added_idx.push_back(entries_.size() - 1);
            ++r.added;
        }
    }

    // 删除的通道先停线程、释放端口，避免同名/同口重建时被占用
    for (std::size_t i = 0; i < old.size(); ++i) {
        if (!consumed[i]) {
            if (old[i].bridge) old[i].bridge->stop();
            old[i].bridge.reset();
            ++r.removed;
        }
    }

    cfg_ = next;
    rebind_kicks();

    for (std::size_t idx : changed_idx) {
        Entry& e = entries_[idx];
        if (!e.bridge) continue;
        if (e.kind == "serial") {
            const BoardConfig* b = board_of(e.name);
            if (b) static_cast<SerialBridge*>(e.bridge.get())->apply_config(*b);
        } else {
            const SSHLinkConfig* s = link_of(e.name);
            if (s) static_cast<SshBridge*>(e.bridge.get())->apply_config(*s);
        }
        e.started = e.bridge->running();
    }

    for (std::size_t idx : added_idx) {
        Entry& e = entries_[idx];
        bool auto_open = true;
        if (e.kind == "serial") {
            const BoardConfig* b = board_of(e.name);
            auto_open = b ? b->auto_open : false;
        }
        start_entry(e, auto_open);
    }

    return r;
}

bool AppCore::upsert_serial(const BoardConfig& board, const std::string& old_name,
                            std::string* err) {
    if (board.name.empty()) {
        if (err) *err = "通道名不能为空";
        return false;
    }
    if (old_name.empty() && (board_of(board.name) || link_of(board.name))) {
        if (err) *err = "通道名已存在：" + board.name;
        return false;
    }
    BoardConfig nb = board;
    // 通道编辑对话框只编辑串口参数；保留对话框没有暴露的字段，避免编辑一次就丢密码/快捷指令/按钮
    std::string src = old_name.empty() ? board.name : old_name;
    const BoardConfig* oldBoard = board_of(src);
    if (oldBoard) {
        nb.username = oldBoard->username;
        nb.password = oldBoard->password;
        nb.password_present = oldBoard->password_present;
        nb.buffer_size = oldBoard->buffer_size;
        nb.log_dir = oldBoard->log_dir;
        nb.timestamp = oldBoard->timestamp;
        nb.buttons = oldBoard->buttons;
        nb.quick_commands = oldBoard->quick_commands;
    }
    AppConfig next = cfg_;
    if (!old_name.empty() && old_name != board.name) {
        if (board_of(board.name) || link_of(board.name)) {
            if (err) *err = "通道名已存在：" + board.name;
            return false;
        }
        erase_named(&next.boards, old_name);
        erase_named(&next.links, old_name);
        for (std::string& pane : next.ui.pane_channels)
            if (pane == old_name) pane = board.name;
    }
    erase_named(&next.boards, board.name);
    erase_named(&next.links, board.name);
    next.boards.push_back(nb);
    if (next.abs_path.empty()) {
        next.abs_path = util::join_path(project_root_, "ssh_bridge_config.json");
    }
    bool saved = config_save(next);

    if (!saved) {
        if (err) *err = "配置落盘失败：" + next.abs_path;
        return false;
    }
    ReloadResult r = reload_config();

    if (!r.ok()) {
        if (err) *err = r.error;
        return false;
    }
    return true;
}

bool AppCore::upsert_ssh(const SSHLinkConfig& link, const std::string& old_name,
                         std::string* err) {

    if (link.name.empty()) {
        if (err) *err = "通道名不能为空";
        return false;
    }
    if (old_name.empty() && (board_of(link.name) || link_of(link.name))) {
        if (err) *err = "通道名已存在：" + link.name;
        return false;
    }
    SSHLinkConfig nl = link;
    std::string src = old_name.empty() ? link.name : old_name;
    const SSHLinkConfig* oldLink = link_of(src);
    if (oldLink) {
        // 对话框不编辑按钮/快捷指令/日志缓冲；保留这些设置
        nl.buttons = oldLink->buttons;
        nl.quick_commands = oldLink->quick_commands;
        nl.buffer_size = oldLink->buffer_size;
        nl.log_dir = oldLink->log_dir;
    }
    AppConfig next = cfg_;
    if (!old_name.empty() && old_name != link.name) {
        if (board_of(link.name) || link_of(link.name)) {
            if (err) *err = "通道名已存在：" + link.name;
            return false;
        }
        erase_named(&next.boards, old_name);
        erase_named(&next.links, old_name);
        for (std::string& pane : next.ui.pane_channels)
            if (pane == old_name) pane = link.name;
    }
    erase_named(&next.boards, link.name);
    erase_named(&next.links, link.name);
    next.links.push_back(nl);
    if (next.abs_path.empty()) {
        next.abs_path = util::join_path(project_root_, "ssh_bridge_config.json");
    }
    bool saved = config_save(next);

    if (!saved) {
        if (err) *err = "配置落盘失败：" + next.abs_path;
        return false;
    }
    ReloadResult r = reload_config();

    if (!r.ok()) {
        if (err) *err = r.error;
        return false;
    }
    return true;
}

bool AppCore::delete_channel(const std::string& name, std::string* err) {
    if (name.empty()) {
        if (err) *err = "未选择通道";
        return false;
    }
    AppConfig next = cfg_;
    bool found = false;
    for (const BoardConfig& b : next.boards) if (b.name == name) found = true;
    for (const SSHLinkConfig& s : next.links) if (s.name == name) found = true;
    if (!found) {
        if (err) *err = "通道不存在：" + name;
        return false;
    }
    erase_named(&next.boards, name);
    erase_named(&next.links, name);
    for (std::string& pane : next.ui.pane_channels)
        if (pane == name) pane.clear();   // 删除后不要再把画框绑回不存在的通道
    bool saved = config_save(next);

    if (!saved) {
        if (err) *err = "配置落盘失败：" + next.abs_path;
        return false;
    }
    ReloadResult r = reload_config();

    if (!r.ok()) {
        if (err) *err = r.error;
        return false;
    }
    return true;
}

bool AppCore::save_ui_layout(int split, const std::vector<std::string>& pane_channels, int active_pane) {
    AppConfig next = cfg_;
    next.ui.split = (split == 4) ? 4 : (split == 2 ? 2 : 1);
    next.ui.pane_channels = pane_channels;
    next.ui.pane_channels.resize(static_cast<std::size_t>(next.ui.split));
    int active = std::max(0, std::min(active_pane, next.ui.split - 1));
    next.ui.active_pane = active;
    if (!config_save(next)) return false;
    cfg_.ui = next.ui;
    return true;
}

void AppCore::set_service_config(const AppConfig& cfg) {
    cfg_.relays = cfg.relays;
    cfg_.tftp = cfg.tftp;
    cfg_.share = cfg.share;
    cfg_.nfs = cfg.nfs;
    // 继电器改名会同步按钮里的 relay 字段；这里把按钮一起带回内存，避免重启前状态不一致
    cfg_.boards = cfg.boards;
    cfg_.links = cfg.links;
}

const BoardConfig* AppCore::board_of(const std::string& name) const {
    for (const BoardConfig& b : cfg_.boards) if (b.name == name) return &b;
    return nullptr;
}

const SSHLinkConfig* AppCore::link_of(const std::string& name) const {
    for (const SSHLinkConfig& s : cfg_.links) if (s.name == name) return &s;
    return nullptr;
}

bool AppCore::is_ssh_name(const std::string& name) const {
    return link_of(name) != nullptr;
}

bool AppCore::port_in_use(const std::string& port) const {
    if (port.empty()) return false;
    for (const Entry& e : entries_) {
        if (e.kind != "serial" || !e.bridge || !e.bridge->running()) continue;
        const BoardConfig* b = board_of(e.name);
        if (!b) continue;
        if (util::lower(b->serial_device) == util::lower(port)) return true;
    }
    return false;
}

const std::vector<BoardActionConfig>* AppCore::buttons_of(const std::string& name) const {
    if (const BoardConfig* b = board_of(name)) return &b->buttons;
    if (const SSHLinkConfig* s = link_of(name)) return &s->buttons;
    return nullptr;
}

bool AppCore::set_buttons(const std::string& name,
                          const std::vector<BoardActionConfig>& buttons, std::string* err) {
    AppConfig next = cfg_;
    bool found = false;
    for (BoardConfig& b : next.boards) {
        if (b.name == name) { b.buttons = buttons; found = true; break; }
    }
    if (!found) {
        for (SSHLinkConfig& s : next.links) {
            if (s.name == name) { s.buttons = buttons; found = true; break; }
        }
    }
    if (!found) {
        if (err) *err = "通道不存在：" + name;
        return false;
    }
    if (next.abs_path.empty()) {
        next.abs_path = util::join_path(project_root_, "ssh_bridge_config.json");
    }
    if (!config_save(next)) {
        if (err) *err = "配置落盘失败：" + next.abs_path;
        return false;
    }
    cfg_ = next;
    return true;
}

const std::vector<QuickCommandConfig>* AppCore::quick_commands_of(const std::string& name) const {
    if (const BoardConfig* b = board_of(name)) return &b->quick_commands;
    if (const SSHLinkConfig* s = link_of(name)) return &s->quick_commands;
    return nullptr;
}

bool AppCore::set_quick_commands(const std::string& name,
                                 const std::vector<QuickCommandConfig>& commands,
                                 std::string* err) {
    AppConfig next = cfg_;
    bool found = false;
    for (BoardConfig& b : next.boards) {
        if (b.name == name) { b.quick_commands = commands; found = true; break; }
    }
    if (!found) {
        for (SSHLinkConfig& s : next.links) {
            if (s.name == name) { s.quick_commands = commands; found = true; break; }
        }
    }
    if (!found) {
        if (err) *err = "通道不存在：" + name;
        return false;
    }
    if (next.abs_path.empty())
        next.abs_path = util::join_path(project_root_, "ssh_bridge_config.json");
    if (!config_save(next)) {
        if (err) *err = "配置落盘失败：" + next.abs_path;
        return false;
    }
    cfg_ = next;
    return true;
}

IBridge* AppCore::find(const std::string& name) {
    for (Entry& e : entries_) {
        if (e.name == name) return e.bridge.get();
    }
    return nullptr;
}

std::shared_ptr<IBridge> AppCore::find_shared(const std::string& name) {
    for (Entry& e : entries_) {
        if (e.name == name) return e.bridge;
    }
    return nullptr;
}

bool AppCore::resolve_inbound(const std::string& name, std::shared_ptr<IBridge>* bridge,
                              std::string* password, std::string* err) const {
    if (name.empty()) {
        if (err) *err = "用户名为空（应为通道名）";
        return false;
    }
    for (const Entry& e : entries_) {
        if (e.name != name || !e.bridge) continue;
        std::string pass;
        for (const BoardConfig& b : cfg_.boards) {
            if (b.name == name) {
                pass = config_resolve_password(cfg_, b);
                break;
            }
        }
        for (const SSHLinkConfig& s : cfg_.links) {
            if (s.name == name) {
                pass = s.password;
                break;
            }
        }
        if (bridge) *bridge = e.bridge;
        if (password) *password = pass;
        if (err) err->clear();
        return true;
    }
    if (err) *err = "通道不存在：" + name;
    return false;
}

bool AppCore::reconnect(const std::string& name) {
    for (Entry& e : entries_) {
        if (e.name != name || !e.bridge) continue;
        if (e.bridge->running()) {
            e.bridge->reconnect();
        } else if (!e.bridge->start()) {
            return false;
        }
        e.started = true;
        return true;
    }
    return false;
}

bool AppCore::disconnect(const std::string& name) {
    for (Entry& e : entries_) {
        if (e.name != name || !e.bridge) continue;
        e.bridge->stop();
        e.started = false;
        e.state   = "idle";
        return true;
    }
    return false;
}

bool AppCore::clear(const std::string& name) {
    IBridge* b = find(name);
    if (!b) return false;
    b->clear();
    return true;
}

std::string AppCore::line_ending_for(const std::string& name) const {
    for (const BoardConfig& b : cfg_.boards) {
        if (b.name == name && !b.line_ending.empty()) return b.line_ending;
    }
    return "crlf";
}

bool AppCore::save_config_json(const std::string& text, std::string* err) {
    if (cfg_.abs_path.empty()) {
        if (err) *err = "当前配置没有落盘路径";
        return false;
    }
    AppConfig next;
    if (!config_parse_json(text, &next, err)) return false;
    next.abs_path = cfg_.abs_path;
    next.project_root = project_root_;
    if (!config_save(next)) {
        if (err) *err = "写配置文件失败";
        return false;
    }
    if (err) err->clear();
    return true;
}

std::string AppCore::summary() const {
    std::size_t online = 0;
    for (const Entry& e : entries_) {
        if (e.bridge && e.bridge->running()) ++online;
    }
    return "通道 " + std::to_string(entries_.size()) + " · 在线 " + std::to_string(online);
}

std::string AppCore::state_json(bool window_visible) const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "app", "ssh-bridge3");
    cJSON_AddStringToObject(root, "step", "S6");
    cJSON_AddStringToObject(root, "title", "ssh-bridge v3 · 终端工作台");
    cJSON_AddBoolToObject(root, "visible", window_visible);
    cJSON_AddStringToObject(root, "summary", summary().c_str());
    cJSON_AddStringToObject(root, "config", cfg_.abs_path.c_str());

    cJSON* arr = cJSON_AddArrayToObject(root, "channels");
    for (const Entry& e : entries_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e.name.c_str());
        cJSON_AddStringToObject(o, "kind", e.kind.c_str());
        cJSON_AddStringToObject(o, "detail", e.detail.c_str());
        const char* st = "idle";
        if (e.bridge) {
            if (e.bridge->running()) st = "ready";
            else if (e.started) st = "offline";
        }
        cJSON_AddStringToObject(o, "state", st);
        if (e.bridge) {
            cJSON_AddNumberToObject(o, "echo_latency_us",
                                    static_cast<double>(e.bridge->echo_latency_us()));
            cJSON_AddNumberToObject(o, "echo_count",
                                    static_cast<double>(e.bridge->echo_count()));
            cJSON_AddNumberToObject(o, "tx_bytes",
                                    static_cast<double>(e.bridge->tx_bytes()));
            cJSON_AddNumberToObject(o, "rx_bytes",
                                    static_cast<double>(e.bridge->rx_bytes()));
        }
        cJSON_AddItemToArray(arr, o);
    }

    char* text = cJSON_PrintUnformatted(root);
    std::string out = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

}  // namespace sb3





