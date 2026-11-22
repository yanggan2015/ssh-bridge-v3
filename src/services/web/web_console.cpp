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
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/app_core.h"
#include "core/bridge/bridge.h"
#include "core/util/util.h"
#include "services/board_actions/board_action_runner.h"
#include "services/service_manager.h"
#include "ui/ui_messages.h"

namespace sb3 {
namespace {

const char* kNav[][2] = {
    {"dashboard", "仪表盘"}, {"config", "配置"}, {"actions", "板控动作"},
    {"button-script", "快捷/脚本"}, {"relay", "继电器"}, {"tftp", "TFTP"},
    {"nfs", "NFS"}, {"share", "共享目录"}, {"tcp-forward", "TCP 转发"},
    {"tunnel", "隧道"},
};

std::string page_title(const std::string& page) {
    for (const auto& n : kNav) if (page == n[0]) return n[1];
    return "ssh-bridge";
}

std::string nav_html(const std::string& page) {
    std::string out;
    for (const auto& n : kNav) {
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
                       "<aside><div class=\"brand\">ssh-bridge<small>v3 · Web 控制台</small></div><nav>" +
                       nav_html(page) + "</nav><div class=\"aside-foot\">S9 · HTTP + WS<br>read-only 无 token</div></aside>"
                       "<main><header><div><h1>" + page_title(page) + "</h1><div class=\"sub\">同一份通道 ring / cmdq，不另开设备连接。</div></div>"
                       "<div class=\"row\"><input id=\"tokenBox\" type=\"password\" placeholder=\"Bearer token\"><button id=\"saveToken\">保存 token</button></div></header>" +
                       body + "<script src=\"/static/app.js\"></script><script>" + script + "</script></main></div></body></html>";
    return html;
}

std::string json_str(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? std::string(v->valuestring) : std::string();
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

struct WsState {
    std::string channel;
    uint64_t epoch = 0;
    uint64_t seen = 0;
};

struct Job {
    int id = 0;
    bool done = false;
    bool ok = false;
    std::string message;
    std::string output;
};

struct ActionTask {
    std::shared_ptr<Job> job;
    AppConfig cfg;
    std::string channel;
    BoardActionConfig action;
    std::shared_ptr<IBridge> bridge;
};

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
    std::deque<ActionTask> queue;
    unsigned long wake_id = 0;
    std::map<mg_connection*, WsState> ws;
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

    std::string status() const;
    std::string actions() const;
    std::string quick() const;
    std::string jobs_json(int id) const;
    std::string page(const std::string& name) const;
    void handle_http(mg_connection* c, mg_http_message* hm);
    void handle_ws(mg_connection* c, int ev, void* ev_data);
    void push_ws(mg_connection* c, WsState& s);
    void cleanup_job();
    int spawn_action(const std::string& channel, const std::string& id);
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
                if (!a.enable) continue;
                cJSON* o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "channel", e.name.c_str());
                cJSON_AddStringToObject(o, "id", a.id.c_str());
                cJSON_AddStringToObject(o, "label", a.label.c_str());
                cJSON_AddStringToObject(o, "kind", a.backend.c_str());
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
            for (const QuickCommandConfig& q : *qs) {
                cJSON* o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "channel", e.name.c_str());
                cJSON_AddStringToObject(o, "label", q.label.c_str());
                cJSON_AddStringToObject(o, "command", q.command.c_str());
                cJSON_AddItemToArray(items, o);
            }
        }
    }
    return json_text(root);
}

std::string WebConsole::Impl::jobs_json(int id) const {
    std::lock_guard<std::mutex> g(mu);
    auto it = jobs.find(id);
    if (it == jobs.end()) return "{\"ok\":false,\"error\":\"job not found\"}";
    const auto& j = it->second;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", j->id);
    cJSON_AddBoolToObject(root, "done", j->done);
    cJSON_AddBoolToObject(root, "ok", j->ok);
    cJSON_AddStringToObject(root, "message", j->message.c_str());
    cJSON_AddStringToObject(root, "output", j->output.c_str());
    return json_text(root);
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

int WebConsole::Impl::spawn_action(const std::string& channel, const std::string& id) {
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

    ActionTask task;
    task.job = std::make_shared<Job>();
    task.cfg = core->config();
    task.channel = channel;
    task.action = *found;
    task.bridge = core->find_shared(channel);
    int job_id = 0;
    {
        std::lock_guard<std::mutex> qg(qmu);
        if (queue.size() >= 64) return -1;
        {
            std::lock_guard<std::mutex> g(mu);
            task.job->id = next_job++;
            jobs[task.job->id] = task.job;
            job_id = task.job->id;
        }
        queue.push_back(std::move(task));
    }
    qcv.notify_one();
    return job_id;
}

void WebConsole::Impl::worker_loop() {
    for (;;) {
        ActionTask task;
        {
            std::unique_lock<std::mutex> lk(qmu);
            qcv.wait(lk, [this]() { return stop.load() || !queue.empty(); });
            if (stop.load()) return;
            task = std::move(queue.front());
            queue.pop_front();
        }
        BoardActionResult r = run_board_action(
            task.cfg, task.channel, task.action, task.bridge.get(),
            [this](const std::string& port) { return core && core->port_in_use(port); }, 0, {});
        {
            std::lock_guard<std::mutex> g(mu);
            task.job->done = true;
            task.job->ok = r.ok;
            task.job->message = r.message;
            task.job->output = r.output;
        }
        // 结果写完后叫醒 poll 线程：由它清理旧 job，HTTP /job 仍可随时读取。
        if (mgr && wake_id) mg_wakeup(mgr, wake_id, "job", 3);
    }
}

void WebConsole::Impl::cleanup_job() {
    std::lock_guard<std::mutex> g(mu);
    for (auto it = jobs.begin(); it != jobs.end();) {
        if (it->second->done && it->first + 50 < next_job) it = jobs.erase(it);
        else ++it;
    }
}

void WebConsole::Impl::handle_http(mg_connection* c, mg_http_message* hm) {
    if (!hm) return;
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
        if (inm && std::string(inm->buf, inm->len) == "\"sb3-s9-css\"") {
            mg_http_reply(c, 304, "ETag: \"sb3-s9-css\"\r\nCache-Control: max-age=3600\r\n", "");
            return;
        }
        mg_http_reply(c, 200,
                      "Content-Type: text/css; charset=utf-8\r\nETag: \"sb3-s9-css\"\r\n"
                      "Cache-Control: max-age=3600\r\nAccess-Control-Allow-Origin: *\r\n",
                      "%s", web_pages::kCss);
        return;
    }
    if (path == "/static/app.js") {
        mg_str* inm = mg_http_get_header(hm, "If-None-Match");
        if (inm && std::string(inm->buf, inm->len) == "\"sb3-s9-js\"") {
            mg_http_reply(c, 304, "ETag: \"sb3-s9-js\"\r\nCache-Control: max-age=3600\r\n", "");
            return;
        }
        mg_http_reply(c, 200,
                      "Content-Type: application/javascript; charset=utf-8\r\n"
                      "ETag: \"sb3-s9-js\"\r\nCache-Control: max-age=3600\r\n"
                      "Access-Control-Allow-Origin: *\r\n",
                      "%s", web_pages::kCommonJs);
        return;
    }
    if (path == "/health") {
        reply_json_text(c, 200, "{\"ok\":true,\"service\":\"ssh-bridge-web\",\"s9\":true}");
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
        std::string ch = query_var(hm, "channel");
        int chars = std::max(1, std::min(65536, std::atoi(query_var(hm, "chars", "8192").c_str())));
        auto b = bridge(ch);
        if (!b) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
            return;
        }
        uint64_t e = 0, s = 0;
        std::string all = b->read_all(&e, &s);
        if (static_cast<int>(all.size()) > chars) all = all.substr(all.size() - chars);
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "channel", ch.c_str());
        cJSON_AddStringToObject(root, "text", all.c_str());
        cJSON_AddNumberToObject(root, "chars", static_cast<double>(all.size()));
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
        cJSON_Delete(body);
        auto b = bridge(ch);
        if (!b) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
            return;
        }
        b->write(data.data(), data.size());
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "bytes", static_cast<double>(data.size()));
        reply_json(c, 200, root);
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
    if (path == "/api/actions") {
        reply_json_text(c, 200, actions());
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
        cJSON_Delete(body);
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
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"action not found\"}");
            return;
        }
        int job = spawn_action(ch, id);
        if (job < 0) {
            reply_json_text(c, 429, "{\"ok\":false,\"error\":\"action queue full\"}");
            return;
        }
        if (job == 0) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"action not found\"}");
            return;
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"job\":%d}", job);
        reply_json_text(c, 202, buf);
        return;
    }
    if (path == "/job") {
        int id = std::atoi(query_var(hm, "id", "0").c_str());
        reply_json_text(c, 200, jobs_json(id));
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
    if (path == "/services") {
        reply_json_text(c, 200, services ? services->state_json() : "{}");
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
        std::string help = "ssh-bridge Web 控制台\n\n"
                           "读：GET /status /log /actions /quick /relay /services\n"
                           "写：POST /write /action /quick /relay /service，需 Authorization: Bearer <token>\n"
                           "WS：/ws/log?channel=<通道名>，服务端只发 BINARY 帧。\n";
        reply_text(c, 200, help, "text/plain; charset=utf-8");
        return;
    }
    if (path.rfind("/help/", 0) == 0) {
        std::string ch = path.substr(6);
        auto b = bridge(ch);
        if (!b) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
            return;
        }
        reply_text(c, 200, "通道 " + ch + "\n状态：" + (b->running() ? "在线" : "离线") +
                                "\n读写：/log?channel=" + ch + "，/write 需要 token。\n",
                   "text/plain; charset=utf-8");
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
        else if (ev == MG_EV_WAKEUP) self->cleanup_job();
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



