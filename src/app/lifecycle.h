// lifecycle.h — 进程生命周期（S3）：DPI、单实例、消息循环。

#pragma once

#include <windows.h>

namespace sb3 {

void enable_dpi_awareness();

// true = 本进程拿到单实例；false = 已有实例在跑（本函数已尝试把旧窗口拉起来）
bool acquire_single_instance();
void release_single_instance();

// 带菜单快捷键表的消息循环（TranslateAccelerator 必须在循环里）
int run_message_loop(HWND main, HACCEL accel);

}  // namespace sb3
