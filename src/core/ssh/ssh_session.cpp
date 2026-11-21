// ssh_session.cpp — libssh 封装实现（S2）。
// 所有 libssh 调用都在 mutex 内完成：libssh 会话不是线程安全的，
// RX 线程读、TX 线程写必须串行进入。

#include "core/ssh/ssh_session.h"

#include <libssh/libssh.h>

#include <cmath>
#include <mutex>

namespace sb3 {
namespace {

std::once_flag g_init_once;
bool         g_init_ok = false;

void do_init() {
    // 0 = 成功，SSH_ERROR = 失败（已初始化也算成功场景）
    g_init_ok = (ssh_init() == 0);
}

std::string err_of(ssh_session s, const char* prefix) {
    std::string text = prefix ? prefix : "";
    if (!text.empty()) text += ": ";
    const char* e = s ? ssh_get_error(s) : nullptr;
    text += (e && *e) ? e : "unknown";
    return text;
}

}  // namespace

bool ssh_runtime_init() {
    std::call_once(g_init_once, do_init);
    return g_init_ok;
}

struct SshSession::Impl {
    mutable std::mutex mu;
    ssh_session        session = nullptr;
    ssh_channel        channel = nullptr;
    std::string        error;
    bool               ready   = false;
};

SshSession::SshSession() : impl_(new Impl()) {}

SshSession::~SshSession() {
    close();
    delete impl_;
}

bool SshSession::connect(const SshTarget& t) {
    if (!ssh_runtime_init()) {
        impl_->error = "libssh 初始化失败";
        return false;
    }
    close();
    std::lock_guard<std::mutex> lk(impl_->mu);

    ssh_session s = ssh_new();
    if (!s) {
        impl_->error = "ssh_new 失败（内存不足？）";
        return false;
    }

    long timeout_s = static_cast<long>(std::max(1, (t.connect_timeout_ms + 999) / 1000));
    ssh_options_set(s, SSH_OPTIONS_HOST, t.host.c_str());
    ssh_options_set(s, SSH_OPTIONS_PORT, &t.port);
    if (!t.username.empty()) {
        ssh_options_set(s, SSH_OPTIONS_USER, t.username.c_str());
    }
    ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout_s);
    int log_verbosity = SSH_LOG_NOLOG;
    ssh_options_set(s, SSH_OPTIONS_LOG_VERBOSITY, &log_verbosity);

    if (ssh_connect(s) != SSH_OK) {
        impl_->error = err_of(s, "连接失败");
        ssh_free(s);
        return false;
    }

    int auth = SSH_AUTH_DENIED;
    if (!t.password.empty()) {
        auth = ssh_userauth_password(s, nullptr, t.password.c_str());
        if (auth != SSH_AUTH_SUCCESS) {
            int none_rc = ssh_userauth_none(s, nullptr);
            if (none_rc == SSH_AUTH_SUCCESS) auth = none_rc;
        }
    } else {
        auth = ssh_userauth_none(s, nullptr);
    }
    if (auth != SSH_AUTH_SUCCESS) {
        impl_->error = err_of(s, "认证失败");
        if (impl_->error.find("unknown") != std::string::npos) {
            impl_->error += "（用户名/密码被拒绝）";
        }
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }

    ssh_channel ch = ssh_channel_new(s);
    if (!ch || ssh_channel_open_session(ch) != SSH_OK) {
        impl_->error = err_of(s, "打开会话通道失败");
        if (ch) ssh_channel_free(ch);
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }
    std::string term = t.term.empty() ? "xterm-256color" : t.term;
    if (ssh_channel_request_pty_size(ch, term.c_str(), t.width, t.height) != SSH_OK ||
        ssh_channel_request_shell(ch) != SSH_OK) {
        impl_->error = err_of(s, "申请 PTY/shell 失败");
        ssh_channel_free(ch);
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }

    impl_->session = s;
    impl_->channel = ch;
    impl_->ready   = true;
    impl_->error.clear();
    return true;
}

void SshSession::close() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->channel) {
        ssh_channel_send_eof(impl_->channel);
        ssh_channel_close(impl_->channel);
        ssh_channel_free(impl_->channel);
        impl_->channel = nullptr;
    }
    if (impl_->session) {
        ssh_disconnect(impl_->session);
        ssh_free(impl_->session);
        impl_->session = nullptr;
    }
    impl_->ready = false;
}

bool SshSession::connected() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->ready || !impl_->session || !impl_->channel) return false;
    return ssh_channel_is_open(impl_->channel) != 0 && ssh_channel_is_eof(impl_->channel) == 0;
}

const std::string& SshSession::last_error() const { return impl_->error; }

int SshSession::read_some(void* buf, std::size_t cap) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->ready || !impl_->channel) {
        impl_->error = "未连接";
        return -1;
    }
    int rc = ssh_channel_read_nonblocking(impl_->channel, buf, static_cast<uint32_t>(cap), 0);
    if (rc == SSH_ERROR) {
        impl_->error = err_of(impl_->session, "读失败");
        impl_->ready = false;
        return -1;
    }
    if (rc == SSH_EOF) {
        impl_->error = "对端已结束会话";
        impl_->ready = false;
        return -1;
    }
    return rc < 0 ? 0 : rc;
}

bool SshSession::write_all(const void* data, std::size_t n) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->ready || !impl_->channel) {
        impl_->error = "未连接";
        return false;
    }
    const char* p = static_cast<const char*>(data);
    std::size_t left = n;
    int spins = 0;
    while (left > 0) {
        int rc = ssh_channel_write(impl_->channel, p, static_cast<uint32_t>(left));
        if (rc == SSH_ERROR) {
            impl_->error = err_of(impl_->session, "写失败");
            impl_->ready = false;
            return false;
        }
        if (rc == SSH_AGAIN) {
            if (++spins > 200) {
                impl_->error = "写缓冲持续不可用";
                impl_->ready = false;
                return false;
            }
            continue;
        }
        p += rc;
        left -= static_cast<std::size_t>(rc);
    }
    return true;
}

bool SshSession::keepalive() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->ready || !impl_->session) return false;
    if (ssh_send_ignore(impl_->session, "") != SSH_OK) {
        impl_->error = err_of(impl_->session, "保活失败");
        impl_->ready = false;
        return false;
    }
    return true;
}

}  // namespace sb3
