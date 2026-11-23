// agent_scan.cpp — S10/C2：18081 设备面的只读上下文、通道状态与终端探测。
//
// 铁律：本模块只读 AppCore/IBridge 的既有快照与日志环，不写 cmdq、不发 Enter、
// 不打开串口、不调用 18096。终端态只根据日志尾部被动判定；无法判定就是 unknown。
#include "services/agent/agent_scan.h"

#include <cjson/cJSON.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/app_core.h"
#include "services/agent/agent_lock.h"
#include "services/service_manager.h"

namespace sb3 {
namespace agent {
namespace {

const char* kProtocol = "1.1";

std::string json_text(cJSON* root) {
    if (!root) return "{}";
    char* p = cJSON_PrintUnformatted(root);
    std::string out = p ? p : "{}";
    cJSON_free(p);
    cJSON_Delete(root);
    return out;
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

const AppCore::Entry* find_entry(AppCore* core, const std::string& name) {
    if (!core) return nullptr;
    for (const AppCore::Entry& e : core->entries()) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

bool entry_running(const AppCore::Entry& e) {
    return e.bridge && e.bridge->running();
}

std::string entry_state(const AppCore::Entry& e) {
    if (e.bridge) {
        if (e.bridge->running()) return "ready";
        if (e.started) return "offline";
    }
    return "idle";
}

double jnum(const cJSON* o, const char* key, double def = 0.0) {
    const cJSON* v = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, key) : nullptr;
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

bool jbool(const cJSON* o, const char* key, bool def = false) {
    const cJSON* v = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, key) : nullptr;
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

std::string jstr(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, key) : nullptr;
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : std::string();
}

cJSON* bridge_status(const AppCore::Entry& e) {
    if (!e.bridge) return nullptr;
    cJSON* o = cJSON_Parse(e.bridge->status_json().c_str());
    return cJSON_IsObject(o) ? o : (cJSON_Delete(o), nullptr);
}

std::string strip_ansi(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == 0x1b && i + 1 < in.size()) {
            if (in[i + 1] == '[') {
                i += 2;
                while (i < in.size() && !(in[i] >= 0x40 && in[i] <= 0x7e)) ++i;
                continue;
            }
            if (in[i + 1] == ']') {
                i += 2;
                while (i + 1 < in.size() && !(in[i] == 0x1b && in[i + 1] == '\\')) ++i;
                i += 1;
                continue;
            }
            ++i;
            continue;
        }
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') continue;
        out.push_back(in[i]);
    }
    return out;
}

std::string lower_ascii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with_prompt(const std::string& line) {
    std::string t = line;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.pop_back();
    return !t.empty() && (t.back() == '$' || t.back() == '#');
}

struct TerminalProbe {
    std::string state = "unknown";
    std::string confidence = "low";
    std::string reason = "no prompt marker in log tail";
    std::string evidence;
    std::size_t tail_bytes = 0;
};

TerminalProbe classify_terminal(const AppCore::Entry& e, int tail_limit) {
    TerminalProbe p;
    p.tail_bytes = static_cast<std::size_t>(std::max(64, std::min(16384, tail_limit)));
    if (!e.bridge || !e.bridge->running()) {
        p.state = "disconnected";
        p.confidence = "high";
        p.reason = "channel is not running";
        return p;
    }
    std::uint64_t epoch = 0, seen = 0;
    std::string all = e.bridge->read_all(&epoch, &seen);
    std::string tail = strip_ansi(all);
    if (tail.size() > p.tail_bytes) tail = tail.substr(tail.size() - p.tail_bytes);
    if (tail.size() > 120) p.evidence = tail.substr(tail.size() - 120);
    else p.evidence = tail;
    std::string low = lower_ascii(tail);
    if (low.empty()) {
        p.state = "unknown";
        p.confidence = "low";
        p.reason = "log tail is empty";
        return p;
    }

    struct Hit { std::size_t pos; const char* state; const char* reason; };
    std::vector<Hit> hits;
    auto add_hits = [&](const char* needle, const char* state, const char* reason) {
        if (!needle || !*needle) return;
        std::size_t pos = low.rfind(needle);
        if (pos != std::string::npos) hits.push_back({pos, state, reason});
    };
    add_hits("login:", "login", "login prompt");
    add_hits("username:", "login", "username prompt");
    add_hits("password:", "password", "password prompt");
    add_hits("u-boot>", "uboot", "U-Boot prompt");
    add_hits("hit any key", "uboot", "U-Boot autoboot banner");
    add_hits("=>", "uboot", "U-Boot secondary prompt");
    add_hits("root@", "shell", "root shell prompt");

    // 最后一行以 $ / # 结束，视为 shell 提示符；先去掉尾部空白/换行，
    // 避免日志以换行收尾时把真正的提示符行误判为空行。
    std::string probe_tail = tail;
    while (!probe_tail.empty() && std::isspace(static_cast<unsigned char>(probe_tail.back())))
        probe_tail.pop_back();
    std::size_t line_start = probe_tail.rfind('\n');
    std::string last_line = line_start == std::string::npos ? probe_tail : probe_tail.substr(line_start + 1);
    if (ends_with_prompt(last_line)) hits.push_back({low.size() - 1, "shell", "shell prompt at end of tail"});

    if (hits.empty()) {
        p.state = "unknown";
        p.confidence = "low";
        p.reason = "no prompt marker in log tail";
        return p;
    }
    const Hit* best = &hits.front();
    for (const Hit& h : hits) if (h.pos > best->pos) best = &h;
    p.state = best->state;
    p.reason = best->reason;
    p.confidence = (best->pos + 80 >= low.size()) ? "high" : "medium";
    return p;
}

cJSON* terminal_json(const AppCore::Entry& e, int tail_limit) {
    TerminalProbe p = classify_terminal(e, tail_limit);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", p.state.c_str());
    cJSON_AddStringToObject(o, "confidence", p.confidence.c_str());
    cJSON_AddStringToObject(o, "reason", p.reason.c_str());
    cJSON_AddNumberToObject(o, "tail_bytes", static_cast<double>(p.tail_bytes));
    cJSON_AddStringToObject(o, "evidence", p.evidence.c_str());
    return o;
}

void add_buffer(cJSON* o, const cJSON* st) {
    cJSON* b = cJSON_AddObjectToObject(o, "buffer");
    cJSON_AddNumberToObject(b, "capacity", jnum(st, "buffer"));
    cJSON_AddNumberToObject(b, "total", jnum(st, "total"));
    cJSON_AddNumberToObject(b, "epoch", jnum(st, "epoch"));
    cJSON_AddBoolToObject(b, "disk", jbool(st, "log_enabled", false));
}

void add_counters(cJSON* o, const cJSON* st) {
    cJSON* c = cJSON_AddObjectToObject(o, "counters");
    cJSON_AddNumberToObject(c, "tx_bytes", jnum(st, "tx_bytes"));
    cJSON_AddNumberToObject(c, "rx_bytes", jnum(st, "rx_bytes"));
    cJSON_AddNumberToObject(c, "echo_count", jnum(st, "echo_count"));
    cJSON_AddNumberToObject(c, "echo_latency_us", jnum(st, "echo_latency_us"));
}

void add_buttons(cJSON* o, AppCore* core, const std::string& channel) {
    cJSON* arr = cJSON_AddArrayToObject(o, "buttons");
    const std::vector<BoardActionConfig> btns = core ? core->buttons_of(channel) : std::vector<BoardActionConfig>{};
    for (const BoardActionConfig& a : btns) {
        cJSON* b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "id", a.id.c_str());
        cJSON_AddStringToObject(b, "label", a.label.c_str());
        cJSON_AddStringToObject(b, "backend", a.backend.c_str());
        cJSON_AddStringToObject(b, "kind", a.backend.c_str());
        cJSON_AddBoolToObject(b, "enable", a.enable);
        cJSON_AddBoolToObject(b, "enabled", a.enable);
        cJSON_AddBoolToObject(b, "requires_lock", true);
        cJSON_AddBoolToObject(b, "lock_enforced", true);
        cJSON_AddItemToArray(arr, b);
    }
}

void add_features(cJSON* o, const AppCore::Entry& e, AppCore* core) {
    cJSON* f = cJSON_AddObjectToObject(o, "features");
    cJSON_AddBoolToObject(f, "read_log", true);
    cJSON_AddBoolToObject(f, "write", true);
    cJSON_AddBoolToObject(f, "scan", true);
    cJSON_AddBoolToObject(f, "probe", true);
    const std::vector<BoardActionConfig> btns = core ? core->buttons_of(e.name) : std::vector<BoardActionConfig>{};
    cJSON_AddBoolToObject(f, "board_actions", !btns.empty());
    cJSON_AddBoolToObject(f, "serial_control", e.kind == "serial");
    cJSON_AddBoolToObject(f, "ssh_link", e.kind == "ssh");
    cJSON_AddBoolToObject(f, "powershell_link", e.kind == "powershell");
    cJSON_AddBoolToObject(f, "telnet_link", e.kind == "telnet");
    cJSON_AddBoolToObject(f, "incremental_log", true);
    cJSON_AddBoolToObject(f, "log_disk", true);
    cJSON_AddBoolToObject(f, "command", true);
    cJSON_AddBoolToObject(f, "lock", true);
    cJSON_AddBoolToObject(f, "jobs", true);
    cJSON_AddBoolToObject(f, "tftp_artifacts", true);
}

cJSON* channel_json(AppCore* core, const AppCore::Entry& e, bool full, int tail_limit,
                    const std::string& local_ip) {
    cJSON* st = bridge_status(e);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", e.name.c_str());
    cJSON_AddStringToObject(o, "kind", e.kind.c_str());
    cJSON_AddStringToObject(o, "detail", e.detail.c_str());
    bool running = entry_running(e);
    cJSON_AddBoolToObject(o, "running", running);
    cJSON_AddBoolToObject(o, "started", e.started);
    cJSON_AddStringToObject(o, "state", entry_state(e).c_str());

    cJSON* conn = cJSON_AddObjectToObject(o, "connection");
    cJSON_AddBoolToObject(conn, "running", running);
    cJSON_AddBoolToObject(conn, "started", e.started);
    cJSON_AddStringToObject(conn, "mode", e.kind.c_str());
    cJSON_AddStringToObject(conn, "local_ip", local_ip.c_str());
    if (st) {
        std::string peer = jstr(st, "peer");
        if (!peer.empty()) cJSON_AddStringToObject(conn, "peer", peer.c_str());
        std::string port = jstr(st, "port");
        if (!port.empty()) cJSON_AddStringToObject(conn, "port", port.c_str());
        double baud = jnum(st, "baudrate", 0);
        if (baud > 0) cJSON_AddNumberToObject(conn, "baudrate", baud);
        double reconnects = jnum(st, "reconnects", 0);
        if (reconnects > 0) cJSON_AddNumberToObject(conn, "reconnects", reconnects);
        std::string last_error = jstr(st, "last_error");
        if (!last_error.empty()) cJSON_AddStringToObject(o, "last_error", last_error.c_str());
        else cJSON_AddStringToObject(o, "last_error", "");
    } else {
        cJSON_AddStringToObject(o, "last_error", "");
    }
    add_buffer(o, st);
    add_counters(o, st);
    if (full) {
        cJSON_AddItemToObject(o, "terminal", terminal_json(e, tail_limit));
        add_buttons(o, core, e.name);
        add_features(o, e, core);
    } else {
        cJSON* f = cJSON_AddObjectToObject(o, "features");
        const std::vector<BoardActionConfig> btns = core ? core->buttons_of(e.name) : std::vector<BoardActionConfig>{};
        cJSON_AddBoolToObject(f, "board_actions", !btns.empty());
    }
    cJSON_Delete(st);
    return o;
}

cJSON* services_json(ServiceManager* services) {
    if (!services) return cJSON_CreateObject();
    cJSON* o = cJSON_Parse(services->state_json().c_str());
    return cJSON_IsObject(o) ? o : (cJSON_Delete(o), cJSON_CreateObject());
}

void add_takeover_json(cJSON* o, const LeaseInfo& li) {
    if (li.last_takeover_ms <= 0) return;
    cJSON* t = cJSON_AddObjectToObject(o, "last_takeover");
    cJSON_AddStringToObject(t, "reason", li.last_takeover_reason.c_str());
    cJSON_AddStringToObject(t, "actor", li.last_takeover_actor.c_str());
    cJSON_AddStringToObject(t, "previous_owner", li.last_takeover_previous_owner.c_str());
    cJSON_AddNumberToObject(t, "at_ms", static_cast<double>(li.last_takeover_ms));
}

cJSON* lock_json(const std::string& channel) {
    LeaseInfo li = lease_locks().get(channel);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enforced", true);
    cJSON_AddStringToObject(o, "channel", channel.c_str());
    cJSON_AddBoolToObject(o, "held", li.active);
    cJSON_AddBoolToObject(o, "can_takeover", true);
    if (li.active) {
        cJSON_AddStringToObject(o, "owner", li.owner.c_str());
        cJSON_AddNumberToObject(o, "ttl_ms", static_cast<double>(li.ttl_ms));
        cJSON_AddNumberToObject(o, "expires_in_ms", static_cast<double>(li.remaining_ms));
        cJSON_AddNumberToObject(o, "acquired_ms", static_cast<double>(li.acquired_ms));
    } else {
        cJSON_AddStringToObject(o, "owner", "");
        cJSON_AddNumberToObject(o, "expires_in_ms", 0);
    }
    add_takeover_json(o, li);
    return o;
}

cJSON* locks_json() {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enforced", true);
    cJSON_AddStringToObject(o, "note", "per-channel lease lock");
    cJSON* arr = cJSON_AddArrayToObject(o, "leases");
    for (const LeaseInfo& li : lease_locks().list()) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "channel", li.channel.c_str());
        cJSON_AddStringToObject(e, "owner", li.owner.c_str());
        cJSON_AddNumberToObject(e, "ttl_ms", static_cast<double>(li.ttl_ms));
        cJSON_AddNumberToObject(e, "expires_in_ms", static_cast<double>(li.remaining_ms));
        cJSON_AddItemToArray(arr, e);
    }
    return o;
}

void add_global_features(cJSON* root) {
    cJSON* f = cJSON_AddObjectToObject(root, "features");
    cJSON_AddBoolToObject(f, "scan", true);
    cJSON_AddBoolToObject(f, "probe", true);
    cJSON_AddBoolToObject(f, "state", true);
    cJSON_AddBoolToObject(f, "actions", true);
    cJSON_AddBoolToObject(f, "log_read", true);
    cJSON_AddBoolToObject(f, "incremental_log", true);
    cJSON_AddBoolToObject(f, "log_disk", true);
    cJSON_AddBoolToObject(f, "command", true);
    cJSON_AddBoolToObject(f, "lock", true);
    cJSON_AddBoolToObject(f, "jobs", true);
    cJSON_AddBoolToObject(f, "serial_control", true);
    cJSON_AddBoolToObject(f, "ssh_link", true);
    cJSON_AddBoolToObject(f, "powershell_link", true);
    cJSON_AddBoolToObject(f, "telnet_link", true);
    cJSON_AddBoolToObject(f, "tftp_artifacts", true);
}

}  // namespace

std::string status_agent_json(AppCore* core, ServiceManager* services) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "service", "ssh-bridge");
    cJSON_AddStringToObject(root, "view", "agent");
    cJSON_AddStringToObject(root, "protocol", kProtocol);
    cJSON_AddNumberToObject(root, "scanned_at_ms", static_cast<double>(now_ms()));
    cJSON_AddStringToObject(root, "summary", core ? core->summary().c_str() : "");
    std::string local_ip = services ? services->local_ip() : "127.0.0.1";
    cJSON_AddStringToObject(root, "local_ip", local_ip.c_str());
    cJSON* arr = cJSON_AddArrayToObject(root, "channels");
    if (core) {
        for (const AppCore::Entry& e : core->entries())
            cJSON_AddItemToArray(arr, channel_json(core, e, false, 0, local_ip));
    }
    cJSON_AddItemToObject(root, "services", services_json(services));
    cJSON_AddItemToObject(root, "locks", locks_json());
    add_global_features(root);
    return json_text(root);
}

std::string scan_json(AppCore* core, ServiceManager* services) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "service", "ssh-bridge");
    cJSON_AddStringToObject(root, "view", "agent");
    cJSON_AddStringToObject(root, "protocol", kProtocol);
    cJSON_AddNumberToObject(root, "scanned_at_ms", static_cast<double>(now_ms()));
    cJSON_AddStringToObject(root, "summary", core ? core->summary().c_str() : "");
    std::string local_ip = services ? services->local_ip() : "127.0.0.1";
    cJSON_AddStringToObject(root, "local_ip", local_ip.c_str());
    cJSON* arr = cJSON_AddArrayToObject(root, "channels");
    if (core) {
        for (const AppCore::Entry& e : core->entries())
            cJSON_AddItemToArray(arr, channel_json(core, e, true, 2048, local_ip));
    }
    cJSON_AddItemToObject(root, "services", services_json(services));
    cJSON_AddItemToObject(root, "locks", locks_json());
    add_global_features(root);
    return json_text(root);
}

std::string state_json(AppCore* core, ServiceManager* services, const std::string& channel) {
    const AppCore::Entry* e = find_entry(core, channel);
    if (!e) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON* err = cJSON_AddObjectToObject(root, "error");
        cJSON_AddStringToObject(err, "code", "channel_not_found");
        cJSON_AddStringToObject(err, "message", "channel not found");
        return json_text(root);
    }
    std::string local_ip = services ? services->local_ip() : "127.0.0.1";
    cJSON* root = channel_json(core, *e, true, 2048, local_ip);
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "protocol", kProtocol);
    cJSON_AddNumberToObject(root, "scanned_at_ms", static_cast<double>(now_ms()));
    cJSON_AddStringToObject(root, "channel", e->name.c_str());
    cJSON_AddItemToObject(root, "lock", lock_json(e->name));
    cJSON_AddNullToObject(root, "last_action");
    cJSON_AddStringToObject(root, "last_action_note", "job audit is available via /job?id=; last_action summary lands in C6");
    return json_text(root);
}

std::string probe_json(AppCore* core, const std::string& channel, int tail_limit) {
    const AppCore::Entry* e = find_entry(core, channel);
    if (!e) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON* err = cJSON_AddObjectToObject(root, "error");
        cJSON_AddStringToObject(err, "code", "channel_not_found");
        cJSON_AddStringToObject(err, "message", "channel not found");
        return json_text(root);
    }
    TerminalProbe p = classify_terminal(*e, tail_limit);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "protocol", kProtocol);
    cJSON_AddStringToObject(root, "channel", e->name.c_str());
    cJSON_AddStringToObject(root, "kind", e->kind.c_str());
    cJSON_AddBoolToObject(root, "running", entry_running(*e));
    cJSON_AddStringToObject(root, "terminal", p.state.c_str());
    cJSON_AddStringToObject(root, "confidence", p.confidence.c_str());
    cJSON_AddStringToObject(root, "reason", p.reason.c_str());
    cJSON_AddNumberToObject(root, "tail_bytes", static_cast<double>(p.tail_bytes));
    cJSON_AddStringToObject(root, "evidence", p.evidence.c_str());
    cJSON_AddNumberToObject(root, "scanned_at_ms", static_cast<double>(now_ms()));
    cJSON_AddBoolToObject(root, "read_only", true);
    return json_text(root);
}

std::string actions_agent_json(AppCore* core, const std::string& channel) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "view", "agent");
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    if (core) {
        for (const AppCore::Entry& e : core->entries()) {
            if (!channel.empty() && e.name != channel) continue;
            const std::vector<BoardActionConfig> btns = core->buttons_of(e.name);
            for (const BoardActionConfig& a : btns) {
                cJSON* o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "channel", e.name.c_str());
                cJSON_AddStringToObject(o, "channel_kind", e.kind.c_str());
                cJSON_AddStringToObject(o, "id", a.id.c_str());
                cJSON_AddStringToObject(o, "label", a.label.c_str());
                cJSON_AddStringToObject(o, "backend", a.backend.c_str());
                cJSON_AddStringToObject(o, "kind", a.backend.c_str());
                cJSON_AddStringToObject(o, "run", a.run.c_str());
                cJSON_AddBoolToObject(o, "enable", a.enable);
                cJSON_AddBoolToObject(o, "enabled", a.enable);
                cJSON_AddBoolToObject(o, "requires_lock", true);
                cJSON_AddBoolToObject(o, "lock_enforced", true);
                cJSON_AddNumberToObject(o, "delay_ms", a.delay_ms);
                cJSON_AddStringToObject(o, "relay", a.relay.c_str());
                cJSON_AddItemToArray(items, o);
            }
        }
    }
    cJSON_AddNumberToObject(root, "count", static_cast<double>(cJSON_GetArraySize(items)));
    return json_text(root);
}

}  // namespace agent
}  // namespace sb3
