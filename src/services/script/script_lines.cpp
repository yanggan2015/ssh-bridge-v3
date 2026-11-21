#include "services/script/script_lines.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
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

struct Run {
    const ScriptLuaContext* ctx = nullptr;
    double deadline = 0;
    std::string output;
    std::string error;
};

double now_seconds() { return static_cast<double>(util::now_ms()) / 1000.0; }

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool starts_word(const std::string& line, const std::string& word, std::string* rest) {
    if (line.size() < word.size()) return false;
    if (lower(line.substr(0, word.size())) != lower(word)) return false;
    if (line.size() > word.size()) {
        char c = line[word.size()];
        if (c != ' ' && c != '\t') return false;
    }
    if (rest) *rest = trim(line.substr(word.size()));
    return true;
}

void check_deadline(Run& r) {
    if (now_seconds() > r.deadline)
        throw std::runtime_error("脚本超过 180 秒总时限，已中止");
}

void append_output(Run& r, const std::string& s) {
    if (s.empty()) return;
    if (r.ctx && r.ctx->print_hook) {
        std::size_t start = 0;
        while (start < s.size()) {
            std::size_t end = s.find('\n', start);
            std::string line = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) r.ctx->print_hook(line);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    if (r.output.size() >= kMaxOutput) return;
    std::size_t room = kMaxOutput - r.output.size();
    r.output.append(s.data(), std::min(s.size(), room));
    if (r.output.size() >= kMaxOutput) r.output.append("\n...[输出已截断]\n");
}

void print_line(Run& r, const std::string& s) { append_output(r, s + "\n"); }

std::string expand_vars(const std::string& in, const ScriptLuaContext& ctx) {
    struct Var { const char* name; const std::string* value; };
    Var vars[] = {
        {"${USER}", &ctx.username}, {"$USER", &ctx.username},
        {"${PASS}", &ctx.password}, {"$PASS", &ctx.password},
        {"${CHANNEL}", &ctx.channel}, {"$CHANNEL", &ctx.channel},
    };
    std::string out = in;
    for (const Var& v : vars) {
        std::size_t pos = 0;
        while ((pos = out.find(v.name, pos)) != std::string::npos) {
            out.replace(pos, std::strlen(v.name), *v.value);
            pos += v.value->size();
        }
    }
    return out;
}

std::string unescape(const std::string& in) {
    std::string s = in;
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            continue;
        }
        char c = s[++i];
        switch (c) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case '\\': out.push_back('\\'); break;
        case '0': out.push_back('\0'); break;
        case 'x': {
            unsigned v = 0;
            int n = 0;
            while (n < 2 && i + 1 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1]))) {
                char h = s[++i];
                v = (v << 4) | static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(h))
                                                        ? h - '0'
                                                        : std::tolower(static_cast<unsigned char>(h)) - 'a' + 10);
                ++n;
            }
            out.push_back(static_cast<char>(v));
            break;
        }
        default: out.push_back(c); break;
        }
    }
    return out;
}

bool is_ssh_channel(const AppConfig* cfg, const std::string& channel) {
    if (!cfg) return false;
    for (const SSHLinkConfig& s : cfg->links)
        if (s.name == channel) return true;
    return false;
}

std::string line_end(const ScriptLuaContext& ctx) {
    return is_ssh_channel(ctx.config, ctx.channel) ? "\r" : "\r\n";
}

void write_or_fail(Run& r, const std::string& bytes) {
    if (!r.ctx || !r.ctx->bridge) throw std::runtime_error("通道不可用");
    r.ctx->bridge->write(bytes.data(), bytes.size());
}

std::string relay_port(const RelayDeviceConfig& r) {
#ifdef _WIN32
    return r.windows_port;
#else
    return r.linux_port.empty() ? r.windows_port : r.linux_port;
#endif
}

const RelayDeviceConfig* find_relay(const AppConfig* cfg, const std::string& name) {
    if (!cfg) return nullptr;
    for (const RelayDeviceConfig& r : cfg->relays)
        if (r.name == name) return &r;
    return nullptr;
}

void wait_mark_output(Run& r, const std::string& needle, double timeout, bool require_needle,
                      std::string* captured) {
    if (!r.ctx || !r.ctx->bridge) throw std::runtime_error("通道不可用");
    std::uint64_t epoch = 0, seen = 0;
    r.ctx->bridge->read_since(&epoch, &seen);
    double end = now_seconds() + std::max(0.0, timeout);
    std::string acc;
    while (now_seconds() < end) {
        check_deadline(r);
        std::uint64_t e = epoch, s = seen;
        std::string d = r.ctx->bridge->read_since(&e, &s);
        epoch = e;
        seen = s;
        if (!d.empty()) {
            acc += d;
            if (acc.size() > kMaxOutput) acc.resize(kMaxOutput);
            if (acc.find(needle) != std::string::npos) {
                if (captured) *captured = acc;
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (captured) *captured = acc;
    if (require_needle)
        throw std::runtime_error("等待超时：未出现 \"" + needle + "\"");
}

void wait_idle_output(Run& r, int idle_ms, double timeout, std::string* captured) {
    if (!r.ctx || !r.ctx->bridge) throw std::runtime_error("通道不可用");
    std::uint64_t epoch = 0, seen = 0;
    r.ctx->bridge->read_since(&epoch, &seen);
    double end = now_seconds() + std::max(0.0, timeout);
    double last_rx = 0;
    bool got = false;
    std::string acc;
    while (now_seconds() < end) {
        check_deadline(r);
        std::uint64_t e = epoch, s = seen;
        std::string d = r.ctx->bridge->read_since(&e, &s);
        epoch = e;
        seen = s;
        if (!d.empty()) {
            got = true;
            last_rx = now_seconds();
            acc += d;
            if (acc.size() > kMaxOutput) acc.resize(kMaxOutput);
        } else if (got && (now_seconds() - last_rx) * 1000.0 >= idle_ms) {
            if (captured) *captured = acc;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (captured) *captured = acc;
}

void run_send_keys(Run& r, const std::string& rest) {
    std::vector<std::string> toks;
    std::istringstream iss(rest);
    std::string tok;
    while (iss >> tok) toks.push_back(tok);
    double duration = 7.0, interval = 0.1;
    std::string seq;
    for (const std::string& t : toks) {
        if (t.rfind("duration=", 0) == 0) { duration = std::atof(t.c_str() + 9); continue; }
        if (t.rfind("interval=", 0) == 0) { interval = std::atof(t.c_str() + 9); continue; }
        if (!seq.empty()) seq.push_back(' ');
        seq += t;
    }
    KeySequence ks{};
    if (parse_key_sequence(seq.c_str(), &ks) <= 0) throw std::runtime_error("send_keys 序列为空");
    if (duration < 0) duration = 0;
    if (duration > 120) duration = 120;
    if (interval <= 0) interval = 0.1;
    double end = now_seconds() + duration;
    int idx = 0;
    int sent = 0;
    while (now_seconds() < end) {
        check_deadline(r);
        write_or_fail(r, std::string(ks.tokens[idx].bytes, ks.tokens[idx].len));
        ++sent;
        ++idx;
        if (idx >= ks.count) idx = 0;
        double rem = end - now_seconds();
        if (rem <= 0) break;
        double wait = std::min(interval, rem);
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(wait * 1000)));
    }
    print_line(r, "send_keys sent=" + std::to_string(sent));
}

void run_relay(Run& r, const std::string& rest) {
    std::istringstream iss(rest);
    std::string name, chword, state;
    int ch = 0;
    double hold = 1.0;
    if (!(iss >> name >> chword >> ch >> state) || lower(chword) != "ch")
        throw std::runtime_error("用法：relay NAME ch N on|off|pulse [秒]");
    const RelayDeviceConfig* dev = find_relay(r.ctx->config, name);
    if (!dev) throw std::runtime_error("继电器不存在：" + name);
    if (!dev->enable) throw std::runtime_error("继电器未启用：" + name);
    if (ch < 1 || ch > dev->channels) throw std::runtime_error("继电器通道越界");
    iss >> hold;
    if (hold <= 0) hold = 1.0;
    std::string mode = lower(state);
    if (mode != "on" && mode != "off" && mode != "pulse")
        throw std::runtime_error("继电器状态只能是 on/off/pulse");
    if (mode == "on") mode = "hold_on";
    else if (mode == "off") mode = "hold_off";
    char err[256] = {0};
    int rc = relay_ctrl_run_mode(relay_port(*dev).c_str(), dev->baudrate, ch, mode.c_str(), hold,
                                 dev->polarity_inverted ? 1 : 0, dev->io_inverted ? 1 : 0,
                                 dev->post_write_delay_ms, err, sizeof(err));
    if (rc != 0) throw std::runtime_error(err[0] ? err : "继电器执行失败");
    if (r.ctx->relay_notifier) {
        bool on = (mode == "hold_on");
        if (mode == "pulse") on = false;
        r.ctx->relay_notifier(name, ch, mode, on);
    }
    print_line(r, "relay " + name + " CH" + std::to_string(ch) + " " + mode + " ok");
}

void run_action(Run& r, const std::string& rest) {
    std::string id = trim(rest);
    if (id.empty()) throw std::runtime_error("用法：action ID");
    if (!r.ctx->action_invoker) throw std::runtime_error("action 不可用");
    ScriptActionResult ar = r.ctx->action_invoker(id);
    if (!ar.ok) throw std::runtime_error(ar.message.empty() ? "action 失败" : ar.message);
    if (ar.relay && r.ctx->relay_notifier)
        r.ctx->relay_notifier(ar.relay_name, ar.relay_channel, ar.relay_mode, ar.relay_on);
    print_line(r, "action " + id + " ok");
}

void run_hostcmd(Run& r, const std::string& rest, bool echo_output) {
    double timeout_s = 120.0;
    std::string cmd;
    std::istringstream iss(rest);
    std::vector<std::string> toks;
    std::string tok;
    while (iss >> tok) {
        if (tok.rfind("timeout=", 0) == 0) {
            timeout_s = std::atof(tok.c_str() + 8);
            continue;
        }
        if (!cmd.empty()) cmd.push_back(' ');
        cmd += tok;
    }
    if (cmd.empty()) throw std::runtime_error("用法：hostcmd 命令 [timeout=秒]");
    HostCommandResult hr = run_host_command(cmd, static_cast<int>(timeout_s * 1000.0),
                                            r.ctx->config ? r.ctx->config->project_root : "");
    if (echo_output && !hr.output.empty()) append_output(r, hr.output);
    if (!hr.ok) {
        std::string msg = "本机命令失败：" + cmd;
        if (!hr.error.empty()) msg += "（" + hr.error + "）";
        throw std::runtime_error(msg);
    }
    if (hr.exit_code != 0) throw std::runtime_error("本机命令退出码 " + std::to_string(hr.exit_code));
}

void run_remotecmd(Run& r, const std::string& rest) {
    std::string cmd;
    double timeout_s = 10.0;
    std::istringstream iss(rest);
    std::vector<std::string> toks;
    std::string tok;
    while (iss >> tok) {
        if (tok.rfind("timeout=", 0) == 0) {
            timeout_s = std::atof(tok.c_str() + 8);
            continue;
        }
        if (!cmd.empty()) cmd.push_back(' ');
        cmd += tok;
    }
    if (cmd.empty()) throw std::runtime_error("用法：remotecmd 命令 [timeout=秒]");
    write_or_fail(r, cmd + line_end(*r.ctx));
    std::string out;
    wait_idle_output(r, 250, timeout_s, &out);
    if (!out.empty()) append_output(r, out);
    else print_line(r, "remotecmd sent: " + cmd);
}

void run_expect(Run& r, const std::string& rest) {
    std::string needle;
    double timeout = 10.0;
    std::istringstream iss(rest);
    std::vector<std::string> toks;
    std::string tok;
    while (iss >> tok) {
        if (tok.rfind("timeout=", 0) == 0) {
            timeout = std::atof(tok.c_str() + 8);
            continue;
        }
        if (!needle.empty()) needle.push_back(' ');
        needle += tok;
    }
    if (needle.empty()) throw std::runtime_error("用法：expect 文本 [timeout=秒]");
    std::string out;
    wait_mark_output(r, needle, timeout, true, &out);
    print_line(r, "expect ok: " + needle);
}

}  // namespace

bool script_is_lua_source(const std::string& source) {
    std::size_t i = 0;
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF)
        i = 3;
    while (i < source.size() && (source[i] == ' ' || source[i] == '\t' || source[i] == '\r' ||
                                 source[i] == '\n'))
        ++i;
    return source.compare(i, 5, "--lua") == 0 || source.compare(i, 5, "#!lua") == 0;
}

ScriptLuaResult script_lines_run(const std::string& source, const ScriptLuaContext& ctx) {
    ScriptLuaResult out;
    Run run;
    run.ctx = &ctx;
    run.deadline = now_seconds() + kWallSeconds;
    try {
        std::istringstream lines(source);
        std::string line;
        int ln = 0;
        while (std::getline(lines, line)) {
            ++ln;
            check_deadline(run);
            std::string s = trim(line);
            if (s.empty() || s[0] == '#' || (s.size() >= 2 && s[0] == '/' && s[1] == '/')) continue;
            std::string rest;
            auto fail_line = [&](const std::string& msg) {
                throw std::runtime_error("第 " + std::to_string(ln) + " 行: " + msg);
            };

            if (starts_word(s, "msleep", &rest) || starts_word(s, "sleep", &rest)) {
                int ms = std::atoi(rest.c_str());
                if (ms < 0) ms = 0;
                if (ms > 120000) ms = 120000;
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                continue;
            }
            if (starts_word(s, "echo", &rest) || starts_word(s, "print", &rest)) {
                print_line(run, expand_vars(unescape(rest), ctx));
                continue;
            }
            if (starts_word(s, "write", &rest)) {
                std::string text = expand_vars(unescape(rest), ctx);
                if (text.empty()) fail_line("write 内容为空");
                write_or_fail(run, text);
                continue;
            }
            if (starts_word(s, "send_keys", &rest)) { run_send_keys(run, rest); continue; }
            if (starts_word(s, "send", &rest)) {
                char norm[64] = {0};
                std::size_t n = normalize_key_char(rest.c_str(), norm, sizeof(norm));
                if (!n) fail_line("send 参数为空");
                write_or_fail(run, std::string(norm, n));
                continue;
            }
            if (starts_word(s, "keys", &rest)) {
                KeySequence ks{};
                if (parse_key_sequence(rest.c_str(), &ks) <= 0) fail_line("keys 序列为空");
                for (int i = 0; i < ks.count; ++i)
                    write_or_fail(run, std::string(ks.tokens[i].bytes, ks.tokens[i].len));
                continue;
            }
            if (starts_word(s, "relay", &rest)) { run_relay(run, rest); continue; }
            if (starts_word(s, "action", &rest)) { run_action(run, rest); continue; }
            if (starts_word(s, "hostcmd", &rest) || starts_word(s, "host", &rest)) {
                run_hostcmd(run, rest, true);
                continue;
            }
            if (starts_word(s, "remotecmd", &rest) || starts_word(s, "cmd", &rest)) {
                run_remotecmd(run, rest);
                continue;
            }
            if (starts_word(s, "expect", &rest) || starts_word(s, "assert", &rest)) {
                run_expect(run, rest);
                continue;
            }
            if (lower(s) == "login" || starts_word(s, "login", &rest)) {
                std::string user = ctx.username, pass = ctx.password;
                if (!rest.empty()) {
                    std::istringstream iss(rest);
                    iss >> user;
                    iss >> pass;
                }
                if (user.empty()) fail_line("login 缺少用户名");
                write_or_fail(run, user + line_end(ctx));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                write_or_fail(run, pass + line_end(ctx));
                print_line(run, "login sent: " + user);
                continue;
            }
            fail_line("未知指令（支持 relay/msleep/write/send/keys/send_keys/login/action/"
                      "hostcmd/remotecmd/expect/echo）");
        }
    } catch (const std::exception& e) {
        out.ok = false;
        out.error = e.what();
        out.output = run.output;
        return out;
    }
    out.ok = true;
    out.output = run.output;
    return out;
}

}  // namespace sb3
