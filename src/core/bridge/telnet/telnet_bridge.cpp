#include "core/bridge/telnet/telnet_bridge.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cjson/cJSON.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "core/util/util.h"

namespace sb3 {
namespace {

constexpr unsigned char kIac = 255;
constexpr unsigned char kSe = 240;
constexpr unsigned char kSb = 250;
constexpr unsigned char kWill = 251;
constexpr unsigned char kWont = 252;
constexpr unsigned char kDo = 253;
constexpr unsigned char kDont = 254;

constexpr unsigned char kOptBinary = 0;
constexpr unsigned char kOptEcho = 1;
constexpr unsigned char kOptSga = 3;
constexpr unsigned char kOptNaws = 31;

void winsock_init_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

std::uint16_t be16(int value) {
    if (value < 0) value = 0;
    if (value > 65535) value = 65535;
    std::uint16_t v = static_cast<std::uint16_t>(value);
    return static_cast<std::uint16_t>((v << 8) | (v >> 8));
}

}  // namespace

TelnetBridge::TelnetBridge(const TelnetConfig& cfg, std::string project_root)
    : cfg_(cfg), project_root_(std::move(project_root)) {
    winsock_init_once();
    std::size_t cap = cfg_.buffer_size ? cfg_.buffer_size : (1u << 20);
    std::string log_dir = cfg_.log_dir;
    if (log_dir.empty()) log_dir = util::join_path(project_root_, "work\\logs");
    core_.init(cfg_.name.empty() ? std::string("telnet") : cfg_.name, cap, cfg_.timestamp,
               cfg_.log_enabled, log_dir, cfg_.name);
}

TelnetBridge::~TelnetBridge() {
    stop();
    core_.shutdown();
}

bool TelnetBridge::start() {
    if (rx_thread_.joinable() || tx_thread_.joinable()) return true;
    stop_ = false;
    connected_ = false;
    retry_now_ = false;
    cmds_.reset();
    {
        std::lock_guard<std::mutex> lk(err_mu_);
        error_.clear();
    }
    set_state("connecting");
    rx_thread_ = std::thread(&TelnetBridge::rx_loop, this);
    tx_thread_ = std::thread(&TelnetBridge::tx_loop, this);
    return true;
}

void TelnetBridge::stop() {
    if (!rx_thread_.joinable() && !tx_thread_.joinable() && sock_.load() == 0) {
        stop_ = false;
        return;
    }
    stop_ = true;
    connected_ = false;
    cmds_.close();
    // 先真正关闭 socket，确保阻塞中的 recv 立刻返回；只 shutdown 在 Windows 上
    // 对部分已建立连接不一定能唤醒 recv，join 会永久卡住 UI 线程。
    close_socket();
    {
        std::lock_guard<std::mutex> lk(retry_mu_);
        retry_cv_.notify_all();
    }
    if (rx_thread_.joinable()) rx_thread_.join();
    if (tx_thread_.joinable()) tx_thread_.join();
    close_socket();
    set_state("stopped");
}

bool TelnetBridge::running() const {
    return connected_.load() && !stop_.load();
}

void TelnetBridge::reconnect() {
    retry_now_ = true;
    connected_ = false;
    // 与 stop() 同理：Windows 上 shutdown 不能可靠唤醒阻塞中的 recv，
    // 已连接状态点“重连”必须真正关闭 socket，否则 RX 线程会卡住不再握手。
    close_socket();
    std::lock_guard<std::mutex> lk(retry_mu_);
    retry_cv_.notify_all();
}

void TelnetBridge::close_socket() {
    SOCKET s = static_cast<SOCKET>(sock_.exchange(0));
    if (s != 0 && s != INVALID_SOCKET) closesocket(s);
}

void TelnetBridge::shutdown_socket() {
    SOCKET s = static_cast<SOCKET>(sock_.load());
    if (s != 0 && s != INVALID_SOCKET) shutdown(s, SD_BOTH);
}

bool TelnetBridge::wait_retry(int ms) {
    std::unique_lock<std::mutex> lk(retry_mu_);
    retry_cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] {
        return stop_.load() || retry_now_.load();
    });
    if (stop_.load()) return false;
    if (retry_now_.exchange(false)) return true;
    return true;
}

bool TelnetBridge::connect_once() {
    if (stop_.load() || cfg_.host.empty()) {
        if (!cfg_.host.empty()) set_error("Telnet host is empty");
        return false;
    }
    set_state("connecting");
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    std::string port = std::to_string(cfg_.port);
    int gai = getaddrinfo(cfg_.host.c_str(), port.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        set_error("resolve " + cfg_.host + " failed: " + std::to_string(gai));
        set_state("offline");
        return false;
    }

    bool ok = false;
    for (addrinfo* it = res; it && !ok && !stop_.load(); it = it->ai_next) {
        SOCKET s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
        int rc = connect(s, it->ai_addr, static_cast<int>(it->ai_addrlen));
        bool pending = (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK);
        if (rc == SOCKET_ERROR && !pending) {
            closesocket(s);
            continue;
        }
        if (pending) {
            const auto start = std::chrono::steady_clock::now();
            bool ready = false;
            while (!stop_.load()) {
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(s, &wfds);
                timeval tv{0, 100 * 1000};
                int sel = select(0, nullptr, &wfds, nullptr, &tv);
                if (sel > 0 && FD_ISSET(s, &wfds)) {
                    int err = 0;
                    int len = sizeof(err);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                    ready = (err == 0);
                    break;
                }
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed >= cfg_.connect_timeout_ms) break;
            }
            if (!ready) {
                closesocket(s);
                continue;
            }
        }
        nonblocking = 0;
        ioctlsocket(s, FIONBIO, &nonblocking);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        // 500ms 接收超时：既不影响正常交互，又保证 stop()/关闭 socket 时线程能及时醒来。
        DWORD rcv_timeout = 500;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&rcv_timeout), sizeof(rcv_timeout));
        {
            std::lock_guard<std::mutex> lk(sock_mu_);
            sock_.store(static_cast<std::uintptr_t>(s));
        }
        ok = true;
    }
    freeaddrinfo(res);
    if (!ok) {
        set_error("connect " + cfg_.host + ":" + std::to_string(cfg_.port) + " failed");
        set_state("offline");
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(err_mu_);
        error_.clear();
    }
    connected_ = true;
    ++connects_;
    set_state("ready");
    tel_state_ = 0;
    std::memset(peer_will_, 0, sizeof(peer_will_));
    std::memset(peer_do_, 0, sizeof(peer_do_));

    // 最小协商：本端 SGA/BINARY/NAWS；远端 ECHO/SGA/BINARY。
    peer_will_[kOptEcho] = true;
    peer_will_[kOptSga] = true;
    peer_will_[kOptBinary] = true;
    peer_do_[kOptSga] = true;
    peer_do_[kOptBinary] = true;
    peer_do_[kOptNaws] = true;
    send_iac(kWill, kOptSga);
    send_iac(kDo, kOptEcho);
    send_iac(kDo, kOptSga);
    send_iac(kWill, kOptBinary);
    send_iac(kDo, kOptBinary);
    send_iac(kWill, kOptNaws);
    send_naws();
    return true;
}

void TelnetBridge::rx_loop() {
    int failures = 0;
    while (!stop_.load()) {
        if (!connected_) {
            if (!connect_once()) {
                if (stop_.load()) break;
                int delay = 2000 << std::min(failures > 3 ? 3 : failures, 3);
                if (delay > 15000) delay = 15000;
                ++failures;
                if (!wait_retry(delay)) break;
                continue;
            }
            failures = 0;
        }
        char buf[8192];
        while (!stop_.load() && connected_) {
            SOCKET s = static_cast<SOCKET>(sock_.load());
            if (s == 0 || s == INVALID_SOCKET) break;
            int got = recv(s, buf, sizeof(buf), 0);
            if (got == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue;
                break;
            }
            if (got == 0) break;
            std::string clean = feed_telnet(reinterpret_cast<unsigned char*>(buf),
                                            static_cast<std::size_t>(got));
            if (!clean.empty()) core_.on_rx(clean.data(), clean.size());
        }
        if (stop_.load()) break;
        connected_ = false;
        close_socket();
        set_error("Telnet connection closed");
        set_state("offline");
        if (!wait_retry(2000)) break;
    }
    connected_ = false;
    close_socket();
}

void TelnetBridge::tx_loop() {
    while (!stop_.load()) {
        std::string out;
        if (!cmds_.pop_all(&out, 80) || out.empty()) continue;
        if (!connected_) continue;
        send_bytes(out);
    }
}

std::size_t TelnetBridge::write(const void* data, std::size_t n) {
    if (data == nullptr || n == 0) return 0;
    if (!running()) return 0;
    if (!cmds_.push(data, n)) return 0;
    core_.mark_tx(n);
    return n;
}

bool TelnetBridge::resize_terminal(int cols, int rows) {
    if (cols < 2 || rows < 2) return false;
    want_cols_ = cols;
    want_rows_ = rows;
    if (running()) send_naws();
    return true;
}

void TelnetBridge::send_bytes(const std::string& data) {
    SOCKET s = static_cast<SOCKET>(sock_.load());
    if (s == 0 || s == INVALID_SOCKET) return;
    std::lock_guard<std::mutex> lk(io_mu_);
    std::size_t sent = 0;
    while (sent < data.size() && !stop_.load()) {
        int chunk = static_cast<int>(std::min<std::size_t>(data.size() - sent, 16 * 1024));
        int rc = send(s, data.data() + sent, chunk, 0);
        if (rc == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                Sleep(2);
                continue;
            }
            set_error("Telnet send failed: " + std::to_string(err));
            connected_ = false;
            shutdown_socket();
            return;
        }
        if (rc <= 0) return;
        sent += static_cast<std::size_t>(rc);
    }
}

void TelnetBridge::send_iac(unsigned char command, unsigned char option) {
    std::string data;
    data.push_back(static_cast<char>(kIac));
    data.push_back(static_cast<char>(command));
    data.push_back(static_cast<char>(option));
    send_bytes(data);
}

void TelnetBridge::send_naws() {
    if (!connected_) return;
    int cols = want_cols_.load();
    int rows = want_rows_.load();
    if (cols < 2 || rows < 2) {
        cols = cfg_.width;
        rows = cfg_.height;
    }
    std::string data;
    data.push_back(static_cast<char>(kIac));
    data.push_back(static_cast<char>(kSb));
    data.push_back(static_cast<char>(kOptNaws));
    std::uint16_t cw = be16(cols);
    std::uint16_t ch = be16(rows);
    data.push_back(static_cast<char>(cw & 0xff));
    data.push_back(static_cast<char>((cw >> 8) & 0xff));
    data.push_back(static_cast<char>(ch & 0xff));
    data.push_back(static_cast<char>((ch >> 8) & 0xff));
    data.push_back(static_cast<char>(kIac));
    data.push_back(static_cast<char>(kSe));
    send_bytes(data);
}

std::string TelnetBridge::feed_telnet(const unsigned char* data, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char c = data[i];
        if (tel_state_ == 0) {
            if (c == kIac) tel_state_ = 1;
            else out.push_back(static_cast<char>(c));
            continue;
        }
        if (tel_state_ == 1) {
            if (c == kIac) {
                out.push_back(static_cast<char>(kIac));
                tel_state_ = 0;
            } else if (c == kWill || c == kWont || c == kDo || c == kDont) {
                tel_cmd_ = c;
                tel_state_ = 2;
            } else if (c == kSb) {
                tel_state_ = 3;
            } else {
                tel_state_ = 0;
            }
            continue;
        }
        if (tel_state_ == 2) {
            bool supported_remote = (c == kOptBinary || c == kOptEcho || c == kOptSga);
            bool supported_local = (c == kOptBinary || c == kOptSga || c == kOptNaws);
            if (tel_cmd_ == kWill) {
                if (supported_remote) {
                    if (!peer_will_[c]) {
                        peer_will_[c] = true;
                        send_iac(kDo, c);
                    }
                } else {
                    send_iac(kDont, c);
                }
            } else if (tel_cmd_ == kDo) {
                if (supported_local) {
                    if (!peer_do_[c]) {
                        peer_do_[c] = true;
                        send_iac(kWill, c);
                    }
                    if (c == kOptNaws) send_naws();
                } else {
                    send_iac(kWont, c);
                }
            } else if (tel_cmd_ == kWont) {
                peer_do_[c] = false;
            } else if (tel_cmd_ == kDont) {
                peer_will_[c] = false;
            }
            tel_state_ = 0;
            continue;
        }
        if (tel_state_ == 3) {
            if (c == kIac) tel_state_ = 4;
            continue;
        }
        if (tel_state_ == 4) {
            if (c == kSe) tel_state_ = 0;
            else if (c != kIac) tel_state_ = 3;
            continue;
        }
    }
    return out;
}

std::string TelnetBridge::read_all(std::uint64_t* epoch, std::uint64_t* seen) {
    std::string out;
    core_.log().read_all(&out);
    if (epoch) *epoch = core_.log().epoch();
    if (seen) *seen = core_.log().total();
    return out;
}

std::string TelnetBridge::read_since(std::uint64_t* epoch, std::uint64_t* seen) {
    return core_.log().read_since(epoch, seen, nullptr);
}

std::string TelnetBridge::read_since_ex(std::uint64_t* epoch, std::uint64_t* seen,
                                        bool* dropped) {
    return core_.log().read_since(epoch, seen, dropped);
}

void TelnetBridge::clear() { core_.log().clear(); }

void TelnetBridge::set_disk_log(bool enabled) {
    std::string dir = cfg_.log_dir;
    if (dir.empty()) dir = util::join_path(project_root_, "work\\logs");
    core_.log().set_disk(enabled, dir, cfg_.name);
}

void TelnetBridge::set_kick(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(kick_mu_);
    kick_ = std::move(fn);
    core_.set_kick([this] {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> k(kick_mu_);
            cb = kick_;
        }
        if (cb) cb();
    });
}

void TelnetBridge::apply_config(const TelnetConfig& cfg) {
    stop();
    cfg_ = cfg;
    std::size_t cap = cfg_.buffer_size ? cfg_.buffer_size : (1u << 20);
    std::string log_dir = cfg_.log_dir;
    if (log_dir.empty()) log_dir = util::join_path(project_root_, "work\\logs");
    core_.reinit(cfg_.name.empty() ? std::string("telnet") : cfg_.name, cap, cfg_.timestamp,
                 cfg_.log_enabled, log_dir, cfg_.name);
    {
        std::lock_guard<std::mutex> lk(err_mu_);
        error_.clear();
    }
    start();
}

std::string TelnetBridge::status_json() const {
    cJSON* o = cJSON_CreateObject();
    if (!o) return "{}";
    cJSON_AddStringToObject(o, "kind", "telnet");
    cJSON_AddStringToObject(o, "name", cfg_.name.c_str());
    cJSON_AddStringToObject(o, "state", state().c_str());
    cJSON_AddStringToObject(o, "host", cfg_.host.c_str());
    cJSON_AddNumberToObject(o, "port", cfg_.port);
    cJSON_AddBoolToObject(o, "running", running());
    cJSON_AddNumberToObject(o, "connects", connects_);
    cJSON_AddNumberToObject(o, "echo_count", static_cast<double>(core_.echo_count()));
    cJSON_AddNumberToObject(o, "tx_bytes", static_cast<double>(core_.tx_bytes()));
    cJSON_AddNumberToObject(o, "rx_bytes", static_cast<double>(core_.rx_bytes()));
    cJSON_AddNumberToObject(o, "queue", static_cast<double>(cmds_.depth()));
    cJSON_AddStringToObject(o, "last_error", last_error().c_str());
    char* text = cJSON_PrintUnformatted(o);
    std::string out = text ? text : "{}";
    if (text) cJSON_free(text);
    cJSON_Delete(o);
    return out;
}

void TelnetBridge::set_error(const std::string& msg) {
    std::lock_guard<std::mutex> lk(err_mu_);
    error_ = msg;
}

std::string TelnetBridge::last_error() const {
    std::lock_guard<std::mutex> lk(err_mu_);
    return error_;
}

void TelnetBridge::set_state(const std::string& s) {
    std::lock_guard<std::mutex> lk(err_mu_);
    state_ = s;
}

std::string TelnetBridge::state() const {
    std::lock_guard<std::mutex> lk(err_mu_);
    return state_;
}

}  // namespace sb3
