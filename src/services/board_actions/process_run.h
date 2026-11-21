// process_run.h — 本机命令执行（S12 板卡按钮 / Lua host.exec 共用）。
#pragma once

#include <string>

namespace sb3 {

struct HostCommandResult {
    bool        ok = false;
    int         exit_code = -1;
    std::string output;   // stdout + stderr
    std::string error;    // 启动/超时等错误
};

// 通过 cmd.exe /c 执行，捕获输出；超时杀进程；working_dir 为空则用当前目录。
HostCommandResult run_host_command(const std::string& command, int timeout_ms,
                                   const std::string& working_dir = std::string());

}  // namespace sb3
