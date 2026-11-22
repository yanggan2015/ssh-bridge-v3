// ssh_bridge.h — SSH 出站通道（S2）：与串口共用 IBridge 契约。
// RX 线程独占会话读，TX 线程走同一 CmdQueue；断线指数退避重连，只影响本通道。

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "core/bridge/bridge.h"
#include "core/channel_core.h"
#include "core/cmd_queue.h"
#include "core/config/config.h"
#include "core/ssh/ssh_session.h"

namespace sb3 {

class SshBridge : public IBridge {
public:
    SshBridge(const SSHLinkConfig& cfg, std::string project_root);
    ~SshBridge() override;

    bool start() override;
    void stop() override;
    bool running() const override { return connected_.load(); }
    void reconnect() override;

    // 热应用：换目标/凭据/尺寸，保留 ring 内容（重连一次）
    void apply_config(const SSHLinkConfig& cfg);

    const std::string& name() const override { return cfg_.name; }

    std::size_t write(const void* data, std::size_t n) override;
    std::uint64_t echo_latency_us() const override { return core_.echo_latency_us(); }
    std::uint64_t echo_count() const override { return core_.echo_count(); }
    std::uint64_t tx_bytes() const override { return core_.tx_bytes(); }
    std::uint64_t rx_bytes() const override { return core_.rx_bytes(); }
    std::string read_since(std::uint64_t* epoch, std::uint64_t* seen) override;
    std::string read_all(std::uint64_t* epoch, std::uint64_t* seen) override;
    std::uint64_t notify_seq() const override { return core_.log().notify_seq(); }

    void clear() override;
    void set_disk_log(bool enabled) override;
    std::string status_json() const override;

    void set_kick(std::function<void()> fn) { core_.set_kick(std::move(fn)); }

    // 测试与状态栏用
    bool wait_connected(unsigned timeout_ms);
    bool wait_disconnected(unsigned timeout_ms);
    std::string last_error() const;
    std::string state() const;
    std::uint64_t reconnect_count() const { return reconnects_.load(); }
    std::size_t queue_depth() const { return cmds_.depth(); }

private:
    void rx_loop();
    void tx_loop();
    void set_error(const std::string& msg);
    void set_state(const std::string& s);
    SshTarget target() const;

    SSHLinkConfig cfg_;
    std::string   project_root_;
    ChannelCore   core_;
    SshSession    sess_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> retry_now_{false};
    std::atomic<std::uint64_t> rx_bytes_{0};
    std::atomic<std::uint64_t> tx_bytes_{0};
    std::atomic<std::uint64_t> reconnects_{0};

    std::thread rx_thread_;
    std::thread tx_thread_;
    CmdQueue    cmds_;

    mutable std::mutex err_mu_;
    std::string error_;
    std::string state_ = "idle";

    std::mutex              retry_mu_;
    std::condition_variable retry_cv_;
};

}  // namespace sb3
