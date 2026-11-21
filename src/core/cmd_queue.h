// cmd_queue.h — 下行写队列（串口与 SSH 共用，S1 起）。
// 调用方只 push（不阻塞、不落盘），writer 线程按 wait_ms 批量取走。

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace sb3 {

class CmdQueue {
public:
    void push(const void* data, std::size_t n);
    bool pop_all(std::string* out, unsigned wait_ms);
    void close();
    void reset();
    std::size_t depth() const;

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> q_;
    bool closed_ = false;
};

}  // namespace sb3
