// lifecycle.cpp — 单实例、DPI、消息循环（S3）。
// 退出序列在 Shell 的 WM_DESTROY 里完成（停通道 → join → 释放串口 → PostQuitMessage(0)）。

#include "app/lifecycle.h"

#include <windows.h>


namespace sb3 {
namespace {

HANDLE g_instance = nullptr;

constexpr DWORD kDpiAwarePerMonitorV2 = 0x00000004;  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

}  // namespace

void enable_dpi_awareness() {
    // Win10 1703+ 的 Per-Monitor V2：动态取，取不到再退回系统级 DPI aware
    using SetFn = BOOL(WINAPI*)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetFn set_ctx = reinterpret_cast<SetFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (set_ctx && set_ctx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(kDpiAwarePerMonitorV2)))) {
        return;
    }
    SetProcessDPIAware();
}

bool acquire_single_instance() {
    g_instance = CreateMutexW(nullptr, TRUE, L"Local\\SSHBridgeDesktopSingleInstance");
    if (!g_instance) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND old = FindWindowW(L"SB3_MainWindow", nullptr);
        if (old) {
            ShowWindow(old, SW_RESTORE);
            SetForegroundWindow(old);
        }
        CloseHandle(g_instance);
        g_instance = nullptr;
        return false;
    }
    return true;
}

void release_single_instance() {
    if (g_instance) {
        ReleaseMutex(g_instance);
        CloseHandle(g_instance);
        g_instance = nullptr;
    }
}

int run_message_loop(HWND main, HACCEL accel) {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (accel && main && TranslateAcceleratorW(main, accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

}  // namespace sb3
