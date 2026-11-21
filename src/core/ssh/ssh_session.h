// ssh_session.h — libssh 会话/通道的最小封装（S2）。
// 只做：建链、认证、PTY+shell、非阻塞读、阻塞写、保活、关闭。
// 线程模型：一个 SshSession 只允许一个线程读写（SshBridge 里由 RX 线程独占读，
// TX 线程只写；libssh 会话不允许并发调用，故两者用 mutex 串行化）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sb3 {

struct SshTarget {
    std::string host;
    int         port              = 22;
    std::string username;
    std::string password;          // 空 = 先试 none 认证
    std::string term              = "xterm-256color";
    int         width             = 120;
    int         height            = 32;
    int         connect_timeout_ms = 5000;
    int         keepalive_s        = 3;
};

class SshSession {
public:
    SshSession();
    ~SshSession();
    SshSession(const SshSession&) = delete;
    SshSession& operator=(const SshSession&) = delete;

    // 建链 + 认证 + 申请 PTY + 起 shell；失败返回 false，原因见 last_error()
    bool connect(const SshTarget& t);
    void close();

    bool connected() const;
    const std::string& last_error() const;

    // 读：>0 字节数；0 暂无数据；-1 已断开/出错（原因在 last_error）
    int read_some(void* buf, std::size_t cap);
    // 写：整段写完返回 true；断开返回 false
    bool write_all(const void* data, std::size_t n);
    // 保活：发一个 SSH ignore，返回 false 表示链路已断
    bool keepalive();

private:
    struct Impl;
    Impl* impl_;
};

// 进程内全局 ssh_init（线程安全，幂等）
bool ssh_runtime_init();

}  // namespace sb3
