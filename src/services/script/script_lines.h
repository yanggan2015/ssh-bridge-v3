// script_lines.h — 简易行式脚本（兼容 v2 的 write/send/keys/login/relay/msleep/action）。
// 第一行不是 --lua / #!lua 时走这里；需要分支/循环/回显判断请用 Lua。
#pragma once

#include <string>

#include "services/script/script_lua.h"

namespace sb3 {

bool script_is_lua_source(const std::string& source);

// 同一套上下文：bridge=当前通道，config=全量配置，action_invoker=同通道按钮。
ScriptLuaResult script_lines_run(const std::string& source, const ScriptLuaContext& ctx);

}  // namespace sb3
