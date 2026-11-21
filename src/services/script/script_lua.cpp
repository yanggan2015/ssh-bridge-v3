#include "services/script/script_lua.h"

#include <windows.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/util/key_chars.h"
#include "core/util/util.h"
#include "services/board_actions/process_run.h"
#include "services/relay/relay_ctrl.h"

namespace sb3 {
namespace {

constexpr double kWallSeconds = 180.0;
constexpr std::size_t kMaxOutput = 1024 * 1024;

struct LuaRun {
    const ScriptLuaContext* ctx = nullptr;
    double deadline = 0;
    std::string output;
    std::string error;
};

LuaRun* RunOf(lua_State* L) {
    return *reinterpret_cast<LuaRun**>(lua_getextraspace(L));
}

void AppendOutput(LuaRun* r, const std::string& s) {
    if (!r || s.empty()) return;
    if (r->ctx && r->ctx->print_hook) {
        std::size_t start = 0;
        while (start < s.size()) {
            std::size_t end = s.find('\n', start);
            std::string line = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) r->ctx->print_hook(line);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    if (r->output.size() >= kMaxOutput) return;
    std::size_t room = kMaxOutput - r->output.size();
    r->output.append(s.data(), std::min(s.size(), room));
    if (r->output.size() >= kMaxOutput) r->output.append("\n...[输出已截断]");
}

double NowSeconds() {
    return static_cast<double>(util::now_ms()) / 1000.0;
}

void CheckDeadline(lua_State* L) {
    LuaRun* r = RunOf(L);
    if (r && NowSeconds() > r->deadline) {
        luaL_error(L, "脚本超过 %.0f 秒总时限，已中止", kWallSeconds);
    }
}

void Hook(lua_State* L, lua_Debug*) {
    LuaRun* r = RunOf(L);
    if (r && NowSeconds() > r->deadline) {
        luaL_error(L, "脚本超过 %.0f 秒总时限，已中止", kWallSeconds);
    }
}

IBridge* BridgeOf(lua_State* L) {
    LuaRun* r = RunOf(L);
    if (!r || !r->ctx || !r->ctx->bridge) luaL_error(L, "通道不可用");
    return r->ctx->bridge;
}

bool IsSshChannel(const ScriptLuaContext* ctx) {
    if (!ctx || !ctx->config) return false;
    for (const SSHLinkConfig& s : ctx->config->links) {
        if (s.name == ctx->channel) return true;
    }
    return false;
}

void PushMark(lua_State* L, std::uint64_t epoch, std::uint64_t seen) {
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, static_cast<lua_Integer>(epoch));
    lua_setfield(L, -2, "epoch");
    lua_pushinteger(L, static_cast<lua_Integer>(seen));
    lua_setfield(L, -2, "seen");
}

void ReadMark(lua_State* L, int idx, std::uint64_t* epoch, std::uint64_t* seen) {
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_getfield(L, idx, "epoch");
    *epoch = static_cast<std::uint64_t>(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, idx, "seen");
    *seen = static_cast<std::uint64_t>(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
}

std::string ReadDelta(IBridge* b, std::uint64_t* epoch, std::uint64_t* seen) {
    if (!b || !epoch || !seen) return std::string();
    return b->read_since(epoch, seen);
}

int LMsleep(lua_State* L) {
    int ms = static_cast<int>(luaL_checkinteger(L, 1));
    CheckDeadline(L);
    if (ms < 0) ms = 0;
    if (ms > 120000) ms = 120000;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return 0;
}

int LSleepSeconds(lua_State* L) {
    double sec = luaL_optnumber(L, 1, 1.0);
    CheckDeadline(L);
    if (sec < 0) sec = 0;
    if (sec > 120) sec = 120;
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(sec * 1000)));
    return 0;
}

int LPrint(lua_State* L) {
    LuaRun* r = RunOf(L);
    if (!r) return 0;
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line.push_back('\t');
        if (s && len) line.append(s, len);
        lua_pop(L, 1);
    }
    line.push_back('\n');
    AppendOutput(r, line);
    return 0;
}

int LChannelMark(lua_State* L) {
    IBridge* b = BridgeOf(L);
    CheckDeadline(L);
    std::uint64_t epoch = 0, seen = 0;
    b->read_since(&epoch, &seen);
    PushMark(L, epoch, seen);
    return 1;
}

int LChannelReadSince(lua_State* L) {
    IBridge* b = BridgeOf(L);
    std::uint64_t epoch = 0, seen = 0;
    CheckDeadline(L);
    ReadMark(L, 1, &epoch, &seen);
    std::string d = b->read_since(&epoch, &seen);
    lua_pushlstring(L, d.c_str(), d.size());
    return 1;
}

int LChannelWrite(lua_State* L) {
    IBridge* b = BridgeOf(L);
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    CheckDeadline(L);
    if (!s || n == 0) return 0;
    b->write(s, n);
    return 0;
}

int LChannelSend(lua_State* L) {
    IBridge* b = BridgeOf(L);
    const char* tok = luaL_checkstring(L, 1);
    char norm[64] = {0};
    size_t nn = normalize_key_char(tok, norm, sizeof(norm));
    CheckDeadline(L);
    if (!nn) luaL_error(L, "send 参数为空");
    b->write(norm, nn);
    return 0;
}

int LChannelKeys(lua_State* L) {
    IBridge* b = BridgeOf(L);
    const char* seq = luaL_checkstring(L, 1);
    KeySequence ks{};
    CheckDeadline(L);
    if (parse_key_sequence(seq, &ks) <= 0) luaL_error(L, "keys 序列为空");
    for (int i = 0; i < ks.count; ++i) b->write(ks.tokens[i].bytes, ks.tokens[i].len);
    return 0;
}

int LChannelSendKeys(lua_State* L) {
    IBridge* b = BridgeOf(L);
    const char* seq = luaL_checkstring(L, 1);
    double dur = luaL_optnumber(L, 2, 7.0);
    double iv = luaL_optnumber(L, 3, 0.1);
    KeySequence ks{};
    CheckDeadline(L);
    if (parse_key_sequence(seq, &ks) <= 0) luaL_error(L, "send_keys 序列为空");
    if (dur <= 0) dur = 0;
    double end = NowSeconds() + dur;
    int sent = 0;
    int ti = 0;
    while (NowSeconds() < end) {
        CheckDeadline(L);
        b->write(ks.tokens[ti].bytes, ks.tokens[ti].len);
        ++sent;
        ++ti;
        if (ti >= ks.count) ti = 0;
        double rem = end - NowSeconds();
        if (rem <= 0) break;
        double wait = iv > 0 ? iv : 0.1;
        if (wait > rem) wait = rem;
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(wait * 1000)));
    }
    lua_pushinteger(L, sent);
    return 1;
}

int LChannelLogin(lua_State* L) {
    LuaRun* r = RunOf(L);
    IBridge* b = BridgeOf(L);
    CheckDeadline(L);
    if (!r || !r->ctx || r->ctx->username.empty()) luaL_error(L, "login 需要通道用户名");
    std::string user = r->ctx->username + "\r\n";
    b->write(user.data(), user.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::string pass = r->ctx->password + "\r\n";
    b->write(pass.data(), pass.size());
    return 0;
}

int LChannelWaitMark(lua_State* L) {
    IBridge* b = BridgeOf(L);
    std::uint64_t epoch = 0, seen = 0;
    ReadMark(L, 1, &epoch, &seen);
    size_t needle_n = 0;
    const char* needle = luaL_checklstring(L, 2, &needle_n);
    double timeout = luaL_optnumber(L, 3, 5.0);
    if (!needle_n) luaL_error(L, "wait_mark 需要匹配文本");
    double end = NowSeconds() + std::max(0.0, timeout);
    std::string acc;
    while (NowSeconds() < end) {
        CheckDeadline(L);
        std::uint64_t e2 = epoch, s2 = seen;
        std::string d = b->read_since(&e2, &s2);
        epoch = e2;
        seen = s2;
        if (!d.empty()) {
            acc += d;
            if (acc.size() > kMaxOutput) acc.resize(kMaxOutput);
            if (acc.find(needle) != std::string::npos) {
                lua_pushlstring(L, acc.c_str(), acc.size());
                return 1;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    lua_pushnil(L);
    lua_pushstring(L, "timeout");
    return 2;
}

int LChannelWaitIdle(lua_State* L) {
    IBridge* b = BridgeOf(L);
    std::uint64_t epoch = 0, seen = 0;
    ReadMark(L, 1, &epoch, &seen);
    int idle_ms = static_cast<int>(luaL_optinteger(L, 2, 200));
    double timeout = luaL_optnumber(L, 3, 5.0);
    if (idle_ms < 20) idle_ms = 20;
    if (idle_ms > 60000) idle_ms = 60000;
    double end = NowSeconds() + std::max(0.0, timeout);
    std::string acc;
    bool got = false;
    double last_rx = 0;
    while (NowSeconds() < end) {
        CheckDeadline(L);
        std::uint64_t e2 = epoch, s2 = seen;
        std::string d = b->read_since(&e2, &s2);
        epoch = e2;
        seen = s2;
        if (!d.empty()) {
            got = true;
            last_rx = NowSeconds();
            acc += d;
            if (acc.size() > kMaxOutput) acc.resize(kMaxOutput);
        } else if (got && (NowSeconds() - last_rx) * 1000.0 >= idle_ms) {
            lua_pushlstring(L, acc.c_str(), acc.size());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    lua_pushnil(L);
    lua_pushstring(L, "timeout");
    return 2;
}

int LChannelExec(lua_State* L) {
    IBridge* b = BridgeOf(L);
    const char* cmd = luaL_checkstring(L, 1);
    double timeout = luaL_optnumber(L, 2, 10.0);
    if (timeout <= 0) timeout = 10.0;
    if (timeout > 60.0) timeout = 60.0;
    CheckDeadline(L);
    std::uint64_t epoch = 0, seen = 0;
    b->read_since(&epoch, &seen);
    std::string line = std::string(cmd) + (IsSshChannel(RunOf(L)->ctx) ? "\r" : "\r\n");
    b->write(line.data(), line.size());
    double end = NowSeconds() + timeout;
    std::string acc;
    bool got = false;
    double last_rx = 0;
    while (NowSeconds() < end) {
        CheckDeadline(L);
        std::uint64_t e2 = epoch, s2 = seen;
        std::string d = b->read_since(&e2, &s2);
        epoch = e2;
        seen = s2;
        if (!d.empty()) {
            got = true;
            last_rx = NowSeconds();
            acc += d;
            if (acc.size() > kMaxOutput) acc.resize(kMaxOutput);
        } else if (got && (NowSeconds() - last_rx) * 1000.0 >= 250.0) {
            lua_pushlstring(L, acc.c_str(), acc.size());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    lua_pushlstring(L, acc.c_str(), acc.size());
    return 1;
}

const RelayDeviceConfig* FindRelay(const AppConfig* cfg, const char* name) {
    if (!cfg || !name) return nullptr;
    for (const RelayDeviceConfig& r : cfg->relays) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

std::string RelayPort(const RelayDeviceConfig& r) {
#ifdef _WIN32
    return r.windows_port;
#else
    return r.linux_port.empty() ? r.windows_port : r.linux_port;
#endif
}

int LRelay(lua_State* L) {
    LuaRun* run = RunOf(L);
    const char* name = luaL_checkstring(L, 1);
    int ch = static_cast<int>(luaL_checkinteger(L, 2));
    const char* state = luaL_checkstring(L, 3);
    CheckDeadline(L);
    const RelayDeviceConfig* dev = FindRelay(run && run->ctx ? run->ctx->config : nullptr, name);
    if (!dev) luaL_error(L, "继电器 '%s' 不存在", name);
    if (ch < 1 || ch > dev->channels) luaL_error(L, "继电器通道超出范围");
    int on = 0;
    if (std::strcmp(state, "on") == 0) on = 1;
    else if (std::strcmp(state, "off") == 0) on = 0;
    else luaL_error(L, "relay 状态只能是 on/off");
    char err[256] = {0};
    int rc = relay_ctrl_set(RelayPort(*dev).c_str(), dev->baudrate, ch, on, dev->io_inverted,
                            dev->post_write_delay_ms, err, sizeof(err));
    if (rc != 0) luaL_error(L, "%s", err[0] ? err : "继电器操作失败");
    if (run && run->ctx && run->ctx->relay_notifier)
        run->ctx->relay_notifier(name, ch, on ? "hold_on" : "hold_off", on != 0);
    return 0;
}

int LRelayMode(lua_State* L) {
    LuaRun* run = RunOf(L);
    const char* name = luaL_checkstring(L, 1);
    int ch = static_cast<int>(luaL_checkinteger(L, 2));
    const char* mode = luaL_checkstring(L, 3);
    double hold = luaL_optnumber(L, 4, 1.0);
    int pol = lua_toboolean(L, 5) ? 1 : 0;
    CheckDeadline(L);
    const RelayDeviceConfig* dev = FindRelay(run && run->ctx ? run->ctx->config : nullptr, name);
    if (!dev) luaL_error(L, "继电器 '%s' 不存在", name);
    if (ch < 1 || ch > dev->channels) luaL_error(L, "继电器通道超出范围");
    if (std::strcmp(mode, "hold_on") != 0 && std::strcmp(mode, "hold_off") != 0 &&
        std::strcmp(mode, "pulse") != 0)
        luaL_error(L, "mode 只能是 hold_on/hold_off/pulse");
    char err[256] = {0};
    int rc = relay_ctrl_run_mode(RelayPort(*dev).c_str(), dev->baudrate, ch, mode, hold, pol,
                                 dev->io_inverted, dev->post_write_delay_ms, err, sizeof(err));
    if (rc != 0) luaL_error(L, "%s", err[0] ? err : "继电器模式执行失败");
    if (run && run->ctx && run->ctx->relay_notifier) {
        bool on = std::strcmp(mode, "hold_on") == 0;
        if (std::strcmp(mode, "pulse") == 0) on = false;
        run->ctx->relay_notifier(name, ch, mode, on);
    }
    return 0;
}

void NotifyActionRelay(lua_State* L, const ScriptActionResult& r) {
    LuaRun* run = RunOf(L);
    if (r.relay && run && run->ctx && run->ctx->relay_notifier)
        run->ctx->relay_notifier(r.relay_name, r.relay_channel, r.relay_mode, r.relay_on);
}

int LAction(lua_State* L) {
    LuaRun* run = RunOf(L);
    const char* id = luaL_checkstring(L, 1);
    CheckDeadline(L);
    if (!run || !run->ctx || !run->ctx->action_invoker)
        luaL_error(L, "action 不可用");
    ScriptActionResult r = run->ctx->action_invoker(id);
    if (!r.ok) luaL_error(L, "%s", r.message.empty() ? "action 失败" : r.message.c_str());
    NotifyActionRelay(L, r);
    return 0;
}

int LBoardFixed(lua_State* L, const char* id) {
    LuaRun* run = RunOf(L);
    CheckDeadline(L);
    if (!run || !run->ctx || !run->ctx->action_invoker)
        luaL_error(L, "board 动作不可用");
    ScriptActionResult r = run->ctx->action_invoker(id);
    if (!r.ok) luaL_error(L, "%s", r.message.empty() ? "board 动作失败" : r.message.c_str());
    NotifyActionRelay(L, r);
    return 0;
}

int LBoardPowerOn(lua_State* L) { return LBoardFixed(L, "power_on"); }
int LBoardPowerOff(lua_State* L) { return LBoardFixed(L, "power_off"); }
int LBoardReboot(lua_State* L) { return LBoardFixed(L, "reboot"); }

int LHostExec(lua_State* L) {
    LuaRun* run = RunOf(L);
    const char* cmd = luaL_checkstring(L, 1);
    int timeout = static_cast<int>(luaL_optinteger(L, 2, 120000));
    CheckDeadline(L);
    std::string wd = run && run->ctx && run->ctx->config ? run->ctx->config->project_root
                                                         : std::string();
    HostCommandResult hr = run_host_command(cmd, timeout, wd);
    lua_createtable(L, 0, 3);
    lua_pushboolean(L, hr.ok ? 1 : 0);
    lua_setfield(L, -2, "ok");
    lua_pushinteger(L, hr.exit_code);
    lua_setfield(L, -2, "exit_code");
    std::string out = hr.output;
    if (!hr.error.empty()) {
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
        out += hr.error;
    }
    lua_pushlstring(L, out.c_str(), out.size());
    lua_setfield(L, -2, "output");
    return 1;
}

void RegisterChannel(lua_State* L) {
    static const struct {
        const char* name;
        lua_CFunction fn;
    } methods[] = {
        {"mark", LChannelMark},
        {"read_since", LChannelReadSince},
        {"write", LChannelWrite},
        {"send", LChannelSend},
        {"keys", LChannelKeys},
        {"send_keys", LChannelSendKeys},
        {"login", LChannelLogin},
        {"wait_mark", LChannelWaitMark},
        {"expect", LChannelWaitMark},
        {"wait", LChannelWaitMark},
        {"wait_idle", LChannelWaitIdle},
        {"exec", LChannelExec},
        {nullptr, nullptr},
    };
    lua_newtable(L);
    for (int i = 0; methods[i].name; ++i) {
        lua_pushcfunction(L, methods[i].fn);
        lua_setfield(L, -2, methods[i].name);
    }
    lua_setglobal(L, "channel");
}

}  // namespace

ScriptLuaResult script_lua_run(const std::string& source, const ScriptLuaContext& ctx) {
    ScriptLuaResult out;
    if (source.empty()) {
        out.error = "脚本为空";
        return out;
    }
    lua_State* L = luaL_newstate();
    if (!L) {
        out.error = "Lua 初始化失败";
        return out;
    }
    LuaRun run;
    run.ctx = &ctx;
    run.deadline = NowSeconds() + kWallSeconds;
    *reinterpret_cast<LuaRun**>(lua_getextraspace(L)) = &run;

    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);

    RegisterChannel(L);
    lua_pushcfunction(L, LRelay);
    lua_setglobal(L, "relay");
    lua_pushcfunction(L, LRelayMode);
    lua_setglobal(L, "relay_mode");
    lua_pushcfunction(L, LAction);
    lua_setglobal(L, "action");
    lua_pushcfunction(L, LMsleep);
    lua_setglobal(L, "msleep");
    lua_pushcfunction(L, LSleepSeconds);
    lua_setglobal(L, "sleep");
    lua_pushcfunction(L, LPrint);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, LHostExec);
    lua_setglobal(L, "host_exec");
    // 命名别名：hostcmd = 本机命令，remotecmd = 当前板端会话命令
    lua_pushcfunction(L, LHostExec);
    lua_setglobal(L, "hostcmd");
    lua_pushcfunction(L, LChannelExec);
    lua_setglobal(L, "remotecmd");
    // 更直观的 host.exec(...) 写法与 host_exec 等价
    lua_newtable(L);
    lua_pushcfunction(L, LHostExec);
    lua_setfield(L, -2, "exec");
    lua_setglobal(L, "host");
    // 板卡按钮语法糖：board.power_on() / power_off() / reboot() / action("id")
    lua_newtable(L);
    lua_pushcfunction(L, LAction);
    lua_setfield(L, -2, "action");
    lua_pushcfunction(L, LBoardPowerOn);
    lua_setfield(L, -2, "power_on");
    lua_pushcfunction(L, LBoardPowerOff);
    lua_setfield(L, -2, "power_off");
    lua_pushcfunction(L, LBoardReboot);
    lua_setfield(L, -2, "reboot");
    lua_setglobal(L, "board");

    lua_sethook(L, Hook, LUA_MASKCOUNT, 10000);

    int rc = luaL_loadstring(L, source.c_str());
    if (rc != LUA_OK) {
        out.error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "Lua 语法错误";
        lua_close(L);
        out.output = run.output;
        return out;
    }
    rc = lua_pcall(L, 0, 0, 0);
    if (rc != LUA_OK) {
        out.error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "Lua 运行错误";
        lua_close(L);
        out.output = run.output;
        return out;
    }
    lua_close(L);
    out.ok = true;
    out.output = run.output;
    return out;
}

}  // namespace sb3
