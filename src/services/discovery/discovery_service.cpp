#include "services/discovery/discovery_service.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <cjson/cJSON.h>

namespace sb3 {
namespace {

bool ensure_wsa() {
    static std::atomic<int> state{0};
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

bool query_matches(const char* data, int len) {
    if (len < 6) return false;
    bool magic = false;
    for (int i = 0; i + 5 <= len; ++i) {
        if (std::memcmp(data + i, "EADK1", 5) == 0) {
            magic = true;
            break;
        }
    }
    if (!magic) return false;
    std::string low;
    low.reserve(static_cast<size_t>(len));
    for (int i = 0; i < len; ++i) low.push_back(static_cast<char>(std::tolower(data[i])));
    return low.find("q") != std::string::npos || low.find("discover") != std::string::npos;
}

}  // namespace

struct DiscoveryImpl {
    std::atomic<bool> stop{false};
    SOCKET sock = INVALID_SOCKET;
    int port = 18071;
    std::atomic<int> responses{0};
    std::string host;
    std::string last_peer;
    std::string error;
    DiscoveryConfig cfg;
    AppConfig app;
    std::thread thread;
    std::mutex mu;

    ~DiscoveryImpl() {
        stop.store(true);
        if (thread.joinable()) thread.join();
        if (sock != INVALID_SOCKET) {
            ::closesocket(sock);
            sock = INVALID_SOCKET;
        }
    }
};

namespace {

std::string build_reply(const DiscoveryImpl& st) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "magic", "EADK1");
    cJSON_AddStringToObject(root, "tool", "ssh-bridge");
    cJSON_AddStringToObject(root, "version", "3.0");
    cJSON_AddStringToObject(root, "host", st.host.c_str());
    cJSON_AddNumberToObject(root, "http_port", st.app.web.port);
    cJSON_AddBoolToObject(root, "inbound_ssh", st.app.inbound.enabled);
    cJSON_AddNumberToObject(root, "ssh_port", st.app.inbound.port);

    cJSON* boards = cJSON_AddArrayToObject(root, "boards");
    for (const BoardConfig& b : st.app.boards) {
        if (!b.discoverable) continue;
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", b.name.c_str());
        cJSON_AddStringToObject(o, "type", "serial");
        cJSON_AddStringToObject(o, "serial_device", b.serial_device.c_str());
        cJSON_AddStringToObject(o, "host", st.host.c_str());
        cJSON_AddNumberToObject(o, "ssh_port", st.app.inbound.enabled ? st.app.inbound.port : 0);
        cJSON_AddStringToObject(o, "ssh_user", b.name.c_str());
        cJSON_AddItemToArray(boards, o);
    }

    cJSON* ssh = cJSON_AddArrayToObject(root, "ssh");
    for (const SSHLinkConfig& s : st.app.links) {
        if (!s.discoverable) continue;
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s.name.c_str());
        cJSON_AddStringToObject(o, "host", st.host.c_str());
        cJSON_AddStringToObject(o, "target_host", s.host.c_str());
        cJSON_AddNumberToObject(o, "port", st.app.inbound.enabled ? st.app.inbound.port : s.port);
        cJSON_AddNumberToObject(o, "target_port", s.port);
        cJSON_AddStringToObject(o, "ssh_user", s.name.c_str());
        cJSON_AddItemToArray(ssh, o);
    }

    char* txt = cJSON_PrintUnformatted(root);
    std::string out = txt ? txt : "{}";
    cJSON_free(txt);
    cJSON_Delete(root);
    return out;
}

void recv_loop(std::shared_ptr<DiscoveryImpl> st) {
    char buf[2048];
    while (!st->stop.load()) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(st->sock, &rd);
        timeval tv{0, 500000};
        int n = ::select(0, &rd, nullptr, nullptr, &tv);
        if (n < 0) break;
        if (n == 0) continue;
        sockaddr_in from{};
        int from_len = sizeof(from);
        int r = ::recvfrom(st->sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from),
                           &from_len);
        if (r < 0) continue;
        buf[r] = 0;
        if (!query_matches(buf, r)) continue;
        std::string reply = build_reply(*st);
        ::sendto(st->sock, reply.c_str(), static_cast<int>(reply.size()), 0,
                 reinterpret_cast<sockaddr*>(&from), from_len);
        char peer[64] = "";
        inet_ntop(AF_INET, &from.sin_addr, peer, sizeof(peer));
        std::lock_guard<std::mutex> g(st->mu);
        st->last_peer = std::string(peer) + ":" + std::to_string(ntohs(from.sin_port));
        st->responses.fetch_add(1);
    }
}

}  // namespace

DiscoveryService::DiscoveryService() : impl_(std::make_shared<DiscoveryImpl>()) {}

DiscoveryService::~DiscoveryService() {
    stop();
    impl_.reset();
}

bool DiscoveryService::start(const DiscoveryConfig& cfg, const AppConfig& app,
                             const std::string& host, std::string* err) {
    stop();
    if (!cfg.enable) {
        if (err) err->clear();
        return true;
    }
    if (!ensure_wsa()) {
        if (err) *err = "Winsock 初始化失败";
        return false;
    }
    if (cfg.port < 1 || cfg.port > 65535) {
        if (err) *err = "发现端口越界";
        return false;
    }
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        if (err) *err = "UDP 发现 socket 创建失败";
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(cfg.port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        int e = ::WSAGetLastError();
        ::closesocket(s);
        if (err) *err = "UDP 发现端口 " + std::to_string(cfg.port) + " 绑定失败（" +
                        std::to_string(e) + "）";
        return false;
    }

    std::shared_ptr<DiscoveryImpl> n = std::make_shared<DiscoveryImpl>();
    n->sock = s;
    n->port = cfg.port;
    n->cfg = cfg;
    n->app = app;
    n->host = host.empty() ? "127.0.0.1" : host;
    n->thread = std::thread(recv_loop, n);
    std::shared_ptr<DiscoveryImpl> old = impl_;
    impl_ = n;

    if (old) {
        old->stop.store(true);
        if (old->thread.joinable()) old->thread.join();
        old.reset();
    }
    if (err) err->clear();
    return true;
}

void DiscoveryService::stop() {
    if (!impl_) return;
    std::shared_ptr<DiscoveryImpl> old = impl_;
    impl_ = std::make_shared<DiscoveryImpl>();
    old->stop.store(true);
    if (old->thread.joinable()) old->thread.join();
    old.reset();
}

bool DiscoveryService::running() const { return impl_ && impl_->sock != INVALID_SOCKET; }

int DiscoveryService::port() const { return impl_ ? impl_->port : 0; }

int DiscoveryService::responses() const { return impl_ ? impl_->responses.load() : 0; }

std::string DiscoveryService::status_json() const {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "on", running());
    cJSON_AddNumberToObject(o, "port", impl_ ? impl_->port : 0);
    cJSON_AddNumberToObject(o, "responses", impl_ ? impl_->responses.load() : 0);
    cJSON_AddStringToObject(o, "host", impl_ ? impl_->host.c_str() : "");
    std::string peer, error;
    if (impl_) {
        std::lock_guard<std::mutex> g(impl_->mu);
        peer = impl_->last_peer;
        error = impl_->error;
    }
    cJSON_AddStringToObject(o, "last_peer", peer.c_str());
    cJSON_AddStringToObject(o, "error", error.c_str());
    char* txt = cJSON_PrintUnformatted(o);
    std::string out = txt ? txt : "{}";
    cJSON_free(txt);
    cJSON_Delete(o);
    return out;
}

}  // namespace sb3
