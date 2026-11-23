// config.cpp — JSON 读写（cJSON）。UTF-8 落盘，写临时文件后原子替换。

#include "core/config/config.h"

#include <windows.h>

#include <cjson/cJSON.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <atomic>

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
    if (!out) return false;
    std::wstring wpath = to_wide(path);
    std::FILE* f = _wfopen(wpath.c_str(), L"rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    if (size < 0 || size > 256L * 1024 * 1024) {
        std::fclose(f);
        return false;
    }
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
    // 每个写者使用独立临时文件：固定 .tmp 会让并发写互相覆盖/删除对方的临时文件。
    static std::atomic<unsigned long long> seq{0};
    std::string tmp = path + ".tmp." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(++seq);
    std::FILE* f = _wfopen(to_wide(tmp).c_str(), L"wb");
    if (!f) {
        if (err) *err = "cannot open " + tmp;
        return false;
    }
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        _wremove(to_wide(tmp).c_str());     // 不留半截临时文件
        if (err) *err = "write failed: " + tmp;
        return false;
    }
    if (!MoveFileExW(to_wide(tmp).c_str(), to_wide(path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        _wremove(to_wide(tmp).c_str());
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
    const cJSON* user = cJSON_GetObjectItemCaseSensitive(o, "username");
    if (cJSON_IsString(user) && user->valuestring) def->username = user->valuestring;
    if (jhas(o, "password")) def->password = jstr(o, "password", "");
    int buf = jint(o, "buffer_size", static_cast<int>(def->buffer_size));
    def->buffer_size = buf > 0 ? static_cast<std::uint32_t>(std::min(buf, 256 * 1024 * 1024))
                               : def->buffer_size;
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
    int bb = jint(o, "buffer_size", 0);
    b->buffer_size = bb > 0 ? static_cast<std::uint32_t>(std::min(bb, 256 * 1024 * 1024)) : 0;
    b->log_enabled = jbool(o, "log_enabled", b->log_enabled);
    b->log_dir = jstr(o, "log_dir", "");
    b->password_present = jhas(o, "password");
    b->password = jstr(o, "password", "");
    b->username = jstr(o, "username", "root");
    b->discoverable = jbool(o, "discoverable", false);
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
    int sb = jint(o, "buffer_size", 0);
    s->buffer_size = sb > 0 ? static_cast<std::uint32_t>(std::min(sb, 256 * 1024 * 1024)) : 0;
    s->log_enabled = jbool(o, "log_enabled", true);
    s->log_dir = jstr(o, "log_dir", "");
    s->discoverable = jbool(o, "discoverable", false);
    load_buttons(o, &s->buttons);
    load_quick_commands(o, &s->quick_commands);
}

void load_powershell(const cJSON* o, PowerShellConfig* p) {
    p->name = jstr(o, "name", "");
    p->shell = jstr(o, "shell", "powershell.exe");
    p->args = jstr(o, "args", "-NoLogo -NoProfile -ExecutionPolicy Bypass");
    p->cwd = jstr(o, "cwd", "");
    p->line_ending = jstr(o, "line_ending", "cr");
    int bs = jint(o, "buffer_size", 0);
    p->buffer_size = bs > 0 ? static_cast<std::uint32_t>(std::min(bs, 256 * 1024 * 1024)) : 0;
    p->log_enabled = jbool(o, "log_enabled", true);
    p->log_dir = jstr(o, "log_dir", "");
    p->timestamp = jbool(o, "timestamp", false);
    load_buttons(o, &p->buttons);
    load_quick_commands(o, &p->quick_commands);
}

void load_telnet(const cJSON* o, TelnetConfig* t) {
    t->name = jstr(o, "name", "");
    t->host = jstr(o, "host", "");
    t->port = jint(o, "port", 23);
    if (t->port < 1 || t->port > 65535) t->port = 23;
    t->connect_timeout_ms = jint(o, "connect_timeout_ms", jint(o, "connect_timeout", 5000));
    if (t->connect_timeout_ms < 500) t->connect_timeout_ms = 500;
    if (t->connect_timeout_ms > 60000) t->connect_timeout_ms = 60000;
    t->line_ending = jstr(o, "line_ending", "crlf");
    t->width = jint(o, "width", 120);
    t->height = jint(o, "height", 32);
    if (t->width < 20) t->width = 20;
    if (t->height < 5) t->height = 5;
    int bs = jint(o, "buffer_size", 0);
    t->buffer_size = bs > 0 ? static_cast<std::uint32_t>(std::min(bs, 256 * 1024 * 1024)) : 0;
    t->log_enabled = jbool(o, "log_enabled", true);
    t->log_dir = jstr(o, "log_dir", "");
    t->timestamp = jbool(o, "timestamp", false);
    load_buttons(o, &t->buttons);
    load_quick_commands(o, &t->quick_commands);
}

void load_ui_root(const cJSON* root, UiConfig* ui) {
    const cJSON* u = cJSON_GetObjectItemCaseSensitive(root, "ui");
    if (cJSON_IsObject(u)) {
        ui->close_to_tray = jbool(u, "close_to_tray", true);
        ui->start_minimized = jbool(u, "start_minimized", false);
        ui->scrollback_lines = jint(u, "scrollback_lines", 5000);
        ui->font_size = jint(u, "font_size", 0);
        ui->split = jint(u, "split", 1);
        ui->active_pane = jint(u, "active_pane", 0);
        ui->show_statusbar = jbool(u, "show_statusbar", true);
        ui->pane_channels.clear();
        const cJSON* panes = cJSON_GetObjectItemCaseSensitive(u, "pane_channels");
        if (cJSON_IsArray(panes)) {
            for (const cJSON* it = panes->child; it; it = it->next) {
                if (cJSON_IsString(it) && it->valuestring)
                    ui->pane_channels.emplace_back(it->valuestring);
                else
                    ui->pane_channels.emplace_back();
            }
        }
    }
    const cJSON* w = cJSON_GetObjectItemCaseSensitive(root, "window");
    if (cJSON_IsObject(w)) ui->window_rect = jstr(w, "rect", "");
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
    const cJSON* psh = cJSON_GetObjectItemCaseSensitive(root, "powershell");
    if (cJSON_IsArray(psh)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, psh) {
            if (!cJSON_IsObject(it)) continue;
            if (jhas(it, "enable") && !jbool(it, "enable", true)) continue;
            PowerShellConfig p;
            load_powershell(it, &p);
            out->powershell.push_back(p);
        }
    } else if (cJSON_IsObject(psh)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, psh) {
            if (!cJSON_IsObject(it)) continue;
            if (jhas(it, "enable") && !jbool(it, "enable", true)) continue;
            PowerShellConfig p;
            load_powershell(it, &p);
            if (p.name.empty() && it->string) p.name = it->string;
            out->powershell.push_back(p);
        }
    }
    const cJSON* tnt = cJSON_GetObjectItemCaseSensitive(root, "telnet");
    if (cJSON_IsArray(tnt)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, tnt) {
            if (!cJSON_IsObject(it)) continue;
            if (jhas(it, "enable") && !jbool(it, "enable", true)) continue;
            TelnetConfig t;
            load_telnet(it, &t);
            out->telnet.push_back(t);
        }
    } else if (cJSON_IsObject(tnt)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, tnt) {
            if (!cJSON_IsObject(it)) continue;
            if (jhas(it, "enable") && !jbool(it, "enable", true)) continue;
            TelnetConfig t;
            load_telnet(it, &t);
            if (t.name.empty() && it->string) t.name = it->string;
            out->telnet.push_back(t);
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
        out->web.token = jstr(web, "token", "");
        // 旧配置兼容：显式字段优先；缺失时，已有 token 视为启用，空 token 默认不启用。
        out->web.require_token = jbool(web, "require_token", !out->web.token.empty());
    }
    const cJSON* agent = cJSON_GetObjectItemCaseSensitive(root, "agent");
    if (cJSON_IsObject(agent)) {
        out->agent.enabled = jbool(agent, "enable", false);
        int port = jint(agent, "port", 18096);
        // 旧版本的 UI 桥端口 18086 已废弃；保留 enable 语义，只把端口纠偏到 18096。
        if (port == 18086) port = 18096;
        out->agent.port = port;
        out->agent.token = jstr(agent, "token", "");
    }
    load_ui_root(root, &out->ui);

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
    const cJSON* tfwd = cJSON_GetObjectItemCaseSensitive(root, "tcp_forward");
    if (cJSON_IsObject(tfwd)) {
        out->tcp_fwd.enable = jbool(tfwd, "enable", false);
        out->tcp_fwd.listen_ip = jstr(tfwd, "listen_ip", "0.0.0.0");
        out->tcp_fwd.port_base = jint(tfwd, "port_base", 22000);
        out->tcp_fwd.target_port = jint(tfwd, "target_port", 22);
        out->tcp_fwd.board_cidr = jstr(tfwd, "board_cidr", "192.168.1.0/24");
    }
    const cJSON* tun = cJSON_GetObjectItemCaseSensitive(root, "tunnel");
    if (cJSON_IsObject(tun)) {
        out->tunnel.enable = jbool(tun, "enable", false);
        out->tunnel.http_enable = jbool(tun, "map_http", jbool(tun, "http_enable", true));
        out->tunnel.ssh_enable = jbool(tun, "map_inbound_ssh", jbool(tun, "ssh_enable", false));
        out->tunnel.exe_path = jstr(tun, "exe_path", "");
        out->tunnel.server_addr = jstr(tun, "server_addr", "");
        out->tunnel.server_port = jint(tun, "server_port", 7000);
        out->tunnel.token = jstr(tun, "token", "");
        out->tunnel.http_local_port = jint(tun, "local_port", jint(tun, "http_local_port", 18081));
        out->tunnel.http_remote_port = jint(tun, "remote_port", jint(tun, "http_remote_port", 0));
        out->tunnel.ssh_local_port = jint(tun, "ssh_local_port", 2222);
        out->tunnel.ssh_remote_port = jint(tun, "ssh_remote_port", 0);
    }
    const cJSON* disc = cJSON_GetObjectItemCaseSensitive(root, "discovery");
    if (cJSON_IsObject(disc)) {
        out->discovery.enable = jbool(disc, "enable", false);
        out->discovery.port = jint(disc, "port", 18071);
    }
    // 新键 relay_devices 优先；只有旧配置仅有 relays 时才回退
    const cJSON* relays = cJSON_GetObjectItemCaseSensitive(root, "relay_devices");
    if (!cJSON_IsArray(relays) && !cJSON_IsObject(relays))
        relays = cJSON_GetObjectItemCaseSensitive(root, "relays");
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
    cJSON_AddBoolToObject(o, "discoverable", b.discoverable);
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
    cJSON_AddBoolToObject(o, "discoverable", s.discoverable);
    cJSON* btns = cJSON_AddArrayToObject(o, "buttons");
    for (const BoardActionConfig& a : s.buttons) cJSON_AddItemToArray(btns, button_to_json(a));
    cJSON_AddItemToObject(o, "quick_commands", quick_commands_to_json(s.quick_commands));
    return o;
}

cJSON* powershell_to_json(const PowerShellConfig& p) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", p.name.c_str());
    cJSON_AddStringToObject(o, "shell", p.shell.c_str());
    cJSON_AddStringToObject(o, "args", p.args.c_str());
    cJSON_AddStringToObject(o, "cwd", p.cwd.c_str());
    cJSON_AddStringToObject(o, "line_ending", p.line_ending.c_str());
    cJSON_AddNumberToObject(o, "buffer_size", static_cast<double>(p.buffer_size));
    cJSON_AddBoolToObject(o, "log_enabled", p.log_enabled);
    cJSON_AddStringToObject(o, "log_dir", p.log_dir.c_str());
    cJSON_AddBoolToObject(o, "timestamp", p.timestamp);
    cJSON* btns = cJSON_AddArrayToObject(o, "buttons");
    for (const BoardActionConfig& a : p.buttons) cJSON_AddItemToArray(btns, button_to_json(a));
    cJSON_AddItemToObject(o, "quick_commands", quick_commands_to_json(p.quick_commands));
    return o;
}

cJSON* telnet_to_json(const TelnetConfig& t) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", t.name.c_str());
    cJSON_AddStringToObject(o, "host", t.host.c_str());
    cJSON_AddNumberToObject(o, "port", t.port);
    cJSON_AddNumberToObject(o, "connect_timeout_ms", t.connect_timeout_ms);
    cJSON_AddStringToObject(o, "line_ending", t.line_ending.c_str());
    cJSON_AddNumberToObject(o, "width", t.width);
    cJSON_AddNumberToObject(o, "height", t.height);
    cJSON_AddNumberToObject(o, "buffer_size", static_cast<double>(t.buffer_size));
    cJSON_AddBoolToObject(o, "log_enabled", t.log_enabled);
    cJSON_AddStringToObject(o, "log_dir", t.log_dir.c_str());
    cJSON_AddBoolToObject(o, "timestamp", t.timestamp);
    cJSON* btns = cJSON_AddArrayToObject(o, "buttons");
    for (const BoardActionConfig& a : t.buttons) cJSON_AddItemToArray(btns, button_to_json(a));
    cJSON_AddItemToObject(o, "quick_commands", quick_commands_to_json(t.quick_commands));
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

// 窗口/界面字段单独写；功能配置绝不出现 ui 段
void save_ui_root(cJSON* root, const AppConfig& cfg) {
    cJSON_DeleteItemFromObjectCaseSensitive(root, "ui");
    cJSON* ui = cJSON_AddObjectToObject(root, "ui");
    if (!ui) return;
    cJSON_AddBoolToObject(ui, "close_to_tray", cfg.ui.close_to_tray);
    cJSON_AddBoolToObject(ui, "start_minimized", cfg.ui.start_minimized);
    cJSON_AddNumberToObject(ui, "scrollback_lines", cfg.ui.scrollback_lines);
    cJSON_AddNumberToObject(ui, "font_size", cfg.ui.font_size);
    cJSON_AddNumberToObject(ui, "split", cfg.ui.split);
    cJSON_AddNumberToObject(ui, "active_pane", cfg.ui.active_pane);
    cJSON_AddBoolToObject(ui, "show_statusbar", cfg.ui.show_statusbar);
    cJSON* panes = cJSON_AddArrayToObject(ui, "pane_channels");
    for (const std::string& name : cfg.ui.pane_channels)
        cJSON_AddItemToArray(panes, cJSON_CreateString(name.c_str()));
    cJSON_DeleteItemFromObjectCaseSensitive(root, "window");
    cJSON* window = cJSON_AddObjectToObject(root, "window");
    if (window) cJSON_AddStringToObject(window, "rect", cfg.ui.window_rect.c_str());
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

    cJSON_DeleteItemFromObjectCaseSensitive(root, "powershell");
    cJSON* parr = cJSON_AddArrayToObject(root, "powershell");
    for (const PowerShellConfig& p : cfg.powershell) cJSON_AddItemToArray(parr, powershell_to_json(p));

    cJSON_DeleteItemFromObjectCaseSensitive(root, "telnet");
    cJSON* tarr = cJSON_AddArrayToObject(root, "telnet");
    for (const TelnetConfig& t : cfg.telnet) cJSON_AddItemToArray(tarr, telnet_to_json(t));

    cJSON* inb = cJSON_GetObjectItemCaseSensitive(root, "inbound_ssh");
    if (!cJSON_IsObject(inb)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "inbound_ssh");
        inb = cJSON_AddObjectToObject(root, "inbound_ssh");
    }
    cJSON_DeleteItemFromObjectCaseSensitive(inb, "enable");
    cJSON_AddBoolToObject(inb, "enable", cfg.inbound.enabled);
    cJSON_DeleteItemFromObjectCaseSensitive(inb, "port");
    cJSON_AddNumberToObject(inb, "port", cfg.inbound.port);

    cJSON* web = cJSON_GetObjectItemCaseSensitive(root, "web");
    if (!cJSON_IsObject(web)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "web");
        web = cJSON_AddObjectToObject(root, "web");
    }
    cJSON_DeleteItemFromObjectCaseSensitive(web, "enable");
    cJSON_AddBoolToObject(web, "enable", cfg.web.enabled);
    cJSON_DeleteItemFromObjectCaseSensitive(web, "port");
    cJSON_AddNumberToObject(web, "port", cfg.web.port);
    cJSON_DeleteItemFromObjectCaseSensitive(web, "require_token");
    cJSON_AddBoolToObject(web, "require_token", cfg.web.require_token);
    cJSON_DeleteItemFromObjectCaseSensitive(web, "token");
    cJSON_AddStringToObject(web, "token", cfg.web.token.c_str());

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

    cJSON_DeleteItemFromObjectCaseSensitive(root, "tcp_forward");
    cJSON* tfwd = cJSON_AddObjectToObject(root, "tcp_forward");
    cJSON_AddBoolToObject(tfwd, "enable", cfg.tcp_fwd.enable);
    cJSON_AddStringToObject(tfwd, "listen_ip", cfg.tcp_fwd.listen_ip.c_str());
    cJSON_AddNumberToObject(tfwd, "port_base", cfg.tcp_fwd.port_base);
    cJSON_AddNumberToObject(tfwd, "target_port", cfg.tcp_fwd.target_port);
    cJSON_AddStringToObject(tfwd, "board_cidr", cfg.tcp_fwd.board_cidr.c_str());

    cJSON_DeleteItemFromObjectCaseSensitive(root, "tunnel");
    cJSON* tun = cJSON_AddObjectToObject(root, "tunnel");
    cJSON_AddBoolToObject(tun, "enable", cfg.tunnel.enable);
    cJSON_AddBoolToObject(tun, "map_http", cfg.tunnel.http_enable);
    cJSON_AddBoolToObject(tun, "map_inbound_ssh", cfg.tunnel.ssh_enable);
    cJSON_AddStringToObject(tun, "exe_path", cfg.tunnel.exe_path.c_str());
    cJSON_AddStringToObject(tun, "server_addr", cfg.tunnel.server_addr.c_str());
    cJSON_AddNumberToObject(tun, "server_port", cfg.tunnel.server_port);
    cJSON_AddStringToObject(tun, "token", cfg.tunnel.token.c_str());
    cJSON_AddNumberToObject(tun, "local_port", cfg.tunnel.http_local_port);
    cJSON_AddNumberToObject(tun, "remote_port", cfg.tunnel.http_remote_port);
    cJSON_AddNumberToObject(tun, "ssh_local_port", cfg.tunnel.ssh_local_port);
    cJSON_AddNumberToObject(tun, "ssh_remote_port", cfg.tunnel.ssh_remote_port);

    cJSON_DeleteItemFromObjectCaseSensitive(root, "discovery");
    cJSON* disc = cJSON_AddObjectToObject(root, "discovery");
    cJSON_AddBoolToObject(disc, "enable", cfg.discovery.enable);
    cJSON_AddNumberToObject(disc, "port", cfg.discovery.port);

    cJSON_DeleteItemFromObjectCaseSensitive(root, "relays");   // 清掉旧键，避免下次加载被旧对象覆盖
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
    // 顺序：显式路径 → 启动目录 → exe 同目录 → 用户目录 → 旧名。
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

std::string ui_path_for(const std::string& functional_path) {
    if (functional_path.empty()) return std::string();
    return util::join_path(util::dir_of(functional_path), "ssh_bridge_ui.json");
}

}  // namespace

std::string config_find_path(const std::string& explicit_path) {
    return find_config_path(explicit_path);
}

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
    out.ui_abs_path = ui_path_for(path);
    out.project_root = util::dir_of(path);

    // 窗口配置优先：同目录 ssh_bridge_ui.json 覆盖旧功能文件里的 ui 段。
    // 加载阶段绝不写盘，旧统一配置仅在保存时拆成两份。
    std::string ui_text;
    if (read_text(out.ui_abs_path, &ui_text) && !ui_text.empty()) {
        cJSON* ui_root = cJSON_Parse(ui_text.c_str());
        if (cJSON_IsObject(ui_root)) load_ui_root(ui_root, &out.ui);
        cJSON_Delete(ui_root);
    }
    return true;
}

std::string config_ui_path(const AppConfig& cfg) {
    if (!cfg.ui_abs_path.empty()) return cfg.ui_abs_path;
    return ui_path_for(cfg.abs_path);
}

bool config_save_ui(const AppConfig& cfg) {
    std::string path = config_ui_path(cfg);
    if (path.empty()) return false;

    cJSON* root = nullptr;
    std::string text;
    if (read_text(path, &text) && !text.empty()) root = cJSON_Parse(text.c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (!root) return false;
    cJSON_DeleteItemFromObjectCaseSensitive(root, "schema");
    cJSON_AddStringToObject(root, "schema", "ssh-bridge-ui/1");
    save_ui_root(root, cfg);

    char* dumped = cJSON_Print(root);
    std::string data = dumped ? dumped : "";
    if (dumped) cJSON_free(dumped);
    cJSON_Delete(root);
    if (data.empty()) return false;
    data.push_back('\n');
    if (!write_text(path, data, nullptr)) return false;

    // 旧统一配置的首次 UI 保存：顺手把 ui/window 从功能文件剥离，完成一次性迁移。
    // 已经是拆分状态时不再触碰功能文件，避免每次切分屏都重写通道/服务配置。
    std::string ftext;
    if (read_text(cfg.abs_path, &ftext) && !ftext.empty()) {
        cJSON* froot = cJSON_Parse(ftext.c_str());
        if (cJSON_IsObject(froot)) {
            bool changed = cJSON_GetObjectItemCaseSensitive(froot, "ui") != nullptr ||
                           cJSON_GetObjectItemCaseSensitive(froot, "window") != nullptr;
            if (changed) {
                cJSON_DeleteItemFromObjectCaseSensitive(froot, "ui");
                cJSON_DeleteItemFromObjectCaseSensitive(froot, "window");
                char* fdumped = cJSON_Print(froot);
                std::string fdata = fdumped ? fdumped : "";
                if (fdumped) cJSON_free(fdumped);
                if (!fdata.empty()) {
                    fdata.push_back('\n');
                    write_text(cfg.abs_path, fdata, nullptr);
                }
            }
        }
        cJSON_Delete(froot);
    }
    return true;
}

bool config_save_functional(const AppConfig& cfg) {
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
    if (!root) return false;
    save_root(root, cfg);
    // 功能配置只保留功能字段；旧统一配置里的 ui/window 迁移到 ui 文件。
    cJSON_DeleteItemFromObjectCaseSensitive(root, "ui");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "window");
    char* dumped = cJSON_Print(root);
    std::string data = dumped ? dumped : "";
    if (dumped) cJSON_free(dumped);
    cJSON_Delete(root);
    if (data.empty()) return false;
    data.push_back('\n');
    return write_text(cfg.abs_path, data, nullptr);
}

bool config_save(const AppConfig& cfg) {
    if (!config_save_functional(cfg)) return false;
    return config_save_ui(cfg);
}

namespace {

// boards 兼容旧版对象写法；patch/delete 前先无损转成数组，避免把旧板卡清空
cJSON* ensure_boards_array(cJSON* root) {
    cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "boards");
    if (cJSON_IsArray(arr)) return arr;
    cJSON* old = arr;
    cJSON* out = cJSON_CreateArray();
    if (!out) return nullptr;
    if (cJSON_IsObject(old)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, old) {
            if (!cJSON_IsObject(it)) continue;
            cJSON* dup = cJSON_Duplicate(it, 1);
            if (!dup) continue;
            if (!cJSON_GetObjectItemCaseSensitive(dup, "name") && it->string)
                cJSON_AddStringToObject(dup, "name", it->string);
            cJSON_AddItemToArray(out, dup);
        }
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, "boards");
    cJSON_AddItemToObject(root, "boards", out);
    return out;
}

}  // namespace

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

    cJSON* arr = ensure_boards_array(root);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(patch);
        cJSON_Delete(root);
        if (err) *err = "cannot create boards array";
        return false;
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
    cJSON* arr = ensure_boards_array(root);
    int idx = 0;
    int found = -1;
    cJSON* it = nullptr;
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (name == jstr(it, "name", "")) { found = idx; break; }
            ++idx;
        }
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

std::string config_export_json(const AppConfig& cfg) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return "{}";
    save_root(root, cfg);
    save_ui_root(root, cfg);
    char* text = cJSON_Print(root);
    std::string out = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

bool config_parse_json(const std::string& text, AppConfig* out, std::string* err) {
    if (!out) return false;
    *out = AppConfig();
    cJSON* root = cJSON_Parse(text.c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        if (err) *err = "配置必须是 JSON 对象";
        return false;
    }
    load_root(root, out);
    cJSON_Delete(root);
    if (err) err->clear();
    return true;
}

std::string config_resolve_password(const AppConfig& cfg, const BoardConfig& board) {
    if (board.password_present) return board.password;   // 显式空串 = 免密
    return cfg.def.password;
}

}  // namespace sb3
