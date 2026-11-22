// board_action_runner.h — S12 板卡按钮执行器（继电器 / 板端命令 / 本机命令 / Lua）。
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

#include "core/bridge/bridge.h"
#include "core/config/config.h"

namespace sb3 {

struct BoardActionResult {
    bool        ok = false;
    std::string message;
    std::string output;
    int         exit_code = 0;
    double      elapsed_ms = 0;
    // 继电器动作回执：供 UI 把按钮执行结果同步到继电器状态面板
    bool        relay = false;
    std::string relay_port;
    std::string relay_name;
    std::string relay_mode;
    int         relay_channel = 0;
    bool        relay_on = false;
};

using PortInUseFn = std::function<bool(const std::string& port)>;

// 同步执行一个按钮；script 的 action() 会以 depth+1 递归调用。
// progress 非空时，脚本 print / 命令输出每行实时回调（可能来自后台线程）。
BoardActionResult run_board_action(const AppConfig& cfg, const std::string& channel,
                                   const BoardActionConfig& action, IBridge* bridge,
                                   const PortInUseFn& port_in_use, int depth = 0,
                                   const std::function<void(const std::string&)>& progress = {});

// 后台执行；完成后向 notify 窗口发送 msg，lParam 是 new AsyncBoardResult*，接收方 delete。
struct AsyncBoardResult {
    bool        ok = false;
    std::string message;
    std::string output;
    std::string channel;
    std::string action_id;
    std::string action_label;
    double      elapsed_ms = 0;
    bool        relay = false;
    std::string relay_port;
    std::string relay_name;
    std::string relay_mode;
    int         relay_channel = 0;
    bool        relay_on = false;
};

void run_board_action_async(const AppConfig& cfg, const std::string& channel,
                            const BoardActionConfig& action, std::shared_ptr<IBridge> bridge,
                            const PortInUseFn& port_in_use, HWND notify, UINT msg,
                            UINT progress_msg = 0);

// 实时输出提示；progress_msg 非 0 时 PostMessage 给 notify，接收方 delete。
struct AsyncBoardProgress {
    std::string channel;
    std::string action_id;
    std::string line;
};

}  // namespace sb3
