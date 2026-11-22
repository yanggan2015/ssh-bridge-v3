// serial_bridge.cpp — Win32 串口通道实现。
// 读泵：持续 overlapped ReadFile；写：cmdq + writer 线程。

#include "core/bridge/serial/serial_bridge.h"

#include <cjson/cJSON.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "core/util/util.h"

namespace sb3 {
namespace {

constexpr DWORD kEventWaitMs = 150;
constexpr DWORD kBufMax      = 4096;

BYTE parity_of(const std::string& p) {
    if (p == "E" || p == "e") return EVENPARITY;
    if (p == "O" || p == "o") return ODDPARITY;
    if (p == "M" || p == "m") return MARKPARITY;
    if (p == "S" || p == "s") return SPACEPARITY;
    return NOPARITY;
}

BYTE stopbits_of(int x10) {
    if (x10 == 15) return ONE5STOPBITS;
    if (x10 == 20) return TWOSTOPBITS;
    return ONESTOPBIT;
}

// 等一个 overlapped 操作；超时则取消并等待收尾
bool wait_io(HANDLE port, OVERLAPPED* ov, DWORD timeout_ms, DWORD* got) {
    DWORD w = WaitForSingleObject(ov->hEvent, timeout_ms);
    if (w == WAIT_OBJECT_0) {
        return GetOverlappedResult(port, ov, got, FALSE) != 0;
    }
    if (w == WAIT_TIMEOUT || w == WAIT_ABANDONED || w == WAIT_FAILED) {
        CancelIoEx(port, ov);
        // 必须等内核真正收尾再返回：调用方的 OVERLAPPED/缓冲区都在栈上，
        // 若取消还没完成就返回，驱动随后写栈会造成内存破坏。
        DWORD n = 0;
        GetOverlappedResult(port, ov, &n, TRUE);
    }
    if (got) *got = 0;
    return false;
}

}  // namespace

// ---------------- SerialBridge ----------------

SerialBridge::SerialBridge(const BoardConfig& cfg, std::string project_root)
    : cfg_(cfg), project_root_(std::move(project_root)) {
    std::size_t cap = cfg_.buffer_size ? cfg_.buffer_size : (1u << 20);
    std::string log_dir = cfg_.log_dir;
    if (log_dir.empty()) log_dir = util::join_path(project_root_, "work\\logs");
    bool disk = cfg_.log_enabled;
    core_.init(cfg_.name.empty() ? std::string("serial") : cfg_.name, cap,
               cfg_.timestamp, disk, log_dir, cfg_.name);
}

SerialBridge::~SerialBridge() {
    stop();
    core_.shutdown();
}

bool SerialBridge::open_port() {
    std::string dev = cfg_.serial_device;
    if (dev.rfind("loop:", 0) == 0) {     // 内置回环端点：自检用，不走 Win32 串口
        loop_ = true;
        return true;
    }
    if (dev.empty()) {
        set_error("serial_device is empty");
        return false;
    }
    std::wstring path = std::wstring(L"\\\\.\\") + std::wstring(dev.begin(), dev.end());
    if (dev.rfind("\\\\", 0) == 0) {
        // 传入的已是 \\.\COMx，直接转宽字符
        path.assign(dev.begin(), dev.end());
    }
    port_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (port_ == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "open %s failed: error %lu%s", dev.c_str(),
                      static_cast<unsigned long>(e),
                      e == ERROR_ACCESS_DENIED ? " (port busy)" : "");
        set_error(buf);
        return false;
    }

    SetupComm(port_, 8192, 8192);
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(port_, &dcb)) {
        set_error("GetCommState failed");
        close_port();
        return false;
    }
    dcb.BaudRate = static_cast<DWORD>(cfg_.baudrate > 0 ? cfg_.baudrate : 115200);
    dcb.ByteSize = static_cast<BYTE>(cfg_.databits >= 5 && cfg_.databits <= 8 ? cfg_.databits : 8);
    dcb.Parity = parity_of(cfg_.parity);
    dcb.StopBits = stopbits_of(cfg_.stopbits_x10);
    dcb.fBinary = TRUE;
    dcb.fParity = (dcb.Parity != NOPARITY);

    bool rtscts = (cfg_.flowcontrol == "rtscts");
    bool xonxoff = (cfg_.flowcontrol == "xonxoff");
    dcb.fOutxCtsFlow = rtscts ? TRUE : FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fRtsControl = rtscts ? RTS_CONTROL_HANDSHAKE
                             : (cfg_.rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE);
    dcb.fDtrControl = cfg_.dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    dcb.fOutX = xonxoff ? TRUE : FALSE;
    dcb.fInX = xonxoff ? TRUE : FALSE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(port_, &dcb)) {
        set_error("SetCommState failed");
        close_port();
        return false;
    }
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 100;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 100;
    SetCommTimeouts(port_, &to);
    PurgeComm(port_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

void SerialBridge::close_port() {
    if (port_ != INVALID_HANDLE_VALUE) {
        CloseHandle(port_);
        port_ = INVALID_HANDLE_VALUE;
    }
}

void SerialBridge::set_error(const std::string& msg) {
    std::lock_guard<std::mutex> lk(err_mu_);
    last_error_ = msg;
}

bool SerialBridge::start() {
    if (running_.load()) return true;
    if (!open_port()) return false;
    stop_.store(false);
    cmds_.reset();               // stop() 会关闭队列，重启要重新打开
    running_.store(true);
    if (loop_) {
        rx_thread_ = std::thread([this] { loop_rx_loop(); });
    } else {
        rx_thread_ = std::thread([this] { rx_loop(); });
    }
    writer_thread_ = std::thread([this] { writer_loop(); });
    return true;
}

void SerialBridge::stop() {
    if (!running_.load()) {
        close_port();
        return;
    }
    stop_.store(true);
    cmds_.close();
    {
        std::lock_guard<std::mutex> lock(loop_mu_);
        loop_cv_.notify_all();
    }
    if (port_ != INVALID_HANDLE_VALUE) CancelIoEx(port_, nullptr);
    if (rx_thread_.joinable()) rx_thread_.join();
    if (writer_thread_.joinable()) writer_thread_.join();
    core_.log().flush_cr_hold();
    close_port();
    running_.store(false);
}

void SerialBridge::reconnect() {
    stop();
    cmds_.reset();
    start();
}

void SerialBridge::apply_config(const BoardConfig& cfg) {
    std::string old;
    bool was_running = running_.load();
    if (was_running) core_.log().read_all(&old);
    stop();

    cfg_ = cfg;
    loop_ = false;
    std::size_t cap = cfg_.buffer_size ? cfg_.buffer_size : (1u << 20);
    std::string log_dir = cfg_.log_dir;
    if (log_dir.empty()) log_dir = util::join_path(project_root_, "work\\logs");
    core_.reinit(cfg_.name.empty() ? std::string("serial") : cfg_.name, cap,
                 cfg_.timestamp, cfg_.log_enabled, log_dir, cfg_.name);
    if (!old.empty()) core_.log().append(old.data(), old.size());

    {
        std::lock_guard<std::mutex> lk(err_mu_);
        last_error_.clear();
    }
    if (was_running || cfg_.auto_open) start();
}

void SerialBridge::rx_loop() {
    // 持续挂一个 overlapped ReadFile，数据到达后交给 ring，再立刻重挂。
    // 不能把 WaitCommEvent 和 ReadFile 共用事件并在超时后取消：CH340 / CP210x
    // 在“先等到 EV_RXCHAR，再 ResetEvent 读”这条路径上会丢掉本次读完成，
    // 表现为端口已打开、tx 正常，但 rx 永远为 0。
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        set_error("CreateEvent failed in rx");
        return;
    }
    char buf[kBufMax];
    while (!stop_.load()) {
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        BOOL ok = ReadFile(port_, buf, kBufMax, &got, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) {
                if (!stop_.load()) {
                    char msg[96];
                    std::snprintf(msg, sizeof(msg), "ReadFile failed: error %lu",
                                  static_cast<unsigned long>(GetLastError()));
                    set_error(msg);
                }
                break;
            }
            DWORD w = WAIT_OBJECT_0;
            for (;;) {
                w = WaitForSingleObject(ov.hEvent, kEventWaitMs);
                if (w == WAIT_OBJECT_0) break;
                if (w == WAIT_TIMEOUT) {
                    if (stop_.load()) {
                        CancelIoEx(port_, &ov);
                        DWORD n = 0;
                        GetOverlappedResult(port_, &ov, &n, TRUE);
                        break;
                    }
                    core_.log().flush_cr_hold();
                    continue;
                }
                CancelIoEx(port_, &ov);
                DWORD n = 0;
                GetOverlappedResult(port_, &ov, &n, TRUE);
                break;
            }
            if (w != WAIT_OBJECT_0) break;
            if (!GetOverlappedResult(port_, &ov, &got, FALSE)) {
                if (!stop_.load()) {
                    char msg[96];
                    std::snprintf(msg, sizeof(msg), "ReadFile overlapped failed: error %lu",
                                  static_cast<unsigned long>(GetLastError()));
                    set_error(msg);
                    continue;
                }
                break;
            }
        }
        if (got == 0) {
            Sleep(1);
            continue;
        }
        std::string chunk(buf, got);
        if (util::lower(cfg_.encoding) == "gbk") {
            chunk = util::gbk_to_utf8(chunk);
        }
        rx_bytes_.fetch_add(got);
        core_.on_rx(chunk.data(), chunk.size());
    }
    CloseHandle(ov.hEvent);
}

void SerialBridge::loop_rx_loop() {
    while (!stop_.load()) {
        std::string batch;
        {
            std::unique_lock<std::mutex> lock(loop_mu_);
            loop_cv_.wait_for(lock, std::chrono::milliseconds(kEventWaitMs),
                              [this] { return stop_.load() || !loop_in_.empty(); });
            if (stop_.load()) break;
            while (!loop_in_.empty()) {
                batch += loop_in_.front();
                loop_in_.pop_front();
            }
        }
        if (batch.empty()) {
            core_.log().flush_cr_hold();
            continue;
        }
        rx_bytes_.fetch_add(batch.size());
        core_.on_rx(batch.data(), batch.size());
    }
}

void SerialBridge::writer_loop() {
    while (!stop_.load()) {
        std::string batch;
        if (!cmds_.pop_all(&batch, 100)) {
            continue;
        }
        if (batch.empty()) continue;
        std::string out = batch;
        if (util::lower(cfg_.encoding) == "gbk") {
            out = util::utf8_to_gbk(batch);
        }
        if (loop_) {
            std::lock_guard<std::mutex> lock(loop_mu_);
            loop_in_.push_back(out);
            loop_cv_.notify_all();
            continue;
        }
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) continue;
        DWORD wrote = 0;
        BOOL ok = WriteFile(port_, out.data(), static_cast<DWORD>(out.size()), &wrote, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            wait_io(port_, &ov, 200, &wrote);
        }
        tx_bytes_.fetch_add(wrote);
        CloseHandle(ov.hEvent);
    }
}

std::size_t SerialBridge::write(const void* data, std::size_t n) {
    if (!running_.load() || data == nullptr || n == 0) return 0;
    if (!cmds_.push(data, n)) return 0;
    core_.mark_tx(n);
    return n;
}

std::string SerialBridge::read_all(std::uint64_t* epoch, std::uint64_t* seen) {
    std::string out;
    core_.log().read_all(&out);
    if (epoch) *epoch = core_.log().epoch();
    if (seen) *seen = core_.log().total();
    return out;
}

std::string SerialBridge::read_since(std::uint64_t* epoch, std::uint64_t* seen) {
    return core_.log().read_since(epoch, seen, nullptr);
}

void SerialBridge::clear() {
    core_.log().clear();
}

void SerialBridge::set_disk_log(bool enabled) {
    std::string dir = cfg_.log_dir;
    if (dir.empty()) dir = util::join_path(project_root_, "work\\logs");
    core_.log().set_disk(enabled, dir, cfg_.name);
}

std::string SerialBridge::status_json() const {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", cfg_.name.c_str());
    cJSON_AddStringToObject(o, "type", loop_ ? "serial-loop" : "serial");
    cJSON_AddBoolToObject(o, "running", running_.load());
    cJSON_AddStringToObject(o, "port", cfg_.serial_device.c_str());
    cJSON_AddNumberToObject(o, "baudrate", cfg_.baudrate);
    cJSON_AddNumberToObject(o, "buffer", static_cast<double>(core_.log().capacity()));
    cJSON_AddNumberToObject(o, "total", static_cast<double>(core_.log().total()));
    cJSON_AddNumberToObject(o, "epoch", static_cast<double>(core_.log().epoch()));
    cJSON_AddNumberToObject(o, "rx_bytes", static_cast<double>(core_.rx_bytes()));
    cJSON_AddNumberToObject(o, "tx_bytes", static_cast<double>(core_.tx_bytes()));
    cJSON_AddNumberToObject(o, "echo_latency_us", static_cast<double>(core_.echo_latency_us()));
    cJSON_AddNumberToObject(o, "echo_count", static_cast<double>(core_.echo_count()));
    cJSON_AddBoolToObject(o, "log_enabled", core_.log().disk_enabled());
    std::string err = last_error();
    cJSON_AddStringToObject(o, "last_error", err.c_str());
    char* s = cJSON_PrintUnformatted(o);
    std::string out = s ? s : "{}";
    if (s) cJSON_free(s);
    cJSON_Delete(o);
    return out;
}

}  // namespace sb3
