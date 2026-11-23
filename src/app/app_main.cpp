// app_main.cpp — 入口：窗口模式 / --selftest 无窗口模式。
// 职责：命令行分发 + 单实例 + 装配（AppCore + Shell）+ 生命周期。
// 依赖方向：app 可依赖 core / ui / services；本文件只做装配，不写业务逻辑。

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "app/app_core.h"
#include "app/lifecycle.h"
#include "app/selftest.h"
#include "core/util/util.h"
#include "ui/shell/shell.h"

namespace {

// --selftest 在 GUI 子系统里没有 stdout：结果重定向到文件，再顺带往控制台回显
int RunSelftest(const std::vector<std::string>& args) {
    const std::string dir = "output\\selftest";
    sb3::util::ensure_dir(dir);
    const std::string path = sb3::util::join_path(dir, "result.txt");
    // freopen 失败时 stdout 会变成 NULL，后续 printf 直接崩（0xC0000005）。
    // 这里必须兜底，把 stdout 接到 NUL，宁可没有输出也不能空指针写。
    if (!std::freopen(path.c_str(), "w", stdout)) {
        (void)std::freopen("NUL", "w", stdout);
    }
    int code = sb3::selftest_main(args);
    std::fflush(stdout);

    // 关键：不能 fclose(stdout) 之后再 freopen——对已关闭的流 freopen 是未定义行为。
    // 直接用 freopen 把 stdout 从结果文件上摘下来。
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (std::freopen("CONOUT$", "w", stdout)) {
            std::FILE* in = std::fopen(path.c_str(), "rb");
            if (in) {
                char buf[1024];
                std::size_t n = 0;
                while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
                    std::fwrite(buf, 1, n, stdout);
                }
                std::fclose(in);
            }
            std::fflush(stdout);
        }
    } else {
        (void)std::freopen("NUL", "w", stdout);
    }
    return code;
}

bool ParseSelftest(int argc, wchar_t** argv, std::vector<std::string>* args) {
    args->clear();
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring w(argv[i]);
        if (!found && (w == L"--selftest" || w == L"-selftest")) {
            found = true;
            continue;
        }
        if (found) {
            std::string s(w.begin(), w.end());
            args->push_back(s);
        }
    }
    return found;
}

std::wstring to_wide_path(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string current_dir() {
    wchar_t buf[32768] = L"";
    DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), buf);
    if (n == 0) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string s(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), s.data(), need, nullptr, nullptr);
    return s;
}

// 启动诊断：窗口路径没有 stdout，出问题时靠这份小日志定位（每次启动追加）
void boot_log(const char* step) {
    sb3::util::ensure_dir("output");
    std::FILE* f = std::fopen("output\\boot.log", "a");
    if (!f) return;
    std::fprintf(f, "[%s] %s (err=%lu)\n", sb3::util::log_time_text().c_str(), step,
                 static_cast<unsigned long>(GetLastError()));
    std::fclose(f);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nShow) {
    (void)lpCmdLine;
    sb3::enable_dpi_awareness();

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> sargs;
    bool selftest = argv && ParseSelftest(argc, argv, &sargs);
    if (argv) LocalFree(argv);
    if (selftest) return RunSelftest(sargs);

    boot_log("start");
    if (!sb3::acquire_single_instance()) {
        boot_log("single-instance: already running");
        return 0;
    }
    boot_log("mutex acquired");

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    sb3::AppCore core;
    core.init(current_dir());
    // 进程工作目录切到配置所在目录：绿色版从任意位置双击启动时，
    // output\ 下的状态/日志/窗口偏好都落在包目录，而不是 System32。
    if (!core.config().project_root.empty()) {
        SetCurrentDirectoryW(to_wide_path(core.config().project_root).c_str());
    }
    boot_log(("core.init config=" + core.config().abs_path).c_str());
    core.start_auto();
    boot_log("core.start_auto done");

    sb3::Shell shell;
    int show = (nShow == SW_SHOWMINIMIZED || nShow == SW_SHOWMINNOACTIVE) ? SW_SHOW : nShow;
    if (core.config().ui.start_minimized) show = SW_HIDE;   // 托盘/服务模式：窗口不弹出
    if (!shell.create(hInst, show, &core)) {
        boot_log("shell.create FAILED");
        core.stop_all();
        sb3::release_single_instance();
        return 1;
    }

    boot_log("shell.create ok, entering loop");
    int code = sb3::run_message_loop(shell.hwnd(), shell.accel());
    boot_log(("loop exit code=" + std::to_string(code)).c_str());
    sb3::release_single_instance();
    return code;
}
