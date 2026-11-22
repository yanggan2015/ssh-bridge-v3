#include "services/board_actions/board_action_runner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "core/util/util.h"
#include "services/board_actions/process_run.h"
#include "services/relay/relay_ctrl.h"
#include "services/script/script_lua.h"
#include "services/script/script_lines.h"

namespace sb3 {
namespace {

std::mutex g_log_mu;

const RelayDeviceConfig* find_relay(const AppConfig& cfg, const std::string& name) {
    for (const RelayDeviceConfig& r : cfg.relays) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

std::string relay_port(const RelayDeviceConfig& r) {
#ifdef _WIN32
    return r.windows_port;
#else
    return r.linux_port.empty() ? r.windows_port : r.linux_port;
#endif
}

const BoardConfig* find_board(const AppConfig& cfg, const std::string& name) {
    for (const BoardConfig& b : cfg.boards) {
        if (b.name == name) return &b;
    }
    return nullptr;
}

const SSHLinkConfig* find_link(const AppConfig& cfg, const std::string& name) {
    for (const SSHLinkConfig& s : cfg.links) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

const std::vector<BoardActionConfig>* buttons_of(const AppConfig& cfg, const std::string& channel) {
    if (const BoardConfig* b = find_board(cfg, channel)) return &b->buttons;
    if (const SSHLinkConfig* s = find_link(cfg, channel)) return &s->buttons;
    return nullptr;
}

const BoardActionConfig* find_action(const AppConfig& cfg, const std::string& channel,
                                     const std::string& id) {
    const std::vector<BoardActionConfig>* list = buttons_of(cfg, channel);
    if (!list) return nullptr;
    for (const BoardActionConfig& a : *list) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

std::string line_ending_for(const AppConfig& cfg, const std::string& channel) {
    if (const BoardConfig* b = find_board(cfg, channel)) {
        if (b->line_ending == "cr") return "\r";
        if (b->line_ending == "lf") return "\n";
        if (b->line_ending == "none") return "";
    }
    // SSH PTY 下 CR 与 LF 都会各自提交一行，CRLF 会多出一个空提示符。
    if (find_link(cfg, channel)) return "\r";
    return "\r\n";
}

std::string normalize_lua(std::string source) {
    // Lua 5.4 loadstring 不认 #!lua；把首行 shebang 变成注释。
    if (source.rfind("#!lua", 0) == 0) {
        std::size_t eol = source.find('\n');
        if (eol == std::string::npos) return "--";
        source = "--" + source.substr(eol);
    }
    return source;
}

void append_log(const std::string& channel, const BoardActionConfig& a, bool ok,
                const std::string& message, const std::string& output) {
    std::lock_guard<std::mutex> lock(g_log_mu);
    util::ensure_dir("output");
    std::FILE* f = std::fopen("output\\board_action.log", "ab");
    if (!f) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char head[128];
    std::snprintf(head, sizeof(head), "[%04u-%02u-%02u %02u:%02u:%02u] %s/%s %s: ", st.wYear,
                  st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, channel.c_str(),
                  a.id.c_str(), ok ? "OK" : "FAIL");
    std::fwrite(head, 1, std::strlen(head), f);
    std::fwrite(message.data(), 1, message.size(), f);
    if (!output.empty()) {
        std::fwrite("\n", 1, 1, f);
        std::fwrite(output.data(), 1, output.size(), f);
    }
    std::fwrite("\n", 1, 1, f);
    std::fclose(f);
}

BoardActionResult fail(const std::string& msg, double elapsed) {
    BoardActionResult r;
    r.ok = false;
    r.message = msg;
    r.elapsed_ms = elapsed;
    return r;
}

}  // namespace

BoardActionResult run_board_action(const AppConfig& cfg, const std::string& channel,
                                   const BoardActionConfig& action, IBridge* bridge,
                                   const PortInUseFn& port_in_use, int depth,
                                   const std::function<void(const std::string&)>& progress) {
    auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    if (depth > 6) return fail("按钮递归过深，已中止", elapsed());
    if (action.id.empty()) return fail("按钮 id 为空", elapsed());
    if (!action.enable) return fail("按钮未启用：" + action.id, elapsed());

    BoardActionResult r;
    if (action.backend == "relay") {
        const RelayDeviceConfig* dev = find_relay(cfg, action.relay);
        if (!dev) return fail("继电器设备不存在：" + action.relay, elapsed());
        if (!dev->enable) return fail("继电器设备未启用：" + action.relay, elapsed());
        std::string port = relay_port(*dev);
        if (port.empty()) return fail("继电器串口未配置：" + action.relay, elapsed());
        if (port_in_use && port_in_use(port))
            return fail("继电器串口 " + port + " 正被运行中的通道占用，请先断开该通道", elapsed());
        if (action.channel < 1 || action.channel > dev->channels)
            return fail("继电器通道超出范围：1.." + std::to_string(dev->channels), elapsed());
        std::string mode = action.mode;
        if (mode != "hold_on" && mode != "hold_off" && mode != "pulse")
            return fail("未知继电器模式：" + mode, elapsed());
        double hold = action.hold_seconds;
        if (hold < 0.05) hold = 0.05;
        if (hold > 30.0) hold = 30.0;
        char err[256] = {0};
        int rc = relay_ctrl_run_mode(port.c_str(), dev->baudrate, action.channel, mode.c_str(),
                                     hold, action.polarity_inverted ? 1 : 0,
                                     dev->io_inverted ? 1 : 0, dev->post_write_delay_ms, err,
                                     sizeof(err));
        if (rc != 0) return fail(err[0] ? err : "继电器执行失败", elapsed());
        r.ok = true;
        r.message = "继电器 " + action.relay + " CH" + std::to_string(action.channel) + " " + mode;
        r.relay = true;
        r.relay_name = action.relay;
        r.relay_port = port;
        r.relay_channel = action.channel;
        r.relay_mode = mode;
        r.relay_on = (mode == "hold_on");
        r.elapsed_ms = elapsed();
        append_log(channel, action, r.ok, r.message, r.output);
        return r;
    }

    if (action.backend == "commands") {
        if (action.commands.empty()) return fail("按钮没有配置命令", elapsed());
        std::string cmd_out;
        int last_code = 0;
        for (const std::string& command : action.commands) {
            if (command.empty()) continue;
            if (action.run == "host") {
                HostCommandResult hr = run_host_command(command, 120000, cfg.project_root);
                last_code = hr.exit_code;
                if (!hr.output.empty()) {
                    if (!cmd_out.empty()) cmd_out.push_back('\n');
                    cmd_out += hr.output;
                }
                if (!hr.ok) {
                    r.ok = false;
                    r.message = "本机命令失败：" + command;
                    if (!hr.error.empty()) r.message += "（" + hr.error + "）";
                    r.output = cmd_out;
                    r.exit_code = last_code;
                    r.elapsed_ms = elapsed();
                    append_log(channel, action, false, r.message, r.output);
                    return r;
                }
            } else {
                if (!bridge) return fail("通道不可用：" + channel, elapsed());
                std::string line = command + line_ending_for(cfg, channel);
                if (bridge->write(line.data(), line.size()) == 0)
                    return fail("命令发送失败（通道可能已断开）：" + command, elapsed());
                if (action.delay_ms > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(action.delay_ms));
            }
        }
        r.ok = true;
        r.message = (action.run == "host" ? "本机命令已执行：" : "命令已发送到通道：") +
                    std::to_string(action.commands.size()) + " 条";
        r.exit_code = last_code;
        r.output = cmd_out;
        r.elapsed_ms = elapsed();
        append_log(channel, action, true, r.message, r.output);
        return r;
    }

    if (action.backend == "script") {
        if (action.script.empty()) return fail("按钮没有配置脚本", elapsed());
        if (!bridge) return fail("通道不可用：" + channel, elapsed());
        ScriptLuaContext ctx;
        ctx.bridge = bridge;
        ctx.config = &cfg;
        ctx.channel = channel;
        if (const BoardConfig* b = find_board(cfg, channel)) {
            ctx.username = b->username;
            ctx.password = b->password_present ? b->password : cfg.def.password;
        } else if (const SSHLinkConfig* s = find_link(cfg, channel)) {
            ctx.username = s->username;
            ctx.password = s->password_present ? s->password : cfg.def.password;
        }
        ctx.print_hook = progress;
        ctx.relay_notifier = [&r, &cfg](const std::string& name, int ch,
                                        const std::string& mode, bool on) {
            const RelayDeviceConfig* dev = find_relay(cfg, name);
            r.relay = true;
            r.relay_name = name;
            r.relay_port = dev ? (dev->windows_port.empty() ? dev->linux_port : dev->windows_port)
                               : std::string();
            r.relay_channel = ch;
            r.relay_mode = mode;
            r.relay_on = on;
        };
        ctx.action_invoker = [&](const std::string& id) -> ScriptActionResult {
            const BoardActionConfig* nested = find_action(cfg, channel, id);
            if (!nested)
                return ScriptActionResult{false, "action '" + id + "' 不存在", false, "", "", 0, false};
            BoardActionResult ar = run_board_action(cfg, channel, *nested, bridge, port_in_use,
                                                    depth + 1, progress);
            ScriptActionResult sr;
            sr.ok = ar.ok;
            sr.message = ar.message;
            sr.relay = ar.relay;
            sr.relay_name = ar.relay_name;
            sr.relay_mode = ar.relay_mode;
            sr.relay_channel = ar.relay_channel;
            sr.relay_on = ar.relay_on;
            return sr;
        };
        ScriptLuaResult lr = script_is_lua_source(action.script)
                                 ? script_lua_run(normalize_lua(action.script), ctx)
                                 : script_lines_run(action.script, ctx);
        r.output = lr.output;
        if (!lr.ok) {
            r.ok = false;
            r.message = (script_is_lua_source(action.script) ? "Lua 执行失败：" : "脚本执行失败：") +
                        lr.error;
            r.elapsed_ms = elapsed();
            append_log(channel, action, false, r.message, r.output);
            return r;
        }
        r.ok = true;
        r.message = (script_is_lua_source(action.script) ? "Lua 执行完成" : "脚本执行完成") +
                    (lr.output.empty() ? std::string() : "（有输出）");
        r.elapsed_ms = elapsed();
        append_log(channel, action, true, r.message, r.output);
        return r;
    }

    return fail("未知按钮类型：" + action.backend, elapsed());
}

void run_board_action_async(const AppConfig& cfg, const std::string& channel,
                            const BoardActionConfig& action, std::shared_ptr<IBridge> bridge,
                            const PortInUseFn& port_in_use, HWND notify, UINT msg,
                            UINT progress_msg) {
    std::thread([cfg, channel, action, bridge, port_in_use, notify, msg, progress_msg]() {
        auto progress = [notify, progress_msg, channel, id = action.id](const std::string& line) {
            if (!progress_msg || !notify || !IsWindow(notify)) return;
            AsyncBoardProgress* p = new AsyncBoardProgress();
            p->channel = channel;
            p->action_id = id;
            p->line = line;
            if (!PostMessageW(notify, progress_msg, 0, reinterpret_cast<LPARAM>(p))) delete p;
        };
        BoardActionResult r = run_board_action(cfg, channel, action, bridge.get(), port_in_use, 0, progress);
        AsyncBoardResult* out = new AsyncBoardResult();
        out->ok = r.ok;
        out->message = r.message;
        out->output = r.output;
        out->channel = channel;
        out->action_id = action.id;
        out->action_label = action.label;
        out->elapsed_ms = r.elapsed_ms;
        out->relay = r.relay;
        out->relay_port = r.relay_port;
        out->relay_name = r.relay_name;
        out->relay_mode = r.relay_mode;
        out->relay_channel = r.relay_channel;
        out->relay_on = r.relay_on;
        if (notify && IsWindow(notify)) {
            if (!PostMessageW(notify, msg, 0, reinterpret_cast<LPARAM>(out))) delete out;
        } else {
            delete out;
        }
    }).detach();
}

}  // namespace sb3
