#pragma once

// S12 TCP 转发：按板卡 IP 最后一段映射本地端口。
// 板 192.168.1.37 → 本地 22000 + 37，转发到 192.168.1.37:22。
// 监听使用 WSAPoll(500ms) 统一等待，禁止逐端口 sleep 扫描。

#include <memory>
#include <string>

#include "core/config/config.h"

namespace sb3 {

struct TcpForwardImpl;

class TcpForwardService {
public:
    TcpForwardService();
    ~TcpForwardService();

    TcpForwardService(const TcpForwardService&) = delete;
    TcpForwardService& operator=(const TcpForwardService&) = delete;

    bool start(const TcpForwardConfig& cfg, std::string* err);
    void stop();

    bool        running() const;
    int         listen_count() const;
    int         active_connections() const;
    std::string status_json() const;

    // 供界面/测试查询：给定板卡 IP，返回应连接的本地端口；非法返回 0。
    int mapped_port(const TcpForwardConfig& cfg, const std::string& board_ip) const;

private:
    std::shared_ptr<TcpForwardImpl> impl_;
};

}  // namespace sb3
