// channel_core.cpp — 薄封装，保持 header-only 的定义清晰

#include "core/channel_core.h"

namespace sb3 {

bool ChannelCore::init(const std::string& name, std::size_t cap, bool timestamp, bool disk,
                       const std::string& dir, const std::string& file) {
    name_ = name;
    return log_.init(cap, timestamp, disk, dir, file);
}

void ChannelCore::shutdown() {
    log_.shutdown();
}

bool ChannelCore::reinit(const std::string& name, std::size_t cap, bool timestamp, bool disk,
                         const std::string& dir, const std::string& file) {
    name_ = name;
    return log_.init(cap, timestamp, disk, dir, file);
}

void ChannelCore::mark_tx(std::size_t n) {
    tx_bytes_.fetch_add(n, std::memory_order_relaxed);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    tx_mark_us_.store(static_cast<std::uint64_t>(us), std::memory_order_relaxed);
    tx_pending_.store(true, std::memory_order_release);
}

void ChannelCore::on_rx(const void* data, std::size_t n) {
    rx_bytes_.fetch_add(n, std::memory_order_relaxed);
    if (tx_pending_.exchange(false, std::memory_order_acq_rel)) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
        std::uint64_t mark = tx_mark_us_.load(std::memory_order_relaxed);
        std::uint64_t now = static_cast<std::uint64_t>(us);
        if (now >= mark) {
            last_latency_us_.store(now - mark, std::memory_order_relaxed);
            echo_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    log_.append(data, n);
    if (kick_) kick_();
}

}  // namespace sb3
