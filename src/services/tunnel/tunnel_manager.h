#pragma once

// S12 隧道：把 Web 控制台与入站 SSH 映射到公网。
// 外部程序 eadk-tunnel / frpc，采用 "exe -c runtime.json"；两个独立子进程。

#include <memory>
#include <string>

#include "core/config/config.h"

namespace sb3 {

struct TunnelImpl;

class TunnelManager {
public:
    TunnelManager();
    ~TunnelManager();

    TunnelManager(const TunnelManager&) = delete;
    TunnelManager& operator=(const TunnelManager&) = delete;

    bool start(const TunnelConfig& cfg, const std::string& project_root, std::string* err);
    void stop();

    bool        running() const;
    std::string status_json() const;

private:
    std::shared_ptr<TunnelImpl> impl_;
};

}  // namespace sb3
