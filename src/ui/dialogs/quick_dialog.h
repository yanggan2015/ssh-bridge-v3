// quick_dialog.h — S13 底部快捷指令的添加/编辑窗口。
// 只负责采集“按钮名称 + 下发指令”，不碰通道，保存由 Shell 调 AppCore 落盘。

#pragma once

#include <windows.h>

#include <string>

#include "core/config/config.h"

namespace sb3 {
namespace quick_dlg {

enum Id {
    kIdNameLabel = 5300,
    kIdName      = 5301,
    kIdCmdLabel  = 5302,
    kIdCmd       = 5303,
    kIdHint      = 5304,
};

// 打开模态编辑框；确定返回 true，value 被填好。owner 会被临时禁用。
bool show(HINSTANCE inst, HWND owner, HFONT font, int dpi,
          const std::wstring& title, QuickCommandConfig* value);

}  // namespace quick_dlg
}  // namespace sb3
