// config.h — 配置模型（S1 实现加载/保存/手术式 patch）。
// 与 v2 的 ssh_bridge_config 保持同构键名，便于沿用既有配置文件。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sb3 {

struct DefaultConfig {
    std::string   username    = "root";
    std::string   password;
    std::uint32_t buffer_size = 1u << 20;   // 1 MiB ring
    bool          log_enabled = true;
    std::string   work_dir;                  // 空 = 运行目录下 work
    std::string   log_dir;                   // 空 = work/logs
};

// 底部快捷指令（S13）：点一下即向该通道发送一条命令并回车。
// 与 board buttons 的区别：不跑 Lua、不操作继电器，只做最简单的通道下行。
struct QuickCommandConfig {
    std::string label;      // 按钮文字
    std::string command;    // 原样发到通道，末尾由终端补回车
};

// 板卡/SSH 通道专属按钮（S12）。同一通道内 id 唯一。
struct BoardActionConfig {
    std::string id;
    std::string label;
    bool        enable = true;
    std::string backend = "commands";   // commands / relay / script
    std::string run = "channel";        // commands: channel / host
    std::vector<std::string> commands;  // commands: 一行一条
    int         delay_ms = 200;         // commands: 每条间隔
    std::string relay;                  // relay: 设备名
    int         channel = 1;            // relay: 通道号
    std::string mode = "pulse";         // relay: hold_on / hold_off / pulse
    double      hold_seconds = 1.0;     // relay: pulse / hold 时长
    bool        polarity_inverted = false;
    std::string script;                 // script: Lua 或旧 DSL
};

struct BoardConfig {
    std::string name;
    std::string serial_device;   // "COM5"
    int         baudrate = 115200;
    int         databits = 8;
    std::string parity   = "N";  // N / E / O / M / S
    int         stopbits_x10 = 10;   // 10 / 15 / 20
    std::string flowcontrol = "none";  // none / rtscts / xonxoff
    bool        dtr = true;
    bool        rts = true;
    std::string line_ending = "crlf";  // none / cr / lf / crlf
    std::string encoding = "utf-8";    // utf-8 / gbk
    bool        auto_open = false;
    bool        timestamp = true;
    std::uint32_t buffer_size = 0;      // 0 = 用 default.buffer_size
    bool        log_enabled = true;
    std::string log_dir;
    std::string password;
    bool        password_present = false;  // true 表示配置里显式写了 password（含空串）
    std::string username = "root";
    bool        discoverable = false;  // S12：是否应答 UDP 发现（默认关闭）
    std::vector<BoardActionConfig> buttons;  // S12 板卡专属按钮
    std::vector<QuickCommandConfig> quick_commands;  // S13 底部快捷指令
};

struct SSHLinkConfig {
    std::string name;
    std::string host;
    int         port = 22;
    std::string username;
    std::string password;
    bool        password_present = false;  // 显式写了 target_password（含空串 = 免密）
    std::string term = "xterm-256color";
    int         width = 120;
    int         height = 32;
    int         connect_timeout_ms = 5000;  // SSH_OPTIONS_TIMEOUT，秒级向上取整
    int         keepalive_s = 3;            // 发 ignore 的间隔；0 = 关闭
    std::uint32_t buffer_size = 0;          // 0 = 用 default.buffer_size
    bool          log_enabled = true;
    std::string   log_dir;
    bool          discoverable = false;  // S12：是否应答 UDP 发现（默认关闭）
    std::vector<BoardActionConfig> buttons;  // S12 通道专属按钮
    std::vector<QuickCommandConfig> quick_commands;  // S13 底部快捷指令
};

struct InboundConfig {          // S8
    bool enabled = false;
    int  port    = 2222;
};

struct WebConfig {              // S9
    bool enabled = false;
    int  port    = 18081;
    std::string token;          // 空 = 只读；非空 = 写接口要求 Bearer
};

struct AgentConfig {            // S10
    bool        enabled = false;
    int         port    = 18096;
    std::string token;          // 可选：非空则所有 / 请求要带 X-Ui-Token
};

struct RelayDeviceConfig {      // S11 继电器
    std::string name;
    bool        enable = false;
    std::string windows_port;   // COM3 / COM5
    std::string linux_port;     // /dev/ttyUSB0（跨平台保留）
    int         channels = 2;
    int         baudrate = 9600;
    bool        io_inverted = false;
    int         post_write_delay_ms = 100;
    bool        polarity_inverted = false;  // 脉冲：true = 脉冲突发为高
};

struct TftpConfig {             // S11 TFTP
    bool        enable = false;
    int         port = 69;
    std::string bind_ip;        // 空 = 自动
    std::string root;           // 空 = {work}/share
    bool        allow_write = true;
};

struct ShareConfig {            // S11 共享目录（HTTP）
    bool        enable = false;
    int         port = 18082;
    std::string root;           // 空 = 同 TFTP root
};

struct NfsConfig {              // S11 NFSv3
    bool        enable = false;
    std::string alias = "/share";
    int         uid = 0;
    int         gid = 0;
    std::string root;           // 空 = 同 TFTP root
};

struct TcpForwardConfig {       // S12：板 IP 末字节 → 本地端口
    bool        enable = false;
    std::string listen_ip = "0.0.0.0";
    int         port_base = 22000;
    int         target_port = 22;
    std::string board_cidr = "192.168.1.0/24";
};

struct TunnelConfig {           // S12：外部 eadk-tunnel / frpc 子进程
    bool        enable = false;
    bool        http_enable = true;
    bool        ssh_enable = false;
    std::string exe_path;       // 空 = 程序目录 / tools / output/build 自动查找
    std::string server_addr;
    int         server_port = 7000;
    std::string token;
    int         http_local_port = 18081;
    int         http_remote_port = 0;   // 0 = 18xxx 自动分配
    int         ssh_local_port = 2222;
    int         ssh_remote_port = 0;    // 0 = 28xxx 自动分配
};

struct DiscoveryConfig {        // S12：UDP 18071 应答 EADK1
    bool        enable = false;
    int         port = 18071;
};

struct UiConfig {               // S3 / S4 / S12（只写入 ssh_bridge_ui.json）
    bool close_to_tray   = true;
    bool start_minimized = false;
    int  scrollback_lines = 5000;  // 终端回看行数
    int  font_size        = 0;     // 0 = 默认 15px
    int  split            = 1;     // 1 / 2 / 4
    std::vector<std::string> pane_channels;  // 每个画框绑定的通道；空串=空画框
    int  active_pane      = 0;     // 重启后激活的画框序号（0 起）
    bool show_statusbar   = true;  // 状态栏显示/隐藏
    std::string window_rect;       // "left,top,right,bottom"；空 = 默认/旧 ini
};

struct AppConfig {
    DefaultConfig              def;
    std::vector<BoardConfig>   boards;
    std::vector<SSHLinkConfig> links;
    InboundConfig              inbound;
    WebConfig                  web;
    AgentConfig                agent;
    UiConfig                   ui;
    std::vector<RelayDeviceConfig> relays;
    TftpConfig                 tftp;
    ShareConfig                share;
    NfsConfig                  nfs;
    TcpForwardConfig           tcp_fwd;
    TunnelConfig               tunnel;
    DiscoveryConfig            discovery;

    std::string abs_path;        // 实际加载到的功能配置文件绝对路径
    std::string ui_abs_path;     // 同目录的窗口/界面配置文件绝对路径
    std::string project_root;    // 运行目录（work/keys/logs 的父目录）
};

// 加载：按 显式路径 → cwd → exe 同目录 → %USERPROFILE%\.ssh-bridge\ → 遗留 config.json。
// 功能配置同目录的 ssh_bridge_ui.json 会覆盖旧功能文件里的 ui 段；加载阶段不写盘。
bool config_load(AppConfig& out, const std::string& explicit_path = std::string());

// 返回按上述顺序实际命中的路径；无命中返回空。供诊断与自动化验证使用。
std::string config_find_path(const std::string& explicit_path = std::string());

// 写回功能配置（abs_path）与窗口配置（ui_abs_path），两份文件互不包含对方字段
bool config_save(const AppConfig& cfg);
// 只写功能配置：通道/服务/端口等；不触碰窗口配置
bool config_save_functional(const AppConfig& cfg);
// 只写窗口配置：分屏、字体、状态栏、窗口矩形等；不触碰功能配置
bool config_save_ui(const AppConfig& cfg);
// 与功能配置同目录的 ssh_bridge_ui.json 路径（用于诊断/Web 展示）
std::string config_ui_path(const AppConfig& cfg);

// 把当前已知配置导出为 JSON 文本（网页只读预览/备份用；保留已知字段全集）
std::string config_export_json(const AppConfig& cfg);
// 把网页表单回传的 JSON 解析成 AppConfig（不落盘；落盘由 AppCore::save_config_json 做）。
bool config_parse_json(const std::string& text, AppConfig* out, std::string* err);

// 手术式修改：只改指定通道，其它字段原样保留
bool config_patch_board(const std::string& path, const std::string& name,
                        const std::string& json_patch, std::string* err = nullptr);
bool config_delete_board(const std::string& path, const std::string& name,
                         std::string* err = nullptr);

// 三级密码解析：通道有 password 键（含空串）→ 用它；否则 default.password
std::string config_resolve_password(const AppConfig& cfg, const BoardConfig& board);

}  // namespace sb3
