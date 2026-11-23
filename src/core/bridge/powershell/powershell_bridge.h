// powershell_bridge.h — 本地 PowerShell 持久会话通道。
// 实现方式：CreateProcessW + 匿名管道 + -EncodedCommand REPL，
// 不依赖 ConPTY；下行按行提交，桥负责本地回显与编辑。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "core/bridge/bridge.h"
#include "core/channel_core.h"
#include "core/cmd_queue.h"
#include "core/config/config.h"

namespace sb3 {

class PowerShellBridge : public IBridge {
public:
    PowerShellBridge(const PowerShellConfig& cfg, std::string project_root);
    ~PowerShellBridge() override;

    bool start() override;
    void stop() override;
    bool running() const override;
    void reconnect() override;

    const std::string& name() const override { return cfg_.name; }
    std::size_t write(const void* data, std::size_t n) override;
    bool resize_terminal(int cols, int rows) override;
    std::uint64_t echo_latency_us() const override { return core_.echo_latency_us(); }
    std::uint64_t echo_count() const override { return core_.echo_count(); }
    std::uint64_t tx_bytes() const override { return core_.tx_bytes(); }
    std::uint64_t rx_bytes() const override { return core_.rx_bytes(); }

    std::string read_since(std::uint64_t* epoch, std::uint64_t* seen) override;
    std::string read_since_ex(std::uint64_t* epoch, std::uint64_t* seen, bool* dropped) override;
    std::string read_all(std::uint64_t* epoch, std::uint64_t* seen) override;
    std::uint64_t log_epoch() const override { return core_.log().epoch(); }
    std::uint64_t log_total() const override { return core_.log().total(); }
    std::uint64_t log_capacity() const override { return core_.log().capacity(); }
    std::uint64_t notify_seq() const override { return core_.log().notify_seq(); }

    void clear() override;
    void set_disk_log(bool enabled) override;
    bool disk_log_enabled() const override { return core_.log().disk_enabled(); }
    std::string disk_log_error() const override { return core_.log().last_error(); }

    std::string status_json() const override;
    void set_kick(std::function<void()> fn);
    void apply_config(const PowerShellConfig& cfg);
    std::string last_error() const;
    std::string state() const;
    int pid() const;

private:
    void rx_loop();
    void tx_loop();
    void submit_line(const std::string& line);
    void echo(const std::string& bytes);
    void pop_last_utf8();
    void set_error(const std::string& msg);
    void set_state(const std::string& s);
    void cleanup_handles();
    std::string build_command_line() const;

    PowerShellConfig cfg_;
    std::string project_root_;
    mutable ChannelCore core_;
    CmdQueue cmds_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> io_alive_{false};
    std::atomic<bool> process_alive_{false};
    std::thread rx_thread_;
    std::thread tx_thread_;

    void* process_ = nullptr;       // HANDLE
    void* thread_ = nullptr;        // HANDLE
    void* stdin_w_ = nullptr;       // 子进程 stdin 写端
    void* stdout_r_ = nullptr;      // 子进程 stdout 读端

    mutable std::mutex input_mu_;
    std::string line_;
    bool skip_lf_ = false;

    mutable std::mutex kick_mu_;
    std::function<void()> kick_;

    mutable std::mutex err_mu_;
    std::string error_;
    std::string state_ = "stopped";
    int reconnects_ = 0;
};

}  // namespace sb3
