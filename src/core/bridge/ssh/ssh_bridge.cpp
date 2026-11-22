// ssh_bridge.cpp — SSH 出站通道实现（S2）。
// 线程：RX 负责建链/读/保活/断线退避；TX 负责把 CmdQueue 里的字节写进通道。

#include "core/bridge/ssh/ssh_bridge.h"

#include <windows.h>

#include <cjson/cJSON.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "core/util/util.h"

namespace sb3 {

SshBridge::SshBridge(const SSHLinkConfig& cfg, std::string project_root)
    : cfg_(cfg), project_root_(std::move(project_root)) {
    std::size_t cap = cfg_.buffer_size ? cfg_.buffer_size : (1u << 20);
    std::string log_dir = cfg_.log_dir;
    if (log_dir.empty()) log_dir = util::join_path(project_root_, "work\\logs");
    core_.init(cfg_.name.empty() ? std::string("ssh") : cfg_.name, cap, true,
               cfg_.log_enabled, log_dir, cfg_.name);
}

SshBridge::~SshBridge() {
    stop();
    core_.shutdown();
}

SshTarget SshBridge::target() const {
    SshTarget t;
    t.host = cfg_.host;
    t.port = cfg_.port;
    t.username = cfg_.username;
    t.password = cfg_.password;
    t.term = cfg_.term;
    t.width = cfg_.width;
    t.height = cfg_.height;
    int wc = want_cols_.load();
    int wr = want_rows_.load();
    if (wc > 1 && wr > 1) {
        t.width = wc;
        t.height = wr;
    }
    t.connect_timeout_ms = cfg_.connect_timeout_ms;
    t.keepalive_s = cfg_.keepalive_s;
    return t;
}

bool SshBridge::start() {
    if (rx_thread_.joinable() || tx_thread_.joinable()) return true;
    stop_ = false;
    retry_now_ = false;
    cmds_.reset();
    set_state("connecting");
    rx_thread_ = std::thread(&SshBridge::rx_loop, this);
    tx_thread_ = std::thread(&SshBridge::tx_loop, this);
    return true;
}

void SshBridge::stop() {
    stop_ = true;
    cmds_.close();
    {
        std::lock_guard<std::mutex> lk(retry_mu_);
        retry_cv_.notify_all();
    }
    sess_.close();
    if (rx_thread_.joinable()) rx_thread_.join();
    if (tx_thread_.joinable()) tx_thread_.join();
    connected_ = false;
    set_state("stopped");
}

void SshBridge::reconnect() {
    retry_now_ = true;
    connected_ = false;
    sess_.close();
    std::lock_guard<std::mutex> lk(retry_mu_);
    retry_cv_.notify_all();
}

void SshBridge::apply_config(const SSHLinkConfig& cfg) {
    std::string old;
    bool was_running = rx_thread_.joinable() || tx_thread_.joinable();
    if (was_running) core_.log().read_all(&old);
    stop();

    cfg_ = cfg;
    std::size_t cap = cfg_.buffer_size ? cfg_.buffer_size : (1u << 20);
    std::string log_dir = cfg_.log_dir;
    if (log_dir.empty()) log_dir = util::join_path(project_root_, "work\\logs");
    core_.reinit(cfg_.name.empty() ? std::string("ssh") : cfg_.name, cap, true,
                 cfg_.log_enabled, log_dir, cfg_.name);
    if (!old.empty()) core_.log().append(old.data(), old.size());
    { std::lock_guard<std::mutex> lk(err_mu_); error_.clear(); }
    start();
}

std::size_t SshBridge::write(const void* data, std::size_t n) {
    if (data == nullptr || n == 0) return 0;
    // 断线期间不接收下行，避免 write() 声称成功但命令被 TX 线程丢弃
    if (stop_.load() || !connected_.load()) return 0;
    if (!cmds_.push(data, n)) return 0;
    core_.mark_tx(n);
    return n;
}

bool SshBridge::resize_terminal(int cols, int rows) {
    if (cols < 2 || rows < 2) return false;
    want_cols_ = cols;
    want_rows_ = rows;
    if (stop_.load() || !connected_.load()) return true;   // 已记住，重连建 PTY 时生效
    return sess_.resize_pty(cols, rows);
}

std::string SshBridge::read_all(std::uint64_t* epoch, std::uint64_t* seen) {
    std::string out;
    core_.log().read_all(&out);
    if (epoch) *epoch = core_.log().epoch();
    if (seen) *seen = core_.log().total();
    return out;
}

std::string SshBridge::read_since(std::uint64_t* epoch, std::uint64_t* seen) {
    return core_.log().read_since(epoch, seen, nullptr);
}

void SshBridge::clear() { core_.log().clear(); }

void SshBridge::set_disk_log(bool enabled) {
    core_.log().set_disk(enabled, cfg_.log_dir, cfg_.name);
}

void SshBridge::set_error(const std::string& msg) {
    std::lock_guard<std::mutex> lk(err_mu_);
    error_ = msg;
}

std::string SshBridge::last_error() const {
    std::lock_guard<std::mutex> lk(err_mu_);
    return error_;
}

void SshBridge::set_state(const std::string& s) {
    std::lock_guard<std::mutex> lk(err_mu_);
    state_ = s;
}

std::string SshBridge::state() const {
    std::lock_guard<std::mutex> lk(err_mu_);
    return state_;
}

bool SshBridge::wait_connected(unsigned timeout_ms) {
    std::uint64_t t0 = util::now_ms();
    while (util::now_ms() - t0 < timeout_ms) {
        if (connected_.load()) return true;
        Sleep(20);
    }
    return connected_.load();
}

bool SshBridge::wait_disconnected(unsigned timeout_ms) {
    std::uint64_t t0 = util::now_ms();
    while (util::now_ms() - t0 < timeout_ms) {
        if (!connected_.load()) return true;
        Sleep(10);
    }
    return !connected_.load();
}

void SshBridge::rx_loop() {
    unsigned backoff_s = 1;
    char buf[4096];
    while (!stop_.load()) {
        set_state("connecting");
        SshTarget t = target();
        if (sess_.connect(t)) {
            connected_ = true;
            backoff_s = 1;
            reconnects_ += 1;
            set_error("");
            set_state("ready");
            std::uint64_t last_ka = util::now_ms();
            while (!stop_.load() && connected_.load()) {
                int n = sess_.read_some(buf, sizeof(buf));
                if (n < 0) {
                    set_error(sess_.last_error());
                    connected_ = false;
                    break;
                }
                if (n > 0) {
                    core_.on_rx(buf, static_cast<std::size_t>(n));
                    rx_bytes_ += static_cast<std::uint64_t>(n);
                    last_ka = util::now_ms();
                    continue;
                }
                std::uint64_t now = util::now_ms();
                if (cfg_.keepalive_s > 0 &&
                    now - last_ka >= static_cast<std::uint64_t>(cfg_.keepalive_s) * 1000u) {
                    if (!sess_.keepalive()) {
                        set_error(sess_.last_error());
                        connected_ = false;
                        break;
                    }
                    last_ka = now;
                }
                Sleep(1);
            }
            sess_.close();
        } else {
            set_error(sess_.last_error());
            sess_.close();
        }
        connected_ = false;
        if (stop_.load()) break;
        set_state("offline");

        std::unique_lock<std::mutex> lk(retry_mu_);
        bool woken = retry_cv_.wait_for(lk, std::chrono::seconds(backoff_s),
                                        [this] { return stop_.load() || retry_now_.load(); });
        if (stop_.load()) break;
        if (woken && retry_now_.load()) {
            retry_now_ = false;
            backoff_s = 1;
        } else {
            backoff_s = std::min(backoff_s * 2, 30u);
        }
    }
    set_state("stopped");
}

void SshBridge::tx_loop() {
    std::string out;
    while (!stop_.load()) {
        if (!cmds_.pop_all(&out, 100)) {
            if (stop_.load()) break;
            continue;
        }
        if (out.empty()) continue;
        if (!connected_.load()) continue;  // 断线期间丢弃，避免补发陈旧命令
        std::size_t sent = 0;
        while (sent < out.size() && !stop_.load()) {
            std::size_t chunk = std::min<std::size_t>(out.size() - sent, 1024);
            if (!sess_.write_all(out.data() + sent, chunk)) {
                set_error(sess_.last_error());
                connected_ = false;
                sess_.close();
                break;
            }
            sent += chunk;
            tx_bytes_ += static_cast<std::uint64_t>(chunk);
        }
    }
}

std::string SshBridge::status_json() const {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", cfg_.name.c_str());
    cJSON_AddStringToObject(o, "type", "ssh");
    cJSON_AddBoolToObject(o, "running", connected_.load());
    cJSON_AddStringToObject(o, "state", state().c_str());
    std::string peer = cfg_.host + ":" + std::to_string(cfg_.port);
    cJSON_AddStringToObject(o, "peer", peer.c_str());
    cJSON_AddStringToObject(o, "last_error", last_error().c_str());
    cJSON_AddNumberToObject(o, "reconnects", static_cast<double>(reconnects_.load()));
    cJSON_AddNumberToObject(o, "rx_bytes", static_cast<double>(rx_bytes_.load()));
    cJSON_AddNumberToObject(o, "tx_bytes", static_cast<double>(tx_bytes_.load()));
    cJSON_AddNumberToObject(o, "echo_latency_us", static_cast<double>(core_.echo_latency_us()));
    cJSON_AddNumberToObject(o, "echo_count", static_cast<double>(core_.echo_count()));
    cJSON_AddNumberToObject(o, "queue", static_cast<double>(cmds_.depth()));
    cJSON_AddNumberToObject(o, "buffer", static_cast<double>(core_.log().capacity()));
    cJSON_AddBoolToObject(o, "log_enabled", cfg_.log_enabled);
    char* s = cJSON_PrintUnformatted(o);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(o);
    return out;
}

}  // namespace sb3
