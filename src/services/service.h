// service.h — 服务面契约
// 每个可选服务（SSH 入站 / 网页 / Agent 接口 / 继电器 / TFTP / NFS ...）
// 都实现本接口，由 app 层按配置启停。新增服务不需要改动已有服务。

#pragma once

#include <string>

namespace sb3 {

struct AppConfig;

class IService {
public:
    virtual ~IService() = default;

    // 稳定标识，用于日志、状态、开关配置
    virtual const char* name() const = 0;

    // 由 app 层注入配置后启动；返回 false 表示启动失败（端口占用等）
    virtual bool start(const AppConfig& cfg) = 0;

    // 幂等停止：未运行调用也应安全返回
    virtual void stop() = 0;

    virtual bool running() const = 0;

    // 运行时开关（步骤 3 的三个开关复用此入口）
    virtual bool enabled() const = 0;
    virtual void set_enabled(bool on) = 0;

    // 状态快照（JSON），并入全局 /status
    virtual std::string status_json() const = 0;
};

}  // namespace sb3
