#include "services/service_manager.h"

#include <windows.h>
#include <iphlpapi.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>

#include <cjson/cJSON.h>

#include "core/util/util.h"
#include "services/nfs/nfs_svc.h"
#include "services/relay/relay_ctrl.h"
#include "services/share/share_server.h"
#include "services/tftp/tftp_svc.h"

namespace sb3 {
namespace {

std::uint64_t now_ms() { return static_cast<std::uint64_t>(GetTickCount64()); }

std::string pick_local_ip() {
    char ip[64] = "";
    tftp_pick_default_ip(ip, sizeof(ip));
    return ip;
}

std::string resolve_against(const std::string& root, const std::string& p) {
    if (p.empty()) return "";
    std::filesystem::path fp(p);
    if (fp.is_absolute()) return fp.lexically_normal().string();
    return (std::filesystem::path(root) / fp).lexically_normal().string();
}

}  // namespace

ServiceManager::ServiceManager(const std::string& project_root) : project_root_(project_root) {
    share_root_ = (std::filesystem::path(project_root) / "work" / "share").string();
    tftp_ = tftp_svc_create();
}

ServiceManager::~ServiceManager() {
    stop_all();
    if (tftp_) tftp_svc_destroy(tftp_);
    tftp_ = nullptr;
}

void ServiceManager::apply_config(const AppConfig& cfg) {
    cfg_ = cfg;
    std::string root = cfg.tftp.root.empty() ? "" : resolve_against(project_root_, cfg.tftp.root);
    if (root.empty()) root = (std::filesystem::path(project_root_) / "work" / "share").string();
    share_root_ = (std::filesystem::path(root)).lexically_normal().string();
}

const std::string& ServiceManager::share_root() { return share_root_; }

std::string ServiceManager::resolve_path(const std::string& p) const {
    return resolve_against(project_root_, p);
}

std::string ServiceManager::local_ip() const { return pick_local_ip(); }

void ServiceManager::ensure_layout() {
    try {
        std::filesystem::create_directories(std::filesystem::path(share_root_) / "common");
        for (const BoardConfig& b : cfg_.boards) {
            if (b.name.empty()) continue;
            if (b.name.find('/') != std::string::npos || b.name.find('\\') != std::string::npos ||
                b.name.find("..") != std::string::npos)
                continue;
            std::filesystem::create_directories(std::filesystem::path(share_root_) / b.name);
        }
    } catch (...) {
    }
}

void ServiceManager::set_error(std::string* dst, const std::string& msg) {
    if (dst) *dst = msg;
}

bool ServiceManager::start_tftp(const TftpConfig& cfg, std::string* err) {
    if (!tftp_) tftp_ = tftp_svc_create();
    std::string root = cfg.root.empty() ? share_root_ : resolve_against(project_root_, cfg.root);
    ensure_layout();
    char e[256] = "";
    if (tftp_svc_start_ex(tftp_, root.c_str(), cfg.bind_ip.c_str(), cfg.port, cfg.allow_write, e,
                          sizeof(e)) != 0) {
        tftp_note_ = e;
        set_error(err, e);
        return false;
    }
    tftp_note_ = "已启动";
    if (err) err->clear();
    return true;
}

void ServiceManager::stop_tftp() {
    if (tftp_) tftp_svc_stop(tftp_);
    tftp_note_ = "已停止";
}

bool ServiceManager::tftp_running() const { return tftp_ && tftp_svc_running(tftp_); }

bool ServiceManager::start_share(const ShareConfig& cfg, std::string* err) {
    if (!share_) share_ = new ShareServer();
    std::string root = cfg.root.empty() ? share_root_ : resolve_against(project_root_, cfg.root);
    ensure_layout();
    std::string e;
    if (!share_->start(root, cfg.port, &e)) {
        share_note_ = e;
        set_error(err, e);
        return false;
    }
    share_note_ = "已启动";
    if (err) err->clear();
    return true;
}

void ServiceManager::stop_share() {
    if (share_) share_->stop();
    share_note_ = "已停止";
}

bool ServiceManager::share_running() const { return share_ && share_->running(); }

bool ServiceManager::start_nfs(const NfsConfig& cfg, std::string* err) {
    std::string root = cfg.root.empty() ? share_root_ : resolve_against(project_root_, cfg.root);
    ensure_layout();
    char e[256] = "";
    if (nfs_svc_start(root.c_str(), cfg.alias.c_str(), static_cast<unsigned>(cfg.uid),
                      static_cast<unsigned>(cfg.gid), e, sizeof(e)) != 0) {
        nfs_on_ = false;
        nfs_note_ = e;
        set_error(err, e);
        return false;
    }
    nfs_on_ = true;
    nfs_note_ = "已启动";
    if (err) err->clear();
    return true;
}

void ServiceManager::stop_nfs() {
    nfs_svc_stop();
    nfs_on_ = false;
    nfs_note_ = "已停止";
}

bool ServiceManager::nfs_running() const { return nfs_on_ && nfs_svc_running(); }

void ServiceManager::stop_all() {
    stop_relay_jobs();
    stop_tftp();
    stop_share();
    stop_nfs();
}

void ServiceManager::stop_relay_jobs() {
    if (relay_job_.joinable()) relay_job_.join();
    relay_busy_ = false;
}

bool ServiceManager::relay_set(const RelayDeviceConfig& dev, int channel, bool on,
                               std::string* err) {
    if (!dev.enable) {
        set_error(err, "继电器设备未启用");
        return false;
    }
    if (dev.windows_port.empty()) {
        set_error(err, "未选择串口");
        return false;
    }
    if (channel < 1 || channel > dev.channels) {
        set_error(err, "通道越界（设备只有 " + std::to_string(dev.channels) + " 路）");
        return false;
    }
    char e[256] = "";
    if (relay_ctrl_set(dev.windows_port.c_str(), dev.baudrate, channel, on ? 1 : 0,
                       dev.io_inverted ? 1 : 0, dev.post_write_delay_ms, e, sizeof(e)) != 0) {
        set_error(err, e[0] ? e : "继电器操作失败");
        {
            std::lock_guard<std::mutex> g(relay_mu_);
            for (auto& p : relay_state_) {
                if (p.first == dev.windows_port && channel - 1 < static_cast<int>(p.second.size())) {
                    auto& s = p.second[channel - 1];
                    s.last_error = e[0] ? e : "write failed";
                    s.last_ms = now_ms();
                }
            }
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> g(relay_mu_);
        bool found = false;
        for (auto& p : relay_state_) {
            if (p.first != dev.windows_port) continue;
            found = true;
            if (channel - 1 < static_cast<int>(p.second.size())) {
                auto& s = p.second[channel - 1];
                s.on = on;
                s.busy = false;
                s.mode = on ? "hold_on" : "hold_off";
                s.last_error.clear();
                s.last_ms = now_ms();
            }
        }
        if (!found) {
            std::vector<RelayChannelState> v(static_cast<size_t>(std::max(1, dev.channels)));
            if (channel - 1 < static_cast<int>(v.size())) {
                v[channel - 1].on = on;
                v[channel - 1].mode = on ? "hold_on" : "hold_off";
                v[channel - 1].last_ms = now_ms();
            }
            relay_state_.push_back({dev.windows_port, v});
        }
    }
    if (err) err->clear();
    return true;
}

bool ServiceManager::relay_pulse_async(const RelayDeviceConfig& dev, int channel,
                                       double hold_seconds) {
    return relay_run_mode_async(dev, channel, "pulse", hold_seconds, dev.polarity_inverted);
}

bool ServiceManager::relay_run_mode_async(const RelayDeviceConfig& dev, int channel,
                                          const std::string& mode, double hold_seconds,
                                          bool polarity_inverted) {
    if (mode != "hold_on" && mode != "hold_off" && mode != "pulse") return false;
    if (relay_busy_.load()) return false;
    if (relay_job_.joinable()) relay_job_.join();
    relay_busy_ = true;
    {
        std::lock_guard<std::mutex> g(relay_mu_);
        bool found = false;
        for (auto& p : relay_state_) {
            if (p.first != dev.windows_port) continue;
            found = true;
            if (channel - 1 < static_cast<int>(p.second.size())) {
                p.second[channel - 1].busy = true;
                p.second[channel - 1].mode = mode;
                p.second[channel - 1].last_error.clear();
            }
        }
        if (!found) {
            std::vector<RelayChannelState> v(static_cast<size_t>(std::max(1, dev.channels)));
            if (channel - 1 < static_cast<int>(v.size())) {
                v[channel - 1].busy = true;
                v[channel - 1].mode = mode;
            }
            relay_state_.push_back({dev.windows_port, v});
        }
    }
    RelayDeviceConfig copy = dev;
    relay_job_ = std::thread([this, copy, channel, mode, hold_seconds, polarity_inverted]() {
        char e[256] = "";
        int rc = relay_ctrl_run_mode(copy.windows_port.c_str(), copy.baudrate, channel, mode.c_str(),
                                     hold_seconds, polarity_inverted ? 1 : 0,
                                     copy.io_inverted ? 1 : 0, copy.post_write_delay_ms, e,
                                     sizeof(e));
        bool final_on = (mode == "hold_on");
        std::lock_guard<std::mutex> g(relay_mu_);
        for (auto& p : relay_state_) {
            if (p.first != copy.windows_port) continue;
            if (channel - 1 < static_cast<int>(p.second.size())) {
                auto& s = p.second[channel - 1];
                s.busy = false;
                s.on = rc == 0 ? final_on : false;
                s.mode = rc == 0 ? (mode == "pulse" ? "idle" : mode) : "error";
                s.last_error = rc == 0 ? "" : (e[0] ? e : "继电器模式执行失败");
                s.last_ms = now_ms();
            }
        }
        relay_busy_ = false;
    });
    return true;
}

bool ServiceManager::relay_busy() const { return relay_busy_.load(); }

ServiceManager::RelayChannelState ServiceManager::relay_state(const std::string& port,
                                                             int channel) const {
    std::lock_guard<std::mutex> g(relay_mu_);
    for (const auto& p : relay_state_) {
        if (p.first != port) continue;
        if (channel - 1 >= 0 && channel - 1 < static_cast<int>(p.second.size()))
            return p.second[channel - 1];
    }
    return RelayChannelState{};
}

void ServiceManager::relay_note_state(const std::string& port, int channel, bool on,
                                      const std::string& mode, const std::string& error) {
    if (port.empty() || channel < 1) return;
    std::lock_guard<std::mutex> g(relay_mu_);
    bool found = false;
    for (auto& p : relay_state_) {
        if (p.first != port) continue;
        found = true;
        if (channel - 1 < static_cast<int>(p.second.size())) {
            auto& s = p.second[channel - 1];
            s.on = on;
            s.busy = false;
            s.mode = mode;
            s.last_error = error;
            s.last_ms = now_ms();
        }
    }
    if (!found) {
        int count = channel;
        std::vector<RelayChannelState> v(static_cast<std::size_t>(count));
        v[channel - 1].on = on;
        v[channel - 1].busy = false;
        v[channel - 1].mode = mode;
        v[channel - 1].last_error = error;
        v[channel - 1].last_ms = now_ms();
        relay_state_.push_back({port, v});
    }
}

std::string ServiceManager::share_url() const {
    if (!share_running()) return "";
    return share_->base_url(pick_local_ip());
}

std::string ServiceManager::tftp_status() const {
    if (!tftp_running()) return tftp_note_.empty() ? "未启动" : tftp_note_;
    return "监听 UDP " + std::to_string(tftp_svc_port(tftp_)) + " · 根 " + tftp_svc_root(tftp_);
}

std::string ServiceManager::share_status() const {
    if (!share_running()) return share_note_.empty() ? "未启动" : share_note_;
    return "监听 HTTP " + std::to_string(share_->port()) + " · 根 " + share_->root();
}

std::string ServiceManager::nfs_status() const {
    if (!nfs_running()) return nfs_note_.empty() ? "未启动" : nfs_note_;
    return "NFSv3 111/1058/2049 · 导出 " + share_root_;
}

std::string ServiceManager::relay_json() const {
    cJSON* arr = cJSON_CreateArray();
    std::lock_guard<std::mutex> g(relay_mu_);
    for (const RelayDeviceConfig& d : cfg_.relays) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", d.name.c_str());
        cJSON_AddStringToObject(o, "port", d.windows_port.c_str());
        cJSON_AddBoolToObject(o, "enabled", d.enable);
        cJSON_AddNumberToObject(o, "channels", d.channels);
        cJSON* ch = cJSON_AddArrayToObject(o, "state");
        std::vector<RelayChannelState> states(static_cast<size_t>(std::max(1, d.channels)));
        for (const auto& p : relay_state_) {
            if (p.first == d.windows_port) {
                states = p.second;
                break;
            }
        }
        for (int i = 0; i < d.channels; ++i) {
            cJSON* s = cJSON_CreateObject();
            const RelayChannelState& st =
                i < static_cast<int>(states.size()) ? states[i] : RelayChannelState{};
            cJSON_AddNumberToObject(s, "channel", i + 1);
            cJSON_AddBoolToObject(s, "on", st.on);
            cJSON_AddBoolToObject(s, "busy", st.busy);
            cJSON_AddStringToObject(s, "mode", st.mode.c_str());
            cJSON_AddStringToObject(s, "error", st.last_error.c_str());
            cJSON_AddNumberToObject(s, "last_ms", static_cast<double>(st.last_ms));
            cJSON_AddItemToArray(ch, s);
        }
        cJSON_AddItemToArray(arr, o);
    }
    char* txt = cJSON_PrintUnformatted(arr);
    std::string out = txt ? txt : "[]";
    cJSON_free(txt);
    cJSON_Delete(arr);
    return out;
}

std::string ServiceManager::state_json() const {
    cJSON* root = cJSON_CreateObject();
    cJSON* t = cJSON_AddObjectToObject(root, "tftp");
    cJSON_AddBoolToObject(t, "on", tftp_running());
    cJSON_AddNumberToObject(t, "port", tftp_ ? tftp_svc_port(tftp_) : cfg_.tftp.port);
    cJSON_AddStringToObject(t, "root", tftp_running() ? tftp_svc_root(tftp_) : share_root_.c_str());
    cJSON_AddStringToObject(t, "note", tftp_status().c_str());
    cJSON* s = cJSON_AddObjectToObject(root, "share");
    cJSON_AddBoolToObject(s, "on", share_running());
    cJSON_AddNumberToObject(s, "port", share_ ? share_->port() : cfg_.share.port);
    cJSON_AddStringToObject(s, "root", share_root_.c_str());
    cJSON_AddStringToObject(s, "url", share_url().c_str());
    cJSON_AddStringToObject(s, "note", share_status().c_str());
    cJSON* n = cJSON_AddObjectToObject(root, "nfs");
    cJSON_AddBoolToObject(n, "on", nfs_running());
    cJSON_AddStringToObject(n, "alias", cfg_.nfs.alias.c_str());
    cJSON_AddStringToObject(n, "root", share_root_.c_str());
    cJSON_AddStringToObject(n, "note", nfs_status().c_str());
    cJSON* r = cJSON_Parse(relay_json().c_str());
    if (cJSON_IsArray(r)) cJSON_AddItemToObject(root, "relay_devices", r);
    else cJSON_Delete(r);
    cJSON_AddStringToObject(root, "local_ip", pick_local_ip().c_str());
    char* txt = cJSON_PrintUnformatted(root);
    std::string out = txt ? txt : "{}";
    cJSON_free(txt);
    cJSON_Delete(root);
    return out;
}

}  // namespace sb3
