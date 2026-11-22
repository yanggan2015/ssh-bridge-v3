// inbound_gateway.h — S8 SSH 入站共享网关。
// 一个进程只开一个监听口；用户名=通道名，会话不新建 target SSH，
// 而是直接读该通道 ring、写该通道 cmdq，因此与本地终端看到同一路字节流。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/bridge/bridge.h"

namespace sb3 {

struct InboundTarget {
    std::shared_ptr<IBridge> bridge;
    std::string password;   // 空 = 允许 none 认证
};

class InboundGateway {
public:
    // 返回 false 时 err 是可读原因；成功时 target 必须填好。
    using Resolver = std::function<bool(const std::string& channel_name, InboundTarget* target,
                                        std::string* err)>;

    InboundGateway();
    ~InboundGateway();
    InboundGateway(const InboundGateway&) = delete;
    InboundGateway& operator=(const InboundGateway&) = delete;

    // hostkey_path：首次生成 RSA 2048 私钥并落盘，后续复用。
    bool start(int port, const std::string& hostkey_path, Resolver resolver, std::string* err);
    void stop();

    bool running() const;
    int port() const;
    int sessions() const;
    std::string status_json() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace sb3
