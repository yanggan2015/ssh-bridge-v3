#pragma once

// S12 UDP 发现：监听 18071，收到 EADK1 查询后返回本机可发现通道。
// 仅 enable && discoverable 的通道会被返回；默认全部 discoverable=false。

#include <memory>
#include <string>

#include "core/config/config.h"

namespace sb3 {

struct DiscoveryImpl;

class DiscoveryService {
public:
    DiscoveryService();
    ~DiscoveryService();

    DiscoveryService(const DiscoveryService&) = delete;
    DiscoveryService& operator=(const DiscoveryService&) = delete;

    bool start(const DiscoveryConfig& cfg, const AppConfig& app, const std::string& host,
               std::string* err);
    void stop();

    bool        running() const;
    int         port() const;
    int         responses() const;
    std::string status_json() const;

private:
    std::shared_ptr<DiscoveryImpl> impl_;
};

}  // namespace sb3
