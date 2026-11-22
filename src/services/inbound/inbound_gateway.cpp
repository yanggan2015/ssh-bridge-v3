// inbound_gateway.cpp — S8 SSH 入站共享网关实现（libssh server）。
//
// 设计口径：
//   * 一个 ssh_bind 监听 inbound_ssh.port（默认 2222）；
//   * 用户名 = 已存在的通道名，密码取该通道配置；
//   * 入站会话不新建 target SSH，而是把客户端字节写进该通道 cmdq，
//     同时用自己的 epoch/seen 游标读该通道同一份 ring；
//   * 关闭开关后不 bind，也不回退到每板一个端口。

#include "services/inbound/inbound_gateway.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <libssh/libssh.h>
#include <libssh/server.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>

#include "core/ssh/ssh_session.h"
#include "core/util/util.h"

namespace sb3 {
namespace {

constexpr int kAuthTimeoutMs = 15000;
constexpr int kChannelTimeoutMs = 10000;
constexpr std::size_t kMaxChunk = 64 * 1024;
constexpr std::size_t kHistoryTail = 4096;

std::string ssh_error_text(ssh_session s, const char* fallback) {
    const char* e = s ? ssh_get_error(s) : nullptr;
    return (e && *e) ? std::string(e) : std::string(fallback ? fallback : "SSH 错误");
}

std::string ssh_error_text(ssh_bind b, const char* fallback) {
    const char* e = b ? ssh_get_error(b) : nullptr;
    return (e && *e) ? std::string(e) : std::string(fallback ? fallback : "SSH 监听错误");
}

void append_log(const std::string& path, const std::string& line) {
    if (path.empty()) return;
    std::FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) return;
    std::string stamp = util::log_time_text();
    std::fprintf(f, "[%s] %s\n", stamp.c_str(), line.c_str());
    std::fclose(f);
}

std::string peer_text(ssh_session s) {
    socket_t fd = ssh_get_fd(s);
    if (fd == INVALID_SOCKET) return "?";
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "?";
    char host[64] = "";
    if (inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host)) == nullptr) return "?";
    return std::string(host) + ":" + std::to_string(ntohs(addr.sin_port));
}

std::string message_password(ssh_message msg) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    const char* p = ssh_message_auth_password(msg);
#pragma GCC diagnostic pop
    return p ? std::string(p) : std::string();
}

ssh_key load_or_make_hostkey(const std::string& path, std::string* err) {
    std::filesystem::path fp = std::filesystem::u8path(path);
    try {
        std::filesystem::create_directories(fp.parent_path());
    } catch (...) {
    }

    ssh_key key = nullptr;
    if (std::filesystem::exists(fp)) {
        if (ssh_pki_import_privkey_file(path.c_str(), nullptr, nullptr, nullptr, &key) == SSH_OK &&
            key) {
            return key;
        }
        if (key) ssh_key_free(key);
        key = nullptr;
    }

    ssh_pki_ctx ctx = ssh_pki_ctx_new();
    int bits = 2048;
    if (ctx) (void)ssh_pki_ctx_options_set(ctx, SSH_PKI_OPTION_RSA_KEY_SIZE, &bits);
    const int gen = ssh_pki_generate_key(SSH_KEYTYPE_RSA, ctx, &key);
    if (ctx) ssh_pki_ctx_free(ctx);
    if (gen != SSH_OK || !key) {
        if (key) ssh_key_free(key);
        if (err) *err = "生成 SSH hostkey 失败";
        return nullptr;
    }
    if (ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr, path.c_str()) != SSH_OK) {
        ssh_key_free(key);
        if (err) *err = "写入 SSH hostkey 失败：" + path;
        return nullptr;
    }
    return key;
}

}  // namespace

struct InboundGateway::Impl {
    std::atomic<bool> running{false};
    std::atomic<bool> stop{false};
    std::atomic<int>  session_count{0};
    int port = 2222;
    Resolver resolver;
    std::string hostkey_path;
    std::string log_path;
    ssh_bind bind = nullptr;
    ssh_key hostkey = nullptr;
    std::thread accept_thread;
    std::vector<std::future<void>> sessions;
    mutable std::mutex mu;
    std::string note;

    void set_note(const std::string& s) {
        std::lock_guard<std::mutex> g(mu);
        note = s;
    }

    std::string get_note() const {
        std::lock_guard<std::mutex> g(mu);
        return note;
    }

    bool resolve(const std::string& name, InboundTarget* target, std::string* err) {
        if (!resolver) {
            if (err) *err = "入站通道解析器未设置";
            return false;
        }
        return resolver(name, target, err);
    }

    void run_session(ssh_session session, const std::string& channel_name, InboundTarget target);
    void accept_loop();
    void close_session(ssh_session session, ssh_channel channel);
};

void InboundGateway::Impl::close_session(ssh_session session, ssh_channel channel) {
    if (channel) {
        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
    }
    if (session) {
        ssh_disconnect(session);
        ssh_free(session);
    }
}

void InboundGateway::Impl::run_session(ssh_session session, const std::string& channel_name,
                                       InboundTarget target) {
    ssh_channel channel = nullptr;
    bool shell_ok = false;
    std::uint64_t epoch = 0, seen = 0;
    std::uint64_t last_seq = 0;
    std::string err;

    // 认证阶段由 accept_loop 已处理一半：这里继续接收 channel open / pty / shell。
    const std::uint64_t t0 = GetTickCount64();
    while (!stop.load() && running.load() && !shell_ok) {
        if (GetTickCount64() - t0 > static_cast<std::uint64_t>(kChannelTimeoutMs)) {
            err = "等待 SSH 会话通道超时";
            break;
        }
        ssh_message msg = ssh_message_get(session);
        if (!msg) {
            if (!ssh_is_connected(session)) {
                err = "客户端在申请会话前断开";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const int type = ssh_message_type(msg);
        const int subtype = ssh_message_subtype(msg);
        if (type == SSH_REQUEST_CHANNEL_OPEN && subtype == SSH_CHANNEL_SESSION) {
            if (!channel) channel = ssh_message_channel_request_open_reply_accept(msg);
        } else if (type == SSH_REQUEST_CHANNEL && subtype == SSH_CHANNEL_REQUEST_PTY) {
            (void)ssh_message_channel_request_reply_success(msg);
        } else if (type == SSH_REQUEST_CHANNEL &&
                   (subtype == SSH_CHANNEL_REQUEST_SHELL || subtype == SSH_CHANNEL_REQUEST_ENV ||
                    subtype == SSH_CHANNEL_REQUEST_WINDOW_CHANGE)) {
            (void)ssh_message_channel_request_reply_success(msg);
            if (subtype == SSH_CHANNEL_REQUEST_SHELL) shell_ok = true;
        } else {
            (void)ssh_message_reply_default(msg);
        }
        ssh_message_free(msg);
    }

    if (!channel || !shell_ok) {
        append_log(log_path, "入站会话失败 user=" + channel_name + " reason=" +
                                  (err.empty() ? "未申请 shell" : err));
        close_session(session, channel);
        return;
    }

    if (!target.bridge) {
        err = "通道对象不存在";
    } else {
        if (!target.bridge->running()) {
            const std::string tip = "\r\n[ssh-bridge] 通道 " + channel_name +
                                    " 当前未连接；重连后本条会话会自动恢复输出。\r\n";
            (void)ssh_channel_write(channel, tip.data(), static_cast<uint32_t>(tip.size()));
        }
        // 先读一次把游标对齐到当前尾部，避免把整段历史一次性灌给新客户端。
        std::string history = target.bridge->read_all(&epoch, &seen);
        if (history.size() > kHistoryTail) history = history.substr(history.size() - kHistoryTail);
        if (!history.empty()) {
            if (ssh_channel_write(channel, history.data(),
                                  static_cast<uint32_t>(history.size())) < 0) {
                err = "写初始回看失败";
            }
        }
        last_seq = target.bridge->notify_seq();
    }

    append_log(log_path, "入站会话已接入 user=" + channel_name + " peer=" + peer_text(session) +
                              (err.empty() ? "" : (" note=" + err)));

    while (!stop.load() && running.load() && channel && ssh_channel_is_open(channel) &&
           !ssh_channel_is_eof(channel)) {
        bool progressed = false;
        char buf[kMaxChunk];
        for (int i = 0; i < 8; ++i) {
            const int n = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), 0);
            if (n > 0) {
                if (target.bridge) target.bridge->write(buf, static_cast<std::size_t>(n));
                progressed = true;
                continue;
            }
            if (n < 0) {
                // 客户端关闭/断链；不要把它误当成“暂无数据”。
                if (!ssh_channel_is_open(channel) || ssh_channel_is_eof(channel)) break;
                if (n == SSH_ERROR) {
                    err = "读取入站客户端失败";
                    stop.store(true);
                }
            }
            break;
        }

        if (target.bridge) {
            const std::uint64_t seq = target.bridge->notify_seq();
            if (seq != last_seq) {
                std::string data = target.bridge->read_since(&epoch, &seen);
                if (!data.empty()) {
                    if (ssh_channel_write(channel, data.data(),
                                          static_cast<uint32_t>(data.size())) < 0) {
                        err = "写回显到入站客户端失败";
                        break;
                    }
                    progressed = true;
                }
                last_seq = seq;
            }
        }

        if (!progressed) std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }

    if (!err.empty()) append_log(log_path, "入站会话结束 user=" + channel_name + " reason=" + err);
    close_session(session, channel);
}

void InboundGateway::Impl::accept_loop() {
    while (!stop.load()) {
        const socket_t fd = ssh_bind_get_fd(bind);
        if (fd == INVALID_SOCKET) {
            set_note("SSH 监听句柄无效");
            break;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        const int ready = select(0, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;

        ssh_session session = ssh_new();
        if (!session) {
            set_note("创建 SSH 会话失败");
            continue;
        }
        int timeout = kAuthTimeoutMs / 1000;
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
        if (ssh_bind_accept(bind, session) != SSH_OK) {
            append_log(log_path, "accept 失败：" + ssh_error_text(session, "ssh_bind_accept"));
            ssh_free(session);
            continue;
        }
        if (ssh_handle_key_exchange(session) != SSH_OK) {
            append_log(log_path, "key exchange 失败：" + ssh_error_text(session, "key exchange"));
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }
        ssh_set_blocking(session, 0);

        std::string channel_name;
        InboundTarget target;
        std::string why;
        const std::uint64_t auth_t0 = GetTickCount64();
        bool authed = false;
        while (!stop.load() && running.load() && !authed) {
            if (GetTickCount64() - auth_t0 > static_cast<std::uint64_t>(kAuthTimeoutMs)) {
                why = "认证超时";
                break;
            }
            ssh_message msg = ssh_message_get(session);
            if (!msg) {
                if (!ssh_is_connected(session)) {
                    why = "客户端在认证前断开";
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            const int type = ssh_message_type(msg);
            const int subtype = ssh_message_subtype(msg);
            if (type == SSH_REQUEST_AUTH) {
                const char* user = ssh_message_auth_user(msg);
                const std::string name = user ? user : "";
                std::string err;
                InboundTarget t;
                const bool known = !name.empty() && resolve(name, &t, &err);
                if (subtype == SSH_AUTH_METHOD_NONE && known && t.password.empty()) {
                    channel_name = name;
                    target = t;
                    (void)ssh_message_auth_reply_success(msg, 0);
                    authed = true;
                } else if (subtype == SSH_AUTH_METHOD_PASSWORD && known && !t.password.empty()) {
                    const std::string pass = message_password(msg);
                    if (!pass.empty() && t.password == pass) {
                        channel_name = name;
                        target = t;
                        (void)ssh_message_auth_reply_success(msg, 0);
                        authed = true;
                    } else {
                        why = "密码错误或通道不存在";
                    }
                } else {
                    why = known ? "该通道要求密码认证" : "通道不存在";
                }
                if (!authed) {
                    int methods = SSH_AUTH_METHOD_PASSWORD;
                    if (known && t.password.empty()) methods |= SSH_AUTH_METHOD_NONE;
                    (void)ssh_message_auth_set_methods(msg, methods);
                    (void)ssh_message_reply_default(msg);
                }
            } else {
                (void)ssh_message_reply_default(msg);
            }
            ssh_message_free(msg);
        }

        if (!authed) {
            append_log(log_path, "入站认证失败 peer=" + peer_text(session) + " reason=" + why);
            ssh_disconnect(session);
            ssh_free(session);
            if (stop.load()) break;
            continue;
        }

        append_log(log_path, "入站认证通过 user=" + channel_name + " peer=" + peer_text(session));
        session_count.fetch_add(1);
        sessions.emplace_back(std::async(std::launch::async, [this, session, channel_name, target]() {
            run_session(session, channel_name, target);
            session_count.fetch_sub(1);
        }));
        // 回收已结束的 future，避免长跑进程只增不减。
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (it->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (bind) {
        ssh_bind_free(bind);
        bind = nullptr;
    }
    // IMPORT_KEY 由 libssh 在 bind 内持有/复制；这里不再二次释放，
    // 否则停止监听时会出现 ntdll 堆损坏（0xc0000374）。
    hostkey = nullptr;
    running.store(false);
}

InboundGateway::InboundGateway() : impl_(new Impl()) {}

InboundGateway::~InboundGateway() { stop(); delete impl_; }

bool InboundGateway::start(int port, const std::string& hostkey_path, Resolver resolver,
                           std::string* err) {
    if (impl_->running.load()) return true;
    if (port <= 0 || port > 65535) {
        if (err) *err = "入站 SSH 端口越界";
        return false;
    }
    if (!ssh_runtime_init()) {
        if (err) *err = "libssh 初始化失败";
        return false;
    }

    impl_->stop.store(false);
    impl_->port = port;
    impl_->resolver = std::move(resolver);
    impl_->hostkey_path = hostkey_path;
    impl_->log_path = (std::filesystem::u8path(hostkey_path).parent_path() / "inbound.log").u8string();
    impl_->note.clear();

    impl_->hostkey = load_or_make_hostkey(hostkey_path, err);
    if (!impl_->hostkey) return false;

    ssh_bind bind = ssh_bind_new();
    if (!bind) {
        if (err) *err = "创建 SSH bind 失败";
        ssh_key_free(impl_->hostkey);
        impl_->hostkey = nullptr;
        return false;
    }
    const char* addr = "0.0.0.0";
    (void)ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, addr);
    (void)ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT, &port);
    if (ssh_bind_options_set(bind, SSH_BIND_OPTIONS_IMPORT_KEY, impl_->hostkey) != SSH_OK ||
        ssh_bind_listen(bind) != SSH_OK) {
        if (err) *err = "入站 SSH 监听失败：" + ssh_error_text(bind, "bind");
        ssh_bind_free(bind);
        ssh_key_free(impl_->hostkey);
        impl_->hostkey = nullptr;
        return false;
    }

    impl_->bind = bind;
    impl_->running.store(true);
    impl_->set_note("监听 0.0.0.0:" + std::to_string(port));
    impl_->accept_thread = std::thread([this]() { impl_->accept_loop(); });
    if (err) err->clear();
    return true;
}

void InboundGateway::stop() {
    if (!impl_) return;
    impl_->stop.store(true);
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    for (auto& f : impl_->sessions) {
        if (f.valid()) f.wait();
    }
    impl_->sessions.clear();
    impl_->running.store(false);
}

bool InboundGateway::running() const { return impl_ && impl_->running.load(); }
int InboundGateway::port() const { return impl_ ? impl_->port : 0; }
int InboundGateway::sessions() const { return impl_ ? impl_->session_count.load() : 0; }

std::string InboundGateway::status_json() const {
    char buf[512];
    const bool on = running();
    const int sessions = this->sessions();
    std::string note = impl_ ? impl_->get_note() : std::string();
    if (!on && note.empty()) note = "未启动";
    std::snprintf(buf, sizeof(buf),
                  "{\"on\":%s,\"port\":%d,\"sessions\":%d,\"note\":\"%s\"}",
                  on ? "true" : "false", port(), sessions, note.c_str());
    return buf;
}

}  // namespace sb3
