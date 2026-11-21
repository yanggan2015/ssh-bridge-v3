// cmd_queue.cpp — 下行写队列实现（从 serial_bridge 抽出，串口与 SSH 共用）。

#include "core/cmd_queue.h"

#include <chrono>

namespace sb3 {

void CmdQueue::push(const void* data, std::size_t n) {
    if (data == nullptr || n == 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) return;
    q_.emplace_back(static_cast<const char*>(data), n);
    cv_.notify_one();
}

bool CmdQueue::pop_all(std::string* out, unsigned wait_ms) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                      [this] { return closed_ || !q_.empty(); })) {
        return false;
    }
    if (q_.empty()) return !closed_;
    out->clear();
    while (!q_.empty()) {
        out->append(q_.front());
        q_.pop_front();
    }
    return true;
}

void CmdQueue::close() {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
    cv_.notify_all();
}

void CmdQueue::reset() {
    std::lock_guard<std::mutex> lock(mu_);
    q_.clear();
    closed_ = false;
}

std::size_t CmdQueue::depth() const {
    std::lock_guard<std::mutex> lock(mu_);
    return q_.size();
}

}  // namespace sb3
