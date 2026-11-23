#include "core/bridge/powershell/powershell_bridge.h"

#include <windows.h>

#include <cjson/cJSON.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/util/util.h"

namespace sb3 {
namespace {

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string base64(const std::vector<unsigned char>& in) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        unsigned int v = static_cast<unsigned int>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned int>(in[i + 1]) << 8;
        if (i + 2 < in.size()) v |= static_cast<unsigned int>(in[i + 2]);
        out.push_back(kTable[(v >> 18) & 0x3f]);
        out.push_back(kTable[(v >> 12) & 0x3f]);
        out.push_back(i + 1 < in.size() ? kTable[(v >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < in.size() ? kTable[v & 0x3f] : '=');
    }
    return out;
}

// 持久 REPL：stdin 读一行，Invoke-Expression 后把纯文本写回 stdout。
// -EncodedCommand 避免引号/换行在命令行里被拆坏；stderr 由父进程接 NUL。
const char* repl_script() {
    return
        "$ErrorActionPreference='Continue'\r\n"
        "[Console]::OutputEncoding=[Text.Encoding]::UTF8\r\n"
        "$OutputEncoding=[Text.Encoding]::UTF8\r\n"
        "while($true){\r\n"
        "  [Console]::Out.Write('PS> ');[Console]::Out.Flush()\r\n"
        "  $line=[Console]::In.ReadLine()\r\n"
        "  if($null -eq $line){break}\r\n"
        "  if($line.Trim() -eq 'exit'){break}\r\n"
        "  try{\r\n"
        "    $r=Invoke-Expression $line 2>&1 | Out-String -Width 4096\r\n"
        "    if($r.Length -gt 0){[Console]::Out.Write($r);[Console]::Out.Flush()}\r\n"
        "  }catch{[Console]::Out.WriteLine('ERR: '+$_.Exception.Message);"
        "[Console]::Out.Flush()}\r\n"
        "}\r\n";
}

std::string encoded_repl() {
    static const std::string kEncoded = [] {
        const char* text = repl_script();
        std::size_t n = std::strlen(text);
        std::vector<unsigned char> u16;
        u16.reserve((n + 1) * 2);
        for (std::size_t i = 0; i < n; ++i) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c < 0x80) {
                u16.push_back(c);
                u16.push_back(0);
            } else {
                // 脚本本身是 ASCII；出现非 ASCII 时按 UTF-8 逐字节保留，避免静默丢内容。
                u16.push_back(c);
                u16.push_back(0);
            }
        }
        return base64(u16);
    }();
    return kEncoded;
}

std::string quote_shell(const std::string& shell) {
    if (shell.empty()) return "\"powershell.exe\"";
    std::string out = "\"";
    out += shell;
    out += "\"";
    return out;
}

}  // namespace

PowerShellBridge::PowerShellBridge(const PowerShellConfig& cfg, std::string project_root)
    : cfg_(cfg), project_root_(std::move(project_root)) {
    std::size_t cap = cfg_.buffer_size ? cfg_.buffer_size : (1u << 20);
    std::string log_dir = cfg_.log_dir;
    if (log_dir.empty()) log_dir = util::join_path(project_root_, "work\\logs");
    core_.init(cfg_.name.empty() ? std::string("powershell") : cfg_.name, cap, cfg_.timestamp,
               cfg_.log_enabled, log_dir, cfg_.name);
}

PowerShellBridge::~PowerShellBridge() {
    stop();
    core_.shutdown();
}

std::string PowerShellBridge::build_command_line() const {
    std::string cmd = quote_shell(cfg_.shell);
    if (!cfg_.args.empty()) cmd += " " + cfg_.args;
    cmd += " -EncodedCommand ";
    cmd += encoded_repl();
    return cmd;
}

bool PowerShellBridge::start() {
    if (rx_thread_.joinable() || tx_thread_.joinable() || process_) stop();
    stop_ = false;
    io_alive_ = false;
    process_alive_ = false;
    cmds_.reset();
    {
        std::lock_guard<std::mutex> lk(err_mu_);
        error_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(input_mu_);
        line_.clear();
        skip_lf_ = false;
    }
    set_state("starting");

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE in_r = nullptr;
    HANDLE in_w = nullptr;
    HANDLE out_r = nullptr;
    HANDLE out_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) {
        if (in_r) CloseHandle(in_r);
        if (in_w) CloseHandle(in_w);
        if (out_r) CloseHandle(out_r);
        if (out_w) CloseHandle(out_w);
        set_error("CreatePipe failed");
        set_state("offline");
        return false;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, 0, &sa, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        CloseHandle(in_r);
        CloseHandle(in_w);
        CloseHandle(out_r);
        CloseHandle(out_w);
        set_error("open NUL failed");
        set_state("offline");
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = nul;

    std::wstring wcmd = to_wide(build_command_line());
    std::vector<wchar_t> mutable_cmd(wcmd.begin(), wcmd.end());
    mutable_cmd.push_back(L'\0');
    std::wstring wcwd = to_wide(cfg_.cwd);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr,
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(nul);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "CreateProcess failed (error %lu)", GetLastError());
        set_error(msg);
        set_state("offline");
        return false;
    }

    stdin_w_ = in_w;
    stdout_r_ = out_r;
    process_ = pi.hProcess;
    thread_ = pi.hThread;
    io_alive_ = true;
    process_alive_ = true;
    set_state("ready");
    rx_thread_ = std::thread(&PowerShellBridge::rx_loop, this);
    tx_thread_ = std::thread(&PowerShellBridge::tx_loop, this);
    return true;
}

void PowerShellBridge::stop() {
    if (!rx_thread_.joinable() && !tx_thread_.joinable() && !process_) {
        stop_ = false;
        return;
    }
    stop_ = true;
    io_alive_ = false;
    cmds_.close();
    if (tx_thread_.joinable()) tx_thread_.join();
    {
        std::lock_guard<std::mutex> lk(input_mu_);
        if (stdin_w_) {
            CloseHandle(static_cast<HANDLE>(stdin_w_));
            stdin_w_ = nullptr;
        }
    }
    if (process_) {
        HANDLE p = static_cast<HANDLE>(process_);
        if (WaitForSingleObject(p, 300) == WAIT_TIMEOUT) TerminateProcess(p, 0);
        WaitForSingleObject(p, 1000);
    }
    if (stdout_r_) CancelIoEx(static_cast<HANDLE>(stdout_r_), nullptr);
    if (rx_thread_.joinable()) rx_thread_.join();
    cleanup_handles();
    process_alive_ = false;
    set_state("stopped");
}

bool PowerShellBridge::running() const {
    if (!process_alive_ || !io_alive_ || !process_) return false;
    HANDLE p = static_cast<HANDLE>(process_);
    return WaitForSingleObject(p, 0) != WAIT_OBJECT_0;
}

void PowerShellBridge::reconnect() {
    ++reconnects_;
    stop();
    start();
}

void PowerShellBridge::cleanup_handles() {
    if (process_) { CloseHandle(static_cast<HANDLE>(process_)); process_ = nullptr; }
    if (thread_) { CloseHandle(static_cast<HANDLE>(thread_)); thread_ = nullptr; }
    if (stdin_w_) { CloseHandle(static_cast<HANDLE>(stdin_w_)); stdin_w_ = nullptr; }
    if (stdout_r_) { CloseHandle(static_cast<HANDLE>(stdout_r_)); stdout_r_ = nullptr; }
}

void PowerShellBridge::rx_loop() {
    char buf[4096];
    while (!stop_.load()) {
        DWORD got = 0;
        BOOL ok = ReadFile(static_cast<HANDLE>(stdout_r_), buf, sizeof(buf), &got, nullptr);
        if (!ok || got == 0) break;
        core_.on_rx(buf, static_cast<std::size_t>(got));
    }
    io_alive_ = false;
    process_alive_ = false;
    if (!stop_.load()) {
        set_error("PowerShell process ended");
        set_state("offline");
    }
}

void PowerShellBridge::tx_loop() {
    while (!stop_.load()) {
        std::string out;
        if (!cmds_.pop_all(&out, 80) || out.empty()) continue;
        std::size_t sent = 0;
        while (sent < out.size() && !stop_.load()) {
            DWORD wrote = 0;
            DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(out.size() - sent, 64 * 1024));
            if (!WriteFile(static_cast<HANDLE>(stdin_w_), out.data() + sent, chunk,
                           &wrote, nullptr) || wrote == 0) {
                set_error("PowerShell stdin write failed");
                io_alive_ = false;
                return;
            }
            sent += wrote;
        }
    }
}

std::size_t PowerShellBridge::write(const void* data, std::size_t n) {
    if (data == nullptr || n == 0) return 0;
    if (!running()) return 0;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::string echo_bytes;
    std::lock_guard<std::mutex> lk(input_mu_);
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char c = p[i];
        if (skip_lf_) {
            skip_lf_ = false;
            if (c == '\n') continue;
        }
        if (c == '\r' || c == '\n') {
            echo_bytes += "\r\n";
            echo(echo_bytes);
            echo_bytes.clear();
            submit_line(line_);
            line_.clear();
            skip_lf_ = (c == '\r');
            continue;
        }
        if (c == 0x08 || c == 0x7f) {
            if (!line_.empty()) {
                pop_last_utf8();
                echo_bytes += "\b \b";
            }
            continue;
        }
        if (c == 0x1b || c < 0x20) continue;  // 方向键/控制键暂不支持
        line_.push_back(static_cast<char>(c));
        echo_bytes.push_back(static_cast<char>(c));
    }
    echo(echo_bytes);
    return n;
}

void PowerShellBridge::echo(const std::string& bytes) {
    if (!bytes.empty()) core_.on_rx(bytes.data(), bytes.size());
}

void PowerShellBridge::pop_last_utf8() {
    if (line_.empty()) return;
    std::size_t pos = line_.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(line_[pos]) & 0xC0) == 0x80) --pos;
    line_.erase(pos);
}

void PowerShellBridge::submit_line(const std::string& line) {
    std::string payload = line;
    payload += "\r\n";
    if (cmds_.push(payload.data(), payload.size()))
        core_.mark_tx(payload.size());
}

bool PowerShellBridge::resize_terminal(int, int) { return false; }

std::string PowerShellBridge::read_all(std::uint64_t* epoch, std::uint64_t* seen) {
    std::string out;
    core_.log().read_all(&out);
    if (epoch) *epoch = core_.log().epoch();
    if (seen) *seen = core_.log().total();
    return out;
}

std::string PowerShellBridge::read_since(std::uint64_t* epoch, std::uint64_t* seen) {
    return core_.log().read_since(epoch, seen, nullptr);
}

std::string PowerShellBridge::read_since_ex(std::uint64_t* epoch, std::uint64_t* seen,
                                            bool* dropped) {
    return core_.log().read_since(epoch, seen, dropped);
}

void PowerShellBridge::clear() { core_.log().clear(); }

void PowerShellBridge::set_disk_log(bool enabled) {
    std::string dir = cfg_.log_dir;
    if (dir.empty()) dir = util::join_path(project_root_, "work\\logs");
    core_.log().set_disk(enabled, dir, cfg_.name);
}

void PowerShellBridge::set_kick(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(kick_mu_);
    kick_ = std::move(fn);
    core_.set_kick([this] {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> k(kick_mu_);
            cb = kick_;
        }
        if (cb) cb();
    });
}

void PowerShellBridge::apply_config(const PowerShellConfig& cfg) {
    stop();
    cfg_ = cfg;
    std::size_t cap = cfg_.buffer_size ? cfg_.buffer_size : (1u << 20);
    std::string log_dir = cfg_.log_dir;
    if (log_dir.empty()) log_dir = util::join_path(project_root_, "work\\logs");
    core_.reinit(cfg_.name.empty() ? std::string("powershell") : cfg_.name, cap, cfg_.timestamp,
                 cfg_.log_enabled, log_dir, cfg_.name);
    {
        std::lock_guard<std::mutex> lk(err_mu_);
        error_.clear();
    }
    start();
}

int PowerShellBridge::pid() const {
    if (!process_) return 0;
    return static_cast<int>(GetProcessId(static_cast<HANDLE>(process_)));
}

std::string PowerShellBridge::status_json() const {
    cJSON* o = cJSON_CreateObject();
    if (!o) return "{}";
    cJSON_AddStringToObject(o, "kind", "powershell");
    cJSON_AddStringToObject(o, "name", cfg_.name.c_str());
    cJSON_AddStringToObject(o, "state", state().c_str());
    cJSON_AddStringToObject(o, "shell", cfg_.shell.c_str());
    cJSON_AddNumberToObject(o, "pid", pid());
    cJSON_AddBoolToObject(o, "running", running());
    cJSON_AddNumberToObject(o, "echo_count", static_cast<double>(core_.echo_count()));
    cJSON_AddNumberToObject(o, "tx_bytes", static_cast<double>(core_.tx_bytes()));
    cJSON_AddNumberToObject(o, "rx_bytes", static_cast<double>(core_.rx_bytes()));
    cJSON_AddNumberToObject(o, "queue", static_cast<double>(cmds_.depth()));
    cJSON_AddNumberToObject(o, "reconnects", reconnects_);
    cJSON_AddStringToObject(o, "last_error", last_error().c_str());
    char* text = cJSON_PrintUnformatted(o);
    std::string out = text ? text : "{}";
    if (text) cJSON_free(text);
    cJSON_Delete(o);
    return out;
}

void PowerShellBridge::set_error(const std::string& msg) {
    std::lock_guard<std::mutex> lk(err_mu_);
    error_ = msg;
}

std::string PowerShellBridge::last_error() const {
    std::lock_guard<std::mutex> lk(err_mu_);
    return error_;
}

void PowerShellBridge::set_state(const std::string& s) {
    std::lock_guard<std::mutex> lk(err_mu_);
    state_ = s;
}

std::string PowerShellBridge::state() const {
    std::lock_guard<std::mutex> lk(err_mu_);
    return state_;
}

}  // namespace sb3
