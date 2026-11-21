#include "services/board_actions/process_run.h"

#include <windows.h>

#include <atomic>
#include <thread>
#include <vector>

namespace sb3 {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow_utf8(const char* data, std::size_t n) {
    if (!data || n == 0) return std::string();
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(n), nullptr, 0);
    if (need <= 0) return std::string(data, n);
    std::wstring w(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(n), w.data(), need);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), need, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return std::string();
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), need, out.data(), bytes, nullptr, nullptr);
    return out;
}

}  // namespace

HostCommandResult run_host_command(const std::string& command, int timeout_ms,
                                   const std::string& working_dir) {
    HostCommandResult r;
    if (command.empty()) {
        r.error = "empty command";
        return r;
    }
    if (timeout_ms <= 0) timeout_ms = 120000;
    if (timeout_ms > 300000) timeout_ms = 300000;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        r.error = "CreatePipe failed";
        return r;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"cmd.exe /d /s /c \"chcp 65001>nul & " + widen(command) + L"\"";
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');
    std::wstring cwd = widen(working_dir);

    BOOL created = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr,
                                  cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(out_w);
    if (!created) {
        CloseHandle(out_r);
        r.error = "CreateProcess failed: " + std::to_string(GetLastError());
        return r;
    }

    std::string output;
    std::thread reader([&]() {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(out_r, buf, sizeof(buf), &got, nullptr) && got > 0) {
            if (output.size() < 1024 * 1024) {
                std::size_t room = 1024 * 1024 - output.size();
                output.append(buf, got < room ? got : room);
            }
        }
    });

    DWORD wait = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 124);
        r.error = "命令超时（" + std::to_string(timeout_ms) + " ms），已终止";
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (reader.joinable()) reader.join();
    CloseHandle(out_r);

    r.output = narrow_utf8(output.data(), output.size());
    // 去掉 chcp 可能留下的 BOM/空白行
    while (!r.output.empty() && (r.output[0] == '\r' || r.output[0] == '\n')) r.output.erase(0, 1);
    r.ok = wait != WAIT_TIMEOUT && r.exit_code == 0;
    if (!r.ok && r.error.empty()) r.error = "exit code " + std::to_string(r.exit_code);
    return r;
}

}  // namespace sb3
