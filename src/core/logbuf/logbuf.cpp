// logbuf.cpp — 环形日志实现。单写多读，读侧不加锁。

#include "core/logbuf/logbuf.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "core/util/util.h"

namespace sb3 {
namespace {

constexpr std::size_t kMinCap = 4 * 1024;

}  // namespace

LogBuffer::~LogBuffer() {
    shutdown();
}

bool LogBuffer::init(std::size_t cap, bool timestamp, bool disk,
                     const std::string& dir, const std::string& name) {
    shutdown();
    cap_ = std::max(cap, kMinCap);
    ring_.assign(cap_, '\0');
    total_.store(0);
    notify_.store(0);
    epoch_.store(1);
    timestamp_ = timestamp;
    ansi_state_ = 0;
    at_line_start_ = true;
    cr_hold_.clear();
    last_error_.clear();

    if (disk && !disk_open(dir, name)) {
        return false;
    }
    return true;
}

void LogBuffer::shutdown() {
    disk_flush_stop_.store(true);
    if (flusher_.joinable()) flusher_.join();
    flush_cr_hold();
    disk_close();
    cap_ = 0;
    ring_.clear();
}

void LogBuffer::append(const void* data, std::size_t n) {
    if (cap_ == 0 || data == nullptr || n == 0) return;

    std::string chunk;
    if (!cr_hold_.empty()) {
        chunk.swap(cr_hold_);
    }
    const char* p = static_cast<const char*>(data);
    chunk.append(p, n);

    // 块尾裸 CR 先扣住，等下一批字节判断 CRLF / CR 重写
    if (chunk.size() >= 1 && chunk.back() == '\r') {
        cr_hold_ = "\r";
        chunk.pop_back();
    }
    if (chunk.empty()) return;

    std::vector<char> payload;
    build_payload(chunk.data(), chunk.size(), &payload);
    write_ring(payload.data(), payload.size());
    disk_write(payload.data(), payload.size());
}

void LogBuffer::flush_cr_hold() {
    if (cr_hold_.empty() || cap_ == 0) return;
    std::string one;
    one.swap(cr_hold_);
    std::vector<char> payload;
    build_payload(one.data(), one.size(), &payload);
    write_ring(payload.data(), payload.size());
    disk_write(payload.data(), payload.size());
}

void LogBuffer::build_payload(const char* data, std::size_t n, std::vector<char>* out) {
    out->clear();
    out->reserve(n + 40);
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        bool printable = (c >= 0x20) || (c == '\t');
        if (timestamp_ && at_line_start_ && ansi_state_ == 0 && printable) {
            std::string ts = util::log_time_stamp();
            out->insert(out->end(), ts.begin(), ts.end());
            at_line_start_ = false;
        }
        out->push_back(static_cast<char>(c));
        if (c == 0x1b) {
            ansi_state_ = 1;
        } else if (ansi_state_ == 1) {
            ansi_state_ = (c == '[') ? 2 : (c == ']') ? 3 : 0;
        } else if (ansi_state_ == 2) {
            if (c >= 0x40 && c <= 0x7e) ansi_state_ = 0;
        } else if (ansi_state_ == 3) {
            if (c == 0x07) ansi_state_ = 0;
        } else if (c == '\n') {
            at_line_start_ = true;
        } else if (c != '\r') {
            at_line_start_ = false;
        }
    }
}

void LogBuffer::write_ring(const char* data, std::size_t n) {
    if (cap_ == 0 || n == 0) return;
    if (n > cap_) {                       // 一次超过整个容量，只保留最新一段
        data += (n - cap_);
        n = cap_;
    }
    std::uint64_t start = total_.load(std::memory_order_relaxed);
    std::size_t off = util::ring_index(start, cap_);
    std::size_t first = std::min(n, cap_ - off);
    std::memcpy(ring_.data() + off, data, first);
    if (n > first) {
        std::memcpy(ring_.data(), data + first, n - first);
    }
    // 先落字节，再发布 total：读者按 total 取数据
    total_.store(start + n, std::memory_order_release);
    notify_.fetch_add(1, std::memory_order_release);
}

std::string LogBuffer::read_since(std::uint64_t* epoch, std::uint64_t* seen, bool* dropped) {
    bool drop = false;
    std::string out;
    if (epoch == nullptr || seen == nullptr) return out;

    std::uint64_t e = epoch_.load(std::memory_order_acquire);
    std::uint64_t t = total_.load(std::memory_order_acquire);
    if (*epoch != e) {                    // 首次读，或 ring 被清空
        *epoch = e;
        *seen = t;                        // 从当前时刻开始，不补历史
    }
    std::uint64_t s = *seen;
    if (s > t) s = t;                     // 游标被夹紧
    if (t - s > cap_) {
        s = t - cap_;
        drop = true;
    }
    std::size_t len = static_cast<std::size_t>(t - s);
    if (len > 0) {
        out.resize(len);
        std::size_t off = util::ring_index(s, cap_);
        std::size_t first = std::min(len, cap_ - off);
        std::memcpy(out.data(), ring_.data() + off, first);
        if (len > first) {
            std::memcpy(out.data() + first, ring_.data(), len - first);
        }
    }
    *seen = t;
    if (dropped) *dropped = drop;
    return out;
}

void LogBuffer::read_all(std::string* out) {
    if (out == nullptr) return;
    out->clear();
    std::uint64_t t = total_.load(std::memory_order_acquire);
    if (t == 0 || cap_ == 0) return;
    std::size_t n = static_cast<std::size_t>(t < cap_ ? t : cap_);
    std::uint64_t start = t - n;
    std::size_t off = util::ring_index(start, cap_);
    out->resize(n);
    std::size_t first = (n < cap_ - off) ? n : (cap_ - off);
    std::memcpy(out->data(), ring_.data() + off, first);
    if (n > first) std::memcpy(out->data() + first, ring_.data(), n - first);
}

void LogBuffer::clear() {
    total_.store(0, std::memory_order_release);
    epoch_.fetch_add(1, std::memory_order_release);
    notify_.fetch_add(1, std::memory_order_release);
}

bool LogBuffer::disk_enabled() const {
    return disk_ != nullptr;
}

bool LogBuffer::disk_open(const std::string& dir, const std::string& name) {
    disk_close();                    // 自带锁，必须在取锁之前调用，避免自锁
    std::string d = dir.empty() ? std::string(".") : dir;
    if (!util::ensure_dir(d)) {
        last_error_ = "cannot create log dir: " + d;
        return false;
    }
    std::string file = util::join_path(d, name + ".log");
    std::FILE* f = std::fopen(file.c_str(), "ab");
    if (!f) {
        last_error_ = "cannot open log file: " + file;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(disk_mu_);
        disk_ = f;
    }
    disk_flush_stop_.store(false);
    if (!flusher_.joinable()) {
        flusher_ = std::thread([this] { flush_loop(); });
    }
    return true;
}

void LogBuffer::disk_close() {
    std::lock_guard<std::mutex> lock(disk_mu_);
    if (disk_) {
        std::fflush(disk_);
        std::fclose(disk_);
        disk_ = nullptr;
    }
}

bool LogBuffer::set_disk(bool enabled, const std::string& dir, const std::string& name) {
    if (!enabled) {
        disk_flush_stop_.store(true);
        if (flusher_.joinable()) flusher_.join();
        disk_close();
        return true;
    }
    return disk_open(dir, name);
}

void LogBuffer::disk_write(const char* data, std::size_t n) {
    if (!disk_ || n == 0) return;
    std::lock_guard<std::mutex> lock(disk_mu_);
    if (!disk_) return;
    std::fwrite(data, 1, n, disk_);
    dirty_.store(true, std::memory_order_release);
}

void LogBuffer::flush_loop() {
    while (!disk_flush_stop_.load(std::memory_order_acquire)) {
        Sleep(250);
        if (!dirty_.exchange(false, std::memory_order_acq_rel)) continue;
        std::lock_guard<std::mutex> lock(disk_mu_);
        if (disk_) std::fflush(disk_);
    }
}

}  // namespace sb3
