// agent_scan.h — S10/C2：18081 设备面的只读上下文与终端探测。
#pragma once

#include <string>

namespace sb3 {

class AppCore;
class ServiceManager;

namespace agent {

// 精简 Agent 状态：通道表 + 服务 + 锁摘要，不含 UI 控件树。
std::string status_agent_json(AppCore* core, ServiceManager* services);

// 全通道上下文：连接、缓冲区、按钮、features、错误、终端态。
std::string scan_json(AppCore* core, ServiceManager* services);

// 单通道业务状态：连接、终端、游标、锁、最近动作、last_error。
std::string state_json(AppCore* core, ServiceManager* services, const std::string& channel);

// 纯被动探测：只读日志尾部。
std::string probe_json(AppCore* core, const std::string& channel, int tail_limit);

// Agent 视图按钮：与 UI 按钮同源，附加 enabled/requires_lock。
std::string actions_agent_json(AppCore* core, const std::string& channel);

}  // namespace agent
}  // namespace sb3
