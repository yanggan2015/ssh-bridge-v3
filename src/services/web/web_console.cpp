// web_console.cpp — S9：单端口 HTTP + WebSocket + 10 个手机可用的网页。
//
// 设计口径：http_port 默认 18081；/ws/log 下行 BINARY；上行 /write 进 cmdq；
// 无 token 只读（status/log），写接口 401；3 个 WS 会话各自独立游标。
#include "services/web/web_console.h"
#include "services/web/web_pages.h"

#include <winsock2.h>
#include <windows.h>
#include <mongoose.h>
#include <cjson/cJSON.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/app_core.h"
#include "core/bridge/bridge.h"
#include "core/util/key_chars.h"
#include "core/util/util.h"
#include "services/agent/agent_contract.h"
#include "services/agent/agent_lock.h"
#include "services/agent/agent_scan.h"
#include "services/board_actions/board_action_runner.h"
#include "services/service_manager.h"
#include "ui/ui_messages.h"

namespace sb3 {
namespace {

const char* kNav[][3] = {
    {"dashboard", "仪表盘", "总览"},
    {"channels", "通道管理", "总览"},
    {"actions", "板控动作", "操作"},
    {"button-script", "快捷/脚本", "操作"},
    {"relay", "继电器", "操作"},
    {"tftp", "TFTP", "服务"},
    {"nfs", "NFS", "服务"},
    {"share", "共享目录", "服务"},
    {"tcp-forward", "TCP 转发", "服务"},
    {"tunnel", "隧道", "服务"},
    {"agent", "Agent 接口", "系统"},
    {"config", "配置", "系统"},
};

std::string page_title(const std::string& page) {
    for (const auto& n : kNav) if (page == n[0]) return n[1];
    return "ssh-bridge";
}

std::string page_subtitle(const std::string& page) {
    if (page == "dashboard") return "终端、通道与服务状态总览；同一份 ring，不另开设备连接。";
    if (page == "channels") return "串口与 SSH 通道的新增、编辑、改名和删除；保存后由 UI 线程重载。";
    if (page == "agent") return "查看本机 Agent UI 桥配置，并直接做 capabilities / tree / state / logs 自检。";
    if (page == "config") return "基础 / 详细两套表单；保存后由 UI 线程重载，避免 Web 线程直接改内存。";
    if (page == "actions") return "编辑并测试板控按钮：commands、relay、script 三种执行方式。";
    if (page == "button-script") return "管理每个通道底部的快捷指令，发送只走当前通道。";
    if (page == "relay") return "管理继电器设备、端口与通道，并做 ON/OFF/脉冲测试。";
    if (page == "tftp") return "TFTP 启停、端口与根目录状态，以及共享文件管理。";
    if (page == "nfs") return "NFS 导出状态、共享根目录与文件管理。";
    if (page == "share") return "HTTP 共享状态、访问地址与共享文件管理。";
    if (page == "tcp-forward") return "板卡 IP 末段映射到本地端口；保存后由 UI 线程重载。";
    if (page == "tunnel") return "外部 eadk-tunnel / frpc 子进程；HTTP 与入站 SSH 分别映射。";
    return "同一份通道 ring / cmdq。";
}

std::string nav_html(const std::string& page) {
    std::string out;
    std::string group;
    for (const auto& n : kNav) {
        if (group != n[2]) {
            group = n[2];
            out += "<div class=\"nav-title\">" + group + "</div>";
        }
        out += "<a href=\"/" + std::string(n[0]) + "\"" +
               (page == n[0] ? " class=\"on\"" : "") + ">" + n[1] + "</a>";
    }
    return out;
}

std::string page_shell(const std::string& page, const std::string& body, const std::string& script) {
    std::string html = "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
                       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                       "<title>" + page_title(page) + " · ssh-bridge</title>"
                       "<link rel=\"stylesheet\" href=\"/static/app.css\"></head><body><div class=\"layout\">"
                       "<aside class=\"sidebar\"><a class=\"brand\" href=\"/dashboard\"><span class=\"brand-mark\">SB</span><span><strong>ssh-bridge</strong><small>v3 · Web 控制台</small></span></a><nav>" +
                       nav_html(page) + "</nav><div class=\"sidebar-foot\"><div class=\"side-status\"><span class=\"status-dot\" id=\"sideDot\"></span><span id=\"sideState\">只读模式</span></div><div class=\"side-meta\">S9 · HTTP + WS<br>Bearer token · 仅本机/受信网络</div></div></aside>"
                       "<main><div class=\"shell\"><header class=\"page-head\"><div class=\"title-wrap\"><h1>" + page_title(page) + "</h1><div class=\"sub\">" + page_subtitle(page) + "</div></div>"
                       "<div class=\"header-tools\"><span class=\"chip\" id=\"tokenState\">只读</span><div class=\"token-box\"><input id=\"tokenBox\" type=\"password\" placeholder=\"Bearer token\" autocomplete=\"off\" spellcheck=\"false\" aria-label=\"Bearer token\"><button id=\"saveToken\" class=\"btn ghost\" type=\"button\">保存 token</button></div></div></header>" +
                       body + "</div></main></div>"
                       "<div class=\"toast\" id=\"toast\" aria-live=\"polite\"></div>"
                       "<dialog id=\"confirmDlg\"><form method=\"dialog\"><div class=\"dialog-body\"><h3 id=\"confirmTitle\">确认</h3><p id=\"confirmText\"></p></div><div class=\"dialog-actions\"><button class=\"btn ghost\" value=\"cancel\">取消</button><button class=\"btn danger\" id=\"confirmOk\" value=\"ok\">确认</button></div></form></dialog>"
                       "<dialog id=\"promptDlg\"><form method=\"dialog\"><div class=\"dialog-body\"><h3 id=\"promptTitle\">输入</h3><label class=\"muted\" id=\"promptLabel\" for=\"promptInput\">名称</label><input id=\"promptInput\" autocomplete=\"off\"></div><div class=\"dialog-actions\"><button class=\"btn ghost\" value=\"cancel\">取消</button><button class=\"btn\" id=\"promptOk\" value=\"ok\">确定</button></div></form></dialog>"
                       "<script src=\"/static/app.js\"></script><script>" + script + "</script></body></html>";
    return html;
}

std::string json_str(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? std::string(v->valuestring) : std::string();
}

int json_int(const cJSON* obj, const char* key, int def) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? static_cast<int>(v->valuedouble) : def;
}

bool json_bool(const cJSON* obj, const char* key, bool def) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    return def;
}

bool valid_id(const std::string& id) {
    static const std::regex re("^[A-Za-z0-9_.-]{1,64}$");
    return std::regex_match(id, re);
}

cJSON* channel_object(cJSON* root, const std::string& name) {
    if (!cJSON_IsObject(root)) return nullptr;
    for (const char* key : {"boards", "ssh"}) {
        cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, key);
        if (!cJSON_IsArray(arr)) continue;
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, arr) {
            if (cJSON_IsObject(it) && json_str(it, "name") == name) return it;
        }
    }
    return nullptr;
}

// config 兼容旧版 {名字: {...}} 映射；Web 写回前统一转数组，避免把旧板卡清空。
cJSON* ensure_channel_array(cJSON* root, const char* key) {
    cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsArray(arr)) return arr;
    cJSON* out = cJSON_CreateArray();
    if (cJSON_IsObject(arr)) {
        cJSON* child = nullptr;
        cJSON_ArrayForEach(child, arr) {
            cJSON* item = cJSON_Duplicate(child, 1);
            if (cJSON_IsObject(item) && !cJSON_GetObjectItemCaseSensitive(item, "name"))
                cJSON_AddStringToObject(item, "name", child->string ? child->string : "");
            cJSON_AddItemToArray(out, item);
        }
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    cJSON_AddItemToObject(root, key, out);
    return out;
}

cJSON* find_named_in_array(cJSON* arr, const std::string& name, int* index) {
    if (!cJSON_IsArray(arr)) return nullptr;
    int i = 0;
    cJSON* it = nullptr;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsObject(it) && json_str(it, "name") == name) {
            if (index) *index = i;
            return it;
        }
        ++i;
    }
    return nullptr;
}

void copy_str_field(cJSON* dst, const cJSON* src, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(src, key);
    if (cJSON_IsString(v) && v->valuestring) {
        cJSON_DeleteItemFromObjectCaseSensitive(dst, key);
        cJSON_AddStringToObject(dst, key, v->valuestring);
    }
}

void copy_int_field(cJSON* dst, const cJSON* src, const char* key, int def, int lo, int hi) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(src, key);
    int n = cJSON_IsNumber(v) ? static_cast<int>(v->valuedouble) : def;
    n = std::max(lo, std::min(hi, n));
    cJSON_DeleteItemFromObjectCaseSensitive(dst, key);
    cJSON_AddNumberToObject(dst, key, n);
}

void copy_bool_field(cJSON* dst, const cJSON* src, const char* key, bool def) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(src, key);
    bool b = cJSON_IsBool(v) ? cJSON_IsTrue(v) : (cJSON_IsNumber(v) ? v->valuedouble != 0 : def);
    cJSON_DeleteItemFromObjectCaseSensitive(dst, key);
    cJSON_AddBoolToObject(dst, key, b);
}

bool valid_channel_name(const std::string& name, std::string* err) {
    if (name.empty()) { if (err) *err = "通道名不能为空"; return false; }
    if (name.size() > 40) { if (err) *err = "通道名过长（≤40 字符）"; return false; }
    if (name.find('|') != std::string::npos || name.find('\n') != std::string::npos ||
        name.find('\r') != std::string::npos) {
        if (err) *err = "通道名不能含 | 或换行";
        return false;
    }
    return true;
}

void add_action_json(cJSON* o, const BoardActionConfig& a) {
    cJSON_AddStringToObject(o, "id", a.id.c_str());
    cJSON_AddStringToObject(o, "label", a.label.c_str());
    cJSON_AddBoolToObject(o, "enable", a.enable);
    cJSON_AddStringToObject(o, "backend", a.backend.c_str());
    cJSON_AddStringToObject(o, "kind", a.backend.c_str());
    cJSON_AddStringToObject(o, "run", a.run.c_str());
    cJSON* cmds = cJSON_AddArrayToObject(o, "commands");
    for (const std::string& c : a.commands) cJSON_AddItemToArray(cmds, cJSON_CreateString(c.c_str()));
    cJSON_AddNumberToObject(o, "delay_ms", a.delay_ms);
    cJSON_AddStringToObject(o, "relay", a.relay.c_str());
    cJSON_AddNumberToObject(o, "relay_channel", a.channel);
    cJSON_AddStringToObject(o, "mode", a.mode.c_str());
    cJSON_AddNumberToObject(o, "hold_seconds", a.hold_seconds);
    cJSON_AddBoolToObject(o, "polarity_inverted", a.polarity_inverted);
    cJSON_AddStringToObject(o, "script", a.script.c_str());
}

bool parse_action(const cJSON* o, BoardActionConfig* out, std::string* err) {
    if (!cJSON_IsObject(o) || !out) {
        if (err) *err = "action 必须是对象";
        return false;
    }
    BoardActionConfig a;
    a.id = json_str(o, "id");
    a.label = json_str(o, "label");
    a.enable = json_bool(o, "enable", true);
    a.backend = json_str(o, "backend");
    if (a.backend.empty()) a.backend = "commands";
    a.run = json_str(o, "run");
    if (a.run.empty()) a.run = "channel";
    if (!valid_id(a.id)) {
        if (err) *err = "id 只允许字母、数字、点、下划线、短横线，长度 1-64";
        return false;
    }
    if (a.label.empty()) {
        if (err) *err = "label 不能为空";
        return false;
    }
    if (a.backend != "commands" && a.backend != "relay" && a.backend != "script") {
        if (err) *err = "backend 必须是 commands/relay/script";
        return false;
    }
    a.delay_ms = std::max(0, json_int(o, "delay_ms", 200));
    a.relay = json_str(o, "relay");
    a.channel = json_int(o, "relay_channel", json_int(o, "channel", 1));
    a.mode = json_str(o, "mode");
    if (a.mode.empty()) a.mode = "pulse";
    a.hold_seconds = 1.0;
    const cJSON* hold = cJSON_GetObjectItemCaseSensitive(o, "hold_seconds");
    if (cJSON_IsNumber(hold)) a.hold_seconds = hold->valuedouble;
    a.polarity_inverted = json_bool(o, "polarity_inverted", false);
    a.script = json_str(o, "script");
    const cJSON* cmds = cJSON_GetObjectItemCaseSensitive(o, "commands");
    if (cJSON_IsArray(cmds)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, cmds) {
            if (cJSON_IsString(it) && it->valuestring && *it->valuestring) {
                a.commands.push_back(it->valuestring);
            }
        }
    }
    if (a.backend == "commands" && a.commands.empty()) {
        if (err) *err = "commands 至少需要一条非空命令";
        return false;
    }
    if (a.backend == "script" && a.script.empty()) {
        if (err) *err = "script 不能为空";
        return false;
    }
    if (a.backend == "relay" && a.relay.empty()) {
        if (err) *err = "relay 不能为空";
        return false;
    }
    *out = a;
    return true;
}

std::string now_text(std::filesystem::file_time_type t) {
    auto sys = std::chrono::system_clock::now() +
               std::chrono::duration_cast<std::chrono::system_clock::duration>(
                   t - std::filesystem::file_time_type::clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sys);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32] = "";
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

bool safe_share_path(ServiceManager* services, const std::string& rel,
                     std::filesystem::path* out, std::string* err) {
    if (!services || !out) {
        if (err) *err = "服务未就绪";
        return false;
    }
    std::string r = rel;
    std::replace(r.begin(), r.end(), '\\', '/');
    if (r.size() >= 2 && r[1] == ':') {
        if (err) *err = "不允许绝对路径";
        return false;
    }
    while (!r.empty() && r.front() == '/') r.erase(r.begin());
    std::vector<std::string> parts;
    std::string cur;
    for (char c : r) {
        if (c == '/') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    for (const std::string& p : parts) {
        if (p == ".") continue;
        if (p == ".." || p.find(':') != std::string::npos || p.find('\0') != std::string::npos) {
            if (err) *err = "路径不允许包含 .. 或盘符";
            return false;
        }
    }
    try {
        std::filesystem::path root = std::filesystem::weakly_canonical(
            std::filesystem::u8path(services->share_root()));
        std::filesystem::path p = std::filesystem::weakly_canonical(
            root / std::filesystem::u8path(r));
        std::string root_s = root.lexically_normal().generic_u8string();
        std::string p_s = p.lexically_normal().generic_u8string();
        std::transform(root_s.begin(), root_s.end(), root_s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(p_s.begin(), p_s.end(), p_s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (p_s != root_s && p_s.rfind(root_s + "/", 0) != 0) {
            if (err) *err = "路径越出共享根目录";
            return false;
        }
        *out = p;
        return true;
    } catch (const std::exception& e) {
        if (err) *err = std::string("路径解析失败：") + e.what();
        return false;
    }
}

bool reply_file_bytes(mg_connection* c, const std::string& data, const std::string& name) {
    std::string safe;
    for (char ch : name) safe += (ch == '"' || ch == '\\' || ch == '\r' || ch == '\n') ? '_' : ch;
    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                      "Content-Length: " + std::to_string(data.size()) +
                      "\r\nContent-Disposition: attachment; filename=\"" + safe + "\""
                      "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    mg_send(c, hdr.data(), hdr.size());
    mg_send(c, data.data(), data.size());
    c->is_draining = 1;
    return true;
}

std::string json_text(cJSON* root, bool formatted = false) {
    if (!root) return "{}";
    char* p = formatted ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    std::string out = p ? p : "{}";
    cJSON_free(p);
    cJSON_Delete(root);
    return out;
}

std::string query_var(const mg_http_message* hm, const char* name, std::string def = "") {
    char buf[256] = "";
    int n = mg_http_get_var(&hm->query, name, buf, sizeof(buf));
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : def;
}

std::string body_string(const mg_http_message* hm) {
    return std::string(hm->body.buf ? hm->body.buf : "", hm->body.len);
}

bool method_is(const mg_http_message* hm, const char* m) {
    return hm && hm->method.len == std::strlen(m) &&
           std::string(hm->method.buf, hm->method.len) == m;
}

// C1：错误响应带 request_id；不记录任何 token 或请求正文。
std::string new_request_id() {
    static std::atomic<unsigned long long> seq{0};
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    return "r-" + std::to_string(ms) + "-" + std::to_string(++seq);
}

void reply_text(mg_connection* c, int code, const std::string& body, const char* ctype) {
    std::string hdrs = std::string("Content-Type: ") + ctype +
                       "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n";
    mg_http_reply(c, code, hdrs.c_str(), "%s", body.c_str());
}

void reply_json(mg_connection* c, int code, cJSON* root) {
    reply_text(c, code, json_text(root), "application/json; charset=utf-8");
}

void reply_json_text(mg_connection* c, int code, const std::string& text) {
    reply_text(c, code, text, "application/json; charset=utf-8");
}

// C3：JSON 响应附加日志游标头；extra_headers 每行必须以 CRLF 结尾。
void reply_json_headers(mg_connection* c, int code, cJSON* root, const std::string& extra_headers) {
    std::string body = json_text(root);
    std::string hdrs = "Content-Type: application/json; charset=utf-8\r\n"
                       "Cache-Control: no-store\r\n"
                       "Access-Control-Allow-Origin: *\r\n" + extra_headers;
    mg_http_reply(c, code, hdrs.c_str(), "%s", body.c_str());
}

bool parse_u64(const std::string& s, std::uint64_t* out) {
    if (s.empty() || out == nullptr) return false;
    for (char ch : s) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno == ERANGE || end == s.c_str() || end == nullptr || *end != '\0') return false;
    *out = static_cast<std::uint64_t>(v);
    return true;
}

struct WsState {
    std::string channel;
    uint64_t epoch = 0;
    uint64_t seen = 0;
};

struct Job {
    int id = 0;
    std::string kind;                  // action / command / command_block / exec / send_keys
    std::string channel;
    std::string state = "queued";      // queued / running / done / failed / timeout / cancelled
    bool done = false;
    bool ok = false;
    bool timed_out = false;
    bool marker = false;
    std::string message;
    std::string output;
    std::string error;
    std::string request_id;
    std::string session;
    std::string action_id;
    std::int64_t created_ms = 0;
    std::int64_t started_ms = 0;
    std::int64_t ended_ms = 0;
    int timeout_ms = 0;
    std::uint64_t bytes = 0;
    std::atomic<bool> cancel_requested{false};
    std::atomic<int> progress_sent{0};
    std::atomic<int> progress_total{0};
};

enum class TaskKind { Action, Command, CommandBlock, Exec, SendKeys };

struct Task {
    std::shared_ptr<Job> job;
    TaskKind kind = TaskKind::Command;
    AppConfig cfg;
    std::string channel;
    std::string action_id;
    std::string data;                  // command / write 原始下行
    std::string command;               // block/exec 原命令
    std::string line_ending;
    std::string marker;                // exec 结束标记
    std::vector<std::string> keys;     // send_keys 已归一化字节序列
    int interval_ms = 50;
    int timeout_ms = 2000;
    std::shared_ptr<IBridge> bridge;
};

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string header_text(const mg_http_message* hm, const char* name) {
    if (!hm) return std::string();
    mg_str* h = mg_http_get_header(const_cast<mg_http_message*>(hm), name);
    return h ? std::string(h->buf, h->len) : std::string();
}

std::string request_id_of(const mg_http_message* hm) {
    std::string id = header_text(hm, "X-Request-Id");
    return id.empty() ? new_request_id() : id;
}

// \r / \n / none；Agent 可在 JSON 里直接给控制字符，也兼容字面 "\\r\\n"。
std::string decode_line_ending(const std::string& raw, bool command_default) {
    if (raw.empty()) return command_default ? "\r" : std::string();
    std::string s = raw;
    for (std::size_t p = 0; (p = s.find("\\r", p)) != std::string::npos; p += 1) s.replace(p, 2, "\r");
    for (std::size_t p = 0; (p = s.find("\\n", p)) != std::string::npos; p += 1) s.replace(p, 2, "\n");
    if (s == "none" || s == "None" || s == "NONE") return std::string();
    if (s == "cr" || s == "CR") return "\r";
    if (s == "lf" || s == "LF") return "\n";
    if (s == "crlf" || s == "CRLF") return "\r\n";
    return s;
}

cJSON* job_object(const Job& j) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", j.id);
    cJSON_AddNumberToObject(o, "job_id", j.id);
    cJSON_AddStringToObject(o, "kind", j.kind.c_str());
    cJSON_AddStringToObject(o, "channel", j.channel.c_str());
    cJSON_AddStringToObject(o, "state", j.state.c_str());
    cJSON_AddBoolToObject(o, "done", j.done);
    cJSON_AddBoolToObject(o, "ok", j.ok);
    cJSON_AddBoolToObject(o, "timed_out", j.timed_out);
    cJSON_AddBoolToObject(o, "marker", j.marker);
    cJSON_AddStringToObject(o, "message", j.message.c_str());
    cJSON_AddStringToObject(o, "output", j.output.c_str());
    cJSON_AddStringToObject(o, "error", j.error.c_str());
    cJSON_AddStringToObject(o, "request_id", j.request_id.c_str());
    cJSON_AddStringToObject(o, "session", j.session.c_str());
    cJSON_AddStringToObject(o, "action_id", j.action_id.c_str());
    cJSON_AddNumberToObject(o, "created_ms", static_cast<double>(j.created_ms));
    cJSON_AddNumberToObject(o, "started_ms", static_cast<double>(j.started_ms));
    cJSON_AddNumberToObject(o, "ended_ms", static_cast<double>(j.ended_ms));
    std::int64_t end = j.ended_ms ? j.ended_ms : now_ms();
    std::int64_t dur = j.started_ms ? std::max<std::int64_t>(0, end - j.started_ms) : 0;
    cJSON_AddNumberToObject(o, "duration_ms", static_cast<double>(dur));
    cJSON_AddNumberToObject(o, "bytes", static_cast<double>(j.bytes));
    cJSON_AddBoolToObject(o, "cancel_requested", j.cancel_requested.load());
    cJSON* progress = cJSON_AddObjectToObject(o, "progress");
    int sent = j.progress_sent.load();
    int total = j.progress_total.load();
    cJSON_AddNumberToObject(progress, "sent", sent);
    cJSON_AddNumberToObject(progress, "total", total);
    cJSON_AddNumberToObject(progress, "percent", total > 0 ? (100.0 * sent / total) : 0);
    cJSON* audit = cJSON_AddObjectToObject(o, "audit");
    cJSON_AddStringToObject(audit, "request_id", j.request_id.c_str());
    cJSON_AddStringToObject(audit, "session", j.session.c_str());
    cJSON_AddStringToObject(audit, "channel", j.channel.c_str());
    cJSON_AddStringToObject(audit, "action", j.action_id.c_str());
    cJSON_AddStringToObject(audit, "status", j.state.c_str());
    cJSON_AddNumberToObject(audit, "duration_ms", static_cast<double>(dur));
    cJSON_AddNumberToObject(audit, "bytes", static_cast<double>(j.bytes));
    return o;
}

// 列表摘要：不携带 output/error 全文，避免 /jobs 变成日志搬运接口。
cJSON* job_summary_object(const Job& j) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", j.id);
    cJSON_AddNumberToObject(o, "job_id", j.id);
    cJSON_AddStringToObject(o, "kind", j.kind.c_str());
    cJSON_AddStringToObject(o, "channel", j.channel.c_str());
    cJSON_AddStringToObject(o, "state", j.state.c_str());
    cJSON_AddBoolToObject(o, "ok", j.ok);
    cJSON_AddBoolToObject(o, "timed_out", j.timed_out);
    cJSON_AddBoolToObject(o, "cancel_requested", j.cancel_requested.load());
    cJSON_AddStringToObject(o, "request_id", j.request_id.c_str());
    cJSON_AddNumberToObject(o, "created_ms", static_cast<double>(j.created_ms));
    cJSON_AddNumberToObject(o, "started_ms", static_cast<double>(j.started_ms));
    cJSON_AddNumberToObject(o, "ended_ms", static_cast<double>(j.ended_ms));
    cJSON_AddNumberToObject(o, "bytes", static_cast<double>(j.bytes));
    cJSON_AddNumberToObject(o, "progress_sent", j.progress_sent.load());
    cJSON_AddNumberToObject(o, "progress_total", j.progress_total.load());
    return o;
}

struct CaptureResult {
    std::string output;
    bool timed_out = false;
    bool marker = false;
    bool cancelled = false;
    std::uint64_t bytes = 0;
};

CaptureResult capture_after_write(IBridge* bridge, const std::string& payload,
                                 const std::string& marker, int timeout_ms, bool need_marker,
                                 const std::atomic<bool>* cancel = nullptr) {
    CaptureResult r;
    if (!bridge) {
        r.timed_out = true;
        return r;
    }
    std::uint64_t epoch = 0, seen = 0;
    bridge->read_since(&epoch, &seen);
    r.bytes = static_cast<std::uint64_t>(bridge->write(payload.data(), payload.size()));
    std::int64_t start = now_ms();
    std::int64_t last_rx = 0;
    bool got = false;
    constexpr std::size_t kMaxCapture = 1u << 20;
    while (true) {
        if (cancel && cancel->load()) {
            r.cancelled = true;
            break;
        }
        std::uint64_t e = epoch, s = seen;
        std::string d = bridge->read_since(&e, &s);
        epoch = e;
        seen = s;
        if (!d.empty()) {
            r.output += d;
            if (r.output.size() > kMaxCapture) r.output.resize(kMaxCapture);
            got = true;
            last_rx = now_ms();
        }
        std::int64_t elapsed = now_ms() - start;
        if (need_marker && r.output.find(marker) != std::string::npos) {
            r.marker = true;
            break;
        }
        if (!need_marker && got && now_ms() - last_rx >= 150) break;
        if (elapsed >= timeout_ms) {
            r.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    if (need_marker && r.marker) {
        std::size_t p = r.output.rfind(marker);
        if (p != std::string::npos) {
            std::size_t line = r.output.find_last_of("\r\n", p);
            r.output.resize(line == std::string::npos ? 0 : line + 1);
        }
    }
    return r;
}

std::uint64_t send_key_tokens(IBridge* bridge, const std::vector<std::string>& keys,
                              int interval_ms, Job* job = nullptr, bool* cancelled = nullptr) {
    std::uint64_t sent = 0;
    if (!bridge) return sent;
    if (job) job->progress_total.store(static_cast<int>(keys.size()));
    for (const std::string& k : keys) {
        if (job && job->cancel_requested.load()) {
            if (cancelled) *cancelled = true;
            break;
        }
        if (k.empty()) continue;
        sent += static_cast<std::uint64_t>(bridge->write(k.data(), k.size()));
        if (job) job->progress_sent.fetch_add(1);
        if (interval_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    return sent;
}

}  // namespace

struct WebConsole::Impl {
    AppCore* core = nullptr;
    ServiceManager* services = nullptr;
    mg_mgr* mgr = nullptr;
    std::thread thread;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> stop{false};
    int port = 18081;
    std::string token;
    void* notify = nullptr;
    std::string error;
    mutable std::mutex mu;
    mutable std::mutex qmu;
    std::condition_variable qcv;
    std::deque<Task> queue;
    unsigned long wake_id = 0;
    std::map<mg_connection*, WsState> ws;
    std::map<mg_connection*, int> pending_conn_job;  // 异步 HTTP 回复：连接 → job
    std::map<int, mg_connection*> pending_job_conn;
    std::map<int, std::shared_ptr<Job>> jobs;
    int next_job = 1;

    bool write_ok(mg_http_message* hm) const {
        if (token.empty()) return false;
        mg_str* h = mg_http_get_header(hm, "Authorization");
        if (!h) return false;
        std::string v(h->buf, h->len);
        return v == "Bearer " + token;
    }

    std::shared_ptr<IBridge> bridge(const std::string& name) {
        return core ? core->find_shared(name) : nullptr;
    }

    agent::UiBridgeInfo ui_info() const {
        agent::UiBridgeInfo info;
        if (!core) return info;
        const AgentConfig& a = core->config().agent;
        info.enabled = a.enabled;
        info.port = a.port;
        info.token_required = !a.token.empty();
        return info;
    }

    std::string status() const;
    std::string actions() const;
    std::string quick() const;
    std::string relay_devices() const;
    bool patch_config(const std::function<bool(cJSON*)>& apply, std::string* err);
    std::string jobs_json(int id) const;
    std::string jobs_list_json(const std::string& state, const std::string& channel,
                               int before_id, int limit) const;
    bool channel_busy(const std::string& channel, int* job_id, std::string* state) const;
    std::string page(const std::string& name) const;
    void handle_http(mg_connection* c, mg_http_message* hm);
    void handle_ws(mg_connection* c, int ev, void* ev_data);
    void push_ws(mg_connection* c, WsState& s);
    void cleanup_job();
    std::shared_ptr<Job> make_job(const std::string& kind, const std::string& channel,
                                  const std::string& request_id, const std::string& session,
                                  int* busy_id = nullptr, std::string* busy_state = nullptr);
    int enqueue_task(Task&& task);
    void on_wakeup(mg_str* data);
    void flush_job_reply(int id);
    void drop_pending(mg_connection* c);
    bool hold_job(mg_connection* c, const std::shared_ptr<Job>& job, std::string* done_body);
    bool allow_channel_write(mg_connection* c, const std::string& channel,
                             const std::string& session, const std::string& rid);
    void reply_channel_busy(mg_connection* c, const std::string& channel, int busy_id,
                            const std::string& busy_state, const std::string& rid);
    std::string job_reply_json(const Job& j) const;
    int spawn_action(const std::string& channel, const std::string& id,
                     const std::string& request_id, const std::string& session);
    void worker_loop();
    void start_thread();
};

std::string WebConsole::Impl::status() const {
    cJSON* root = core ? cJSON_Parse(core->state_json(true).c_str()) : nullptr;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (services) {
        cJSON* svc = cJSON_Parse(services->state_json().c_str());
        if (cJSON_IsObject(svc)) cJSON_AddItemToObject(root, "services", svc);
        else cJSON_Delete(svc);
    }
    cJSON* web = cJSON_AddObjectToObject(root, "web");
    cJSON_AddBoolToObject(web, "on", running.load());
    cJSON_AddNumberToObject(web, "port", port);
    cJSON_AddBoolToObject(web, "readonly", token.empty());
    std::string ip = services ? services->local_ip() : "127.0.0.1";
    cJSON_AddStringToObject(web, "url", ("http://" + ip + ":" + std::to_string(port)).c_str());
    return json_text(root);
}

std::string WebConsole::Impl::actions() const {
    cJSON* root = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    if (core) {
        for (const AppCore::Entry& e : core->entries()) {
            const std::vector<BoardActionConfig>* btns = core->buttons_of(e.name);
            if (!btns) continue;
            for (const BoardActionConfig& a : *btns) {
                cJSON* o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "channel", e.name.c_str());
                add_action_json(o, a);
                cJSON_AddStringToObject(o, "channel_kind", e.kind.c_str());
                cJSON_AddItemToArray(items, o);
            }
        }
    }
    return json_text(root);
}

std::string WebConsole::Impl::quick() const {
    cJSON* root = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    if (core) {
        for (const AppCore::Entry& e : core->entries()) {
            const std::vector<QuickCommandConfig>* qs = core->quick_commands_of(e.name);
            if (!qs) continue;
            for (std::size_t i = 0; i < qs->size(); ++i) {
                const QuickCommandConfig& q = (*qs)[i];
                cJSON* o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "channel", e.name.c_str());
                cJSON_AddNumberToObject(o, "index", static_cast<double>(i));
                cJSON_AddStringToObject(o, "label", q.label.c_str());
                cJSON_AddStringToObject(o, "command", q.command.c_str());
                cJSON_AddItemToArray(items, o);
            }
        }
    }
    return json_text(root);
}

std::string WebConsole::Impl::relay_devices() const {
    cJSON* root = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    cJSON* states = services ? cJSON_Parse(services->relay_json().c_str()) : nullptr;
    if (core) {
        for (const RelayDeviceConfig& d : core->config().relays) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", d.name.c_str());
            cJSON_AddBoolToObject(o, "enable", d.enable);
            cJSON_AddBoolToObject(o, "enabled", d.enable);
            cJSON_AddStringToObject(o, "port", d.windows_port.c_str());
            cJSON_AddStringToObject(o, "windows_port", d.windows_port.c_str());
            cJSON_AddStringToObject(o, "linux_port", d.linux_port.c_str());
            cJSON_AddNumberToObject(o, "channels", d.channels);
            cJSON_AddNumberToObject(o, "baudrate", d.baudrate);
            cJSON_AddBoolToObject(o, "io_inverted", d.io_inverted);
            cJSON_AddNumberToObject(o, "post_write_delay_ms", d.post_write_delay_ms);
            cJSON_AddBoolToObject(o, "polarity_inverted", d.polarity_inverted);
            cJSON* state = nullptr;
            if (cJSON_IsArray(states)) {
                cJSON* it = nullptr;
                cJSON_ArrayForEach(it, states) {
                    if (cJSON_IsObject(it) && json_str(it, "name") == d.name) {
                        state = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(it, "state"), 1);
                        break;
                    }
                }
            }
            if (state) cJSON_AddItemToObject(o, "state", state);
            else cJSON_AddItemToObject(o, "state", cJSON_CreateArray());
            cJSON_AddItemToArray(items, o);
        }
    }
    cJSON_Delete(states);
    return json_text(root);
}

bool WebConsole::Impl::patch_config(const std::function<bool(cJSON*)>& apply, std::string* err) {
    if (!core) {
        if (err) *err = "core 未就绪";
        return false;
    }
    cJSON* root = cJSON_Parse(core->config_json_text().c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        if (err) *err = "当前配置无法解析";
        return false;
    }
    if (!apply(root)) {
        cJSON_Delete(root);
        if (err && err->empty()) *err = "配置修改被拒绝";
        return false;
    }
    std::string text = json_text(root, false);
    if (!core->save_config_json(text, err)) return false;
    if (notify) PostMessageW(static_cast<HWND>(notify), kMsgWebReload, 0, 0);
    return true;
}

std::string WebConsole::Impl::jobs_json(int id) const {
    std::lock_guard<std::mutex> g(mu);
    auto it = jobs.find(id);
    if (it == jobs.end()) return "{\"ok\":false,\"error\":\"job not found\"}";
    cJSON* root = job_object(*it->second);
    cJSON_AddNumberToObject(root, "id", it->second->id);
    return json_text(root);
}

std::string WebConsole::Impl::job_reply_json(const Job& j) const {
    cJSON* root = job_object(j);
    cJSON_AddNumberToObject(root, "elapsed_ms", static_cast<double>(
        j.started_ms ? std::max<std::int64_t>(0, (j.ended_ms ? j.ended_ms : now_ms()) - j.started_ms) : 0));
    return json_text(root);
}

std::string WebConsole::Impl::jobs_list_json(const std::string& state, const std::string& channel,
                                             int before_id, int limit) const {
    std::lock_guard<std::mutex> g(mu);
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(root, "jobs");
    int count = 0;
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
        const Job& j = *it->second;
        if (before_id > 0 && j.id >= before_id) continue;
        if (!state.empty() && j.state != state) continue;
        if (!channel.empty() && j.channel != channel) continue;
        if (count >= limit) break;
        cJSON_AddItemToArray(arr, job_summary_object(j));
        ++count;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddNumberToObject(root, "limit", limit);
    cJSON_AddStringToObject(root, "state", state.c_str());
    cJSON_AddStringToObject(root, "channel", channel.c_str());
    cJSON_AddNumberToObject(root, "total", static_cast<double>(jobs.size()));
    return json_text(root);
}

bool WebConsole::Impl::channel_busy(const std::string& channel, int* job_id, std::string* state) const {
    std::lock_guard<std::mutex> g(mu);
    for (const auto& kv : jobs) {
        const Job& j = *kv.second;
        if (j.channel != channel) continue;
        if (j.state != "queued" && j.state != "running") continue;
        if (job_id) *job_id = j.id;
        if (state) *state = j.state;
        return true;
    }
    return false;
}

std::shared_ptr<Job> WebConsole::Impl::make_job(const std::string& kind, const std::string& channel,
                                                const std::string& request_id, const std::string& session,
                                                int* busy_id, std::string* busy_state) {
    auto job = std::make_shared<Job>();
    job->kind = kind;
    job->channel = channel;
    job->request_id = request_id;
    job->session = session;
    job->action_id = kind;
    job->created_ms = now_ms();
    std::lock_guard<std::mutex> g(mu);
    for (const auto& kv : jobs) {
        const Job& j = *kv.second;
        if (j.channel != channel) continue;
        if (j.state != "queued" && j.state != "running") continue;
        if (busy_id) *busy_id = j.id;
        if (busy_state) *busy_state = j.state;
        return nullptr;
    }
    job->id = next_job++;
    jobs[job->id] = job;
    return job;
}

int WebConsole::Impl::enqueue_task(Task&& task) {
    if (!task.job) return -1;
    std::lock_guard<std::mutex> qg(qmu);
    if (queue.size() >= 64) return -1;
    int id = task.job->id;
    queue.push_back(std::move(task));
    qcv.notify_one();
    return id;
}

void WebConsole::Impl::on_wakeup(mg_str* data) {
    int id = 0;
    if (data && data->len >= sizeof(int)) {
        std::memcpy(&id, data->buf, sizeof(int));
    }
    if (id > 0) flush_job_reply(id);
    cleanup_job();
}

void WebConsole::Impl::flush_job_reply(int id) {
    mg_connection* conn = nullptr;
    std::string body;
    {
        std::lock_guard<std::mutex> g(mu);
        auto pc = pending_job_conn.find(id);
        if (pc == pending_job_conn.end()) return;
        auto jt = jobs.find(id);
        if (jt == jobs.end() || !jt->second->done) return;
        conn = pc->second;
        body = job_reply_json(*jt->second);
        pending_job_conn.erase(pc);
        pending_conn_job.erase(conn);
    }
    if (conn) reply_text(conn, 200, body, "application/json; charset=utf-8");
}

void WebConsole::Impl::drop_pending(mg_connection* c) {
    std::lock_guard<std::mutex> g(mu);
    auto it = pending_conn_job.find(c);
    if (it == pending_conn_job.end()) return;
    pending_job_conn.erase(it->second);
    pending_conn_job.erase(it);
}

bool WebConsole::Impl::hold_job(mg_connection* c, const std::shared_ptr<Job>& job,
                                std::string* done_body) {
    std::lock_guard<std::mutex> g(mu);
    if (job->done) {
        if (done_body) *done_body = job_reply_json(*job);
        return false;
    }
    pending_conn_job[c] = job->id;
    pending_job_conn[job->id] = c;
    return true;
}

bool WebConsole::Impl::allow_channel_write(mg_connection* c, const std::string& channel,
                                           const std::string& session, const std::string& rid) {
    agent::LeaseInfo blocker;
    if (agent::lease_locks().check(channel, session, &blocker) == 0) return true;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON* err = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(err, "code", "agent_lock");
    cJSON_AddStringToObject(err, "message", "channel is locked by another session");
    cJSON_AddStringToObject(err, "owner", blocker.owner.c_str());
    cJSON_AddNumberToObject(err, "expires_in_ms", static_cast<double>(blocker.remaining_ms));
    cJSON_AddBoolToObject(err, "can_takeover", true);
    cJSON_AddStringToObject(root, "request_id", rid.c_str());
    reply_json(c, 423, root);
    return false;
}

void WebConsole::Impl::reply_channel_busy(mg_connection* c, const std::string& channel, int busy_id,
                                         const std::string& busy_state, const std::string& rid) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON* err = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(err, "code", "channel_busy");
    cJSON_AddStringToObject(err, "message", "channel already has an active job");
    cJSON_AddStringToObject(err, "channel", channel.c_str());
    cJSON_AddNumberToObject(err, "job_id", busy_id);
    cJSON_AddStringToObject(err, "state", busy_state.c_str());
    cJSON_AddStringToObject(root, "request_id", rid.c_str());
    reply_json(c, 429, root);
}

std::string WebConsole::Impl::page(const std::string& name) const {
    return page_shell(name, web_pages::page_body(name), web_pages::page_script(name));
}

void WebConsole::Impl::push_ws(mg_connection* c, WsState& s) {
    if (!c || !c->is_websocket) return;
    auto b = bridge(s.channel);
    if (!b) return;
    const uint64_t seq = b->notify_seq();
    (void)seq;
    std::string data = b->read_since(&s.epoch, &s.seen);
    if (!data.empty()) {
        mg_ws_send(c, data.data(), data.size(), WEBSOCKET_OP_BINARY);
    }
}

int WebConsole::Impl::spawn_action(const std::string& channel, const std::string& id,
                                   const std::string& request_id, const std::string& session) {
    if (!core) return -1;
    const BoardActionConfig* found = nullptr;
    const std::vector<BoardActionConfig>* btns = core->buttons_of(channel);
    if (btns) {
        for (const BoardActionConfig& a : *btns) {
            if (a.id == id) {
                found = &a;
                break;
            }
        }
    }
    if (!found) return 0;

    Task task;
    task.job = make_job("action", channel, request_id, session);
    if (!task.job) return -2;
    task.job->action_id = id;
    task.kind = TaskKind::Action;
    task.cfg = core->config();
    task.channel = channel;
    task.action_id = id;
    task.bridge = core->find_shared(channel);
    int job_id = enqueue_task(std::move(task));
    return job_id;
}

void WebConsole::Impl::worker_loop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(qmu);
            qcv.wait(lk, [this]() { return stop.load() || !queue.empty(); });
            if (stop.load()) return;
            task = std::move(queue.front());
            queue.pop_front();
        }
        auto job = task.job;
        if (job->done || job->cancel_requested.load()) {
            {
                std::lock_guard<std::mutex> g(mu);
                if (!job->done) {
                    job->done = true;
                    job->ok = false;
                    job->state = "cancelled";
                    job->message = "cancelled";
                    job->error = "cancelled";
                    job->ended_ms = now_ms();
                }
            }
            if (mgr && wake_id) {
                int id = job->id;
                mg_wakeup(mgr, wake_id, &id, sizeof(id));
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> g(mu);
            job->state = "running";
            job->started_ms = now_ms();
        }
        bool ok = false;
        std::string message;
        std::string output;
        bool timed_out = false;
        bool marker = false;
        bool cancelled = false;
        std::uint64_t bytes = 0;

        if (task.kind == TaskKind::Action) {
            const BoardActionConfig* found = nullptr;
            const std::vector<BoardActionConfig>* btns = core ? core->buttons_of(task.channel) : nullptr;
            if (btns) {
                for (const BoardActionConfig& a : *btns) {
                    if (a.id == task.action_id) {
                        found = &a;
                        break;
                    }
                }
            }
            if (!found) {
                message = "action not found";
            } else {
                BoardActionResult r = run_board_action(
                    task.cfg, task.channel, *found, task.bridge.get(),
                    [this](const std::string& port) { return core && core->port_in_use(port); }, 0, {});
                ok = r.ok;
                message = r.message;
                output = r.output;
                job->progress_total.store(1);
                job->progress_sent.store(1);
            }
        } else if (task.kind == TaskKind::Command) {
            if (!task.bridge) {
                message = "channel_down";
            } else {
                bytes = static_cast<std::uint64_t>(task.bridge->write(task.data.data(), task.data.size()));
                ok = bytes > 0 || task.data.empty();
                message = "queued";
            }
        } else if (task.kind == TaskKind::SendKeys) {
            if (!task.bridge) {
                message = "channel_down";
            } else {
                bytes = send_key_tokens(task.bridge.get(), task.keys, task.interval_ms, job.get(), &cancelled);
                ok = !cancelled;
                message = cancelled ? "cancelled" : "sent";
            }
        } else if (task.kind == TaskKind::CommandBlock || task.kind == TaskKind::Exec) {
            if (!task.bridge) {
                message = "channel_down";
            } else {
                std::string payload = task.command;
                payload += task.line_ending;
                if (task.kind == TaskKind::Exec) payload += "echo " + task.marker + task.line_ending;
                CaptureResult cr = capture_after_write(task.bridge.get(), payload, task.marker,
                                                       task.timeout_ms, task.kind == TaskKind::Exec,
                                                       &job->cancel_requested);
                output = std::move(cr.output);
                timed_out = cr.timed_out;
                marker = cr.marker;
                cancelled = cr.cancelled;
                bytes = cr.bytes;
                if (task.kind == TaskKind::Exec && !task.marker.empty()) {
                    std::size_t p = output.rfind(task.marker);
                    if (p != std::string::npos) {
                        std::size_t line = output.find_last_of("\r\n", p);
                        output.resize(line == std::string::npos ? 0 : line + 1);
                    }
                }
                ok = !timed_out;
                message = timed_out ? "timeout" : "done";
            }
        }

        {
            std::lock_guard<std::mutex> g(mu);
            job->done = true;
            job->ok = ok;
            job->timed_out = timed_out;
            job->marker = marker;
            job->message = message;
            job->output = output;
            job->bytes = bytes;
            job->state = cancelled ? "cancelled"
                                   : (timed_out ? "timeout" : (ok ? "done" : "failed"));
            if (cancelled) {
                job->message = "cancelled";
                job->error = "cancelled";
            }
            job->ended_ms = now_ms();
            if (!ok && job->error.empty() && timed_out) job->error = job->message;
        }
        // 结果写完后叫醒 poll 线程：由它回写挂起 HTTP，并清理旧 job。
        if (mgr && wake_id) {
            int id = job->id;
            mg_wakeup(mgr, wake_id, &id, sizeof(id));
        }
    }
}

void WebConsole::Impl::cleanup_job() {
    std::lock_guard<std::mutex> g(mu);
    constexpr std::size_t kKeep = 100;
    constexpr std::int64_t kMaxAgeMs = 10ll * 60 * 1000;
    std::int64_t now = now_ms();
    // 先删超过 10 分钟的终态 job；保留 last_action 由 C9 状态面单独维护。
    for (auto it = jobs.begin(); it != jobs.end();) {
        const Job& j = *it->second;
        const bool terminal = j.state == "done" || j.state == "failed" ||
                              j.state == "timeout" || j.state == "cancelled";
        if (terminal && now - (j.ended_ms ? j.ended_ms : j.created_ms) > kMaxAgeMs) it = jobs.erase(it);
        else ++it;
    }
    // 再按 id 从旧到新裁到 100 条，只裁终态，运行中的 job 不丢。
    while (jobs.size() > kKeep) {
        bool erased = false;
        for (auto it = jobs.begin(); it != jobs.end(); ++it) {
            const Job& j = *it->second;
            if (j.state == "done" || j.state == "failed" || j.state == "timeout" || j.state == "cancelled") {
                jobs.erase(it);
                erased = true;
                break;
            }
        }
        if (!erased) break;
    }
}

void WebConsole::Impl::handle_http(mg_connection* c, mg_http_message* hm) {
    if (!hm) return;
    // A1-5：请求体总量上限 16MB；超限不解析、不落日志，直接结构化 413。
    if (hm->body.len > 16u * 1024u * 1024u) {
        reply_json_text(c, 413, agent::error_json("request_too_large", "body exceeds 16MB", new_request_id()));
        return;
    }
    std::string path(hm->uri.buf, hm->uri.len);
    if (path == "/" || path == "") path = "/dashboard";
    if (path.size() > 5 && path.rfind(".html") == path.size() - 5) path.resize(path.size() - 5);

    if (path == "/ws/log") {
        std::string ch = query_var(hm, "channel");
        if (ch.empty()) {
            reply_text(c, 400, "channel required", "text/plain; charset=utf-8");
            return;
        }
        if (!bridge(ch)) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
            return;
        }
        WsState st;
        st.channel = ch;
        {
            std::lock_guard<std::mutex> g(mu);
            ws[c] = st;
        }
        mg_ws_upgrade(c, hm, nullptr);
        return;
    }

    if (path == "/static/app.css") {
        mg_str* inm = mg_http_get_header(hm, "If-None-Match");
        if (inm && std::string(inm->buf, inm->len) == "\"sb3-s14-css\"") {
            mg_http_reply(c, 304, "ETag: \"sb3-s14-css\"\r\nCache-Control: max-age=3600\r\n", "");
            return;
        }
        mg_http_reply(c, 200,
                      "Content-Type: text/css; charset=utf-8\r\nETag: \"sb3-s14-css\"\r\n"
                      "Cache-Control: max-age=3600\r\nAccess-Control-Allow-Origin: *\r\n",
                      "%s", web_pages::kCss);
        return;
    }
    if (path == "/static/app.js") {
        mg_str* inm = mg_http_get_header(hm, "If-None-Match");
        if (inm && std::string(inm->buf, inm->len) == "\"sb3-s14-js\"") {
            mg_http_reply(c, 304, "ETag: \"sb3-s14-js\"\r\nCache-Control: max-age=3600\r\n", "");
            return;
        }
        mg_http_reply(c, 200,
                      "Content-Type: application/javascript; charset=utf-8\r\n"
                      "ETag: \"sb3-s14-js\"\r\nCache-Control: max-age=3600\r\n"
                      "Access-Control-Allow-Origin: *\r\n",
                      "%s", web_pages::kCommonJs);
        return;
    }
    if (path == "/health") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", new_request_id()));
            return;
        }
        reply_json_text(c, 200, agent::health_json(port, ui_info()));
        return;
    }
    if (path == "/api/agent/resolve") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", new_request_id()));
            return;
        }
        agent::ResolveResult r = agent::resolve_channel(core, query_var(hm, "board"),
                                                        query_var(hm, "link"), query_var(hm, "channel"));
        if (!r.ok) {
            reply_json_text(c, r.http_status,
                            agent::error_json(r.error_code, r.error_message, new_request_id()));
            return;
        }
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "channel", r.name.c_str());
        cJSON_AddStringToObject(out, "kind", r.kind.c_str());
        cJSON_AddBoolToObject(out, "running", r.running);
        reply_json(c, 200, out);
        return;
    }
    if (path == "/status" && query_var(hm, "view") == "agent") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", new_request_id()));
            return;
        }
        reply_json_text(c, 200, agent::status_agent_json(core, services));
        return;
    }
    if (path == "/agent/scan") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", new_request_id()));
            return;
        }
        reply_json_text(c, 200, agent::scan_json(core, services));
        return;
    }
    if (path == "/agent/probe") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", new_request_id()));
            return;
        }
        agent::ResolveResult r = agent::resolve_channel(core, query_var(hm, "board"),
                                                        query_var(hm, "link"), query_var(hm, "channel"));
        if (!r.ok) {
            reply_json_text(c, r.http_status,
                            agent::error_json(r.error_code, r.error_message, new_request_id()));
            return;
        }
        int tail = std::atoi(query_var(hm, "tail", "2048").c_str());
        reply_json_text(c, 200, agent::probe_json(core, r.name, tail));
        return;
    }
    if (path == "/agent/state") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", new_request_id()));
            return;
        }
        agent::ResolveResult r = agent::resolve_channel(core, query_var(hm, "board"),
                                                        query_var(hm, "link"), query_var(hm, "channel"));
        if (!r.ok) {
            reply_json_text(c, r.http_status,
                            agent::error_json(r.error_code, r.error_message, new_request_id()));
            return;
        }
        reply_json_text(c, 200, agent::state_json(core, services, r.name));
        return;
    }
    if (path == "/actions" && query_var(hm, "view") == "agent") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", new_request_id()));
            return;
        }
        std::string channel = query_var(hm, "channel");
        if (!channel.empty()) {
            agent::ResolveResult r = agent::resolve_channel(core, "", "", channel);
            if (!r.ok) {
                reply_json_text(c, r.http_status,
                                agent::error_json(r.error_code, r.error_message, new_request_id()));
                return;
            }
            channel = r.name;
        }
        reply_json_text(c, 200, agent::actions_agent_json(core, channel));
        return;
    }
    if (path == "/favicon.ico") {
        mg_http_reply(c, 204, "Cache-Control: max-age=86400\r\n", "");
        return;
    }
    if (path == "/status") {
        reply_json_text(c, 200, status());
        return;
    }
    if (path == "/log") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", new_request_id()));
            return;
        }
        std::string ch = query_var(hm, "channel");
        auto b = bridge(ch);
        if (!b) {
            reply_json_text(c, 404, agent::error_json("channel_not_found", "channel not found", new_request_id()));
            return;
        }
        const std::string since_arg = query_var(hm, "since");
        const std::string epoch_arg = query_var(hm, "epoch");
        const bool incremental = !since_arg.empty();
        int chars = incremental ? 65536 : 8192;
        std::string chars_arg = query_var(hm, "chars");
        if (!chars_arg.empty()) chars = std::max(1, std::min(65536, std::atoi(chars_arg.c_str())));

        std::uint64_t epoch = 0, total = 0, next_since = 0;
        std::uint64_t since = 0, req_epoch = 0, dropped_bytes = 0;
        std::string text;
        bool dropped = false, reset = false, truncated = false;
        std::string reset_reason;

        auto reset_to_tail = [&](const char* reason) {
            reset = true;
            reset_reason = reason ? reason : "";
            text.clear();
            epoch = 0;
            total = 0;
            text = b->read_all(&epoch, &total);
            if (static_cast<int>(text.size()) > chars)
                text = text.substr(text.size() - static_cast<std::size_t>(chars));
            next_since = total;
            dropped = false;
            dropped_bytes = 0;
            truncated = false;
        };

        if (!incremental) {
            text = b->read_all(&epoch, &total);
            if (static_cast<int>(text.size()) > chars)
                text = text.substr(text.size() - static_cast<std::size_t>(chars));
            next_since = total;
        } else {
            if (epoch_arg.empty()) {
                reply_json_text(c, 400, agent::error_json("invalid_cursor", "epoch is required with since", new_request_id()));
                return;
            }
            if (!parse_u64(since_arg, &since) || !parse_u64(epoch_arg, &req_epoch)) {
                reply_json_text(c, 400, agent::error_json("invalid_cursor", "since/epoch must be unsigned integers", new_request_id()));
                return;
            }
            epoch = req_epoch;
            std::uint64_t cur_epoch = b->log_epoch();
            std::uint64_t cur_total = b->log_total();
            if (req_epoch != cur_epoch) {
                reset_to_tail("epoch_mismatch");
            } else if (since > cur_total) {
                reset_to_tail("cursor_ahead");
            } else {
                std::uint64_t seen = since;
                bool dropped_flag = false;
                text = b->read_since_ex(&epoch, &seen, &dropped_flag);
                std::uint64_t after_epoch = b->log_epoch();
                std::uint64_t after_total = b->log_total();
                if (epoch != req_epoch || after_epoch != epoch) {
                    reset_to_tail("epoch_changed");
                } else {
                    dropped = dropped_flag;
                    total = after_total;
                    std::uint64_t start_cursor = since;
                    if (dropped) {
                        std::uint64_t cap = b->log_capacity();
                        start_cursor = total > cap ? total - cap : 0;
                        if (since < start_cursor) dropped_bytes = start_cursor - since;
                    }
                    if (static_cast<int>(text.size()) > chars) {
                        text.resize(static_cast<std::size_t>(chars));
                        truncated = true;
                    }
                    next_since = start_cursor + static_cast<std::uint64_t>(text.size());
                }
            }
        }

        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "channel", ch.c_str());
        cJSON_AddStringToObject(root, "mode", incremental ? "incremental" : "tail");
        cJSON_AddStringToObject(root, "text", text.c_str());
        cJSON_AddNumberToObject(root, "chars", static_cast<double>(text.size()));
        cJSON_AddNumberToObject(root, "epoch", static_cast<double>(epoch));
        cJSON_AddNumberToObject(root, "total", static_cast<double>(total));
        cJSON_AddNumberToObject(root, "next_since", static_cast<double>(next_since));
        cJSON_AddBoolToObject(root, "dropped", dropped);
        cJSON_AddNumberToObject(root, "dropped_bytes", static_cast<double>(dropped_bytes));
        cJSON_AddBoolToObject(root, "reset", reset);
        cJSON_AddBoolToObject(root, "truncated", truncated);
        if (incremental) cJSON_AddNumberToObject(root, "since", static_cast<double>(since));
        if (!reset_reason.empty()) cJSON_AddStringToObject(root, "reset_reason", reset_reason.c_str());
        std::string extra = "X-Log-Epoch: " + std::to_string(epoch) + "\r\n" +
                            "X-Log-Total: " + std::to_string(total) + "\r\n" +
                            "X-Log-Dropped: " + std::string(dropped ? "true" : "false") + "\r\n";
        reply_json_headers(c, 200, root, extra);
        return;
    }
    if (path == "/log/download") {
        std::string ch = query_var(hm, "channel");
        auto b = bridge(ch);
        if (!b) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
            return;
        }
        uint64_t e = 0, s = 0;
        std::string all = b->read_all(&e, &s);
        std::string fn = "ssh-bridge-" + ch + ".log";
        for (char& x : fn) if (!std::isalnum(static_cast<unsigned char>(x)) && x != '-' && x != '_' && x != '.') x = '_';
        std::string hdr = "Content-Type: text/plain; charset=utf-8\r\n"
                          "Content-Disposition: attachment; filename=\"" + fn + "\"\r\n"
                          "Cache-Control: no-store\r\n";
        mg_http_reply(c, 200, hdr.c_str(), "%s", all.c_str());
        return;
    }
    if (path == "/log/clear") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        std::string ch = query_var(hm, "channel");
        auto b = bridge(ch);
        if (!b) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
            return;
        }
        b->clear();
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "channel", ch.c_str());
        cJSON_AddNumberToObject(root, "epoch", static_cast<double>(b->log_epoch()));
        cJSON_AddNumberToObject(root, "total", static_cast<double>(b->log_total()));
        reply_json(c, 200, root);
        return;
    }
    if (path == "/log/disk") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "POST required", new_request_id()));
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, agent::error_json("unauthorized", "write requires Bearer token", new_request_id()));
            return;
        }
        std::string raw = body_string(hm);
        cJSON* body = raw.empty() ? nullptr : cJSON_Parse(raw.c_str());
        if (!raw.empty() && !cJSON_IsObject(body)) {
            cJSON_Delete(body);
            reply_json_text(c, 400, agent::error_json("invalid_json", "request body must be a JSON object", new_request_id()));
            return;
        }
        std::string ch = cJSON_IsObject(body) ? json_str(body, "channel") : std::string();
        if (ch.empty()) ch = query_var(hm, "channel");
        bool enabled = false;
        bool has_enabled = false;
        const cJSON* ev = cJSON_IsObject(body) ? cJSON_GetObjectItemCaseSensitive(body, "enabled") : nullptr;
        if (cJSON_IsBool(ev)) {
            enabled = cJSON_IsTrue(ev);
            has_enabled = true;
        } else if (cJSON_IsNumber(ev)) {
            enabled = ev->valuedouble != 0;
            has_enabled = true;
        }
        if (!has_enabled) {
            std::string q = query_var(hm, "enabled");
            if (q == "true" || q == "1") { enabled = true; has_enabled = true; }
            else if (q == "false" || q == "0") { enabled = false; has_enabled = true; }
        }
        cJSON_Delete(body);
        if (!has_enabled) {
            reply_json_text(c, 400, agent::error_json("invalid_argument", "enabled must be a boolean", new_request_id()));
            return;
        }
        auto b = bridge(ch);
        if (!b) {
            reply_json_text(c, 404, agent::error_json("channel_not_found", "channel not found", new_request_id()));
            return;
        }
        b->set_disk_log(enabled);
        bool actual = b->disk_log_enabled();
        if (enabled && !actual) {
            std::string err = b->disk_log_error();
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON_AddStringToObject(root, "error", "log_disk_open_failed");
            cJSON_AddStringToObject(root, "message", err.empty() ? "cannot open log file" : err.c_str());
            cJSON_AddStringToObject(root, "channel", ch.c_str());
            cJSON_AddStringToObject(root, "request_id", new_request_id().c_str());
            reply_json(c, 500, root);
            return;
        }
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "channel", ch.c_str());
        cJSON_AddBoolToObject(root, "enabled", enabled);
        cJSON_AddBoolToObject(root, "disk_enabled", actual);
        std::string err = b->disk_log_error();
        if (!err.empty()) cJSON_AddStringToObject(root, "last_error", err.c_str());
        reply_json(c, 200, root);
        return;
    }
    if (path == "/write") {
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string ch = json_str(body, "channel");
        std::string data = json_str(body, "data");
        if (data.empty()) data = json_str(body, "text");
        std::string session = json_str(body, "session");
        if (session.empty()) session = header_text(hm, "X-Agent-Session");
        cJSON_Delete(body);
        std::string rid = request_id_of(hm);
        auto b = bridge(ch);
        if (!b) {
            reply_json_text(c, 404, agent::error_json("channel_not_found", "channel not found", rid));
            return;
        }
        if (!allow_channel_write(c, ch, session, rid)) return;
        if (!b->running()) {
            reply_json_text(c, 409, agent::error_json("channel_down", "channel is not running", rid));
            return;
        }
        std::size_t n = b->write(data.data(), data.size());
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "bytes", static_cast<double>(n));
        cJSON_AddStringToObject(root, "request_id", rid.c_str());
        reply_json(c, 202, root);
        return;
    }
    if (path == "/lock" || path == "/unlock" || path == "/lock/takeover") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "POST required", request_id_of(hm)));
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, agent::error_json("unauthorized", "write requires Bearer token", request_id_of(hm)));
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string ch = json_str(body, "channel");
        std::string sid = json_str(body, "session_id");
        std::string reason = json_str(body, "reason");
        int ttl = json_int(body, "ttl_ms", 1000);
        cJSON_Delete(body);
        if (sid.empty()) sid = header_text(hm, "X-Agent-Session");
        if (ch.empty()) {
            reply_json_text(c, 400, agent::error_json("invalid_argument", "channel required", request_id_of(hm)));
            return;
        }
        if (!bridge(ch)) {
            reply_json_text(c, 404, agent::error_json("channel_not_found", "channel not found", request_id_of(hm)));
            return;
        }
        std::string rid = request_id_of(hm);
        if (path == "/lock") {
            if (sid.empty()) {
                reply_json_text(c, 400, agent::error_json("session_required", "session_id or X-Agent-Session required", rid));
                return;
            }
            if (ttl == 0) ttl = 1000;
            if (ttl < 100 || ttl > 86400000) {
                reply_json_text(c, 400, agent::error_json("invalid_ttl", "ttl_ms must be 100..86400000", rid));
                return;
            }
            agent::LeaseInfo before = agent::lease_locks().get(ch);
            bool renewed = before.active && before.owner == sid;
            agent::LeaseInfo lease, blocker;
            if (!agent::lease_locks().acquire(ch, sid, ttl, &lease, &blocker)) {
                cJSON* root = cJSON_CreateObject();
                cJSON_AddBoolToObject(root, "ok", false);
                cJSON* err = cJSON_AddObjectToObject(root, "error");
                cJSON_AddStringToObject(err, "code", "agent_lock");
                cJSON_AddStringToObject(err, "message", "channel is locked by another session");
                cJSON_AddStringToObject(err, "owner", blocker.owner.c_str());
                cJSON_AddNumberToObject(err, "expires_in_ms", static_cast<double>(blocker.remaining_ms));
                cJSON_AddBoolToObject(err, "can_takeover", true);
                cJSON_AddStringToObject(root, "request_id", rid.c_str());
                reply_json(c, 423, root);
                return;
            }
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddStringToObject(root, "channel", ch.c_str());
            cJSON_AddStringToObject(root, "owner", sid.c_str());
            cJSON_AddNumberToObject(root, "ttl_ms", ttl);
            cJSON_AddNumberToObject(root, "expires_in_ms", ttl);
            cJSON_AddBoolToObject(root, "renewed", renewed);
            cJSON_AddStringToObject(root, "request_id", rid.c_str());
            reply_json(c, 200, root);
            return;
        }
        if (path == "/unlock") {
            if (sid.empty()) {
                reply_json_text(c, 400, agent::error_json("session_required", "session_id or X-Agent-Session required", rid));
                return;
            }
            agent::LeaseInfo blocker;
            if (agent::lease_locks().check(ch, sid, &blocker) != 0) {
                if (!allow_channel_write(c, ch, sid, rid)) return;
            }
            bool released = false;
            bool ok = agent::lease_locks().release(ch, sid, &released, &blocker);
            if (!ok) {
                if (!allow_channel_write(c, ch, sid, rid)) return;
                reply_json_text(c, 423, agent::error_json("agent_lock", "unlock failed", rid));
                return;
            }
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddStringToObject(root, "channel", ch.c_str());
            cJSON_AddBoolToObject(root, "released", released);
            cJSON_AddStringToObject(root, "request_id", rid.c_str());
            reply_json(c, 200, root);
            return;
        }
        // /lock/takeover：人工显式接管，必须说明 reason。
        if (reason.empty()) {
            reply_json_text(c, 400, agent::error_json("invalid_argument", "reason required", rid));
            return;
        }
        agent::LeaseInfo previous;
        agent::lease_locks().takeover(ch, sid.empty() ? "human" : sid, reason, &previous);
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "channel", ch.c_str());
        cJSON_AddStringToObject(root, "previous_owner", previous.active ? previous.owner.c_str() : "");
        cJSON_AddStringToObject(root, "reason", reason.c_str());
        cJSON_AddNumberToObject(root, "at_ms", static_cast<double>(now_ms()));
        cJSON_AddStringToObject(root, "request_id", rid.c_str());
        reply_json(c, 200, root);
        return;
    }

    if (path == "/command" || path == "/command_block") {
        const bool block = (path == "/command_block");
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "POST required", request_id_of(hm)));
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, agent::error_json("unauthorized", "write requires Bearer token", request_id_of(hm)));
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string ch = json_str(body, "channel");
        std::string command = json_str(body, "command");
        std::string le = decode_line_ending(json_str(body, "line_ending"), true);
        std::string session = json_str(body, "session");
        if (session.empty()) session = header_text(hm, "X-Agent-Session");
        int timeout_ms = json_int(body, "timeout_ms", block ? 2000 : 0);
        cJSON_Delete(body);
        if (ch.empty() || command.empty()) {
            reply_json_text(c, 400, agent::error_json("invalid_argument", "channel and command required", request_id_of(hm)));
            return;
        }
        if (block) {
            if (timeout_ms < 0) {
                reply_json_text(c, 400, agent::error_json("invalid_argument", "timeout_ms must be >= 0", request_id_of(hm)));
                return;
            }
            if (timeout_ms == 0) timeout_ms = 2000;
            if (timeout_ms > 60000) timeout_ms = 60000;
        }
        auto b = bridge(ch);
        std::string rid = request_id_of(hm);
        if (!b) {
            reply_json_text(c, 404, agent::error_json("channel_not_found", "channel not found", rid));
            return;
        }
        if (!allow_channel_write(c, ch, session, rid)) return;
        if (!b->running()) {
            reply_json_text(c, 409, agent::error_json("channel_down", "channel is not running", rid));
            return;
        }
        Task task;
        int busy_id = 0;
        std::string busy_state;
        task.job = make_job(block ? "command_block" : "command", ch, rid, session, &busy_id, &busy_state);
        if (!task.job) {
            reply_channel_busy(c, ch, busy_id, busy_state, rid);
            return;
        }
        auto job = task.job;
        task.kind = block ? TaskKind::CommandBlock : TaskKind::Command;
        task.cfg = core ? core->config() : AppConfig{};
        task.channel = ch;
        task.command = command;
        task.line_ending = le;
        task.timeout_ms = timeout_ms;
        task.bridge = b;
        if (!block) task.data = command + le;
        int job_id = enqueue_task(std::move(task));
        if (job_id < 0) {
            std::lock_guard<std::mutex> g(mu);
            jobs.erase(job->id);
            reply_json_text(c, 429, agent::error_json("queue_full", "job queue full", rid));
            return;
        }
        if (!block) {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddNumberToObject(root, "job_id", job_id);
            cJSON_AddBoolToObject(root, "queued", true);
            cJSON_AddStringToObject(root, "request_id", rid.c_str());
            reply_json(c, 202, root);
            return;
        }
        std::string done_body;
        if (!hold_job(c, job, &done_body)) {
            reply_text(c, 200, done_body, "application/json; charset=utf-8");
        }
        return;
    }

    if (path == "/agent/exec") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "POST required", request_id_of(hm)));
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, agent::error_json("unauthorized", "write requires Bearer token", request_id_of(hm)));
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string ch = json_str(body, "channel");
        std::string command = json_str(body, "command");
        std::string le = decode_line_ending(json_str(body, "line_ending"), true);
        std::string session = json_str(body, "session");
        if (session.empty()) session = header_text(hm, "X-Agent-Session");
        int timeout_ms = json_int(body, "timeout_ms", 10000);
        cJSON_Delete(body);
        if (ch.empty() || command.empty()) {
            reply_json_text(c, 400, agent::error_json("invalid_argument", "channel and command required", request_id_of(hm)));
            return;
        }
        if (timeout_ms < 0) {
            reply_json_text(c, 400, agent::error_json("invalid_argument", "timeout_ms must be >= 0", request_id_of(hm)));
            return;
        }
        if (timeout_ms == 0) timeout_ms = 10000;
        if (timeout_ms > 60000) timeout_ms = 60000;
        auto b = bridge(ch);
        std::string rid = request_id_of(hm);
        if (!b) {
            reply_json_text(c, 404, agent::error_json("channel_not_found", "channel not found", rid));
            return;
        }
        if (!allow_channel_write(c, ch, session, rid)) return;
        if (!b->running()) {
            reply_json_text(c, 409, agent::error_json("channel_down", "channel is not running", rid));
            return;
        }
        int busy_id = 0;
        std::string busy_state;
        if (channel_busy(ch, &busy_id, &busy_state)) {
            reply_channel_busy(c, ch, busy_id, busy_state, rid);
            return;
        }
        std::string probe = agent::probe_json(core, ch, 4096);
        std::string term;
        cJSON* pj = cJSON_Parse(probe.c_str());
        if (pj) {
            term = json_str(pj, "terminal");
            cJSON_Delete(pj);
        }
        if (term != "shell") {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON* err = cJSON_AddObjectToObject(root, "error");
            cJSON_AddStringToObject(err, "code", "state_mismatch");
            cJSON_AddStringToObject(err, "message", "channel terminal is not shell");
            cJSON_AddStringToObject(err, "terminal", term.empty() ? "unknown" : term.c_str());
            cJSON_AddStringToObject(root, "request_id", rid.c_str());
            reply_json(c, 409, root);
            return;
        }
        Task task;
        task.job = make_job("exec", ch, rid, session, &busy_id, &busy_state);
        if (!task.job) {
            reply_channel_busy(c, ch, busy_id, busy_state, rid);
            return;
        }
        auto job = task.job;
        task.kind = TaskKind::Exec;
        task.cfg = core ? core->config() : AppConfig{};
        task.channel = ch;
        task.command = command;
        task.line_ending = le;
        task.timeout_ms = timeout_ms;
        task.marker = "__C4_END_" + std::to_string(job->id) + "__";
        task.bridge = b;
        int job_id = enqueue_task(std::move(task));
        if (job_id < 0) {
            std::lock_guard<std::mutex> g(mu);
            jobs.erase(job->id);
            reply_json_text(c, 429, agent::error_json("queue_full", "job queue full", rid));
            return;
        }
        std::string done_body;
        if (!hold_job(c, job, &done_body)) {
            reply_text(c, 200, done_body, "application/json; charset=utf-8");
        }
        return;
    }

    if (path == "/send_keys") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "POST required", request_id_of(hm)));
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, agent::error_json("unauthorized", "write requires Bearer token", request_id_of(hm)));
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string ch = json_str(body, "channel");
        std::string session = json_str(body, "session");
        if (session.empty()) session = header_text(hm, "X-Agent-Session");
        int interval_ms = json_int(body, "interval_ms", 50);
        std::vector<std::string> keys;
        bool bad = false;
        cJSON* arr = cJSON_GetObjectItemCaseSensitive(body, "keys");
        if (cJSON_IsArray(arr)) {
            cJSON* it = nullptr;
            cJSON_ArrayForEach(it, arr) {
                if (!cJSON_IsString(it)) {
                    bad = true;
                    break;
                }
                char out[64];
                std::size_t n = normalize_key_char(it->valuestring, out, sizeof(out));
                if (n == 0) {
                    bad = true;
                    break;
                }
                keys.emplace_back(out, n);
                if (keys.size() > 256) {
                    bad = true;
                    break;
                }
            }
        } else {
            bad = true;
        }
        cJSON_Delete(body);
        if (bad || ch.empty() || keys.empty()) {
            reply_json_text(c, 400, agent::error_json("invalid_keys", "keys must be a non-empty string array", request_id_of(hm)));
            return;
        }
        if (interval_ms < 10 || interval_ms > 1000) {
            reply_json_text(c, 400, agent::error_json("invalid_argument", "interval_ms must be 10..1000", request_id_of(hm)));
            return;
        }
        auto b = bridge(ch);
        std::string rid = request_id_of(hm);
        if (!b) {
            reply_json_text(c, 404, agent::error_json("channel_not_found", "channel not found", rid));
            return;
        }
        if (!allow_channel_write(c, ch, session, rid)) return;
        if (!b->running()) {
            reply_json_text(c, 409, agent::error_json("channel_down", "channel is not running", rid));
            return;
        }
        Task task;
        int busy_id = 0;
        std::string busy_state;
        task.job = make_job("send_keys", ch, rid, session, &busy_id, &busy_state);
        if (!task.job) {
            reply_channel_busy(c, ch, busy_id, busy_state, rid);
            return;
        }
        auto job = task.job;
        task.kind = TaskKind::SendKeys;
        task.cfg = core ? core->config() : AppConfig{};
        task.channel = ch;
        task.keys = std::move(keys);
        std::size_t key_count = task.keys.size();
        task.interval_ms = interval_ms;
        task.bridge = b;
        int job_id = enqueue_task(std::move(task));
        if (job_id < 0) {
            std::lock_guard<std::mutex> g(mu);
            jobs.erase(job->id);
            reply_json_text(c, 429, agent::error_json("queue_full", "job queue full", rid));
            return;
        }
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "count", static_cast<double>(key_count));
        cJSON_AddNumberToObject(root, "job_id", job_id);
        cJSON_AddStringToObject(root, "request_id", rid.c_str());
        reply_json(c, 202, root);
        return;
    }

    if (path == "/api/config/save") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        std::string err;
        if (!core || !core->save_config_json(body_string(hm), &err)) {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON_AddStringToObject(root, "error", err.c_str());
            reply_json(c, 400, root);
            return;
        }
        if (notify) PostMessageW(static_cast<HWND>(notify), kMsgWebReload, 0, 0);
        reply_json_text(c, 200, "{\"ok\":true,\"reload\":\"posted\"}");
        return;
    }
    if (path == "/api/config") {
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"config requires Bearer token\"}");
            return;
        }
        reply_text(c, 200, core ? core->config_json_text() : "{}", "application/json; charset=utf-8");
        return;
    }
    if (path == "/api/help") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", new_request_id()));
            return;
        }
        reply_json_text(c, 200, agent::help_json(core, port, ui_info(), !token.empty()));
        return;
    }
    if (path == "/api/agent/status") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", new_request_id()));
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"agent status requires Bearer token\"}");
            return;
        }
        reply_json_text(c, 200, agent::status_json(core, port, ui_info(), !token.empty()));
        return;
    }
    if (path == "/api/channels") {
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"channels requires Bearer token\"}");
            return;
        }
        if (!core) {
            reply_json_text(c, 500, "{\"ok\":false,\"error\":\"core not ready\"}");
            return;
        }
        if (!method_is(hm, "GET") && !method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"GET or POST required\"}");
            return;
        }
        cJSON* root = cJSON_Parse(core->config_json_text().c_str());
        if (!cJSON_IsObject(root)) {
            cJSON_Delete(root);
            reply_json_text(c, 500, "{\"ok\":false,\"error\":\"config parse failed\"}");
            return;
        }
        if (method_is(hm, "GET")) {
            ensure_channel_array(root, "boards");
            ensure_channel_array(root, "ssh");
            cJSON* out = cJSON_CreateObject();
            cJSON_AddBoolToObject(out, "ok", true);
            cJSON_AddItemToObject(out, "boards",
                                  cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(root, "boards"), 1));
            cJSON_AddItemToObject(out, "ssh",
                                  cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(root, "ssh"), 1));
            cJSON_Delete(root);
            reply_json(c, 200, out);
            return;
        }

        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        cJSON* ch = body ? cJSON_GetObjectItemCaseSensitive(body, "channel") : nullptr;
        std::string kind = body ? json_str(body, "kind") : std::string();
        std::string old_name = body ? util::trim(json_str(body, "old_name")) : std::string();
        std::string name = cJSON_IsObject(ch) ? util::trim(json_str(ch, "name")) : std::string();
        std::string err;
        if (!cJSON_IsObject(ch) || (kind != "serial" && kind != "ssh") ||
            !valid_channel_name(name, &err)) {
            cJSON_Delete(body);
            cJSON_Delete(root);
            cJSON* e = cJSON_CreateObject();
            cJSON_AddBoolToObject(e, "ok", false);
            cJSON_AddStringToObject(e, "error", err.empty() ? "kind/channel/name invalid" : err.c_str());
            reply_json(c, 400, e);
            return;
        }
        ensure_channel_array(root, "boards");
        ensure_channel_array(root, "ssh");
        if (channel_object(root, name) && name != old_name) {
            cJSON_Delete(body);
            cJSON_Delete(root);
            reply_json_text(c, 409, "{\"ok\":false,\"error\":\"通道名已存在\"}");
            return;
        }
        cJSON* arr = ensure_channel_array(root, kind == "serial" ? "boards" : "ssh");
        int idx = -1;
        cJSON* target = find_named_in_array(arr, old_name.empty() ? name : old_name, &idx);
        if (!target && !old_name.empty()) {
            cJSON_Delete(body);
            cJSON_Delete(root);
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"通道不存在\"}");
            return;
        }
        bool created = false;
        if (!target) {
            target = cJSON_CreateObject();
            cJSON_AddItemToArray(arr, target);
            created = true;
        }
        if (!name.empty()) {
            cJSON_DeleteItemFromObjectCaseSensitive(target, "name");
            cJSON_AddStringToObject(target, "name", name.c_str());
        }
        if (kind == "serial") {
            std::string dev = util::trim(json_str(ch, "serial_device"));
            if (dev.empty()) {
                cJSON_Delete(body);
                cJSON_Delete(root);
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"串口设备不能为空\"}");
                return;
            }
            copy_str_field(target, ch, "serial_device");
            copy_int_field(target, ch, "baudrate", 115200, 50, 4000000);
            copy_int_field(target, ch, "databits", 8, 5, 8);
            copy_str_field(target, ch, "parity");
            copy_int_field(target, ch, "stopbits_x10", 10, 10, 20);
            copy_str_field(target, ch, "flowcontrol");
            copy_bool_field(target, ch, "dtr", true);
            copy_bool_field(target, ch, "rts", true);
            copy_str_field(target, ch, "line_ending");
            copy_str_field(target, ch, "encoding");
            copy_bool_field(target, ch, "auto_open", true);
            copy_bool_field(target, ch, "timestamp", false);
            copy_bool_field(target, ch, "discoverable", false);
            copy_str_field(target, ch, "username");
            copy_str_field(target, ch, "password");
            copy_bool_field(target, ch, "log_enabled", true);
        } else {
            std::string host = util::trim(json_str(ch, "host"));
            if (host.empty()) {
                cJSON_Delete(body);
                cJSON_Delete(root);
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"SSH 主机不能为空\"}");
                return;
            }
            copy_str_field(target, ch, "host");
            copy_int_field(target, ch, "port", 22, 1, 65535);
            copy_str_field(target, ch, "username");
            copy_str_field(target, ch, "password");
            copy_str_field(target, ch, "term");
            copy_int_field(target, ch, "width", 120, 40, 500);
            copy_int_field(target, ch, "height", 32, 10, 200);
            copy_int_field(target, ch, "connect_timeout_ms", 5000, 100, 120000);
            const cJSON* ka = cJSON_GetObjectItemCaseSensitive(ch, "keepalive");
            if (!cJSON_IsNumber(ka)) ka = cJSON_GetObjectItemCaseSensitive(ch, "keepalive_s");
            copy_int_field(target, ch, "keepalive", cJSON_IsNumber(ka) ? static_cast<int>(ka->valuedouble) : 3,
                          0, 3600);
            copy_bool_field(target, ch, "discoverable", false);
            copy_bool_field(target, ch, "log_enabled", true);
        }
        // 改名时同步分屏画框里保存的通道名。
        if (!old_name.empty() && old_name != name) {
            cJSON* ui = cJSON_GetObjectItemCaseSensitive(root, "ui");
            cJSON* panes = ui ? cJSON_GetObjectItemCaseSensitive(ui, "pane_channels") : nullptr;
            if (cJSON_IsArray(panes)) {
                std::vector<int> hit;
                int i = 0;
                for (cJSON* p = panes->child; p; p = p->next, ++i) {
                    if (cJSON_IsString(p) && p->valuestring && old_name == p->valuestring) hit.push_back(i);
                }
                for (int h : hit) cJSON_ReplaceItemInArray(panes, h, cJSON_CreateString(name.c_str()));
            }
        }
        if (created) {
            const cJSON* buttons = cJSON_GetObjectItemCaseSensitive(ch, "buttons");
            if (buttons) cJSON_AddItemToObject(target, "buttons", cJSON_Duplicate(buttons, 1));
            const cJSON* quick = cJSON_GetObjectItemCaseSensitive(ch, "quick_commands");
            if (quick) cJSON_AddItemToObject(target, "quick_commands", cJSON_Duplicate(quick, 1));
        }
        std::string text = json_text(root, false), save_err;
        bool saved = core->save_config_json(text, &save_err);
        cJSON_Delete(body);
        if (!saved) {
            cJSON* e = cJSON_CreateObject();
            cJSON_AddBoolToObject(e, "ok", false);
            cJSON_AddStringToObject(e, "error", save_err.c_str());
            reply_json(c, 500, e);
            return;
        }
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "name", name.c_str());
        cJSON_AddStringToObject(out, "kind", kind.c_str());
        reply_json(c, 200, out);
        if (notify) PostMessageW(static_cast<HWND>(notify), kMsgWebReload, 0, 0);
        return;
    }
    if (path == "/api/channels/delete") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"channels requires Bearer token\"}");
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string kind = body ? json_str(body, "kind") : std::string();
        std::string name = body ? util::trim(json_str(body, "name")) : std::string();
        if (!cJSON_IsObject(body) || (kind != "serial" && kind != "ssh") || name.empty()) {
            cJSON_Delete(body);
            reply_json_text(c, 400, "{\"ok\":false,\"error\":\"kind/name required\"}");
            return;
        }
        cJSON* root = cJSON_Parse(core ? core->config_json_text().c_str() : "{}");
        if (!cJSON_IsObject(root)) {
            cJSON_Delete(body);
            cJSON_Delete(root);
            reply_json_text(c, 500, "{\"ok\":false,\"error\":\"config parse failed\"}");
            return;
        }
        cJSON* arr = ensure_channel_array(root, kind == "serial" ? "boards" : "ssh");
        int idx = -1;
        cJSON* target = find_named_in_array(arr, name, &idx);
        if (!target) {
            cJSON_Delete(body);
            cJSON_Delete(root);
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"通道不存在\"}");
            return;
        }
        cJSON_DeleteItemFromArray(arr, idx);
        cJSON* ui = cJSON_GetObjectItemCaseSensitive(root, "ui");
        cJSON* panes = ui ? cJSON_GetObjectItemCaseSensitive(ui, "pane_channels") : nullptr;
        if (cJSON_IsArray(panes)) {
            std::vector<int> hit;
            int i = 0;
            for (cJSON* p = panes->child; p; p = p->next, ++i) {
                if (cJSON_IsString(p) && p->valuestring && name == p->valuestring) hit.push_back(i);
            }
            for (int h : hit) cJSON_ReplaceItemInArray(panes, h, cJSON_CreateString(""));
        }
        cJSON_Delete(body);
        std::string text = json_text(root, false), save_err;
        bool saved = core->save_config_json(text, &save_err);
        if (!saved) {
            cJSON* e = cJSON_CreateObject();
            cJSON_AddBoolToObject(e, "ok", false);
            cJSON_AddStringToObject(e, "error", save_err.c_str());
            reply_json(c, 500, e);
            return;
        }
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "name", name.c_str());
        reply_json(c, 200, out);
        if (notify) PostMessageW(static_cast<HWND>(notify), kMsgWebReload, 0, 0);
        return;
    }
    if (path == "/api/actions") {
        if (method_is(hm, "POST")) {
            if (!write_ok(hm)) {
                reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
                return;
            }
            cJSON* body = cJSON_Parse(body_string(hm).c_str());
            if (!cJSON_IsObject(body)) {
                cJSON_Delete(body);
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"invalid json\"}");
                return;
            }
            std::string channel = json_str(body, "channel");
            std::string old_id = json_str(body, "old_id");
            const cJSON* ao = cJSON_GetObjectItemCaseSensitive(body, "action");
            BoardActionConfig a;
            std::string err;
            if (!parse_action(ao, &a, &err)) {
                cJSON_Delete(body);
                cJSON* root = cJSON_CreateObject();
                cJSON_AddBoolToObject(root, "ok", false);
                cJSON_AddStringToObject(root, "error", err.c_str());
                reply_json(c, 400, root);
                return;
            }
            cJSON_Delete(body);
            bool ok = patch_config([&](cJSON* root) -> bool {
                cJSON* ch = channel_object(root, channel);
                if (!cJSON_IsObject(ch)) {
                    err = "channel not found";
                    return false;
                }
                cJSON* buttons = cJSON_GetObjectItemCaseSensitive(ch, "buttons");
                if (!cJSON_IsArray(buttons)) {
                    buttons = cJSON_AddArrayToObject(ch, "buttons");
                    if (!buttons) {
                        err = "cannot create buttons";
                        return false;
                    }
                }
                int found = -1;
                cJSON* it = nullptr;
                int idx = 0;
                cJSON_ArrayForEach(it, buttons) {
                    if (cJSON_IsObject(it) && json_str(it, "id") == (old_id.empty() ? a.id : old_id)) {
                        found = idx;
                        break;
                    }
                    ++idx;
                }
                if (!old_id.empty() && found < 0) {
                    err = "old_id not found";
                    return false;
                }
                if (old_id.empty() && found >= 0) {
                    err = "id already exists";
                    return false;
                }
                if (a.backend == "relay") {
                    bool relay_ok = false;
                    cJSON* devs = cJSON_GetObjectItemCaseSensitive(root, "relay_devices");
                    cJSON* dev = nullptr;
                    if (cJSON_IsArray(devs)) {
                        cJSON_ArrayForEach(dev, devs) {
                            if (cJSON_IsObject(dev) && json_str(dev, "name") == a.relay) {
                                relay_ok = true;
                                break;
                            }
                        }
                    }
                    if (!relay_ok) {
                        err = "relay not found";
                        return false;
                    }
                }
                cJSON* o = cJSON_CreateObject();
                add_action_json(o, a);
                if (found >= 0) cJSON_DeleteItemFromArray(buttons, found);
                cJSON_AddItemToArray(buttons, o);
                return true;
            }, &err);
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", ok);
            cJSON_AddStringToObject(root, "error", err.c_str());
            if (ok) cJSON_AddStringToObject(root, "reload", "posted");
            reply_json(c, ok ? 200 : 400, root);
            return;
        }
        reply_json_text(c, 200, actions());
        return;
    }
    if (path == "/api/actions/delete") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string channel = json_str(body, "channel");
        std::string id = json_str(body, "id");
        cJSON_Delete(body);
        std::string err;
        bool ok = patch_config([&](cJSON* root) -> bool {
            cJSON* ch = channel_object(root, channel);
            if (!cJSON_IsObject(ch)) {
                err = "channel not found";
                return false;
            }
            cJSON* buttons = cJSON_GetObjectItemCaseSensitive(ch, "buttons");
            if (!cJSON_IsArray(buttons)) {
                err = "action not found";
                return false;
            }
            int idx = 0;
            cJSON* it = nullptr;
            cJSON_ArrayForEach(it, buttons) {
                if (cJSON_IsObject(it) && json_str(it, "id") == id) {
                    cJSON_DeleteItemFromArray(buttons, idx);
                    return true;
                }
                ++idx;
            }
            err = "action not found";
            return false;
        }, &err);
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", ok);
        cJSON_AddStringToObject(root, "error", err.c_str());
        reply_json(c, ok ? 200 : 400, root);
        return;
    }
    if (path == "/api/quick/commands") {
        if (method_is(hm, "POST")) {
            if (!write_ok(hm)) {
                reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
                return;
            }
            cJSON* body = cJSON_Parse(body_string(hm).c_str());
            if (!cJSON_IsObject(body)) {
                cJSON_Delete(body);
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"invalid json\"}");
                return;
            }
            std::string channel = json_str(body, "channel");
            std::string label = json_str(body, "label");
            std::string command = json_str(body, "command");
            std::string op = json_str(body, "op");
            int index = json_int(body, "index", -1);
            cJSON_Delete(body);
            if (label.empty() && op != "delete") {
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"label 不能为空\"}");
                return;
            }
            std::string err;
            bool ok = patch_config([&](cJSON* root) -> bool {
                cJSON* ch = channel_object(root, channel);
                if (!cJSON_IsObject(ch)) {
                    err = "channel not found";
                    return false;
                }
                cJSON* qs = cJSON_GetObjectItemCaseSensitive(ch, "quick_commands");
                if (!cJSON_IsArray(qs)) {
                    qs = cJSON_AddArrayToObject(ch, "quick_commands");
                    if (!qs) {
                        err = "cannot create quick_commands";
                        return false;
                    }
                }
                int size = cJSON_GetArraySize(qs);
                if (op == "delete") {
                    if (index < 0 || index >= size) {
                        err = "quick command not found";
                        return false;
                    }
                    cJSON_DeleteItemFromArray(qs, index);
                    return true;
                }
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "label", label.c_str());
                cJSON_AddStringToObject(item, "command", command.c_str());
                if (index >= 0 && index < size) {
                    cJSON_DeleteItemFromArray(qs, index);
                }
                cJSON_AddItemToArray(qs, item);
                return true;
            }, &err);
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", ok);
            cJSON_AddStringToObject(root, "error", err.c_str());
            reply_json(c, ok ? 200 : 400, root);
            return;
        }
        reply_json_text(c, 200, quick());
        return;
    }
    if (path == "/quick") {
        if (method_is(hm, "POST")) {
            if (!write_ok(hm)) {
                reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
                return;
            }
            cJSON* body = cJSON_Parse(body_string(hm).c_str());
            std::string ch = json_str(body, "channel");
            std::string cmd = json_str(body, "command");
            cJSON_Delete(body);
            auto b = bridge(ch);
            if (!b) {
                reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
                return;
            }
            std::string data = cmd;
            if (data.find('\n') == std::string::npos && data.find('\r') == std::string::npos) data += "\r";
            b->write(data.data(), data.size());
            reply_json_text(c, 200, "{\"ok\":true}");
            return;
        }
        reply_json_text(c, 200, quick());
        return;
    }
    if (path == "/action") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string ch = json_str(body, "channel");
        std::string id = json_str(body, "id");
        std::string backend = json_str(body, "backend");
        std::string session = json_str(body, "session");
        cJSON_Delete(body);
        std::string rid = request_id_of(hm);
        const BoardActionConfig* found = nullptr;
        const auto* btns = core ? core->buttons_of(ch) : nullptr;
        if (btns) {
            for (const BoardActionConfig& a : *btns) {
                if (a.id == id) {
                    found = &a;
                    break;
                }
            }
        }
        if (!found) {
            reply_json_text(c, 404, agent::error_json("action_not_found", "action not found", rid));
            return;
        }
        if (!backend.empty() && backend != found->backend) {
            reply_json_text(c, 400, agent::error_json("backend_mismatch", "requested backend does not match config", rid));
            return;
        }
        if (!allow_channel_write(c, ch, session, rid)) return;
        if (!found->enable) {
            reply_json_text(c, 409, agent::error_json("action_disabled", "action disabled", rid));
            return;
        }
        auto b = bridge(ch);
        if (!b) {
            reply_json_text(c, 404, agent::error_json("channel_not_found", "channel not found", rid));
            return;
        }
        if (!b->running()) {
            reply_json_text(c, 409, agent::error_json("channel_down", "channel is not running", rid));
            return;
        }
        if (session.empty()) session = header_text(hm, "X-Agent-Session");
        int busy_id = 0;
        std::string busy_state;
        if (channel_busy(ch, &busy_id, &busy_state)) {
            reply_channel_busy(c, ch, busy_id, busy_state, rid);
            return;
        }
        int job = spawn_action(ch, id, rid, session);
        if (job == -2) {
            reply_channel_busy(c, ch, 0, "queued", rid);
            return;
        }
        if (job < 0) {
            reply_json_text(c, 429, agent::error_json("queue_full", "action queue full", rid));
            return;
        }
        if (job == 0) {
            reply_json_text(c, 404, agent::error_json("action_not_found", "action not found", rid));
            return;
        }
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "job", job);
        cJSON_AddNumberToObject(root, "job_id", job);
        cJSON_AddBoolToObject(root, "queued", true);
        cJSON_AddStringToObject(root, "request_id", rid.c_str());
        reply_json(c, 202, root);
        return;
    }
    if (path == "/job") {
        int id = std::atoi(query_var(hm, "id", "0").c_str());
        reply_json_text(c, 200, jobs_json(id));
        return;
    }
    if (path == "/jobs" || (path.size() >= 7 && path.compare(0, 6, "/jobs/") == 0)) {
        cleanup_job();
        if (path == "/jobs") {
            if (!method_is(hm, "GET")) {
                reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", request_id_of(hm)));
                return;
            }
            std::string state = query_var(hm, "state");
            if (!state.empty() && state != "queued" && state != "running" && state != "done" &&
                state != "failed" && state != "timeout" && state != "cancelled") {
                reply_json_text(c, 400, agent::error_json("invalid_argument", "unknown state filter", request_id_of(hm)));
                return;
            }
            int limit = std::atoi(query_var(hm, "limit", "20").c_str());
            if (limit <= 0) limit = 20;
            if (limit > 100) limit = 100;
            int before_id = std::atoi(query_var(hm, "before_id", "0").c_str());
            reply_json_text(c, 200, jobs_list_json(state, query_var(hm, "channel"), before_id, limit));
            return;
        }
        std::string tail = path.substr(6);
        bool cancel = false;
        if (tail.size() > 7 && tail.compare(tail.size() - 7, 7, "/cancel") == 0) {
            tail.resize(tail.size() - 7);
            cancel = true;
        }
        while (!tail.empty() && tail.back() == '/') tail.pop_back();
        int id = std::atoi(tail.c_str());
        if (id <= 0 || std::to_string(id) != tail) {
            reply_json_text(c, 404, agent::error_json("job_not_found", "invalid job id", request_id_of(hm)));
            return;
        }
        if (!cancel) {
            if (!method_is(hm, "GET")) {
                reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", request_id_of(hm)));
                return;
            }
            reply_json_text(c, 200, jobs_json(id));
            return;
        }
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "POST required", request_id_of(hm)));
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, agent::error_json("unauthorized", "write requires Bearer token", request_id_of(hm)));
            return;
        }
        std::string rid = request_id_of(hm);
        std::lock_guard<std::mutex> g(mu);
        auto it = jobs.find(id);
        if (it == jobs.end()) {
            reply_json_text(c, 404, agent::error_json("job_not_found", "job not found", rid));
            return;
        }
        Job& j = *it->second;
        if (j.state == "done" || j.state == "failed" || j.state == "timeout" || j.state == "cancelled") {
            reply_json_text(c, 409, agent::error_json("job_finished", "job already finished", rid));
            return;
        }
        j.cancel_requested.store(true);
        if (j.state == "queued") {
            j.done = true;
            j.ok = false;
            j.state = "cancelled";
            j.message = "cancelled";
            j.error = "cancelled";
            j.ended_ms = now_ms();
        }
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "id", id);
        cJSON_AddNumberToObject(root, "job_id", id);
        cJSON_AddStringToObject(root, "state", j.state.c_str());
        cJSON_AddBoolToObject(root, "cancel_requested", true);
        cJSON_AddStringToObject(root, "request_id", rid.c_str());
        reply_json(c, 202, root);
        return;
    }
    if (path == "/api/relay") {
        if (method_is(hm, "POST")) {
            if (!write_ok(hm)) {
                reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
                return;
            }
            cJSON* body = cJSON_Parse(body_string(hm).c_str());
            std::string name = json_str(body, "name");
            int ch = static_cast<int>(cJSON_GetObjectItem(body, "channel")
                                          ? cJSON_GetObjectItem(body, "channel")->valuedouble
                                          : 1);
            std::string op = json_str(body, "op");
            cJSON_Delete(body);
            const RelayDeviceConfig* dev = nullptr;
            if (core) {
                for (const RelayDeviceConfig& d : core->config().relays) {
                    if (d.name == name) {
                        dev = &d;
                        break;
                    }
                }
            }
            if (!dev || !services) {
                reply_json_text(c, 404, "{\"ok\":false,\"error\":\"relay not found\"}");
                return;
            }
            std::string err;
            bool ok = false;
            if (op == "on") ok = services->relay_set(*dev, ch, true, &err);
            else if (op == "off") ok = services->relay_set(*dev, ch, false, &err);
            else if (op == "pulse") ok = services->relay_pulse_async(*dev, ch, 1.0);
            else err = "op must be on/off/pulse";
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", ok);
            cJSON_AddStringToObject(root, "error", err.c_str());
            reply_json(c, ok ? 200 : 400, root);
            return;
        }
        reply_json_text(c, 200, services ? services->relay_json() : "[]");
        return;
    }
    if (path == "/api/relay/devices") {
        if (method_is(hm, "POST")) {
            if (!write_ok(hm)) {
                reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
                return;
            }
            cJSON* body = cJSON_Parse(body_string(hm).c_str());
            if (!cJSON_IsObject(body)) {
                cJSON_Delete(body);
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"invalid json\"}");
                return;
            }
            const cJSON* dev = cJSON_GetObjectItemCaseSensitive(body, "device");
            if (!cJSON_IsObject(dev)) dev = body;
            std::string old_name = json_str(body, "old_name");
            std::string name = json_str(dev, "name");
            std::string port = json_str(dev, "windows_port");
            if (port.empty()) port = json_str(dev, "port");
            std::string linux_port = json_str(dev, "linux_port");
            int channels = json_int(dev, "channels", 2);
            int baud = json_int(dev, "baudrate", 9600);
            int delay = json_int(dev, "post_write_delay_ms", 0);
            bool enable = json_bool(dev, "enable", json_bool(dev, "enabled", true));
            bool io_inv = json_bool(dev, "io_inverted", false);
            bool pol_inv = json_bool(dev, "polarity_inverted", false);
            cJSON_Delete(body);
            if (name.empty() || port.empty() || channels < 1 || channels > 16 || baud < 1200 || baud > 115200 || delay < 0) {
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"设备名/端口不能为空，通道 1-16，波特率 1200-115200\"}");
                return;
            }
            std::string err;
            bool ok = patch_config([&](cJSON* root) -> bool {
                cJSON* devs = cJSON_GetObjectItemCaseSensitive(root, "relay_devices");
                if (!cJSON_IsArray(devs)) {
                    devs = cJSON_AddArrayToObject(root, "relay_devices");
                    if (!devs) {
                        err = "cannot create relay_devices";
                        return false;
                    }
                }
                cJSON* existing = nullptr;
                cJSON* it = nullptr;
                cJSON_ArrayForEach(it, devs) {
                    if (!cJSON_IsObject(it)) continue;
                    std::string n = json_str(it, "name");
                    if (!old_name.empty() && n == old_name) existing = it;
                    if (name == n && old_name != name) existing = it;
                }
                if (!old_name.empty() && !existing) {
                    err = "relay device not found";
                    return false;
                }
                if (old_name.empty() && existing) {
                    err = "relay device name already exists";
                    return false;
                }
                if (old_name.empty()) {
                    existing = cJSON_CreateObject();
                    cJSON_AddItemToArray(devs, existing);
                }
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "name");
                cJSON_AddStringToObject(existing, "name", name.c_str());
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "enable");
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "enabled");
                cJSON_AddBoolToObject(existing, "enable", enable);
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "windows_port");
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "port");
                cJSON_AddStringToObject(existing, "windows_port", port.c_str());
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "linux_port");
                cJSON_AddStringToObject(existing, "linux_port", linux_port.c_str());
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "channels");
                cJSON_AddNumberToObject(existing, "channels", channels);
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "baudrate");
                cJSON_AddNumberToObject(existing, "baudrate", baud);
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "io_inverted");
                cJSON_AddBoolToObject(existing, "io_inverted", io_inv);
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "post_write_delay_ms");
                cJSON_AddNumberToObject(existing, "post_write_delay_ms", delay);
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "polarity_inverted");
                cJSON_AddBoolToObject(existing, "polarity_inverted", pol_inv);
                if (!old_name.empty() && old_name != name) {
                    for (const char* key : {"boards", "ssh"}) {
                        cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, key);
                        cJSON* ch = nullptr;
                        if (!cJSON_IsArray(arr)) continue;
                        cJSON_ArrayForEach(ch, arr) {
                            cJSON* btns = cJSON_GetObjectItemCaseSensitive(ch, "buttons");
                            cJSON* btn = nullptr;
                            if (!cJSON_IsArray(btns)) continue;
                            cJSON_ArrayForEach(btn, btns) {
                                cJSON* rel = cJSON_GetObjectItemCaseSensitive(btn, "relay");
                                if (cJSON_IsString(rel) && rel->valuestring &&
                                    std::string(rel->valuestring) == old_name) {
                                    cJSON_DeleteItemFromObjectCaseSensitive(btn, "relay");
                                    cJSON_AddStringToObject(btn, "relay", name.c_str());
                                }
                            }
                        }
                    }
                }
                return true;
            }, &err);
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", ok);
            cJSON_AddStringToObject(root, "error", err.c_str());
            reply_json(c, ok ? 200 : 400, root);
            return;
        }
        reply_json_text(c, 200, relay_devices());
        return;
    }
    if (path == "/api/relay/devices/delete") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string name = json_str(body, "name");
        cJSON_Delete(body);
        std::string err;
        bool ok = patch_config([&](cJSON* root) -> bool {
            cJSON* devs = cJSON_GetObjectItemCaseSensitive(root, "relay_devices");
            if (!cJSON_IsArray(devs)) {
                err = "relay device not found";
                return false;
            }
            int idx = 0;
            cJSON* it = nullptr;
            cJSON_ArrayForEach(it, devs) {
                if (cJSON_IsObject(it) && json_str(it, "name") == name) break;
                ++idx;
            }
            if (!cJSON_IsObject(it)) {
                err = "relay device not found";
                return false;
            }
            cJSON* refs = cJSON_CreateArray();
            for (const char* key : {"boards", "ssh"}) {
                cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, key);
                cJSON* ch = nullptr;
                if (!cJSON_IsArray(arr)) continue;
                cJSON_ArrayForEach(ch, arr) {
                    cJSON* btns = cJSON_GetObjectItemCaseSensitive(ch, "buttons");
                    cJSON* btn = nullptr;
                    if (!cJSON_IsArray(btns)) continue;
                    cJSON_ArrayForEach(btn, btns) {
                        cJSON* rel = cJSON_GetObjectItemCaseSensitive(btn, "relay");
                        if (cJSON_IsString(rel) && rel->valuestring && std::string(rel->valuestring) == name) {
                            cJSON_AddItemToArray(refs, cJSON_CreateString(json_str(btn, "id").c_str()));
                        }
                    }
                }
            }
            if (cJSON_GetArraySize(refs) > 0) {
                cJSON* root2 = cJSON_CreateObject();
                cJSON_AddBoolToObject(root2, "ok", false);
                cJSON_AddStringToObject(root2, "error", "设备仍被板控按钮引用，请先修改这些按钮");
                cJSON_AddItemToObject(root2, "refs", refs);
                err = json_text(root2, false);
                return false;
            }
            cJSON_Delete(refs);
            cJSON_DeleteItemFromArray(devs, idx);
            return true;
        }, &err);
        if (!ok && err.rfind("{", 0) == 0) {
            reply_json_text(c, 409, err);
            return;
        }
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", ok);
        cJSON_AddStringToObject(root, "error", err.c_str());
        reply_json(c, ok ? 200 : 400, root);
        return;
    }
    if (path == "/services") {
        reply_json_text(c, 200, services ? services->state_json() : "{}");
        return;
    }
    if (path == "/api/files/download") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"GET required\"}");
            return;
        }
        std::string rel = query_var(hm, "path");
        std::filesystem::path p;
        std::string err;
        if (!safe_share_path(services, rel, &p, &err)) {
            reply_json_text(c, 400, "{\"ok\":false,\"error\":\"" + err + "\"}");
            return;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(p, ec)) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"file not found\"}");
            return;
        }
        std::uintmax_t sz = std::filesystem::file_size(p, ec);
        if (ec || sz > 64ull * 1024 * 1024) {
            reply_json_text(c, 413, "{\"ok\":false,\"error\":\"file too large (max 64MB)\"}");
            return;
        }
        std::ifstream in(p, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in && !data.empty()) {
            reply_json_text(c, 500, "{\"ok\":false,\"error\":\"read failed\"}");
            return;
        }
        reply_file_bytes(c, data, p.filename().u8string());
        return;
    }
    if (path == "/api/files") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"GET required\"}");
            return;
        }
        std::string rel = query_var(hm, "path");
        std::string err;
        std::filesystem::path p;
        if (!safe_share_path(services, rel, &p, &err)) {
            reply_json_text(c, 400, "{\"ok\":false,\"error\":\"" + err + "\"}");
            return;
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(p, ec)) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"directory not found\"}");
            return;
        }
        struct Row {
            std::string name, path, mtime;
            uintmax_t size = 0;
            bool dir = false;
        };
        std::vector<Row> rows;
        try {
            for (const auto& entry : std::filesystem::directory_iterator(
                     p, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (ec && !entry.exists()) continue;
                Row r;
                r.name = entry.path().filename().u8string();
                r.path = rel.empty() ? r.name : (rel + "/" + r.name);
                r.dir = entry.is_directory(ec);
                if (!r.dir) {
                    r.size = entry.file_size(ec);
                    if (ec) r.size = 0;
                }
                r.mtime = now_text(entry.last_write_time(ec));
                if (ec) r.mtime.clear();
                rows.push_back(std::move(r));
                if (rows.size() >= 2000) break;
            }
        } catch (const std::exception& e) {
            reply_json_text(c, 500, "{\"ok\":false,\"error\":\"list failed\"}");
            return;
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.dir != b.dir) return a.dir;
            return a.name < b.name;
        });
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "root", services->share_root().c_str());
        cJSON_AddStringToObject(root, "path", rel.c_str());
        std::filesystem::path parent = std::filesystem::u8path(rel).parent_path();
        cJSON_AddStringToObject(root, "parent", parent.generic_u8string().c_str());
        cJSON* items = cJSON_AddArrayToObject(root, "items");
        for (const Row& r : rows) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", r.name.c_str());
            cJSON_AddStringToObject(o, "path", r.path.c_str());
            cJSON_AddBoolToObject(o, "dir", r.dir);
            cJSON_AddNumberToObject(o, "size", static_cast<double>(r.size));
            cJSON_AddStringToObject(o, "mtime", r.mtime.c_str());
            cJSON_AddItemToArray(items, o);
        }
        reply_json(c, 200, root);
        return;
    }
    if (path == "/api/files/upload") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        if (hm->body.len > 16ull * 1024 * 1024) {
            reply_json_text(c, 413, "{\"ok\":false,\"error\":\"upload too large (max 16MB)\"}");
            return;
        }
        std::string dir = query_var(hm, "path");
        std::string name = query_var(hm, "name");
        if (name.empty() || name == "." || name == ".." ||
            name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
            reply_json_text(c, 400, "{\"ok\":false,\"error\":\"filename 非法\"}");
            return;
        }
        std::string rel = dir.empty() ? name : (dir + "/" + name);
        std::filesystem::path p;
        std::string err;
        if (!safe_share_path(services, rel, &p, &err)) {
            reply_json_text(c, 400, "{\"ok\":false,\"error\":\"" + err + "\"}");
            return;
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(p.parent_path(), ec)) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"目标目录不存在\"}");
            return;
        }
        if (std::filesystem::exists(p, ec)) {
            reply_json_text(c, 409, "{\"ok\":false,\"error\":\"同名文件已存在\"}");
            return;
        }
        std::ofstream out(p, std::ios::binary);
        if (!out) {
            reply_json_text(c, 500, "{\"ok\":false,\"error\":\"创建文件失败\"}");
            return;
        }
        std::string data = body_string(hm);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out) {
            reply_json_text(c, 500, "{\"ok\":false,\"error\":\"写入失败\"}");
            return;
        }
        reply_json_text(c, 200, "{\"ok\":true,\"path\":\"" + rel + "\"}");
        return;
    }
    if (path == "/api/files/delete") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string rel = json_str(body, "path");
        cJSON_Delete(body);
        std::filesystem::path p;
        std::string err;
        if (rel.empty() || !safe_share_path(services, rel, &p, &err)) {
            reply_json_text(c, 400, "{\"ok\":false,\"error\":\"" + (err.empty() ? "path 不能为空" : err) + "\"}");
            return;
        }
        std::error_code ec;
        std::filesystem::path root = std::filesystem::weakly_canonical(
            std::filesystem::u8path(services->share_root()), ec);
        if (!ec && std::filesystem::equivalent(root, p, ec)) {
            reply_json_text(c, 400, "{\"ok\":false,\"error\":\"不能删除共享根目录\"}");
            return;
        }
        if (!std::filesystem::exists(p, ec)) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"file not found\"}");
            return;
        }
        if (std::filesystem::is_directory(p, ec) && !std::filesystem::is_empty(p, ec)) {
            reply_json_text(c, 409, "{\"ok\":false,\"error\":\"目录非空，未删除\"}");
            return;
        }
        if (!std::filesystem::remove(p, ec)) {
            reply_json_text(c, 500, "{\"ok\":false,\"error\":\"删除失败\"}");
            return;
        }
        reply_json_text(c, 200, "{\"ok\":true}");
        return;
    }
    if (path == "/api/files/mkdir") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string dir = json_str(body, "path");
        std::string name = json_str(body, "name");
        cJSON_Delete(body);
        if (name.empty() || name == "." || name == ".." ||
            name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
            reply_json_text(c, 400, "{\"ok\":false,\"error\":\"目录名非法\"}");
            return;
        }
        std::string rel = dir.empty() ? name : (dir + "/" + name);
        std::filesystem::path p;
        std::string err;
        if (!safe_share_path(services, rel, &p, &err)) {
            reply_json_text(c, 400, "{\"ok\":false,\"error\":\"" + err + "\"}");
            return;
        }
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            reply_json_text(c, 409, "{\"ok\":false,\"error\":\"同名目录已存在\"}");
            return;
        }
        if (!std::filesystem::create_directory(p, ec)) {
            reply_json_text(c, 500, "{\"ok\":false,\"error\":\"创建目录失败\"}");
            return;
        }
        reply_json_text(c, 200, "{\"ok\":true,\"path\":\"" + rel + "\"}");
        return;
    }
    if (path == "/api/tftp/config" || path == "/api/nfs/config" || path == "/api/share/config") {
        const std::string key = path.find("tftp") != std::string::npos ? "tftp"
                              : path.find("nfs") != std::string::npos ? "nfs" : "share";
        if (method_is(hm, "POST")) {
            if (!write_ok(hm)) {
                reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
                return;
            }
            cJSON* body = cJSON_Parse(body_string(hm).c_str());
            if (!cJSON_IsObject(body)) {
                cJSON_Delete(body);
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"invalid json\"}");
                return;
            }
            bool enable = json_bool(body, "enable", false);
            int port = json_int(body, "port", key == "tftp" ? 69 : (key == "nfs" ? 2049 : 18082));
            std::string root = util::trim(json_str(body, "root"));
            std::string bind_ip = util::trim(json_str(body, "bind_ip"));
            std::string alias = util::trim(json_str(body, "alias"));
            int uid = json_int(body, "uid", 0);
            int gid = json_int(body, "gid", 0);
            bool allow_write = json_bool(body, "allow_write", true);
            cJSON_Delete(body);
            if ((key != "nfs" && (port < 1 || port > 65535)) || uid < 0 || uid > 65535 || gid < 0 || gid > 65535) {
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"端口或 UID/GID 超出范围\"}");
                return;
            }
            if (key == "nfs" && (!alias.empty() && alias[0] != '/')) {
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"NFS 别名必须以 / 开头\"}");
                return;
            }
            std::string err;
            bool ok = patch_config([&](cJSON* cfg_root) -> bool {
                cJSON_DeleteItemFromObjectCaseSensitive(cfg_root, key.c_str());
                cJSON* sec = cJSON_AddObjectToObject(cfg_root, key.c_str());
                if (!sec) { err = "cannot create service config"; return false; }
                cJSON_AddBoolToObject(sec, "enable", enable);
                if (key == "nfs") {
                    cJSON_AddStringToObject(sec, "alias", alias.empty() ? "/share" : alias.c_str());
                    cJSON_AddNumberToObject(sec, "uid", uid);
                    cJSON_AddNumberToObject(sec, "gid", gid);
                } else {
                    cJSON_AddNumberToObject(sec, "port", port);
                    if (key == "tftp") {
                        cJSON_AddStringToObject(sec, "bind_ip", bind_ip.c_str());
                        cJSON_AddBoolToObject(sec, "allow_write", allow_write);
                    }
                }
                cJSON_AddStringToObject(sec, "root", root.c_str());
                return true;
            }, &err);
            cJSON* out = cJSON_CreateObject();
            cJSON_AddBoolToObject(out, "ok", ok);
            cJSON_AddStringToObject(out, "error", err.c_str());
            reply_json(c, ok ? 200 : 400, out);
            return;
        }
        cJSON* root = cJSON_Parse(core ? core->config_json_text().c_str() : "{}");
        cJSON* sec = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, key.c_str()) : nullptr;
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddItemToObject(out, "config", cJSON_IsObject(sec) ? cJSON_Duplicate(sec, 1) : cJSON_CreateObject());
        cJSON_Delete(root);
        reply_json(c, 200, out);
        return;
    }
    if (path == "/service") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string name = json_str(body, "name");
        bool on = cJSON_IsTrue(cJSON_GetObjectItem(body, "on"));
        cJSON_Delete(body);
        if (!services || !core) {
            reply_json_text(c, 500, "{\"ok\":false,\"error\":\"services not ready\"}");
            return;
        }
        std::string err;
        bool ok = false;
        const AppConfig& cfg = core->config();
        if (name == "tftp") {
            if (on) ok = services->start_tftp(cfg.tftp, &err);
            else { services->stop_tftp(); ok = true; }
        } else if (name == "share") {
            if (on) ok = services->start_share(cfg.share, &err);
            else { services->stop_share(); ok = true; }
        } else if (name == "nfs") {
            if (on) ok = services->start_nfs(cfg.nfs, &err);
            else { services->stop_nfs(); ok = true; }
        } else if (name == "inbound") {
            if (on) ok = services->start_inbound(cfg.inbound, &err);
            else { services->stop_inbound(); ok = true; }
        } else if (name == "tcp-forward") {
            if (on) ok = services->start_tcp_forward(cfg.tcp_fwd, &err);
            else { services->stop_tcp_forward(); ok = true; }
        } else if (name == "tunnel") {
            if (on) ok = services->start_tunnel(cfg.tunnel, &err);
            else { services->stop_tunnel(); ok = true; }
        } else if (name == "discovery") {
            if (on) ok = services->start_discovery(cfg.discovery, cfg, &err);
            else { services->stop_discovery(); ok = true; }
        } else {
            err = "unknown service";
        }
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", ok);
        cJSON_AddStringToObject(root, "error", err.c_str());
        reply_json(c, ok ? 200 : 400, root);
        return;
    }
    if (path == "/api/tcp-forward/config") {
        if (method_is(hm, "POST")) {
            if (!write_ok(hm)) {
                reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
                return;
            }
            cJSON* body = cJSON_Parse(body_string(hm).c_str());
            if (!cJSON_IsObject(body)) {
                cJSON_Delete(body);
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"invalid json\"}");
                return;
            }
            bool enable = json_bool(body, "enable", false);
            std::string listen_ip = json_str(body, "listen_ip");
            int port_base = json_int(body, "port_base", 22000);
            int target_port = json_int(body, "target_port", 22);
            std::string board_cidr = json_str(body, "board_cidr");
            cJSON_Delete(body);
            if (port_base < 1 || port_base > 65535 || target_port < 1 || target_port > 65535) {
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"端口必须在 1-65535\"}");
                return;
            }
            std::string err;
            bool ok = patch_config([&](cJSON* root) -> bool {
                cJSON_DeleteItemFromObjectCaseSensitive(root, "tcp_forward");
                cJSON* sec = cJSON_AddObjectToObject(root, "tcp_forward");
                if (!sec) {
                    err = "cannot create tcp_forward";
                    return false;
                }
                cJSON_AddBoolToObject(sec, "enable", enable);
                cJSON_AddStringToObject(sec, "listen_ip", listen_ip.c_str());
                cJSON_AddNumberToObject(sec, "port_base", port_base);
                cJSON_AddNumberToObject(sec, "target_port", target_port);
                cJSON_AddStringToObject(sec, "board_cidr", board_cidr.c_str());
                return true;
            }, &err);
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", ok);
            cJSON_AddStringToObject(root, "error", err.c_str());
            reply_json(c, ok ? 200 : 400, root);
            return;
        }
        cJSON* root = cJSON_Parse(core ? core->config_json_text().c_str() : "{}");
        cJSON* sec = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "tcp_forward") : nullptr;
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddItemToObject(out, "config", cJSON_IsObject(sec) ? cJSON_Duplicate(sec, 1) : cJSON_CreateObject());
        cJSON_Delete(root);
        reply_json(c, 200, out);
        return;
    }
    if (path == "/api/tunnel/config") {
        if (method_is(hm, "POST")) {
            if (!write_ok(hm)) {
                reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
                return;
            }
            cJSON* body = cJSON_Parse(body_string(hm).c_str());
            if (!cJSON_IsObject(body)) {
                cJSON_Delete(body);
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"invalid json\"}");
                return;
            }
            bool enable = json_bool(body, "enable", false);
            bool map_http = json_bool(body, "map_http", true);
            bool map_ssh = json_bool(body, "map_inbound_ssh", false);
            std::string exe_path = json_str(body, "exe_path");
            std::string server_addr = json_str(body, "server_addr");
            int server_port = json_int(body, "server_port", 7000);
            std::string token = json_str(body, "token");
            int local_port = json_int(body, "local_port", 18081);
            int remote_port = json_int(body, "remote_port", 0);
            int ssh_local_port = json_int(body, "ssh_local_port", 2222);
            int ssh_remote_port = json_int(body, "ssh_remote_port", 0);
            cJSON_Delete(body);
            if (server_port < 1 || server_port > 65535 || local_port < 1 || local_port > 65535 ||
                (remote_port < 0 || remote_port > 65535) || (ssh_local_port < 1 || ssh_local_port > 65535) ||
                (ssh_remote_port < 0 || ssh_remote_port > 65535)) {
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"端口范围 1-65535\"}");
                return;
            }
            if (enable && server_addr.empty()) {
                reply_json_text(c, 400, "{\"ok\":false,\"error\":\"启用隧道时服务地址不能为空\"}");
                return;
            }
            std::string err;
            bool ok = patch_config([&](cJSON* root) -> bool {
                cJSON_DeleteItemFromObjectCaseSensitive(root, "tunnel");
                cJSON* sec = cJSON_AddObjectToObject(root, "tunnel");
                if (!sec) {
                    err = "cannot create tunnel";
                    return false;
                }
                cJSON_AddBoolToObject(sec, "enable", enable);
                cJSON_AddBoolToObject(sec, "map_http", map_http);
                cJSON_AddBoolToObject(sec, "map_inbound_ssh", map_ssh);
                cJSON_AddStringToObject(sec, "exe_path", exe_path.c_str());
                cJSON_AddStringToObject(sec, "server_addr", server_addr.c_str());
                cJSON_AddNumberToObject(sec, "server_port", server_port);
                cJSON_AddStringToObject(sec, "token", token.c_str());
                cJSON_AddNumberToObject(sec, "local_port", local_port);
                cJSON_AddNumberToObject(sec, "remote_port", remote_port);
                cJSON_AddNumberToObject(sec, "ssh_local_port", ssh_local_port);
                cJSON_AddNumberToObject(sec, "ssh_remote_port", ssh_remote_port);
                return true;
            }, &err);
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", ok);
            cJSON_AddStringToObject(root, "error", err.c_str());
            reply_json(c, ok ? 200 : 400, root);
            return;
        }
        cJSON* root = cJSON_Parse(core ? core->config_json_text().c_str() : "{}");
        cJSON* sec = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "tunnel") : nullptr;
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddItemToObject(out, "config", cJSON_IsObject(sec) ? cJSON_Duplicate(sec, 1) : cJSON_CreateObject());
        cJSON_Delete(root);
        reply_json(c, 200, out);
        return;
    }
    if (path == "/api/tcp-forward" || path == "/api/tunnel" || path == "/api/discovery") {
        const char* key = path == "/api/tcp-forward" ? "tcp_forward"
                          : path == "/api/tunnel"      ? "tunnel"
                                                       : "discovery";
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        bool running = false;
        if (services) {
            cJSON* all = cJSON_Parse(services->state_json().c_str());
            cJSON* sec = all ? cJSON_GetObjectItemCaseSensitive(all, key) : nullptr;
            if (cJSON_IsObject(sec)) {
                running = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sec, "on"));
                cJSON* dup = cJSON_Duplicate(sec, 1);
                if (dup) cJSON_AddItemToObject(root, "status", dup);
            }
            cJSON_Delete(all);
        }
        cJSON_AddBoolToObject(root, "running", running);
        reply_json(c, 200, root);
        return;
    }
    if (path == "/help") {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", new_request_id()));
            return;
        }
        reply_text(c, 200, agent::help_text(core, port, ui_info()), "text/plain; charset=utf-8");
        return;
    }
    if (path.rfind("/help/", 0) == 0) {
        if (!method_is(hm, "GET")) {
            reply_json_text(c, 405, agent::error_json("method_not_allowed", "GET required", new_request_id()));
            return;
        }
        char decoded[512] = "";
        int n = mg_url_decode(path.c_str() + 6, path.size() - 6, decoded, sizeof(decoded), 0);
        std::string ch = n > 0 ? std::string(decoded, static_cast<std::size_t>(n)) : std::string();
        if (ch.empty() || !core || !core->find_shared(ch)) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
            return;
        }
        reply_text(c, 200, agent::channel_help(core, ch, port), "text/plain; charset=utf-8");
        return;
    }

    // 10 个页面
    for (const auto& n : kNav) {
        if (path == std::string("/") + n[0]) {
            reply_text(c, 200, page(n[0]), "text/html; charset=utf-8");
            return;
        }
    }
    reply_text(c, 404, "not found", "text/plain; charset=utf-8");
}

void WebConsole::Impl::handle_ws(mg_connection* c, int ev, void* ev_data) {
    (void)ev_data;
    if (ev == MG_EV_CLOSE) {
        drop_pending(c);
        std::lock_guard<std::mutex> g(mu);
        ws.erase(c);
        return;
    }
    auto it = ws.find(c);
    if (it == ws.end()) return;
    WsState& s = it->second;
    if (ev == MG_EV_WS_OPEN) {
        auto b = bridge(s.channel);
        if (!b) return;
        uint64_t e = 0, seen = 0;
        std::string tail = b->read_all(&e, &seen);
        if (tail.size() > 8192) tail = tail.substr(tail.size() - 8192);
        if (!tail.empty()) mg_ws_send(c, tail.data(), tail.size(), WEBSOCKET_OP_BINARY);
        s.epoch = e;
        s.seen = seen;
    } else if (ev == MG_EV_POLL) {
        push_ws(c, s);
    }
}

void WebConsole::Impl::start_thread() {
    worker = std::thread([this]() { worker_loop(); });
    thread = std::thread([this]() {
        while (!stop.load()) {
            mg_mgr_poll(mgr, 10);
            cleanup_job();
        }
    });
}

WebConsole::WebConsole() : impl_(new Impl()) {}
WebConsole::~WebConsole() { stop(); delete impl_; }

void WebConsole::set_notify(void* hwnd) {
    if (impl_) impl_->notify = hwnd;
}

bool WebConsole::start(AppCore* core, ServiceManager* services, int port,
                       const std::string& token, std::string* err) {
    if (impl_->running.load()) return true;
    if (port <= 0 || port > 65535) {
        if (err) *err = "Web 端口越界";
        return false;
    }
    impl_->core = core;
    impl_->services = services;
    impl_->port = port;
    impl_->token = token;
    impl_->error.clear();
    impl_->stop.store(false);
    impl_->mgr = new mg_mgr();
    mg_mgr_init(impl_->mgr);
    mg_wakeup_init(impl_->mgr);
    char url[160];
    std::snprintf(url, sizeof(url), "http://0.0.0.0:%d", port);
    mg_connection* lc = mg_http_listen(impl_->mgr, url, [](mg_connection* c, int ev, void* ev_data) {
        Impl* self = static_cast<Impl*>(c->fn_data);
        if (!self) return;
        if (ev == MG_EV_HTTP_MSG) self->handle_http(c, static_cast<mg_http_message*>(ev_data));
        else if (ev == MG_EV_WAKEUP) self->on_wakeup(static_cast<mg_str*>(ev_data));
        else if (ev == MG_EV_WS_OPEN || ev == MG_EV_WS_MSG || ev == MG_EV_WS_CTL ||
                 ev == MG_EV_CLOSE || ev == MG_EV_POLL) self->handle_ws(c, ev, ev_data);
    }, impl_);
    if (!lc) {
        if (err) *err = "Web 端口 " + std::to_string(port) + " 被占用或无法监听";
        impl_->error = *err;
        mg_mgr_free(impl_->mgr);
        delete impl_->mgr;
        impl_->mgr = nullptr;
        return false;
    }

    // WS 升级路由需要先拿到 http message；在 HTTP handler 里登记 state 再升级。
    // mg_http_listen 的 c->fn_data 是 impl_；handle_http 通过 query 判断 /ws/log。
    impl_->wake_id = lc->id;
    impl_->running.store(true);
    impl_->start_thread();
    if (err) err->clear();
    return true;
}

void WebConsole::stop() {
    if (!impl_) return;
    impl_->stop.store(true);
    impl_->qcv.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->mgr) {
        mg_mgr_free(impl_->mgr);
        delete impl_->mgr;
        impl_->mgr = nullptr;
    }
    impl_->wake_id = 0;
    impl_->running.store(false);
    std::lock_guard<std::mutex> g(impl_->mu);
    impl_->ws.clear();
    impl_->pending_conn_job.clear();
    impl_->pending_job_conn.clear();
    impl_->jobs.clear();
}

bool WebConsole::running() const { return impl_ && impl_->running.load(); }
int WebConsole::port() const { return impl_ ? impl_->port : 0; }
std::string WebConsole::error() const { return impl_ ? impl_->error : std::string(); }
std::string WebConsole::token() const { return impl_ ? impl_->token : std::string(); }
void WebConsole::configure(int port, const std::string& token) {
    if (!impl_) return;
    impl_->port = port;
    impl_->token = token;
}
std::string WebConsole::status_json() const {
    if (!impl_) return "{}";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "on", impl_->running.load());
    cJSON_AddNumberToObject(root, "port", impl_->port);
    cJSON_AddBoolToObject(root, "readonly", impl_->token.empty());
    cJSON_AddStringToObject(root, "error", impl_->error.c_str());
    return json_text(root);
}

}  // namespace sb3



