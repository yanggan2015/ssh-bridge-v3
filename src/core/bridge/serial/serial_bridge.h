// serial_bridge.h — 串口通道：overlapped 读泵 + 写队列 + 重连。
// 只依赖 core；没有窗口、没有端口。S1 的唯一通道实现。

#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "core/bridge/bridge.h"
#include "core/channel_core.h"
#include "core/cmd_queue.h"
#include "core/config/config.h"

namespace sb3 {

class SerialBridge : public IBridge {
public:
    SerialBridge(const BoardConfig& cfg, std::string project_root);
    ~SerialBridge() override;

    bool start() override;
    void stop() override;
    bool running() const override { return running_.load() && io_alive_.load(); }
    void reconnect() override;

    // 热应用：换 DCB / 行尾 / ring 参数，保留 ring 内容（旧内容重灌）
    void apply_config(const BoardConfig& cfg);

    // C7：串口生命周期与线控。loop: 端点只记录状态，不触碰真实硬件。
    bool set_lines(bool dtr, bool rts, std::string* err);
    bool send_break(int duration_ms, std::string* err);
    std::string lines_json() const;
    bool is_loop() const { return loop_; }

    const std::string& name() const override { return cfg_.name; }

    std::size_t write(const void* data, std::size_t n) override;
    std::uint64_t echo_latency_us() const override { return core_.echo_latency_us(); }
    std::uint64_t echo_count() const override { return core_.echo_count(); }
    std::uint64_t tx_bytes() const override { return core_.tx_bytes(); }
    std::uint64_t rx_bytes() const override { return core_.rx_bytes(); }
    std::string read_since(std::uint64_t* epoch, std::uint64_t* seen) override;
    std::string read_since_ex(std::uint64_t* epoch, std::uint64_t* seen, bool* dropped) override;
    std::uint64_t log_epoch() const override { return core_.log().epoch(); }
    std::uint64_t log_total() const override { return core_.log().total(); }
    std::uint64_t log_capacity() const override { return core_.log().capacity(); }
    std::string read_all(std::uint64_t* epoch, std::uint64_t* seen) override;
    std::uint64_t notify_seq() const override { return core_.log().notify_seq(); }

    void clear() override;
    void set_disk_log(bool enabled) override;
    bool disk_log_enabled() const override { return core_.log().disk_enabled(); }
    std::string disk_log_error() const override { return core_.log().last_error(); }
    std::string status_json() const override;

    // 唤醒读者（UI PostMessage / HTTP kick / 入站队列）
    void set_kick(std::function<void()> fn) { core_.set_kick(std::move(fn)); }

    std::string last_error() const {
        std::lock_guard<std::mutex> lk(err_mu_);
        return last_error_;
    }

private:
    bool open_port();
    void close_port();
    void rx_loop();
    void loop_rx_loop();
    void writer_loop();
    void set_error(const std::string& msg);

    BoardConfig cfg_;
    std::string project_root_;
    ChannelCore core_;

    HANDLE port_ = INVALID_HANDLE_VALUE;
    std::atomic<bool> running_{false};
    std::atomic<bool> io_alive_{false};       // RX 线程存活；退出即视为掉线
    std::atomic<bool> stop_{false};
    std::thread rx_thread_;
    std::thread writer_thread_;
    CmdQueue cmds_;
    bool loop_ = false;                       // "loop:" 设备：内置回环端点（无板自检用）
    std::mutex loop_mu_;
    std::condition_variable loop_cv_;
    std::deque<std::string> loop_in_;
    std::atomic<std::uint64_t> rx_bytes_{0};
    std::atomic<std::uint64_t> tx_bytes_{0};
    std::atomic<bool> dtr_{true};
    std::atomic<bool> rts_{true};
    std::atomic<int> last_break_ms_{0};
    mutable std::mutex err_mu_;
    std::string last_error_;
};

}  // namespace sb3
