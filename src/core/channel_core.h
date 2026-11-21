// channel_core.h — 一条通道的数据核心：ring + 唤醒回调。
// RX 泵线程只调 on_rx；读者各持游标读 log()。

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "core/logbuf/logbuf.h"

namespace sb3 {

class ChannelCore {
public:
    bool init(const std::string& name, std::size_t cap, bool timestamp, bool disk,
              const std::string& dir, const std::string& file);
    void shutdown();

    // 热应用：按新参数重建 ring（旧内容由调用方先读出再 append 回来）
    bool reinit(const std::string& name, std::size_t cap, bool timestamp, bool disk,
                const std::string& dir, const std::string& file);

    // RX 唯一入口：追加 ring 后 kick 一次（唤醒所有读者）
    void on_rx(const void* data, std::size_t n);

    // 下行写入前打点；首个回包到达时算“发送→回显”时延（微秒）
    void mark_tx(std::size_t n);
    std::uint64_t echo_latency_us() const { return last_latency_us_.load(std::memory_order_relaxed); }
    std::uint64_t echo_count() const { return echo_count_.load(std::memory_order_relaxed); }
    std::uint64_t tx_bytes() const { return tx_bytes_.load(std::memory_order_relaxed); }
    std::uint64_t rx_bytes() const { return rx_bytes_.load(std::memory_order_relaxed); }

    LogBuffer& log() { return log_; }
    const LogBuffer& log() const { return log_; }
    const std::string& name() const { return name_; }
    void set_kick(std::function<void()> fn) { kick_ = std::move(fn); }

private:
    std::string name_;
    LogBuffer log_;
    std::function<void()> kick_;
    std::atomic<std::uint64_t> tx_bytes_{0};
    std::atomic<std::uint64_t> rx_bytes_{0};
    std::atomic<std::uint64_t> tx_mark_us_{0};
    std::atomic<std::uint64_t> last_latency_us_{0};
    std::atomic<std::uint64_t> echo_count_{0};
    std::atomic<bool> tx_pending_{false};
};

}  // namespace sb3
