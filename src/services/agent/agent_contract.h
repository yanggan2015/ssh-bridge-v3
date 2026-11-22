// agent_contract.h — S10/C1：18081 设备 Agent 面的发现契约与状态投影。
// 只负责把 AppCore 的当前配置/通道投影成 JSON/文本，不执行任何板控动作。
#pragma once

#include <cstdint>
#include <string>

namespace sb3 {

class AppCore;

namespace agent {

struct UiBridgeInfo {
    bool enabled = false;
    int port = 18096;
    bool token_required = false;
};

struct ResolveResult {
    int http_status = 400;
    bool ok = false;
    std::string name;
    std::string kind;
    bool running = false;
    std::string error_code;
    std::string error_message;
};

std::string health_json(int web_port, const UiBridgeInfo& ui);
std::string help_json(AppCore* core, int web_port, const UiBridgeInfo& ui, bool web_token_required);
std::string status_json(AppCore* core, int web_port, const UiBridgeInfo& ui, bool web_token_required);
std::string help_text(AppCore* core, int web_port, const UiBridgeInfo& ui);
std::string channel_help(AppCore* core, const std::string& channel, int web_port);
ResolveResult resolve_channel(AppCore* core, const std::string& board, const std::string& link,
                              const std::string& channel);
std::string error_json(const std::string& code, const std::string& message,
                       const std::string& request_id);

}  // namespace agent
}  // namespace sb3
