// telnet_bridge.h — 原始 TCP Telnet 通道：最小 IAC 协商 + NAWS + 自动重连。

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "core/bridge/bridge.h"
#include "core/channel_core.h"
#include "core/cmd_queue.h"
#include "core/config/config.h"

namespace sb3 {

class TelnetBridge : public IBridge {
public:
    TelnetBridge(const TelnetConfig& cfg, std::string project_root);
    ~TelnetBridge() override;

    bool start() override;
    void stop() override;
    bool running() const override;
    void reconnect() override;

    const std::string& name() const override { return cfg_.name; }
    std::size_t write(const void* data, std::size_t n) override;
    bool resize_terminal(int cols, int rows) override;
    std::uint64_t echo_latency_us() const override { return core_.echo_latency_us(); }
    std::uint64_t echo_count() const override { return core_.echo_count(); }
    std::uint64_t tx_bytes() const override { return core_.tx_bytes(); }
    std::uint64_t rx_bytes() const override { return core_.rx_bytes(); }

    std::string read_since(std::uint64_t* epoch, std::uint64_t* seen) override;
    std::string read_since_ex(std::uint64_t* epoch, std::uint64_t* seen, bool* dropped) override;
    std::string read_all(std::uint64_t* epoch, std::uint64_t* seen) override;
    std::uint64_t log_epoch() const override { return core_.log().epoch(); }
    std::uint64_t log_total() const override { return core_.log().total(); }
    std::uint64_t log_capacity() const override { return core_.log().capacity(); }
    std::uint64_t notify_seq() const override { return core_.log().notify_seq(); }

    void clear() override;
    void set_disk_log(bool enabled) override;
    bool disk_log_enabled() const override { return core_.log().disk_enabled(); }
    std::string disk_log_error() const override { return core_.log().last_error(); }

    void set_kick(std::function<void()> fn);
    void apply_config(const TelnetConfig& cfg);
    std::string status_json() const override;
    std::string last_error() const;
    std::string state() const;

private:
    void rx_loop();
    void tx_loop();
    bool connect_once();
    void close_socket();
    void shutdown_socket();
    bool wait_retry(int ms);
    void send_bytes(const std::string& data);
    void send_iac(unsigned char command, unsigned char option);
    void send_naws();
    std::string feed_telnet(const unsigned char* data, std::size_t n);
    void set_error(const std::string& msg);
    void set_state(const std::string& s);

    TelnetConfig cfg_;
    std::string project_root_;
    mutable ChannelCore core_;
    CmdQueue cmds_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> retry_now_{false};
    std::atomic<std::uintptr_t> sock_{0};
    std::atomic<int> want_cols_{0};
    std::atomic<int> want_rows_{0};

    std::thread rx_thread_;
    std::thread tx_thread_;
    mutable std::mutex sock_mu_;

    mutable std::mutex retry_mu_;
    std::condition_variable retry_cv_;

    std::mutex kick_mu_;
    std::function<void()> kick_;
    std::mutex io_mu_;              // 保护 socket 写与 NAWS

    mutable std::mutex err_mu_;
    std::string error_;
    std::string state_ = "stopped";
    int connects_ = 0;

    // IAC 状态机（仅 RX 线程访问）
    int tel_state_ = 0;
    unsigned char tel_cmd_ = 0;
    bool peer_will_[256] = {};
    bool peer_do_[256] = {};
};

}  // namespace sb3
