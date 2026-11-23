// app_core.cpp — 通道装配、状态汇总与手术式热应用（S3 / S6）。

#include "app/app_core.h"

#include <windows.h>

#include <cjson/cJSON.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

#include "core/bridge/powershell/powershell_bridge.h"
#include "core/bridge/serial/serial_bridge.h"
#include "core/bridge/ssh/ssh_bridge.h"
#include "core/bridge/telnet/telnet_bridge.h"
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

std::string powershell_sig(const PowerShellConfig& p) {
    std::string v;
    v += p.name;                    v += '|';
    v += p.shell;                   v += '|';
    v += p.args;                    v += '|';
    v += p.cwd;                     v += '|';
    v += p.line_ending;             v += '|';
    v += std::to_string(p.buffer_size); v += '|';
    v += p.log_enabled ? "1" : "0"; v += '|';
    v += p.log_dir;                 v += '|';
    v += p.timestamp ? "1" : "0";
    return v;
}

std::string telnet_sig(const TelnetConfig& t) {
    std::string v;
    v += t.name;                    v += '|';
    v += t.host;                    v += '|';
    v += std::to_string(t.port);    v += '|';
    v += std::to_string(t.connect_timeout_ms); v += '|';
    v += t.line_ending;             v += '|';
    v += std::to_string(t.width);   v += '|';
    v += std::to_string(t.height);  v += '|';
    v += std::to_string(t.buffer_size); v += '|';
    v += t.log_enabled ? "1" : "0"; v += '|';
    v += t.log_dir;                 v += '|';
    v += t.timestamp ? "1" : "0";
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

std::string powershell_detail(const PowerShellConfig& p) {
    return p.shell.empty() ? std::string("powershell.exe") : p.shell;
}

std::string telnet_detail(const TelnetConfig& t) {
    return t.host + ":" + std::to_string(t.port);
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

PowerShellConfig* find_powershell(AppConfig& cfg, const std::string& name) {
    for (PowerShellConfig& p : cfg.powershell) if (p.name == name) return &p;
    return nullptr;
}

TelnetConfig* find_telnet(AppConfig& cfg, const std::string& name) {
    for (TelnetConfig& t : cfg.telnet) if (t.name == name) return &t;
    return nullptr;
}

RelayDeviceConfig* find_relay(AppConfig& cfg, const std::string& name) {
    for (RelayDeviceConfig& r : cfg.relays) if (r.name == name) return &r;
    return nullptr;
}

bool has_channel(const AppConfig& cfg, const std::string& name) {
    for (const BoardConfig& b : cfg.boards) if (b.name == name) return true;
    for (const SSHLinkConfig& s : cfg.links) if (s.name == name) return true;
    for (const PowerShellConfig& p : cfg.powershell) if (p.name == name) return true;
    for (const TelnetConfig& t : cfg.telnet) if (t.name == name) return true;
    return false;
}

// 配置事务里改名/删除时统一清掉四类容器；画框绑定由调用方决定改名还是清空。
void erase_channel_config(AppConfig& cfg, const std::string& name) {
    erase_named(&cfg.boards, name);
    erase_named(&cfg.links, name);
    erase_named(&cfg.powershell, name);
    erase_named(&cfg.telnet, name);
}

void rename_in_panes(AppConfig& cfg, const std::string& old_name, const std::string& new_name) {
    for (std::string& pane : cfg.ui.pane_channels)
        if (pane == old_name) pane = new_name;
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

    for (const PowerShellConfig& p : cfg_.powershell) {
        Entry e;
        e.name = p.name.empty() ? std::string("powershell") : p.name;
        e.kind = "powershell";
        e.detail = powershell_detail(p);
        e.sig = powershell_sig(p);
        e.bridge = std::make_shared<PowerShellBridge>(p, project_root_);
        e.wake = std::make_shared<std::atomic<bool>>(false);
        entries_.push_back(std::move(e));
    }

    for (const TelnetConfig& t : cfg_.telnet) {
        Entry e;
        e.name = t.name.empty() ? (t.host + ":" + std::to_string(t.port)) : t.name;
        e.kind = "telnet";
        e.detail = telnet_detail(t);
        e.sig = telnet_sig(t);
        e.bridge = std::make_shared<TelnetBridge>(t, project_root_);
        e.wake = std::make_shared<std::atomic<bool>>(false);
        entries_.push_back(std::move(e));
    }
}

void AppCore::rebind_kicks_locked() {
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
        } else if (e.kind == "ssh") {
            static_cast<SshBridge*>(e.bridge.get())->set_kick(cb);
        } else if (e.kind == "powershell") {
            static_cast<PowerShellBridge*>(e.bridge.get())->set_kick(cb);
        } else if (e.kind == "telnet") {
            static_cast<TelnetBridge*>(e.bridge.get())->set_kick(cb);
        }
    }
}

void AppCore::set_kick(std::function<void(std::size_t)> fn) {
    std::lock_guard<std::mutex> g(entries_mu_);
    kick_fn_ = std::move(fn);
    rebind_kicks_locked();
}

void AppCore::clear_pending(std::size_t idx) {
    std::lock_guard<std::mutex> g(entries_mu_);
    if (idx < entries_.size() && entries_[idx].wake) {
        entries_[idx].wake->store(false, std::memory_order_release);
    }
}

void AppCore::start_entry(Entry& e, bool serial_auto) {
    if (!e.bridge) return;
    const bool managed = (e.kind == "serial" || e.kind == "powershell");
    if (e.bridge->running()) {
        e.started = true;
        if (managed) {
            e.want_open = true;
            e.retry_at_ms = 0;
            e.retry_attempt = 0;
        }
        return;
    }
    // 串口只在 auto_open 时自动开；SSH/PowerShell/Telnet 一律尝试
    if (e.kind == "serial" && !serial_auto) {
        e.want_open = false;
        e.retry_at_ms = 0;
        e.retry_attempt = 0;
        return;
    }
    e.started = e.bridge->start();
    if (managed) {
        e.want_open = true;
        if (e.started) {
            e.retry_at_ms = 0;
            e.retry_attempt = 0;
        } else {
            schedule_serial_retry(e, util::now_ms());
        }
    }
}

void AppCore::schedule_serial_retry(Entry& e, std::uint64_t now) {
    ++e.retry_attempt;
    int shift = e.retry_attempt > 4 ? 3 : e.retry_attempt - 1;
    if (shift < 0) shift = 0;
    int delay_ms = 2000 << shift;          // 2 / 4 / 8 / 16 秒
    if (delay_ms > 15000) delay_ms = 15000;
    e.retry_at_ms = now + static_cast<std::uint64_t>(delay_ms);
}

void AppCore::poll_channel_retry() {
    std::lock_guard<std::mutex> g(entries_mu_);
    const std::uint64_t now = util::now_ms();
    for (Entry& e : entries_) {
        if ((e.kind != "serial" && e.kind != "powershell") || !e.bridge || !e.want_open) continue;
        if (e.bridge->running()) {
            e.started = true;
            if (e.retry_attempt != 0 || e.retry_at_ms != 0) {
                e.retry_attempt = 0;
                e.retry_at_ms = 0;
            }
            continue;
        }
        e.started = false;
        if (e.retry_at_ms == 0) {
            schedule_serial_retry(e, now);
            continue;
        }
        if (now < e.retry_at_ms) continue;
        if (e.bridge->start()) {
            e.started = true;
            e.retry_attempt = 0;
            e.retry_at_ms = 0;
        } else {
            schedule_serial_retry(e, now);
        }
    }
}

void AppCore::start_auto() {
    std::lock_guard<std::mutex> g(entries_mu_);
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
    std::lock_guard<std::mutex> g(entries_mu_);
    for (Entry& e : entries_) {
        if (e.bridge) e.bridge->stop();
        e.started = false;
        e.want_open = false;
        e.retry_at_ms = 0;
        e.retry_attempt = 0;
    }
}

AppCore::ReloadResult AppCore::reload_config() {
    std::lock_guard<std::mutex> g(config_mu_);
    std::lock_guard<std::mutex> ge(entries_mu_);
    ReloadResult r;

    AppConfig next;
    std::string path = cfg_.abs_path;
    config_load(next, path);
    next.project_root = project_root_;
    if (next.abs_path.empty()) {
        r.error = "no config file";
        return r;
    }
    if (next.boards.empty() && next.links.empty() && next.powershell.empty() &&
        next.telnet.empty() && !next.abs_path.empty()) {
        // 允许“删到零通道”，这不是错误
    }

    std::vector<Entry> old = std::move(entries_);
    std::vector<bool> consumed(old.size(), false);
    entries_.clear();
    entries_.reserve(next.boards.size() + next.links.size() + next.powershell.size() +
                     next.telnet.size());

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

    for (const PowerShellConfig& p : next.powershell) {
        std::size_t mi = old.size();
        for (std::size_t i = 0; i < old.size(); ++i) {
            if (!consumed[i] && old[i].kind == "powershell" && old[i].name == p.name) {
                mi = i;
                break;
            }
        }
        if (mi < old.size()) {
            consumed[mi] = true;
            if (old[mi].sig == powershell_sig(p)) {
                old[mi].detail = powershell_detail(p);
                entries_.push_back(std::move(old[mi]));
                ++r.kept;
            } else {
                old[mi].detail = powershell_detail(p);
                old[mi].sig = powershell_sig(p);
                entries_.push_back(std::move(old[mi]));
                changed_idx.push_back(entries_.size() - 1);
                ++r.changed;
            }
        } else {
            Entry e;
            e.name = p.name;
            e.kind = "powershell";
            e.detail = powershell_detail(p);
            e.sig = powershell_sig(p);
            e.bridge = std::make_shared<PowerShellBridge>(p, project_root_);
            e.wake = std::make_shared<std::atomic<bool>>(false);
            entries_.push_back(std::move(e));
            added_idx.push_back(entries_.size() - 1);
            ++r.added;
        }
    }

    for (const TelnetConfig& t : next.telnet) {
        std::size_t mi = old.size();
        for (std::size_t i = 0; i < old.size(); ++i) {
            if (!consumed[i] && old[i].kind == "telnet" && old[i].name == t.name) {
                mi = i;
                break;
            }
        }
        if (mi < old.size()) {
            consumed[mi] = true;
            if (old[mi].sig == telnet_sig(t)) {
                old[mi].detail = telnet_detail(t);
                entries_.push_back(std::move(old[mi]));
                ++r.kept;
            } else {
                old[mi].detail = telnet_detail(t);
                old[mi].sig = telnet_sig(t);
                entries_.push_back(std::move(old[mi]));
                changed_idx.push_back(entries_.size() - 1);
                ++r.changed;
            }
        } else {
            Entry e;
            e.name = t.name;
            e.kind = "telnet";
            e.detail = telnet_detail(t);
            e.sig = telnet_sig(t);
            e.bridge = std::make_shared<TelnetBridge>(t, project_root_);
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
    rebind_kicks_locked();

    for (std::size_t idx : changed_idx) {
        Entry& e = entries_[idx];
        if (!e.bridge) continue;
        if (e.kind == "serial") {
            const BoardConfig* b = find_board(cfg_, e.name);
            if (b) static_cast<SerialBridge*>(e.bridge.get())->apply_config(*b);
            // 重载以新配置为准：auto_open=false 不再守护；需要时用户可手动重连。
            e.want_open = b && b->auto_open;
            e.retry_at_ms = 0;
            e.retry_attempt = 0;
        } else if (e.kind == "ssh") {
            const SSHLinkConfig* s = find_link(cfg_, e.name);
            if (s) static_cast<SshBridge*>(e.bridge.get())->apply_config(*s);
        } else if (e.kind == "powershell") {
            const PowerShellConfig* p = find_powershell(cfg_, e.name);
            if (p) static_cast<PowerShellBridge*>(e.bridge.get())->apply_config(*p);
            e.want_open = true;
            e.retry_at_ms = 0;
            e.retry_attempt = 0;
        } else if (e.kind == "telnet") {
            const TelnetConfig* t = find_telnet(cfg_, e.name);
            if (t) static_cast<TelnetBridge*>(e.bridge.get())->apply_config(*t);
        }
        e.started = e.bridge->running();
    }

    for (std::size_t idx : added_idx) {
        Entry& e = entries_[idx];
        bool auto_open = true;
        if (e.kind == "serial") {
            const BoardConfig* b = find_board(cfg_, e.name);
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
            rename_in_panes(next, old_name, board.name);
            erase_named(&next.boards, old_name);
            erase_named(&next.links, old_name);
            erase_named(&next.powershell, old_name);
            erase_named(&next.telnet, old_name);
        }
        erase_named(&next.boards, board.name);
        erase_named(&next.links, board.name);
        erase_named(&next.powershell, board.name);
        erase_named(&next.telnet, board.name);
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
            rename_in_panes(next, old_name, link.name);
            erase_named(&next.boards, old_name);
            erase_named(&next.links, old_name);
            erase_named(&next.powershell, old_name);
            erase_named(&next.telnet, old_name);
        }
        erase_named(&next.boards, link.name);
        erase_named(&next.links, link.name);
        erase_named(&next.powershell, link.name);
        erase_named(&next.telnet, link.name);
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

bool AppCore::upsert_powershell(const PowerShellConfig& cfg, const std::string& old_name,
                                std::string* err) {
    if (cfg.name.empty()) {
        if (err) *err = "通道名不能为空";
        return false;
    }
    bool saved = update_config([&](AppConfig& next, std::string* e) {
        if (old_name.empty() && has_channel(next, cfg.name)) {
            if (e) *e = "通道名已存在：" + cfg.name;
            return false;
        }
        if (!old_name.empty() && old_name != cfg.name && has_channel(next, cfg.name)) {
            if (e) *e = "通道名已存在：" + cfg.name;
            return false;
        }
        PowerShellConfig np = cfg;
        std::string src = old_name.empty() ? cfg.name : old_name;
        const PowerShellConfig* old = find_powershell(next, src);
        if (old) {
            np.buttons = old->buttons;
            np.quick_commands = old->quick_commands;
            np.buffer_size = old->buffer_size;
            np.log_dir = old->log_dir;
            np.timestamp = old->timestamp;
        }
        if (!old_name.empty() && old_name != cfg.name) {
            rename_in_panes(next, old_name, cfg.name);
            erase_channel_config(next, old_name);
        }
        erase_channel_config(next, cfg.name);
        next.powershell.push_back(np);
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

bool AppCore::upsert_telnet(const TelnetConfig& cfg, const std::string& old_name,
                            std::string* err) {
    if (cfg.name.empty()) {
        if (err) *err = "通道名不能为空";
        return false;
    }
    if (cfg.host.empty()) {
        if (err) *err = "Telnet 主机不能为空";
        return false;
    }
    bool saved = update_config([&](AppConfig& next, std::string* e) {
        if (old_name.empty() && has_channel(next, cfg.name)) {
            if (e) *e = "通道名已存在：" + cfg.name;
            return false;
        }
        if (!old_name.empty() && old_name != cfg.name && has_channel(next, cfg.name)) {
            if (e) *e = "通道名已存在：" + cfg.name;
            return false;
        }
        TelnetConfig nt = cfg;
        std::string src = old_name.empty() ? cfg.name : old_name;
        const TelnetConfig* old = find_telnet(next, src);
        if (old) {
            nt.buttons = old->buttons;
            nt.quick_commands = old->quick_commands;
            nt.buffer_size = old->buffer_size;
            nt.log_dir = old->log_dir;
            nt.timestamp = old->timestamp;
        }
        if (!old_name.empty() && old_name != cfg.name) {
            rename_in_panes(next, old_name, cfg.name);
            erase_channel_config(next, old_name);
        }
        erase_channel_config(next, cfg.name);
        next.telnet.push_back(nt);
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
        erase_named(&next.powershell, name);
        erase_named(&next.telnet, name);
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
                for (PowerShellConfig& p : next.powershell)
                    for (BoardActionConfig& a : p.buttons)
                        if (a.backend == "relay" && a.relay == old_name) a.relay = relay.name;
                for (TelnetConfig& t : next.telnet)
                    for (BoardActionConfig& a : t.buttons)
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
        for (const PowerShellConfig& p : next.powershell) {
            for (const BoardActionConfig& a : p.buttons)
                if (a.backend == "relay" && a.relay == name) {
                    if (e) *e = "设备仍被通道 " + p.name + " 的按钮绑定，不能删除";
                    return false;
                }
        }
        for (const TelnetConfig& t : next.telnet) {
            for (const BoardActionConfig& a : t.buttons)
                if (a.backend == "relay" && a.relay == name) {
                    if (e) *e = "设备仍被通道 " + t.name + " 的按钮绑定，不能删除";
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
    if (!config_save_ui(next)) return false;
    cfg_.ui = next.ui;
    return true;
}

bool AppCore::save_window_rect(const std::string& rect) {
    std::lock_guard<std::mutex> g(config_mu_);
    AppConfig next;
    config_load(next, cfg_.abs_path);
    if (next.abs_path.empty()) next = cfg_;
    next.ui.window_rect = rect;
    if (!config_save_ui(next)) return false;
    cfg_.ui.window_rect = rect;
    return true;
}

bool AppCore::save_statusbar_visible(bool visible) {
    std::lock_guard<std::mutex> g(config_mu_);
    AppConfig next;
    config_load(next, cfg_.abs_path);
    if (next.abs_path.empty()) next = cfg_;
    next.ui.show_statusbar = visible;
    if (!config_save_ui(next)) return false;
    cfg_.ui.show_statusbar = visible;
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
    for (const PowerShellConfig& p : next.powershell) if (p.name == name) exists = true;
    for (const TelnetConfig& t : next.telnet) if (t.name == name) exists = true;
    if (!exists) {
        if (err) *err = "pane channel no longer exists";
        return false;
    }
    next.ui.active_pane = index;
    if (!config_save_ui(next)) {
        if (err) *err = "save config failed";
        return false;
    }
    cfg_.ui = next.ui;
    return true;
}

std::size_t AppCore::channel_count() const {
    std::lock_guard<std::mutex> g(entries_mu_);
    return entries_.size();
}

std::vector<AppCore::Entry> AppCore::entries() const {
    std::lock_guard<std::mutex> g(entries_mu_);
    return entries_;
}

bool AppCore::board_of(const std::string& name, BoardConfig* out) const {
    std::lock_guard<std::mutex> g(config_mu_);
    for (const BoardConfig& b : cfg_.boards) {
        if (b.name == name) {
            if (out) *out = b;
            return true;
        }
    }
    return false;
}

bool AppCore::link_of(const std::string& name, SSHLinkConfig* out) const {
    std::lock_guard<std::mutex> g(config_mu_);
    for (const SSHLinkConfig& s : cfg_.links) {
        if (s.name == name) {
            if (out) *out = s;
            return true;
        }
    }
    return false;
}

bool AppCore::powershell_of(const std::string& name, PowerShellConfig* out) const {
    std::lock_guard<std::mutex> g(config_mu_);
    for (const PowerShellConfig& p : cfg_.powershell) {
        if (p.name == name) {
            if (out) *out = p;
            return true;
        }
    }
    return false;
}

bool AppCore::telnet_of(const std::string& name, TelnetConfig* out) const {
    std::lock_guard<std::mutex> g(config_mu_);
    for (const TelnetConfig& t : cfg_.telnet) {
        if (t.name == name) {
            if (out) *out = t;
            return true;
        }
    }
    return false;
}

std::string AppCore::kind_of(const std::string& name) const {
    std::lock_guard<std::mutex> g(config_mu_);
    for (const BoardConfig& b : cfg_.boards) if (b.name == name) return "serial";
    for (const SSHLinkConfig& s : cfg_.links) if (s.name == name) return "ssh";
    for (const PowerShellConfig& p : cfg_.powershell) if (p.name == name) return "powershell";
    for (const TelnetConfig& t : cfg_.telnet) if (t.name == name) return "telnet";
    return std::string();
}

bool AppCore::is_ssh_name(const std::string& name) const {
    return link_of(name, nullptr);
}

bool AppCore::port_in_use(const std::string& port) const {
    if (port.empty()) return false;
    const AppConfig snapshot = config();
    const std::vector<Entry> es = entries();
    for (const Entry& e : es) {
        if (e.kind != "serial" || !e.bridge || !e.bridge->running()) continue;
        for (const BoardConfig& b : snapshot.boards) {
            if (b.name == e.name && util::lower(b.serial_device) == util::lower(port)) return true;
        }
    }
    return false;
}

std::vector<BoardActionConfig> AppCore::buttons_of(const std::string& name) const {
    std::lock_guard<std::mutex> g(config_mu_);
    for (const BoardConfig& b : cfg_.boards) if (b.name == name) return b.buttons;
    for (const SSHLinkConfig& s : cfg_.links) if (s.name == name) return s.buttons;
    for (const PowerShellConfig& p : cfg_.powershell) if (p.name == name) return p.buttons;
    for (const TelnetConfig& t : cfg_.telnet) if (t.name == name) return t.buttons;
    return {};
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
        if (PowerShellConfig* p = find_powershell(next, name)) {
            p->buttons = buttons;
            return true;
        }
        if (TelnetConfig* t = find_telnet(next, name)) {
            t->buttons = buttons;
            return true;
        }
        if (e) *e = "通道不存在：" + name;
        return false;
    }, err);
    return saved;
}

std::vector<QuickCommandConfig> AppCore::quick_commands_of(const std::string& name) const {
    std::lock_guard<std::mutex> g(config_mu_);
    for (const BoardConfig& b : cfg_.boards) if (b.name == name) return b.quick_commands;
    for (const SSHLinkConfig& s : cfg_.links) if (s.name == name) return s.quick_commands;
    for (const PowerShellConfig& p : cfg_.powershell) if (p.name == name) return p.quick_commands;
    for (const TelnetConfig& t : cfg_.telnet) if (t.name == name) return t.quick_commands;
    return {};
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
        if (PowerShellConfig* p = find_powershell(next, name)) {
            p->quick_commands = commands;
            return true;
        }
        if (TelnetConfig* t = find_telnet(next, name)) {
            t->quick_commands = commands;
            return true;
        }
        if (e) *e = "通道不存在：" + name;
        return false;
    }, err);
    return saved;
}

IBridge* AppCore::find(const std::string& name) {
    std::lock_guard<std::mutex> g(entries_mu_);
    for (Entry& e : entries_) {
        if (e.name == name) return e.bridge.get();
    }
    return nullptr;
}

std::shared_ptr<IBridge> AppCore::find_shared(const std::string& name) {
    std::lock_guard<std::mutex> g(entries_mu_);
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
    const AppConfig snapshot = config();
    const std::vector<Entry> es = entries();
    for (const Entry& e : es) {
        if (e.name != name || !e.bridge) continue;
        std::string pass;
        for (const BoardConfig& b : snapshot.boards) {
            if (b.name == name) {
                pass = config_resolve_password(snapshot, b);
                break;
            }
        }
        for (const SSHLinkConfig& s : snapshot.links) {
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
    std::shared_ptr<IBridge> b;
    bool managed = false;
    {
        std::lock_guard<std::mutex> g(entries_mu_);
        for (Entry& e : entries_) {
            if (e.name != name || !e.bridge) continue;
            b = e.bridge;
            managed = (e.kind == "serial" || e.kind == "powershell");
            if (managed) {
                e.want_open = true;      // 用户显式重连 = 恢复自动守护
                e.retry_at_ms = 0;
                e.retry_attempt = 0;
            }
            break;
        }
    }
    if (!b) return false;
    if (b->running()) {
        b->reconnect();
    } else if (!b->start()) {
        if (managed) {
            std::lock_guard<std::mutex> g(entries_mu_);
            for (Entry& e : entries_) {
                if (e.name != name) continue;
                e.started = false;
                schedule_serial_retry(e, util::now_ms());
                break;
            }
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> g(entries_mu_);
        for (Entry& e : entries_) {
            if (e.name != name) continue;
            e.started = b->running();
            if (managed) {
                e.want_open = true;
                if (e.started) {
                    e.retry_at_ms = 0;
                    e.retry_attempt = 0;
                } else if (e.retry_at_ms == 0) {
                    schedule_serial_retry(e, util::now_ms());
                }
            }
        }
    }
    return true;
}

bool AppCore::disconnect(const std::string& name) {
    bool found = false;
    bool managed = false;
    std::shared_ptr<IBridge> b;
    {
        std::lock_guard<std::mutex> g(entries_mu_);
        for (Entry& e : entries_) {
            if (e.name != name || !e.bridge) continue;
            b = e.bridge;
            managed = (e.kind == "serial" || e.kind == "powershell");
            if (managed) {
                e.want_open = false;     // 手动断开后不再自动重开，直到用户重连/重载
                e.retry_at_ms = 0;
                e.retry_attempt = 0;
            }
            found = true;
            break;
        }
    }
    if (!found) return false;
    b->stop();
    std::lock_guard<std::mutex> g(entries_mu_);
    for (Entry& e : entries_) {
        if (e.name == name) {
            e.started = false;
            e.state = "idle";
            if (managed) {
                e.want_open = false;
                e.retry_at_ms = 0;
                e.retry_attempt = 0;
            }
        }
    }
    return true;
}

bool AppCore::clear(const std::string& name) {
    std::shared_ptr<IBridge> b = find_shared(name);
    if (!b) return false;
    b->clear();
    return true;
}

std::string AppCore::line_ending_for(const std::string& name) const {
    std::lock_guard<std::mutex> g(config_mu_);
    for (const BoardConfig& b : cfg_.boards) {
        if (b.name == name && !b.line_ending.empty()) return b.line_ending;
    }
    for (const PowerShellConfig& p : cfg_.powershell) {
        if (p.name == name && !p.line_ending.empty()) return p.line_ending;
    }
    for (const TelnetConfig& t : cfg_.telnet) {
        if (t.name == name && !t.line_ending.empty()) return t.line_ending;
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
    if (next.ui_abs_path.empty()) next.ui_abs_path = config_ui_path(next);
    if (!apply(next, err)) return false;
    if (!config_save_functional(next)) {
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
    if (next.ui_abs_path.empty()) next.ui_abs_path = config_ui_path(cfg_);
    if (!config_save_functional(next)) {
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
    bool has_ui = text.find("\"ui\"") != std::string::npos;
    if (!has_ui) next.ui = cfg_.ui;
    next.abs_path = cfg_.abs_path;
    next.project_root = project_root_;
    next.ui_abs_path = config_ui_path(cfg_);
    bool saved = has_ui ? config_save(next) : config_save_functional(next);
    if (!saved) {
        if (err) *err = "写配置文件失败";
        return false;
    }
    cfg_ = next;
    if (err) err->clear();
    return true;
}

std::string AppCore::summary() const {
    const std::vector<Entry> es = entries();
    std::size_t online = 0;
    for (const Entry& e : es) {
        if (e.bridge && e.bridge->running()) ++online;
    }
    return "通道 " + std::to_string(es.size()) + " · 在线 " + std::to_string(online);
}

std::string AppCore::state_json(bool window_visible) const {
    std::string cfg_path;
    {
        std::lock_guard<std::mutex> g(config_mu_);
        cfg_path = cfg_.abs_path;
    }
    const std::vector<Entry> es = entries();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "app", "ssh-bridge3");
    cJSON_AddStringToObject(root, "step", "S6");
    cJSON_AddStringToObject(root, "title", "ssh-bridge v3 · 终端工作台");
    cJSON_AddBoolToObject(root, "visible", window_visible);
    cJSON_AddStringToObject(root, "summary", summary().c_str());
    cJSON_AddStringToObject(root, "config", cfg_path.c_str());

    cJSON* arr = cJSON_AddArrayToObject(root, "channels");
    for (const Entry& e : es) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e.name.c_str());
        cJSON_AddStringToObject(o, "kind", e.kind.c_str());
        cJSON_AddStringToObject(o, "detail", e.detail.c_str());
        cJSON_AddStringToObject(o, "line_ending", line_ending_for(e.name).c_str());
        const char* st = "idle";
        if (e.bridge) {
            if (e.bridge->running()) st = "ready";
            else if (e.started || e.want_open) st = "offline";
        }
        cJSON_AddStringToObject(o, "state", st);
        if (e.kind == "serial" || e.kind == "powershell") {
            cJSON_AddBoolToObject(o, "want_open", e.want_open);
            cJSON_AddNumberToObject(o, "retry_attempt", e.retry_attempt);
        }
        if (e.kind == "serial") {
            auto* sb = dynamic_cast<SerialBridge*>(e.bridge.get());
            if (sb) cJSON_AddStringToObject(o, "last_error", sb->last_error().c_str());
        } else if (e.kind == "powershell") {
            auto* pb = dynamic_cast<PowerShellBridge*>(e.bridge.get());
            if (pb) {
                cJSON_AddStringToObject(o, "last_error", pb->last_error().c_str());
                cJSON_AddNumberToObject(o, "pid", pb->pid());
            }
        } else if (e.kind == "telnet") {
            auto* tb = dynamic_cast<TelnetBridge*>(e.bridge.get());
            if (tb) cJSON_AddStringToObject(o, "last_error", tb->last_error().c_str());
        }
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
