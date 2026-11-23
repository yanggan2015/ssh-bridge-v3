// app_core.cpp — 通道装配、状态汇总与手术式热应用（S3 / S6）。

#include "app/app_core.h"

#include <windows.h>

#include <cjson/cJSON.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
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

BoardConfig* find_board(AppConfig& cfg, const std::string& name) {
    for (BoardConfig& b : cfg.boards) if (b.name == name) return &b;
    return nullptr;
}

SSHLinkConfig* find_link(AppConfig& cfg, const std::string& name) {
    for (SSHLinkConfig& s : cfg.links) if (s.name == name) return &s;
    return nullptr;
}

RelayDeviceConfig* find_relay(AppConfig& cfg, const std::string& name) {
    for (RelayDeviceConfig& r : cfg.relays) if (r.name == name) return &r;
    return nullptr;
}

bool has_channel(const AppConfig& cfg, const std::string& name) {
    for (const BoardConfig& b : cfg.boards) if (b.name == name) return true;
    for (const SSHLinkConfig& s : cfg.links) if (s.name == name) return true;
    return false;
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
    std::lock_guard<std::mutex> g(config_mu_);
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
    bool saved = update_config([&](AppConfig& next, std::string* e) {
        if (old_name.empty() && has_channel(next, board.name)) {
            if (e) *e = "通道名已存在：" + board.name;
            return false;
        }
        if (!old_name.empty() && old_name != board.name && has_channel(next, board.name)) {
            if (e) *e = "通道名已存在：" + board.name;
            return false;
        }
        BoardConfig nb = board;
        // 通道编辑对话框只编辑串口参数；保留对话框没有暴露的字段，避免编辑一次就丢密码/快捷指令/按钮
        std::string src = old_name.empty() ? board.name : old_name;
        const BoardConfig* oldBoard = find_board(next, src);
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
        if (!old_name.empty() && old_name != board.name) {
            erase_named(&next.boards, old_name);
            erase_named(&next.links, old_name);
            for (std::string& pane : next.ui.pane_channels)
                if (pane == old_name) pane = board.name;
        }
        erase_named(&next.boards, board.name);
        erase_named(&next.links, board.name);
        next.boards.push_back(nb);
        return true;
    }, err);
    if (!saved) return false;
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
    bool saved = update_config([&](AppConfig& next, std::string* e) {
        if (old_name.empty() && has_channel(next, link.name)) {
            if (e) *e = "通道名已存在：" + link.name;
            return false;
        }
        if (!old_name.empty() && old_name != link.name && has_channel(next, link.name)) {
            if (e) *e = "通道名已存在：" + link.name;
            return false;
        }
        SSHLinkConfig nl = link;
        std::string src = old_name.empty() ? link.name : old_name;
        const SSHLinkConfig* oldLink = find_link(next, src);
        if (oldLink) {
            // 对话框不编辑按钮/快捷指令/日志缓冲；保留这些设置
            nl.buttons = oldLink->buttons;
            nl.quick_commands = oldLink->quick_commands;
            nl.buffer_size = oldLink->buffer_size;
            nl.log_dir = oldLink->log_dir;
        }
        if (!old_name.empty() && old_name != link.name) {
            erase_named(&next.boards, old_name);
            erase_named(&next.links, old_name);
            for (std::string& pane : next.ui.pane_channels)
                if (pane == old_name) pane = link.name;
        }
        erase_named(&next.boards, link.name);
        erase_named(&next.links, link.name);
        next.links.push_back(nl);
        return true;
    }, err);
    if (!saved) return false;
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
    bool saved = update_config([&](AppConfig& next, std::string* e) {
        if (!has_channel(next, name)) {
            if (e) *e = "通道不存在：" + name;
            return false;
        }
        erase_named(&next.boards, name);
        erase_named(&next.links, name);
        for (std::string& pane : next.ui.pane_channels)
            if (pane == name) pane.clear();   // 删除后不要再把画框绑回不存在的通道
        return true;
    }, err);
    if (!saved) return false;
    ReloadResult r = reload_config();

    if (!r.ok()) {
        if (err) *err = r.error;
        return false;
    }
    return true;
}

bool AppCore::upsert_relay(const RelayDeviceConfig& relay, const std::string& old_name,
                           std::string* err) {
    if (relay.name.empty()) {
        if (err) *err = "设备名不能为空";
        return false;
    }
    return update_config([&](AppConfig& next, std::string* e) {
        if (old_name.empty()) {
            if (find_relay(next, relay.name)) {
                if (e) *e = "设备名已存在：" + relay.name;
                return false;
            }
            next.relays.push_back(relay);
        } else {
            RelayDeviceConfig* old = find_relay(next, old_name);
            if (!old) {
                if (e) *e = "继电器设备不存在：" + old_name;
                return false;
            }
            *old = relay;
            if (old_name != relay.name) {
                for (BoardConfig& b : next.boards)
                    for (BoardActionConfig& a : b.buttons)
                        if (a.backend == "relay" && a.relay == old_name) a.relay = relay.name;
                for (SSHLinkConfig& s : next.links)
                    for (BoardActionConfig& a : s.buttons)
                        if (a.backend == "relay" && a.relay == old_name) a.relay = relay.name;
            }
        }
        return true;
    }, err);
}

bool AppCore::delete_relay(const std::string& name, std::string* err) {
    if (name.empty()) {
        if (err) *err = "未选择设备";
        return false;
    }
    return update_config([&](AppConfig& next, std::string* e) {
        RelayDeviceConfig* old = find_relay(next, name);
        if (!old) {
            if (e) *e = "继电器设备不存在：" + name;
            return false;
        }
        for (const BoardConfig& b : next.boards) {
            for (const BoardActionConfig& a : b.buttons)
                if (a.backend == "relay" && a.relay == name) {
                    if (e) *e = "设备仍被通道 " + b.name + " 的按钮绑定，不能删除";
                    return false;
                }
        }
        for (const SSHLinkConfig& s : next.links) {
            for (const BoardActionConfig& a : s.buttons)
                if (a.backend == "relay" && a.relay == name) {
                    if (e) *e = "设备仍被通道 " + s.name + " 的按钮绑定，不能删除";
                    return false;
                }
        }
        erase_named(&next.relays, name);
        return true;
    }, err);
}

bool AppCore::save_ui_layout(int split, const std::vector<std::string>& pane_channels, int active_pane) {
    std::lock_guard<std::mutex> g(config_mu_);
    // 只改 UI 段：必须基于磁盘最新配置，否则会把并发写入的通道/动作覆盖回旧快照。
    AppConfig next;
    config_load(next, cfg_.abs_path);
    if (next.abs_path.empty()) next = cfg_;
    next.ui.split = (split == 4) ? 4 : (split == 2 ? 2 : 1);
    next.ui.pane_channels = pane_channels;
    next.ui.pane_channels.resize(static_cast<std::size_t>(next.ui.split));
    int active = std::max(0, std::min(active_pane, next.ui.split - 1));
    next.ui.active_pane = active;
    if (!config_save(next)) return false;
    cfg_.ui = next.ui;
    return true;
}

AppCore::PaneSnapshot AppCore::pane_snapshot() const {
    std::lock_guard<std::mutex> g(config_mu_);
    PaneSnapshot out;
    out.split = (cfg_.ui.split == 4) ? 4 : (cfg_.ui.split == 2 ? 2 : 1);
    out.channels = cfg_.ui.pane_channels;
    out.channels.resize(static_cast<std::size_t>(out.split));
    out.active = std::max(0, std::min(cfg_.ui.active_pane, out.split - 1));
    return out;
}

bool AppCore::set_active_pane(int index, std::string* err) {
    std::lock_guard<std::mutex> g(config_mu_);
    AppConfig next;
    config_load(next, cfg_.abs_path);
    if (next.abs_path.empty()) next = cfg_;
    int split = (next.ui.split == 4) ? 4 : (next.ui.split == 2 ? 2 : 1);
    next.ui.split = split;
    next.ui.pane_channels.resize(static_cast<std::size_t>(split));
    if (index < 0 || index >= split) {
        if (err) *err = "pane index out of range";
        return false;
    }
    const std::string& name = next.ui.pane_channels[static_cast<std::size_t>(index)];
    if (name.empty()) {
        if (err) *err = "pane is empty";
        return false;
    }
    bool exists = false;
    for (const BoardConfig& b : next.boards) if (b.name == name) exists = true;
    for (const SSHLinkConfig& s : next.links) if (s.name == name) exists = true;
    if (!exists) {
        if (err) *err = "pane channel no longer exists";
        return false;
    }
    next.ui.active_pane = index;
    if (!config_save(next)) {
        if (err) *err = "save config failed";
        return false;
    }
    cfg_.ui = next.ui;
    return true;
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
    bool saved = update_config([&](AppConfig& next, std::string* e) {
        if (BoardConfig* b = find_board(next, name)) {
            b->buttons = buttons;
            return true;
        }
        if (SSHLinkConfig* s = find_link(next, name)) {
            s->buttons = buttons;
            return true;
        }
        if (e) *e = "通道不存在：" + name;
        return false;
    }, err);
    return saved;
}

const std::vector<QuickCommandConfig>* AppCore::quick_commands_of(const std::string& name) const {
    if (const BoardConfig* b = board_of(name)) return &b->quick_commands;
    if (const SSHLinkConfig* s = link_of(name)) return &s->quick_commands;
    return nullptr;
}

bool AppCore::set_quick_commands(const std::string& name,
                                 const std::vector<QuickCommandConfig>& commands,
                                 std::string* err) {
    bool saved = update_config([&](AppConfig& next, std::string* e) {
        if (BoardConfig* b = find_board(next, name)) {
            b->quick_commands = commands;
            return true;
        }
        if (SSHLinkConfig* s = find_link(next, name)) {
            s->quick_commands = commands;
            return true;
        }
        if (e) *e = "通道不存在：" + name;
        return false;
    }, err);
    return saved;
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
    std::lock_guard<std::mutex> g(config_mu_);
    for (const BoardConfig& b : cfg_.boards) {
        if (b.name == name && !b.line_ending.empty()) return b.line_ending;
    }
    return "crlf";
}

bool AppCore::update_config(const std::function<bool(AppConfig&, std::string*)>& apply, std::string* err) {
    if (!apply) {
        if (err) *err = "empty patch";
        return false;
    }
    std::lock_guard<std::mutex> g(config_mu_);
    AppConfig next;
    if (!cfg_.abs_path.empty()) config_load(next, cfg_.abs_path);
    if (next.abs_path.empty()) next = cfg_;
    next.project_root = project_root_;
    if (next.abs_path.empty())
        next.abs_path = util::join_path(project_root_, "ssh_bridge_config.json");
    if (!apply(next, err)) return false;
    if (!config_save(next)) {
        if (err) *err = "配置落盘失败：" + next.abs_path;
        return false;
    }
    cfg_ = next;
    if (err) err->clear();
    return true;
}

bool AppCore::patch_config_json(const std::function<bool(cJSON*)>& apply, std::string* err) {
    if (!apply) {
        if (err) *err = "empty patch";
        return false;
    }
    std::lock_guard<std::mutex> g(config_mu_);
    if (cfg_.abs_path.empty()) {
        if (err) *err = "当前配置没有落盘路径";
        return false;
    }
    // 读磁盘最新内容，而不是尚未 reload 的内存 cfg_；否则连续写会互相覆盖。
    std::string text = config_export_json(cfg_);
    {
        std::ifstream in(cfg_.abs_path, std::ios::binary);
        if (in) {
            std::string disk;
            std::string line;
            while (std::getline(in, line)) disk += line;
            if (!disk.empty()) text = disk;
        }
    }
    cJSON* root = cJSON_Parse(text.c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        if (err) *err = "当前配置无法解析";
        return false;
    }
    if (!apply(root)) {
        cJSON_Delete(root);
        if (err && err->empty()) *err = "配置修改被拒绝";
        return false;
    }
    char* dumped = cJSON_PrintUnformatted(root);
    std::string out = dumped ? dumped : "";
    if (dumped) cJSON_free(dumped);
    cJSON_Delete(root);
    if (out.empty()) {
        if (err) *err = "配置序列化失败";
        return false;
    }
    AppConfig next;
    if (!config_parse_json(out, &next, err)) return false;
    next.abs_path = cfg_.abs_path;
    next.project_root = project_root_;
    if (!config_save(next)) {
        if (err) *err = "写配置文件失败";
        return false;
    }
    // 内存视图必须跟上已落盘事务，避免 Web/Agent 在 UI 重载前读到旧配置。
    cfg_ = next;
    if (err) err->clear();
    return true;
}

bool AppCore::save_config_json(const std::string& text, std::string* err) {
    std::lock_guard<std::mutex> g(config_mu_);
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
    cfg_ = next;
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
    std::string cfg_path;
    {
        std::lock_guard<std::mutex> g(config_mu_);
        cfg_path = cfg_.abs_path;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "app", "ssh-bridge3");
    cJSON_AddStringToObject(root, "step", "S6");
    cJSON_AddStringToObject(root, "title", "ssh-bridge v3 · 终端工作台");
    cJSON_AddBoolToObject(root, "visible", window_visible);
    cJSON_AddStringToObject(root, "summary", summary().c_str());
    cJSON_AddStringToObject(root, "config", cfg_path.c_str());

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





