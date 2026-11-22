// service_manager.h — S11 服务编排：继电器 / TFTP / 共享目录 / NFS 共用一份 share。
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config/config.h"
#include "services/discovery/discovery_service.h"
#include "services/inbound/inbound_gateway.h"
#include "services/tcpfwd/tcp_forward.h"
#include "services/tunnel/tunnel_manager.h"

struct TftpSvc;

namespace sb3 {

class ShareServer;

class ServiceManager {
public:
    struct RelayChannelState {
        bool        on = false;
        bool        busy = false;
        std::string mode = "idle";
        std::string last_error;
        std::uint64_t last_ms = 0;
    };

    explicit ServiceManager(const std::string& project_root);
    ~ServiceManager();

    void apply_config(const AppConfig& cfg);   // 更新 root/设备清单，不主动重启已运行服务
    const std::string& share_root();
    std::string resolve_path(const std::string& p) const;
    void ensure_layout();

    // 三个服务；返回 false 时 err 是可读原因
    bool start_tftp(const TftpConfig& cfg, std::string* err);
    void stop_tftp();
    bool tftp_running() const;
    bool start_share(const ShareConfig& cfg, std::string* err);
    void stop_share();
    bool share_running() const;
    bool start_nfs(const NfsConfig& cfg, std::string* err);
    void stop_nfs();
    bool nfs_running() const;
    void stop_all();

    // S8 入站 SSH：进程级唯一监听；先装 resolver，再启动。
    void set_inbound_resolver(InboundGateway::Resolver resolver);
    bool start_inbound(const InboundConfig& cfg, std::string* err);
    void stop_inbound();
    bool inbound_running() const;
    std::string inbound_status() const;

    // S12：TCP 转发 / 外网隧道 / UDP 发现
    bool start_tcp_forward(const TcpForwardConfig& cfg, std::string* err);
    void stop_tcp_forward();
    bool tcp_forward_running() const;
    std::string tcp_forward_status() const;
    bool start_tunnel(const TunnelConfig& cfg, std::string* err);
    void stop_tunnel();
    bool tunnel_running() const;
    std::string tunnel_status() const;
    bool start_discovery(const DiscoveryConfig& cfg, const AppConfig& app, std::string* err);
    void stop_discovery();
    bool discovery_running() const;
    std::string discovery_status() const;

    // 继电器：同步 set 很快（一次写+短延时），脉冲放后台，不冻结 UI
    bool relay_set(const RelayDeviceConfig& dev, int channel, bool on, std::string* err);
    bool relay_pulse_async(const RelayDeviceConfig& dev, int channel, double hold_seconds);
    bool relay_run_mode_async(const RelayDeviceConfig& dev, int channel, const std::string& mode,
                              double hold_seconds, bool polarity_inverted);
    bool relay_busy() const;
    RelayChannelState relay_state(const std::string& port, int channel) const;
    // 板卡按钮/Lua 走的是同一份串口写入逻辑；执行完把结果同步进状态表，
    // 否则面板还显示“断开”，用户会以为按钮没生效。
    void relay_note_state(const std::string& port, int channel, bool on, const std::string& mode,
                          const std::string& error = std::string());
    void stop_relay_jobs();

    std::string share_url() const;
    std::string tftp_status() const;
    std::string share_status() const;
    std::string nfs_status() const;
    std::string relay_json() const;
    std::string state_json() const;
    std::string local_ip() const;

private:
    void set_error(std::string* dst, const std::string& msg);

    std::string project_root_;
    std::string share_root_;
    AppConfig cfg_;
    TftpSvc* tftp_ = nullptr;
    ShareServer* share_ = nullptr;
    bool nfs_on_ = false;
    std::string tftp_note_, share_note_, nfs_note_;

    InboundGateway inbound_;
    InboundGateway::Resolver inbound_resolver_;
    bool inbound_on_ = false;
    std::string inbound_note_;

    TcpForwardService tcp_fwd_;
    std::string tcp_fwd_note_;
    TunnelManager tunnel_mgr_;
    std::string tunnel_note_;
    DiscoveryService discovery_;
    std::string discovery_note_;

    mutable std::mutex relay_mu_;
    std::vector<std::pair<std::string, std::vector<RelayChannelState>>> relay_state_;
    std::thread relay_job_;
    std::atomic<bool> relay_busy_{false};
};

}  // namespace sb3
