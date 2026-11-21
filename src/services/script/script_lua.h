// script_lua.h — S12 板卡按钮 Lua 5.4 执行器。
#pragma once

#include <functional>
#include <string>

#include "core/bridge/bridge.h"
#include "core/config/config.h"

namespace sb3 {

struct ScriptActionResult {
    bool        ok = false;
    std::string message;
    bool        relay = false;
    std::string relay_name;
    std::string relay_mode;
    int         relay_channel = 0;
    bool        relay_on = false;
};

struct ScriptLuaContext {
    IBridge*        bridge = nullptr;
    const AppConfig* config = nullptr;
    std::string     channel;
    std::string     username;
    std::string     password;
    std::function<ScriptActionResult(const std::string& id)> action_invoker;
    // 脚本内每次继电器动作后回执，供 UI 同步继电器状态面板
    std::function<void(const std::string& relay, int channel, const std::string& mode, bool on)>
        relay_notifier;
    // 每行实时输出（print / 命令回显），供状态栏显示“当前打印行”
    std::function<void(const std::string&)> print_hook;
};

struct ScriptLuaResult {
    bool        ok = false;
    std::string error;
    std::string output;
};

// 同步执行；调用方负责后台线程。总墙钟 180 秒。
ScriptLuaResult script_lua_run(const std::string& source, const ScriptLuaContext& ctx);

}  // namespace sb3
