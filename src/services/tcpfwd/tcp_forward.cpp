#include "services/tcpfwd/tcp_forward.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <cjson/cJSON.h>

namespace sb3 {
namespace {

constexpr int kSlots = 256;

bool ensure_wsa() {
    static std::atomic<int> state{0};  // 0=未初始化 1=已初始化 -1=失败
    int s = state.load();
    if (s == 1) return true;
    if (s == -1) return false;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        state.store(-1);
        return false;
    }
    state.store(1);
    return true;
}

bool send_all(SOCKET s, const char* p, int n) {
    while (n > 0) {
        int w = ::send(s, p, n, 0);
        if (w <= 0) return false;
        p += w;
        n -= w;
    }
    return true;
}

}  // namespace

struct TcpForwardImpl {
    std::atomic<bool> stop{false};
    SOCKET listeners[kSlots] = {};
    std::mutex mu;
    std::set<std::pair<SOCKET, SOCKET>> conns;
    std::atomic<int> active{0};
    std::atomic<unsigned long long> accepted{0};
    std::atomic<unsigned long long> bytes{0};
    std::atomic<unsigned long long> rejected{0};
    std::string error;
    std::string cidr;
    std::string listen_ip;
    int port_base = 22000;
    int target_port = 22;
    int listen_count = 0;
    std::thread accept_thread;

    void close_listeners() {
        for (int i = 0; i < kSlots; ++i) {
            if (listeners[i] != INVALID_SOCKET) {
                ::closesocket(listeners[i]);
                listeners[i] = INVALID_SOCKET;
            }
        }
    }

    void close_conns() {
        std::lock_guard<std::mutex> g(mu);
        for (const auto& p : conns) {
            ::closesocket(p.first);
            ::closesocket(p.second);
        }
        conns.clear();
    }

    ~TcpForwardImpl() {
        stop.store(true);
        close_listeners();
        close_conns();
        if (accept_thread.joinable()) accept_thread.join();
    }
};

namespace {

void relay_loop(std::shared_ptr<TcpForwardImpl> st, SOCKET a, SOCKET b, std::string tag) {
    char* buf = static_cast<char*>(std::malloc(64 * 1024));
    if (!buf) {
        ::closesocket(a);
        ::closesocket(b);
        return;
    }
    bool registered = false;
    {
        std::lock_guard<std::mutex> g(st->mu);
        if (!st->stop.load()) {
            st->conns.insert({a, b});
            st->active.fetch_add(1);
            registered = true;
        }
    }
    if (!registered) {
        std::free(buf);
        ::closesocket(a);
        ::closesocket(b);
        return;
    }

    while (!st->stop.load()) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(a, &rd);
        FD_SET(b, &rd);
        timeval tv{1, 0};
        int n = ::select(0, &rd, nullptr, nullptr, &tv);
        if (n < 0) break;
        if (n == 0) continue;
        if (FD_ISSET(a, &rd)) {
            int r = ::recv(a, buf, 64 * 1024, 0);
            if (r <= 0) break;
            if (!send_all(b, buf, r)) break;
            st->bytes.fetch_add(static_cast<unsigned long long>(r));
        }
        if (FD_ISSET(b, &rd)) {
            int r = ::recv(b, buf, 64 * 1024, 0);
            if (r <= 0) break;
            if (!send_all(a, buf, r)) break;
            st->bytes.fetch_add(static_cast<unsigned long long>(r));
        }
    }

    {
        std::lock_guard<std::mutex> g(st->mu);
        st->conns.erase({a, b});
    }
    st->active.fetch_sub(1);
    ::closesocket(a);
    ::closesocket(b);
    std::free(buf);
    (void)tag;
}

void accept_loop(std::shared_ptr<TcpForwardImpl> st) {
    WSAPOLLFD fds[kSlots];
    while (!st->stop.load()) {
        int nfds = 0;
        for (int i = 0; i < kSlots; ++i) {
            if (st->listeners[i] == INVALID_SOCKET) continue;
            fds[nfds].fd = st->listeners[i];
            fds[nfds].events = POLLRDNORM;
            fds[nfds].revents = 0;
            ++nfds;
        }
        if (nfds == 0) break;
        int rc = WSAPoll(fds, nfds, 500);
        if (rc == SOCKET_ERROR) break;
        if (rc == 0) continue;
        for (int k = 0; k < nfds; ++k) {
            if (!(fds[k].revents & (POLLRDNORM | POLLERR | POLLHUP))) continue;
            SOCKET l = fds[k].fd;
            int idx = -1;
            for (int i = 0; i < kSlots; ++i) {
                if (st->listeners[i] == l) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) continue;
            SOCKET a = ::accept(l, nullptr, nullptr);
            if (a == INVALID_SOCKET) {
                st->rejected.fetch_add(1);
                continue;
            }
            int one = 1;
            ::setsockopt(a, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

            sockaddr_in tgt{};
            tgt.sin_family = AF_INET;
            tgt.sin_port = htons(static_cast<unsigned short>(st->target_port));
            // listener idx == 板卡 IP 最后一段；网段前三段来自 board_cidr。
            std::string prefix = st->cidr;
            size_t slash = prefix.find('/');
            if (slash != std::string::npos) prefix.resize(slash);
            size_t dot = prefix.rfind('.');
            if (dot != std::string::npos) prefix.resize(dot + 1);
            std::string board_ip = prefix + std::to_string(idx);
            if (::inet_pton(AF_INET, board_ip.c_str(), &tgt.sin_addr) != 1) {
                ::closesocket(a);
                st->rejected.fetch_add(1);
                continue;
            }

            SOCKET b = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (b == INVALID_SOCKET) {
                ::closesocket(a);
                st->rejected.fetch_add(1);
                continue;
            }
            // 连接放到转发线程里做，避免某个慢板卡卡住整个 accept 循环。
            st->accepted.fetch_add(1);
            std::thread([st, a, b, board_ip]() {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(static_cast<unsigned short>(st->target_port));
                if (::inet_pton(AF_INET, board_ip.c_str(), &addr.sin_addr) != 1) {
                    ::closesocket(a);
                    ::closesocket(b);
                    return;
                }
                if (::connect(b, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                    ::closesocket(a);
                    ::closesocket(b);
                    return;
                }
                relay_loop(st, a, b, board_ip);
            }).detach();
        }
    }
}

}  // namespace

TcpForwardService::TcpForwardService() : impl_(std::make_shared<TcpForwardImpl>()) {}

TcpForwardService::~TcpForwardService() {
    stop();
    impl_.reset();
}

bool TcpForwardService::start(const TcpForwardConfig& cfg, std::string* err) {
    stop();
    if (!cfg.enable) {
        if (err) err->clear();
        return true;
    }
    if (!ensure_wsa()) {
        if (err) *err = "Winsock 初始化失败";
        return false;
    }
    if (cfg.port_base < 1 || cfg.port_base + kSlots - 1 > 65535) {
        if (err) *err = "TCP 转发 port_base 越界（需 1..65280）";
        return false;
    }
    if (cfg.target_port < 1 || cfg.target_port > 65535) {
        if (err) *err = "TCP 转发 target_port 越界";
        return false;
    }

    std::string cidr = cfg.board_cidr;
    size_t slash = cidr.find('/');
    if (slash == std::string::npos || cidr.substr(slash + 1) != "24") {
        if (err) *err = "board_cidr 必须是 a.b.c.0/24";
        return false;
    }
    std::string network = cidr.substr(0, slash);
    in_addr na{};
    if (::inet_pton(AF_INET, network.c_str(), &na) != 1) {
        if (err) *err = "board_cidr 地址非法";
        return false;
    }
    if ((ntohl(na.s_addr) & 0xffu) != 0) {
        if (err) *err = "board_cidr 必须以 .0 结尾，例如 192.168.1.0/24";
        return false;
    }

    std::shared_ptr<TcpForwardImpl> n = std::make_shared<TcpForwardImpl>();
    n->cidr = network;
    n->listen_ip = cfg.listen_ip.empty() ? "0.0.0.0" : cfg.listen_ip;
    n->port_base = cfg.port_base;
    n->target_port = cfg.target_port;

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = 0;  // 每个槽单独填端口
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (n->listen_ip != "0.0.0.0") {
        if (::inet_pton(AF_INET, n->listen_ip.c_str(), &bind_addr.sin_addr) != 1) {
            if (err) *err = "listen_ip 非法";
            return false;
        }
    }

    std::vector<std::string> failures;
    for (int i = 0; i < kSlots; ++i) {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            failures.emplace_back(std::to_string(cfg.port_base + i) + ":socket");
            continue;
        }
        BOOL excl = TRUE;
        ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl),
                     sizeof(excl));
        bind_addr.sin_port = htons(static_cast<unsigned short>(cfg.port_base + i));
        if (::bind(s, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0 ||
            ::listen(s, 8) != 0) {
            if (failures.size() < 8)
                failures.emplace_back(std::to_string(cfg.port_base + i) + ":" +
                                      std::to_string(::WSAGetLastError()));
            ::closesocket(s);
            continue;
        }
        n->listeners[i] = s;
        ++n->listen_count;
    }
    if (n->listen_count == 0) {
        if (err)
            *err = "TCP 转发端口全部绑定失败" +
                   (failures.empty() ? std::string() : ("（" + failures.front() + "）"));
        return false;
    }
    if (!failures.empty()) {
        std::string note = "部分端口被占用：" + failures.front();
        if (failures.size() > 1) note += " 等 " + std::to_string(failures.size()) + " 个";
        std::lock_guard<std::mutex> g(n->mu);
        n->error = note;
    }

    std::shared_ptr<TcpForwardImpl> old = impl_;
    impl_ = n;
    n->accept_thread = std::thread(accept_loop, n);
    if (old) {
        old->stop.store(true);
        old->close_listeners();
        old->close_conns();
        if (old->accept_thread.joinable()) old->accept_thread.join();
        old.reset();
    }
    if (err) {
        std::lock_guard<std::mutex> g(n->mu);
        *err = n->error;
    }
    return true;
}

void TcpForwardService::stop() {
    if (!impl_) return;
    std::shared_ptr<TcpForwardImpl> old = impl_;
    impl_ = std::make_shared<TcpForwardImpl>();
    old->stop.store(true);
    old->close_listeners();
    old->close_conns();
    if (old->accept_thread.joinable()) old->accept_thread.join();
    old.reset();
}

bool TcpForwardService::running() const { return impl_ && impl_->listen_count > 0; }

int TcpForwardService::listen_count() const { return impl_ ? impl_->listen_count : 0; }

int TcpForwardService::active_connections() const {
    return impl_ ? impl_->active.load() : 0;
}

int TcpForwardService::mapped_port(const TcpForwardConfig& cfg, const std::string& board_ip) const {
    size_t slash = cfg.board_cidr.find('/');
    if (slash == std::string::npos) return 0;
    std::string network = cfg.board_cidr.substr(0, slash);
    size_t dot = network.rfind('.');
    if (dot == std::string::npos) return 0;
    std::string prefix = network.substr(0, dot + 1);
    if (board_ip.compare(0, prefix.size(), prefix) != 0) return 0;
    std::string last = board_ip.substr(prefix.size());
    if (last.empty() || last.find('.') != std::string::npos) return 0;
    int v = std::atoi(last.c_str());
    if (v < 0 || v > 255) return 0;
    return cfg.port_base + v;
}

std::string TcpForwardService::status_json() const {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "on", running());
    cJSON_AddStringToObject(o, "listen_ip", impl_ ? impl_->listen_ip.c_str() : "");
    cJSON_AddNumberToObject(o, "port_base", impl_ ? impl_->port_base : 0);
    cJSON_AddNumberToObject(o, "target_port", impl_ ? impl_->target_port : 0);
    cJSON_AddStringToObject(o, "board_cidr", impl_ ? impl_->cidr.c_str() : "");
    cJSON_AddNumberToObject(o, "listen_count", impl_ ? impl_->listen_count : 0);
    cJSON_AddNumberToObject(o, "active", impl_ ? impl_->active.load() : 0);
    cJSON_AddNumberToObject(o, "accepted", impl_ ? static_cast<double>(impl_->accepted.load()) : 0);
    cJSON_AddNumberToObject(o, "bytes", impl_ ? static_cast<double>(impl_->bytes.load()) : 0);
    std::string err;
    if (impl_) {
        std::lock_guard<std::mutex> g(impl_->mu);
        err = impl_->error;
    }
    cJSON_AddStringToObject(o, "error", err.c_str());
    char* txt = cJSON_PrintUnformatted(o);
    std::string out = txt ? txt : "{}";
    cJSON_free(txt);
    cJSON_Delete(o);
    return out;
}

}  // namespace sb3
