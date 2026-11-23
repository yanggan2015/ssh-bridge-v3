// logbuf.h — 环形日志（每通道一份）。
// 写者只有 RX 泵线程；读者各持 epoch/seen 游标，无锁读。
// 关键不变式：ring 里只放设备上行字节（下行永不写入），时间戳在写 ring 前打一次。

#pragma once

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sb3 {

class LogBuffer {
public:
    LogBuffer() = default;
    ~LogBuffer();

    // cap = ring 容量（字节）；dir/name 落盘用（dir 为空则用 cwd）
    bool init(std::size_t cap, bool timestamp, bool disk,
              const std::string& dir, const std::string& name);

    // 热应用配置：保留已加工的历史回显，只替换容量/时间戳/落盘目标。
    // 历史不会重新打时间戳，也不会重复写入新日志文件；epoch 单调递增。
    bool reinit(std::size_t cap, bool timestamp, bool disk,
                const std::string& dir, const std::string& name);

    // 停线程、关文件；可重复调用
    void shutdown();

    // RX 唯一入口
    void append(const void* data, std::size_t n);

    // 块尾裸 CR 的暂存提交（RX 超时、停止前各调一次）
    void flush_cr_hold();

    // 读增量：调用方首次传 epoch=0/seen=0；epoch 变化表示被清空过。
    // dropped=true 表示调用方落后超过容量，最老的数据已丢。
    std::string read_since(std::uint64_t* epoch, std::uint64_t* seen, bool* dropped);

    // 全量读：取 ring 里现存的所有字节（最多 cap），供"加载历史会话"重放用。
    // 不改变任何读者游标；out 里的内容可能从行中间开始（最老字节已被覆盖）。
    void read_all(std::string* out);

    // 清空：epoch+1、total 归零、notify+1
    void clear();

    std::uint64_t notify_seq() const { return notify_.load(std::memory_order_acquire); }
    std::uint64_t total() const { return total_.load(std::memory_order_acquire); }
    std::uint64_t epoch() const { return epoch_.load(std::memory_order_acquire); }
    std::size_t capacity() const { return cap_; }
    bool disk_enabled() const;

    // 运行中开关落盘（会重建文件）
    bool set_disk(bool enabled, const std::string& dir, const std::string& name);

    std::string last_error() const;

private:
    void write_ring(const char* data, std::size_t n);
    void build_payload(const char* data, std::size_t n, std::vector<char>* out);
    void import_history(const char* data, std::size_t n);
    void scan_state(const char* data, std::size_t n);
    void disk_write(const char* data, std::size_t n);
    bool disk_open(const std::string& dir, const std::string& name);
    void disk_close();
    void disk_close_locked();
    void flush_loop();

    std::size_t cap_ = 0;
    std::vector<char> ring_;
    std::atomic<std::uint64_t> total_{0};
    std::atomic<std::uint64_t> notify_{0};
    std::atomic<std::uint64_t> epoch_{1};

    bool timestamp_ = false;
    std::string cr_hold_;          // 至多 1 个字节

    // 时间戳状态机：只在行首打，且跨过 ANSI 序列后再打
    int ansi_state_ = 0;           // 0 普通 / 1 ESC / 2 CSI / 3 OSC
    bool at_line_start_ = true;

    // 落盘
    std::atomic<bool> disk_flush_stop_{false};
    std::atomic<bool> dirty_{false};
    std::FILE* disk_ = nullptr;
    std::uint64_t disk_bytes_ = 0;          // 当前日志文件大小，disk_mu_ 保护

    mutable std::mutex ring_mu_;            // 保护 ring_/total_/epoch_ 与读取拷贝
    mutable std::mutex disk_mu_;
    std::thread flusher_;

    mutable std::mutex err_mu_;             // 保护 last_error_
    std::string last_error_;
};

}  // namespace sb3
