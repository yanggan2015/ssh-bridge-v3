#include "services/tunnel/tunnel_manager.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cjson/cJSON.h>

namespace sb3 {
namespace {

constexpr int kHttpMin = 18000;
constexpr int kHttpMax = 18999;
constexpr int kSshMin = 28000;
constexpr int kSshMax = 28999;
constexpr int kHealthPort = 18087;
constexpr int kMaxRestarts = 5;

std::string exe_dir() {
    char buf[MAX_PATH * 2] = "";
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n == 0) return "";
    std::filesystem::path p(buf);
    return p.parent_path().u8string();
}

bool file_exists(const std::string& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::exists(std::filesystem::u8path(p), ec);
}

std::string find_exe(const std::string& hint, const std::string& root) {
    if (!hint.empty()) {
        std::filesystem::path hp = std::filesystem::u8path(hint);
        if (!hp.is_absolute()) hp = std::filesystem::u8path(root) / hp;
        if (file_exists(hp.u8string())) return hp.lexically_normal().u8string();
    }
    std::vector<std::string> names = {"eadk-tunnel.exe", "frpc.exe"};
    std::vector<std::string> dirs = {exe_dir(), root, root + "\\tools", root + "\\output\\build"};
    for (const std::string& d : dirs) {
        if (d.empty()) continue;
        for (const std::string& n : names) {
            std::filesystem::path p = std::filesystem::u8path(d) / n;
            if (file_exists(p.u8string())) return p.lexically_normal().u8string();
        }
    }
    return "";
}

bool probe_port(int port, int timeout_ms) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = false;
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        ok = true;
    } else if (::WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wr, ex;
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(s, &wr);
        FD_SET(s, &ex);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (::select(0, nullptr, &wr, &ex, &tv) > 0 && FD_ISSET(s, &wr)) {
            int e = 0;
            int len = sizeof(e);
            if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&e), &len) == 0 &&
                e == 0)
                ok = true;
        }
    }
    ::closesocket(s);
    return ok;
}

std::string json_text(cJSON* root) {
    char* p = cJSON_PrintUnformatted(root);
    std::string s = p ? p : "{}";
    cJSON_free(p);
    cJSON_Delete(root);
    return s;
}

bool write_text(const std::string& path, const std::string& text) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

cJSON* make_runtime(const TunnelConfig& cfg, int local_port, int remote_port, int rmin, int rmax,
                    const std::string& suffix) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "server_addr", cfg.server_addr.c_str());
    cJSON_AddNumberToObject(o, "server_port", cfg.server_port);
    cJSON_AddStringToObject(o, "token", cfg.token.c_str());
    cJSON_AddStringToObject(o, "local_host", "127.0.0.1");
    cJSON_AddNumberToObject(o, "local_port", local_port);
    cJSON_AddNumberToObject(o, "remote_port", remote_port);
    cJSON_AddNumberToObject(o, "remote_port_min", rmin);
    cJSON_AddNumberToObject(o, "remote_port_max", rmax);
    cJSON_AddNumberToObject(o, "http_port", kHealthPort);
    cJSON_AddStringToObject(o, "base_path", "/");
    std::string proxy = "ssh-bridge-" + std::to_string(GetCurrentProcessId()) + "-" + suffix;
    cJSON_AddStringToObject(o, "proxy_name", proxy.c_str());
    cJSON* eadk = cJSON_AddObjectToObject(o, "eadk");
    cJSON_AddStringToObject(eadk, "tool", "tunnel");
    return o;
}

}  // namespace

struct Proc {
    PROCESS_INFORMATION pi{};
    bool active = false;
    int restarts = 0;
    int local_port = 0;
    int remote_port = 0;
    std::string runtime;
    std::string error;
};

struct TunnelImpl {
    std::atomic<bool> stop{false};
    mutable std::mutex mu;
    TunnelConfig cfg;
    std::string root;
    std::string exe;
    Proc http;
    Proc ssh;
    std::string last_error;
    std::thread monitor;

    ~TunnelImpl() {
        stop.store(true);
        if (monitor.joinable()) monitor.join();
        stop_proc(http);
        stop_proc(ssh);
    }

    static void close_proc(Proc& p) {
        if (p.active) {
            WaitForSingleObject(p.pi.hProcess, 0);
            CloseHandle(p.pi.hThread);
            CloseHandle(p.pi.hProcess);
            p.pi = PROCESS_INFORMATION{};
            p.active = false;
        }
    }

    static void stop_proc(Proc& p) {
        if (p.active) {
            TerminateProcess(p.pi.hProcess, 0);
            WaitForSingleObject(p.pi.hProcess, 3000);
            CloseHandle(p.pi.hThread);
            CloseHandle(p.pi.hProcess);
            p.pi = PROCESS_INFORMATION{};
            p.active = false;
        }
    }

    bool launch(Proc& p, const std::string& suffix, int local_port, int remote_port, int rmin,
                int rmax) {
        p.error.clear();
        p.local_port = local_port;
        p.remote_port = remote_port;
        if (p.runtime.empty()) {
            p.runtime = (std::filesystem::u8path(root) / "work" / "tunnel" /
                         (std::string("tunnel_") + suffix + ".json"))
                            .u8string();
        }
        std::string j = json_text(make_runtime(cfg, local_port, remote_port, rmin, rmax, suffix));
        if (!write_text(p.runtime, j)) {
            p.error = "写入隧道配置失败：" + p.runtime;
            return false;
        }
        std::string cmd = "\"" + exe + "\" -c \"" + p.runtime + "\"";
        std::vector<char> cmdline(cmd.begin(), cmd.end());
        cmdline.push_back('\0');
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, root.c_str(), &si, &pi)) {
            p.error = "启动隧道进程失败（" + std::to_string(GetLastError()) + "）";
            return false;
        }
        p.pi = pi;
        p.active = true;
        return true;
    }

    bool proc_alive(Proc& p) {
        if (!p.active) return false;
        DWORD code = 0;
        if (!GetExitCodeProcess(p.pi.hProcess, &code)) return false;
        return code == STILL_ACTIVE;
    }

    void relaunch(Proc& p, bool is_http) {
        if (p.restarts >= kMaxRestarts) {
            p.error = "隧道进程连续退出，已停止自动重启";
            return;
        }
        close_proc(p);
        ++p.restarts;
        bool ok = is_http ? launch(p, "http", cfg.http_local_port,
                                   cfg.http_remote_port > 0 ? cfg.http_remote_port : 0, kHttpMin,
                                   kHttpMax)
                          : launch(p, "ssh", cfg.ssh_local_port,
                                   cfg.ssh_remote_port > 0 ? cfg.ssh_remote_port : 0, kSshMin,
                                   kSshMax);
        if (!ok) last_error = p.error;
    }

    void monitor_loop(std::shared_ptr<TunnelImpl> self) {
        while (!self->stop.load()) {
            for (int i = 0; i < 10 && !self->stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (self->stop.load()) break;
            std::lock_guard<std::mutex> g(self->mu);
            if (self->http.active && !proc_alive(self->http)) relaunch(self->http, true);
            if (self->ssh.active && !proc_alive(self->ssh)) relaunch(self->ssh, false);
        }
    }
};

TunnelManager::TunnelManager() : impl_(std::make_shared<TunnelImpl>()) {}

TunnelManager::~TunnelManager() {
    stop();
    impl_.reset();
}

bool TunnelManager::start(const TunnelConfig& cfg, const std::string& project_root,
                          std::string* err) {
    stop();
    if (!cfg.enable) {
        if (err) err->clear();
        return true;
    }
    if (!cfg.http_enable && !cfg.ssh_enable) {
        if (err) *err = "隧道至少需要开启 HTTP 或入站 SSH 映射之一";
        return false;
    }
    if (cfg.server_addr.empty() || cfg.server_port <= 0 || cfg.server_port > 65535) {
        if (err) *err = "隧道 server_addr / server_port 未配置";
        return false;
    }
    if (!cfg.http_enable && cfg.http_local_port <= 0) {
        if (err) *err = "隧道 HTTP 本地端口非法";
        return false;
    }
    if (cfg.http_remote_port > 0 && (cfg.http_remote_port < kHttpMin || cfg.http_remote_port > kHttpMax)) {
        if (err) *err = "隧道 HTTP 公网端口必须在 18000–18999";
        return false;
    }
    if (cfg.ssh_enable) {
        if (cfg.ssh_local_port <= 0) {
            if (err) *err = "隧道入站 SSH 本地端口非法";
            return false;
        }
        if (cfg.ssh_remote_port > 0 &&
            (cfg.ssh_remote_port < kSshMin || cfg.ssh_remote_port > kSshMax)) {
            if (err) *err = "隧道 SSH 公网端口必须在 28000–28999";
            return false;
        }
    }

    std::string exe = find_exe(cfg.exe_path, project_root);
    if (exe.empty()) {
        if (err) *err = "未找到隧道程序（eadk-tunnel.exe / frpc.exe）";
        return false;
    }
    try {
        std::filesystem::create_directories(std::filesystem::u8path(project_root) / "work" /
                                            "tunnel");
    } catch (...) {
    }

    std::shared_ptr<TunnelImpl> n = std::make_shared<TunnelImpl>();
    n->cfg = cfg;
    n->root = project_root;
    n->exe = exe;
    bool any = false;
    if (cfg.http_enable) {
        any = n->launch(n->http, "http", cfg.http_local_port,
                        cfg.http_remote_port > 0 ? cfg.http_remote_port : 0, kHttpMin, kHttpMax) ||
              any;
        if (!n->http.active) n->last_error = n->http.error;
    }
    if (cfg.ssh_enable) {
        any = n->launch(n->ssh, "ssh", cfg.ssh_local_port,
                        cfg.ssh_remote_port > 0 ? cfg.ssh_remote_port : 0, kSshMin, kSshMax) ||
              any;
        if (!n->ssh.active && n->last_error.empty()) n->last_error = n->ssh.error;
    }
    if (!any) {
        std::string msg = n->last_error.empty() ? "隧道进程启动失败" : n->last_error;
        if (err) *err = msg;
        return false;
    }

    n->monitor = std::thread([n]() { n->monitor_loop(n); });
    std::shared_ptr<TunnelImpl> old = impl_;
    impl_ = n;
    if (old) {
        old->stop.store(true);
        if (old->monitor.joinable()) old->monitor.join();
        old.reset();
    }
    if (err) {
        std::string warn;
        {
            std::lock_guard<std::mutex> g(impl_->mu);
            warn = impl_->last_error;
        }
        *err = warn;  // 部分成功时作为提示；返回 true
    }
    return true;
}

void TunnelManager::stop() {
    if (!impl_) return;
    std::shared_ptr<TunnelImpl> old = impl_;
    impl_ = std::make_shared<TunnelImpl>();
    old->stop.store(true);
    if (old->monitor.joinable()) old->monitor.join();
    old.reset();
}

bool TunnelManager::running() const {
    if (!impl_) return false;
    std::lock_guard<std::mutex> g(impl_->mu);
    return impl_->http.active || impl_->ssh.active;
}

std::string TunnelManager::status_json() const {
    cJSON* o = cJSON_CreateObject();
    bool http_on = false, ssh_on = false;
    DWORD http_pid = 0, ssh_pid = 0;
    int http_local = 0, http_remote = 0, ssh_local = 0, ssh_remote = 0;
    int http_restarts = 0, ssh_restarts = 0;
    std::string http_err, ssh_err, last;
    if (impl_) {
        std::lock_guard<std::mutex> g(impl_->mu);
        http_on = impl_->http.active;
        ssh_on = impl_->ssh.active;
        http_pid = http_on ? impl_->http.pi.dwProcessId : 0;
        ssh_pid = ssh_on ? impl_->ssh.pi.dwProcessId : 0;
        http_local = impl_->http.local_port;
        http_remote = impl_->http.remote_port;
        ssh_local = impl_->ssh.local_port;
        ssh_remote = impl_->ssh.remote_port;
        http_restarts = impl_->http.restarts;
        ssh_restarts = impl_->ssh.restarts;
        http_err = impl_->http.error;
        ssh_err = impl_->ssh.error;
        last = impl_->last_error;
    }
    bool healthy = probe_port(kHealthPort, 200);
    cJSON_AddBoolToObject(o, "on", http_on || ssh_on);
    cJSON_AddStringToObject(o, "exe", impl_ ? impl_->exe.c_str() : "");
    cJSON_AddNumberToObject(o, "health_port", kHealthPort);
    cJSON_AddBoolToObject(o, "healthy", healthy);
    cJSON_AddStringToObject(o, "last_error", last.c_str());
    cJSON* h = cJSON_AddObjectToObject(o, "http");
    cJSON_AddBoolToObject(h, "on", http_on);
    cJSON_AddNumberToObject(h, "pid", http_pid);
    cJSON_AddNumberToObject(h, "local_port", http_local);
    cJSON_AddNumberToObject(h, "remote_port", http_remote);
    cJSON_AddBoolToObject(h, "auto_port", http_remote == 0);
    cJSON_AddNumberToObject(h, "restarts", http_restarts);
    cJSON_AddStringToObject(h, "error", http_err.c_str());
    cJSON* s = cJSON_AddObjectToObject(o, "ssh");
    cJSON_AddBoolToObject(s, "on", ssh_on);
    cJSON_AddNumberToObject(s, "pid", ssh_pid);
    cJSON_AddNumberToObject(s, "local_port", ssh_local);
    cJSON_AddNumberToObject(s, "remote_port", ssh_remote);
    cJSON_AddBoolToObject(s, "auto_port", ssh_remote == 0);
    cJSON_AddNumberToObject(s, "restarts", ssh_restarts);
    cJSON_AddStringToObject(s, "error", ssh_err.c_str());
    char* txt = cJSON_PrintUnformatted(o);
    std::string out = txt ? txt : "{}";
    cJSON_free(txt);
    cJSON_Delete(o);
    return out;
}

}  // namespace sb3
