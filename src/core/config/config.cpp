// config.cpp — JSON 读写（cJSON）。UTF-8 落盘，写临时文件后原子替换。

#include "core/config/config.h"

#include <windows.h>

#include <cjson/cJSON.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

#include "core/util/util.h"

namespace sb3 {
namespace {

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string from_wide(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

bool file_exists(const std::string& path) {
    std::wstring w = to_wide(path);
    DWORD attr = GetFileAttributesW(w.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string cwd() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), buf);
    if (n == 0) return std::string(".");
    return from_wide(std::wstring(buf, n));
}

std::string home_dir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf,
                                      static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return std::string();
    return from_wide(std::wstring(buf, n));
}

bool read_text(const std::string& path, std::string* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->assign(size > 0 ? static_cast<std::size_t>(size) : 0, '\0');
    if (size > 0) {
        std::size_t got = std::fread(out->data(), 1, static_cast<std::size_t>(size), f);
        out->resize(got);
    }
    std::fclose(f);
    return true;
}

bool write_text(const std::string& path, const std::string& data, std::string* err) {
    std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        if (err) *err = "cannot open " + tmp;
        return false;
    }
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!ok) {
        if (err) *err = "write failed: " + tmp;
        return false;
    }
    if (!MoveFileExW(to_wide(tmp).c_str(), to_wide(path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (err) *err = "cannot replace " + path;
        return false;
    }
    return true;
}

const char* jstr(const cJSON* obj, const char* key, const char* fallback) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
    return fallback;
}

int jint(const cJSON* obj, const char* key, int fallback) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) return std::atoi(v->valuestring);
    return fallback;
}

bool jbool(const cJSON* obj, const char* key, bool fallback) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? true : false;
    if (cJSON_IsNumber(v)) return v->valueint != 0;
    if (cJSON_IsString(v)) return util::to_bool(v->valuestring, fallback);
    return fallback;
}

bool jhas(const cJSON* obj, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(obj, key) != nullptr;
}

double jdouble(const cJSON* obj, const char* key, double fallback) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0])
        return std::atof(v->valuestring);
    return fallback;
}

void load_button_obj(const cJSON* o, BoardActionConfig* b) {
    b->id = jstr(o, "id", "");
    b->label = jstr(o, "label", b->id.c_str());
    b->enable = jbool(o, "enable", true);
    b->backend = jstr(o, "backend", "");
    b->run = jstr(o, "run", "channel");
    b->commands.clear();
    const cJSON* cmds = cJSON_GetObjectItemCaseSensitive(o, "commands");
    if (cJSON_IsArray(cmds)) {
        const cJSON* c = nullptr;
        cJSON_ArrayForEach(c, cmds) {
            if (cJSON_IsString(c) && c->valuestring) b->commands.push_back(c->valuestring);
        }
    } else if (cJSON_IsString(cmds) && cmds->valuestring) {
        b->commands.push_back(cmds->valuestring);
    }
    b->delay_ms = jint(o, "delay_ms", 200);
    b->relay = jstr(o, "relay", "");
    b->channel = jint(o, "channel", 1);
    b->mode = jstr(o, "mode", "pulse");
    b->hold_seconds = jdouble(o, "hold_seconds", 1.0);
    b->polarity_inverted = jbool(o, "polarity_inverted", false);
    b->script = jstr(o, "script", "");
    if (b->backend.empty()) {
        if (!b->relay.empty()) b->backend = "relay";
        else if (!b->script.empty()) b->backend = "script";
        else b->backend = "commands";
    }
    if (b->id.empty()) return;
    if (b->label.empty()) b->label = b->id;
}

void load_buttons(const cJSON* o, std::vector<BoardActionConfig>* out) {    const cJSON* buttons = cJSON_GetObjectItemCaseSensitive(o, "buttons");
    if (cJSON_IsArray(buttons)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, buttons) {
            if (!cJSON_IsObject(it)) continue;
            BoardActionConfig b;
            load_button_obj(it, &b);
            if (!b.id.empty()) out->push_back(b);
        }
    }
    // v2 兼容：relay_actions / actions 分别映射到 relay / commands
    const cJSON* legacyRelay = cJSON_GetObjectItemCaseSensitive(o, "relay_actions");
    if (cJSON_IsArray(legacyRelay)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, legacyRelay) {
            if (!cJSON_IsObject(it)) continue;
            BoardActionConfig b;
            load_button_obj(it, &b);
            b.backend = "relay";
            if (!b.id.empty()) out->push_back(b);
        }
    }
    const cJSON* legacyAct = cJSON_GetObjectItemCaseSensitive(o, "actions");
    if (cJSON_IsArray(legacyAct)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, legacyAct) {
            if (!cJSON_IsObject(it)) continue;
            BoardActionConfig b;
            load_button_obj(it, &b);
            b.backend = "commands";
            if (!b.id.empty()) out->push_back(b);
        }
    }
}

void load_quick_commands(const cJSON* o, std::vector<QuickCommandConfig>* out) {
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(o, "quick_commands");
    if (!cJSON_IsArray(arr)) arr = cJSON_GetObjectItemCaseSensitive(o, "quick_buttons");
    if (!cJSON_IsArray(arr)) return;
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsObject(it)) continue;
        QuickCommandConfig q;
        q.label = jstr(it, "label", jstr(it, "name", ""));
        q.command = jstr(it, "command", jstr(it, "cmd", ""));
        if (q.command.empty()) continue;
        if (q.label.empty()) q.label = q.command;
        out->push_back(q);
    }
}

cJSON* quick_commands_to_json(const std::vector<QuickCommandConfig>& items) {
    cJSON* arr = cJSON_CreateArray();
    for (const QuickCommandConfig& q : items) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "label", q.label.c_str());
        cJSON_AddStringToObject(o, "command", q.command.c_str());
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

cJSON* button_to_json(const BoardActionConfig& b) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", b.id.c_str());
    cJSON_AddStringToObject(o, "label", b.label.c_str());
    cJSON_AddBoolToObject(o, "enable", b.enable);
    cJSON_AddStringToObject(o, "backend", b.backend.c_str());
    if (b.backend == "relay") {
        cJSON_AddStringToObject(o, "relay", b.relay.c_str());
        cJSON_AddNumberToObject(o, "channel", b.channel);
        cJSON_AddStringToObject(o, "mode", b.mode.c_str());
        cJSON_AddNumberToObject(o, "hold_seconds", b.hold_seconds);
        cJSON_AddBoolToObject(o, "polarity_inverted", b.polarity_inverted);
    } else if (b.backend == "script") {
        cJSON_AddStringToObject(o, "script", b.script.c_str());
    } else {
        cJSON_AddStringToObject(o, "run", b.run.c_str());
        cJSON* arr = cJSON_AddArrayToObject(o, "commands");
        for (const std::string& c : b.commands) cJSON_AddItemToArray(arr, cJSON_CreateString(c.c_str()));
        cJSON_AddNumberToObject(o, "delay_ms", b.delay_ms);
    }
    return o;
}

void load_default(const cJSON* root, DefaultConfig* def) {
    const cJSON* o = cJSON_GetObjectItemCaseSensitive(root, "default");
    if (!cJSON_IsObject(o)) return;
    def->username = jstr(o, "username", def->username.c_str());
    if (jhas(o, "password")) def->password = jstr(o, "password", "");
    int buf = jint(o, "buffer_size", static_cast<int>(def->buffer_size));
    def->buffer_size = buf > 0 ? static_cast<std::uint32_t>(buf) : def->buffer_size;
    def->log_enabled = jbool(o, "log_enabled", def->log_enabled);
    def->work_dir = jstr(o, "work_dir", "");
    def->log_dir = jstr(o, "log_dir", "");
}

void load_board(const cJSON* o, const char* fallback_name, BoardConfig* b) {
    b->name = jstr(o, "name", fallback_name ? fallback_name : "");
    b->serial_device = jstr(o, "serial_device", jstr(o, "com_port", ""));
    b->baudrate = jint(o, "baudrate", 115200);
    b->databits = jint(o, "databits", 8);
    // v2 的写法是 "none"/"even"/"odd"/"mark"/"space"，3.0 内部用单字符
    std::string parity = jstr(o, "parity", "N");
    if (parity.size() > 1) {
        std::string p;
        for (char c : parity) p.push_back(static_cast<char>(std::tolower(c)));
        if      (p == "none")  parity = "N";
        else if (p == "even")  parity = "E";
        else if (p == "odd")   parity = "O";
        else if (p == "mark")  parity = "M";
        else if (p == "space") parity = "S";
    }
    b->parity = parity;
    // stopbits 在 v2 里是字符串 "1" / "1.5" / "2"，3.0 用 ×10 的整数
    b->stopbits_x10 = jint(o, "stopbits_x10", 0);
    if (b->stopbits_x10 <= 0) {
        int sb = jint(o, "stopbits", 1);
        b->stopbits_x10 = (sb == 15 || sb == 2) ? (sb == 15 ? 15 : 20) : 10;
        const cJSON* sv = cJSON_GetObjectItemCaseSensitive(o, "stopbits");
        if (cJSON_IsString(sv) && sv->valuestring && std::strcmp(sv->valuestring, "1.5") == 0)
            b->stopbits_x10 = 15;
    }
    b->flowcontrol = jstr(o, "flowcontrol", "none");
    b->dtr = jbool(o, "dtr", true);
    b->rts = jbool(o, "rts", true);
    b->line_ending = jstr(o, "line_ending", "crlf");
    b->encoding = jstr(o, "encoding", "utf-8");
    b->auto_open = jbool(o, "auto_open", false);
    b->timestamp = jbool(o, "timestamp", true);
    b->buffer_size = static_cast<std::uint32_t>(jint(o, "buffer_size", 0));
    b->log_enabled = jbool(o, "log_enabled", b->log_enabled);
    b->log_dir = jstr(o, "log_dir", "");
    b->password_present = jhas(o, "password");
    b->password = jstr(o, "password", "");
    b->username = jstr(o, "username", "root");
    load_buttons(o, &b->buttons);
    load_quick_commands(o, &b->quick_commands);
}

void load_ssh(const cJSON* o, SSHLinkConfig* s) {
    s->name = jstr(o, "name", "");
    s->host = jstr(o, "target_host", jstr(o, "host", ""));
    s->port = jint(o, "target_port", jint(o, "port", 22));
    s->username = jstr(o, "target_username", jstr(o, "username", ""));
    s->password_present = jhas(o, "target_password") || jhas(o, "password");
    s->password = jstr(o, "target_password", jstr(o, "password", ""));
    s->term = jstr(o, "term", "xterm-256color");
    s->width = jint(o, "width", 120);
    s->height = jint(o, "height", 32);
    s->connect_timeout_ms = jint(o, "connect_timeout_ms", jint(o, "connect_timeout", 5000));
    s->keepalive_s = jint(o, "keepalive", 3);
    s->buffer_size = static_cast<std::uint32_t>(jint(o, "buffer_size", 0));
    s->log_enabled = jbool(o, "log_enabled", true);
    s->log_dir = jstr(o, "log_dir", "");
    load_buttons(o, &s->buttons);
    load_quick_commands(o, &s->quick_commands);
}

void load_root(const cJSON* root, AppConfig* out) {
    load_default(root, &out->def);
    // boards 支持两种写法：3.0 的数组，以及 v2 的 {名字: {...}} 映射（真实测试配置就是后者）
    const cJSON* blk = cJSON_GetObjectItemCaseSensitive(root, "boards");
    if (cJSON_IsArray(blk)) {
        int i = 0;
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, blk) {
            if (!cJSON_IsObject(it)) { ++i; continue; }
            if (jhas(it, "enable") && !jbool(it, "enable", true)) { ++i; continue; }
            BoardConfig b;
            load_board(it, nullptr, &b);
            if (b.name.empty()) b.name = "board" + std::to_string(i + 1);
            out->boards.push_back(b);
            ++i;
        }
    } else if (cJSON_IsObject(blk)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, blk) {
            if (!cJSON_IsObject(it)) continue;
            if (jhas(it, "enable") && !jbool(it, "enable", true)) continue;
            BoardConfig b;
            load_board(it, it->string, &b);
            out->boards.push_back(b);
        }
    }
    const cJSON* ssh = cJSON_GetObjectItemCaseSensitive(root, "ssh");
    if (cJSON_IsArray(ssh)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, ssh) {
            if (!cJSON_IsObject(it)) continue;
            if (jhas(it, "enable") && !jbool(it, "enable", true)) continue;
            SSHLinkConfig s;
            load_ssh(it, &s);
            out->links.push_back(s);
        }
    } else if (cJSON_IsObject(ssh)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, ssh) {
            if (!cJSON_IsObject(it)) continue;
            if (jhas(it, "enable") && !jbool(it, "enable", true)) continue;
            SSHLinkConfig s;
            load_ssh(it, &s);
            if (s.name.empty() && it->string) s.name = it->string;
            out->links.push_back(s);
        }
    }
    const cJSON* inb = cJSON_GetObjectItemCaseSensitive(root, "inbound_ssh");
    if (cJSON_IsObject(inb)) {
        out->inbound.enabled = jbool(inb, "enable", false);
        out->inbound.port = jint(inb, "port", 2222);
    }
    const cJSON* web = cJSON_GetObjectItemCaseSensitive(root, "web");
    if (cJSON_IsObject(web)) {
        out->web.enabled = jbool(web, "enable", false);
        out->web.port = jint(web, "port", 18081);
    }
    const cJSON* agent = cJSON_GetObjectItemCaseSensitive(root, "agent");
    if (cJSON_IsObject(agent)) {
        out->agent.enabled = jbool(agent, "enable", false);
        out->agent.port = jint(agent, "port", 18086);
        out->agent.token = jstr(agent, "token", "");
    }
    const cJSON* ui = cJSON_GetObjectItemCaseSensitive(root, "ui");
    if (cJSON_IsObject(ui)) {
        out->ui.close_to_tray = jbool(ui, "close_to_tray", true);
        out->ui.start_minimized = jbool(ui, "start_minimized", false);
        out->ui.scrollback_lines = jint(ui, "scrollback_lines", 5000);
        out->ui.font_size = jint(ui, "font_size", 0);
        out->ui.split = jint(ui, "split", 1);
        out->ui.active_pane = jint(ui, "active_pane", 0);
        out->ui.pane_channels.clear();
        const cJSON* panes = cJSON_GetObjectItemCaseSensitive(ui, "pane_channels");
        if (cJSON_IsArray(panes)) {
            for (const cJSON* it = panes->child; it; it = it->next) {
                if (cJSON_IsString(it) && it->valuestring)
                    out->ui.pane_channels.emplace_back(it->valuestring);
                else
                    out->ui.pane_channels.emplace_back();
            }
        }
    }

    const cJSON* tftp = cJSON_GetObjectItemCaseSensitive(root, "tftp");
    if (cJSON_IsObject(tftp)) {
        out->tftp.enable = jbool(tftp, "enable", false);
        out->tftp.port = jint(tftp, "port", 69);
        out->tftp.bind_ip = jstr(tftp, "bind_ip", "");
        out->tftp.root = jstr(tftp, "root", "");
        out->tftp.allow_write = jbool(tftp, "allow_write", true);
    }
    const cJSON* share = cJSON_GetObjectItemCaseSensitive(root, "share");
    if (cJSON_IsObject(share)) {
        out->share.enable = jbool(share, "enable", false);
        out->share.port = jint(share, "port", 18082);
        out->share.root = jstr(share, "root", "");
    }
    const cJSON* nfs = cJSON_GetObjectItemCaseSensitive(root, "nfs");
    if (cJSON_IsObject(nfs)) {
        out->nfs.enable = jbool(nfs, "enable", false);
        out->nfs.alias = jstr(nfs, "alias", "/share");
        out->nfs.uid = jint(nfs, "uid", 0);
        out->nfs.gid = jint(nfs, "gid", 0);
        out->nfs.root = jstr(nfs, "root", "");
    }
    const cJSON* relays = cJSON_GetObjectItemCaseSensitive(root, "relay_devices");
    const cJSON* relayObj = cJSON_GetObjectItemCaseSensitive(root, "relays");
    if (cJSON_IsObject(relayObj)) relays = relayObj;
    if (cJSON_IsArray(relays) || cJSON_IsObject(relays)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, relays) {
            if (!cJSON_IsObject(it)) continue;
            RelayDeviceConfig r;
            r.name = jstr(it, "name", it->string ? it->string : "");
            r.enable = jbool(it, "enable", false);
            r.windows_port = jstr(it, "windows_port", jstr(it, "port", ""));
            r.linux_port = jstr(it, "linux_port", jstr(it, "port", ""));
            r.channels = jint(it, "channels", jint(it, "relay_channels", 2));
            r.baudrate = jint(it, "baudrate", 9600);
            r.io_inverted = jbool(it, "io_inverted", false);
            r.post_write_delay_ms = jint(it, "post_write_delay_ms", 100);
            r.polarity_inverted = jbool(it, "polarity_inverted", false);
            if (!r.name.empty()) out->relays.push_back(r);
        }
    }
}

cJSON* board_to_json(const BoardConfig& b) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", b.name.c_str());
    cJSON_AddStringToObject(o, "serial_device", b.serial_device.c_str());
    cJSON_AddNumberToObject(o, "baudrate", b.baudrate);
    cJSON_AddNumberToObject(o, "databits", b.databits);
    cJSON_AddStringToObject(o, "parity", b.parity.c_str());
    cJSON_AddNumberToObject(o, "stopbits_x10", b.stopbits_x10);
    cJSON_AddStringToObject(o, "flowcontrol", b.flowcontrol.c_str());
    cJSON_AddBoolToObject(o, "dtr", b.dtr);
    cJSON_AddBoolToObject(o, "rts", b.rts);
    cJSON_AddStringToObject(o, "line_ending", b.line_ending.c_str());
    cJSON_AddStringToObject(o, "encoding", b.encoding.c_str());
    cJSON_AddBoolToObject(o, "auto_open", b.auto_open);
    cJSON_AddBoolToObject(o, "timestamp", b.timestamp);
    cJSON_AddNumberToObject(o, "buffer_size", static_cast<double>(b.buffer_size));
    cJSON_AddBoolToObject(o, "log_enabled", b.log_enabled);
    cJSON_AddStringToObject(o, "log_dir", b.log_dir.c_str());
    cJSON_AddStringToObject(o, "password", b.password.c_str());
    cJSON_AddStringToObject(o, "username", b.username.c_str());
    cJSON* btns = cJSON_AddArrayToObject(o, "buttons");
    for (const BoardActionConfig& a : b.buttons) cJSON_AddItemToArray(btns, button_to_json(a));
    cJSON_AddItemToObject(o, "quick_commands", quick_commands_to_json(b.quick_commands));
    return o;
}

cJSON* ssh_to_json(const SSHLinkConfig& s) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", s.name.c_str());
    cJSON_AddStringToObject(o, "host", s.host.c_str());
    cJSON_AddNumberToObject(o, "port", s.port);
    cJSON_AddStringToObject(o, "username", s.username.c_str());
    cJSON_AddStringToObject(o, "password", s.password.c_str());
    cJSON_AddStringToObject(o, "term", s.term.c_str());
    cJSON_AddNumberToObject(o, "connect_timeout_ms", s.connect_timeout_ms);
    cJSON_AddNumberToObject(o, "keepalive", s.keepalive_s);
    cJSON_AddNumberToObject(o, "width", s.width);
    cJSON_AddNumberToObject(o, "height", s.height);
    cJSON_AddNumberToObject(o, "buffer_size", static_cast<double>(s.buffer_size));
    cJSON_AddBoolToObject(o, "log_enabled", s.log_enabled);
    cJSON_AddStringToObject(o, "log_dir", s.log_dir.c_str());
    cJSON* btns = cJSON_AddArrayToObject(o, "buttons");
    for (const BoardActionConfig& a : s.buttons) cJSON_AddItemToArray(btns, button_to_json(a));
    cJSON_AddItemToObject(o, "quick_commands", quick_commands_to_json(s.quick_commands));
    return o;
}

cJSON* default_to_json(const DefaultConfig& d) {    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "username", d.username.c_str());
    cJSON_AddStringToObject(o, "password", d.password.c_str());
    cJSON_AddNumberToObject(o, "buffer_size", static_cast<double>(d.buffer_size));
    cJSON_AddBoolToObject(o, "log_enabled", d.log_enabled);
    cJSON_AddStringToObject(o, "work_dir", d.work_dir.c_str());
    cJSON_AddStringToObject(o, "log_dir", d.log_dir.c_str());
    cJSON_AddBoolToObject(o, "auto_discover_serial", true);
    return o;
}

// 把已知字段写进 root（保留 root 里的其它未知键）
void save_root(cJSON* root, const AppConfig& cfg) {
    cJSON_DeleteItemFromObjectCaseSensitive(root, "default");
    cJSON_AddItemToObject(root, "default", default_to_json(cfg.def));

    cJSON_DeleteItemFromObjectCaseSensitive(root, "boards");
    cJSON* arr = cJSON_AddArrayToObject(root, "boards");
    for (const BoardConfig& b : cfg.boards) cJSON_AddItemToArray(arr, board_to_json(b));

    // SSH 出站通道同样要落盘：早期 save_root 漏写，导致“新建 SSH 通道”重启后消失
    cJSON_DeleteItemFromObjectCaseSensitive(root, "ssh");
    cJSON* sarr = cJSON_AddArrayToObject(root, "ssh");
    for (const SSHLinkConfig& s : cfg.links) cJSON_AddItemToArray(sarr, ssh_to_json(s));

    cJSON* inb = cJSON_GetObjectItemCaseSensitive(root, "inbound_ssh");
    if (!cJSON_IsObject(inb)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "inbound_ssh");
        inb = cJSON_AddObjectToObject(root, "inbound_ssh");
    }
    cJSON_DeleteItemFromObjectCaseSensitive(inb, "enable");
    cJSON_AddBoolToObject(inb, "enable", cfg.inbound.enabled);
    cJSON_DeleteItemFromObjectCaseSensitive(inb, "port");
    cJSON_AddNumberToObject(inb, "port", cfg.inbound.port);

    cJSON* ui = cJSON_GetObjectItemCaseSensitive(root, "ui");
    if (!cJSON_IsObject(ui)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "ui");
        ui = cJSON_AddObjectToObject(root, "ui");
    }
    cJSON_DeleteItemFromObjectCaseSensitive(ui, "close_to_tray");
    cJSON_AddBoolToObject(ui, "close_to_tray", cfg.ui.close_to_tray);
    cJSON_DeleteItemFromObjectCaseSensitive(ui, "start_minimized");
    cJSON_AddBoolToObject(ui, "start_minimized", cfg.ui.start_minimized);
    cJSON_DeleteItemFromObjectCaseSensitive(ui, "scrollback_lines");
    cJSON_AddNumberToObject(ui, "scrollback_lines", cfg.ui.scrollback_lines);
    cJSON_DeleteItemFromObjectCaseSensitive(ui, "font_size");
    cJSON_AddNumberToObject(ui, "font_size", cfg.ui.font_size);
    cJSON_DeleteItemFromObjectCaseSensitive(ui, "split");
    cJSON_AddNumberToObject(ui, "split", cfg.ui.split);
    cJSON_DeleteItemFromObjectCaseSensitive(ui, "active_pane");
    cJSON_AddNumberToObject(ui, "active_pane", cfg.ui.active_pane);
    cJSON_DeleteItemFromObjectCaseSensitive(ui, "pane_channels");
    cJSON* panes = cJSON_AddArrayToObject(ui, "pane_channels");
    for (const std::string& name : cfg.ui.pane_channels)
        cJSON_AddItemToArray(panes, cJSON_CreateString(name.c_str()));

    cJSON* agent = cJSON_GetObjectItemCaseSensitive(root, "agent");
    if (!cJSON_IsObject(agent)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "agent");
        agent = cJSON_AddObjectToObject(root, "agent");
    }
    cJSON_DeleteItemFromObjectCaseSensitive(agent, "enable");
    cJSON_AddBoolToObject(agent, "enable", cfg.agent.enabled);
    cJSON_DeleteItemFromObjectCaseSensitive(agent, "port");
    cJSON_AddNumberToObject(agent, "port", cfg.agent.port);
    cJSON_DeleteItemFromObjectCaseSensitive(agent, "token");
    cJSON_AddStringToObject(agent, "token", cfg.agent.token.c_str());

    cJSON_DeleteItemFromObjectCaseSensitive(root, "tftp");
    cJSON* tftp = cJSON_AddObjectToObject(root, "tftp");
    cJSON_AddBoolToObject(tftp, "enable", cfg.tftp.enable);
    cJSON_AddNumberToObject(tftp, "port", cfg.tftp.port);
    cJSON_AddStringToObject(tftp, "bind_ip", cfg.tftp.bind_ip.c_str());
    cJSON_AddStringToObject(tftp, "root", cfg.tftp.root.c_str());
    cJSON_AddBoolToObject(tftp, "allow_write", cfg.tftp.allow_write);

    cJSON_DeleteItemFromObjectCaseSensitive(root, "share");
    cJSON* share = cJSON_AddObjectToObject(root, "share");
    cJSON_AddBoolToObject(share, "enable", cfg.share.enable);
    cJSON_AddNumberToObject(share, "port", cfg.share.port);
    cJSON_AddStringToObject(share, "root", cfg.share.root.c_str());

    cJSON_DeleteItemFromObjectCaseSensitive(root, "nfs");
    cJSON* nfs = cJSON_AddObjectToObject(root, "nfs");
    cJSON_AddBoolToObject(nfs, "enable", cfg.nfs.enable);
    cJSON_AddStringToObject(nfs, "alias", cfg.nfs.alias.c_str());
    cJSON_AddNumberToObject(nfs, "uid", cfg.nfs.uid);
    cJSON_AddNumberToObject(nfs, "gid", cfg.nfs.gid);
    cJSON_AddStringToObject(nfs, "root", cfg.nfs.root.c_str());

    cJSON_DeleteItemFromObjectCaseSensitive(root, "relay_devices");
    cJSON* relays = cJSON_AddArrayToObject(root, "relay_devices");
    for (const RelayDeviceConfig& r : cfg.relays) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", r.name.c_str());
        cJSON_AddBoolToObject(o, "enable", r.enable);
        cJSON_AddStringToObject(o, "windows_port", r.windows_port.c_str());
        cJSON_AddStringToObject(o, "linux_port", r.linux_port.c_str());
        cJSON_AddNumberToObject(o, "channels", r.channels);
        cJSON_AddNumberToObject(o, "baudrate", r.baudrate);
        cJSON_AddBoolToObject(o, "io_inverted", r.io_inverted);
        cJSON_AddNumberToObject(o, "post_write_delay_ms", r.post_write_delay_ms);
        cJSON_AddBoolToObject(o, "polarity_inverted", r.polarity_inverted);
        cJSON_AddItemToArray(relays, o);
    }
}

std::string exe_dir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return std::string();
    return util::dir_of(from_wide(std::wstring(buf, n)));
}

std::string find_config_path(const std::string& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;
    // 顺序：启动目录 → exe 同目录 → 用户目录 → 旧名。
    // 先看启动目录，便于测试/多实例用不同配置；双击绿色版时启动目录里没有配置，自然落到 exe 同目录。
    std::string c = util::join_path(cwd(), "ssh_bridge_config.json");
    if (file_exists(c)) return c;
    std::string e = util::join_path(exe_dir(), "ssh_bridge_config.json");
    if (file_exists(e)) return e;
    std::string h = home_dir();
    if (!h.empty()) {
        std::string hp = util::join_path(util::join_path(h, ".ssh-bridge"),
                                         "ssh_bridge_config.json");
        if (file_exists(hp)) return hp;
    }
    std::string legacy = util::join_path(cwd(), "config.json");
    if (file_exists(legacy)) return legacy;
    return std::string();
}

}  // namespace

bool config_load(AppConfig& out, const std::string& explicit_path) {
    out = AppConfig();
    std::string path = find_config_path(explicit_path);
    if (path.empty()) return false;

    std::string text;
    if (!read_text(path, &text) || text.empty()) return false;

    cJSON* root = cJSON_Parse(text.c_str());
    if (!root) return false;
    if (cJSON_IsObject(root)) load_root(root, &out);
    cJSON_Delete(root);

    out.abs_path = path;
    out.project_root = util::dir_of(path);
    return true;
}

bool config_save(const AppConfig& cfg) {
    if (cfg.abs_path.empty()) return false;

    cJSON* root = nullptr;
    std::string text;
    if (read_text(cfg.abs_path, &text) && !text.empty()) {
        root = cJSON_Parse(text.c_str());
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    save_root(root, cfg);
    char* dumped = cJSON_Print(root);
    std::string data = dumped ? dumped : "";
    if (dumped) cJSON_free(dumped);
    cJSON_Delete(root);
    if (data.empty()) return false;
    data.push_back('\n');
    return write_text(cfg.abs_path, data, nullptr);
}

bool config_patch_board(const std::string& path, const std::string& name,
                        const std::string& json_patch, std::string* err) {
    if (path.empty()) {
        if (err) *err = "empty config path";
        return false;
    }
    std::string text;
    if (!read_text(path, &text)) {
        if (err) *err = "cannot read " + path;
        return false;
    }
    cJSON* root = cJSON_Parse(text.c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        if (err) *err = "bad json: " + path;
        return false;
    }
    cJSON* patch = cJSON_Parse(json_patch.c_str());
    if (!cJSON_IsObject(patch)) {
        cJSON_Delete(patch);
        cJSON_Delete(root);
        if (err) *err = "bad patch json";
        return false;
    }

    cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "boards");
    if (!cJSON_IsArray(arr)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "boards");
        arr = cJSON_AddArrayToObject(root, "boards");
    }
    cJSON* target = nullptr;
    cJSON* it = nullptr;
    cJSON_ArrayForEach(it, arr) {
        const char* n = jstr(it, "name", "");
        if (name == n) { target = it; break; }
    }
    if (!target) {
        target = cJSON_CreateObject();
        cJSON_AddStringToObject(target, "name", name.c_str());
        cJSON_AddItemToArray(arr, target);
    }
    cJSON* child = patch->child;
    while (child) {
        cJSON* next = child->next;
        cJSON* dup = cJSON_Duplicate(child, 1);
        cJSON_DeleteItemFromObjectCaseSensitive(target, child->string);
        cJSON_AddItemToObject(target, child->string, dup);
        child = next;
    }
    cJSON_Delete(patch);

    char* dumped = cJSON_Print(root);
    std::string data = dumped ? dumped : "";
    if (dumped) cJSON_free(dumped);
    cJSON_Delete(root);
    if (data.empty()) {
        if (err) *err = "serialize failed";
        return false;
    }
    data.push_back('\n');
    return write_text(path, data, err);
}

bool config_delete_board(const std::string& path, const std::string& name,
                         std::string* err) {
    std::string text;
    if (!read_text(path, &text)) {
        if (err) *err = "cannot read " + path;
        return false;
    }
    cJSON* root = cJSON_Parse(text.c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        if (err) *err = "bad json: " + path;
        return false;
    }
    cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "boards");
    int idx = 0;
    int found = -1;
    cJSON* it = nullptr;
    cJSON_ArrayForEach(it, arr) {
        if (name == jstr(it, "name", "")) { found = idx; break; }
        ++idx;
    }
    if (found >= 0) cJSON_DeleteItemFromArray(arr, found);
    char* dumped = cJSON_Print(root);
    std::string data = dumped ? dumped : "";
    if (dumped) cJSON_free(dumped);
    cJSON_Delete(root);
    if (data.empty()) {
        if (err) *err = "serialize failed";
        return false;
    }
    data.push_back('\n');
    return write_text(path, data, err);
}

std::string config_resolve_password(const AppConfig& cfg, const BoardConfig& board) {
    if (board.password_present) return board.password;   // 显式空串 = 免密
    return cfg.def.password;
}

}  // namespace sb3
