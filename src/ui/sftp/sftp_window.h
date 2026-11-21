// sftp_window.h — S7 文件传输面板：本地/远端双栏、队列、进度、取消、MD5。
// 独立顶层窗口（非模态）；SFTP 用独立 libssh 会话，不抢终端 shell。

#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app_core.h"
#include "core/ssh/sftp_client.h"

namespace sb3 {

namespace sftp_ui {
inline constexpr wchar_t kClass[] = L"SB3_SftpWindow";
inline constexpr wchar_t kHistoryClass[] = L"SB3_SftpHistory";
inline constexpr wchar_t kSettingsClass[] = L"SB3_SftpSettings";

enum Id {
    kIdChannel      = 1101,
    kIdConnect      = 1102,
    kIdLocalPath    = 1110,
    kIdLocalList    = 1111,
    kIdLocalRefresh = 1112,
    kIdLocalUp      = 1113,
    kIdRemotePath   = 1120,
    kIdRemoteList   = 1121,
    kIdRemoteRefresh= 1122,
    kIdRemoteUp     = 1123,
    kIdUpload       = 1130,
    kIdDownload     = 1131,
    kIdCancel       = 1132,
    kIdClose        = 1133,
    kIdProgress     = 1140,
    kIdStatus       = 1141,
    kIdQueue        = 1142,
    kIdLocalLabel   = 1150,
    kIdRemoteLabel  = 1151,
    kIdChannelLabel = 1152,
    kIdNameLabel    = 1160,
    kIdName         = 1161,
    kIdMkdir        = 1162,
    kIdRename       = 1163,
    kIdDelete       = 1164,
    kIdLocalInfo    = 1170,
    kIdRemoteInfo   = 1171,
    kIdRetry        = 1180,
    kIdHistoryOpen  = 1190,
    kIdSettingsOpen = 1191,
    kIdHistSummary  = 1200,
    kIdHistList     = 1201,
    kIdHistDetail   = 1202,
    kIdHistClear    = 1203,
    kIdHistClose    = 1204,
    kIdSetSpeed     = 1210,
    kIdSetNote      = 1211,
    kIdSetSave      = 1212,
    kIdSetClose     = 1213,
    kIdSetLabel     = 1214,
};
}  // namespace sftp_ui

class SftpWindow {
public:
    bool open(HINSTANCE inst, HWND parent, AppCore* core, HFONT font, int dpi,
              const std::string& channel);
    void close();
    bool is_open() const { return hwnd_ && IsWindow(hwnd_); }
    HWND hwnd() const { return hwnd_; }
    std::string state_json();
    void refresh_local();
    void refresh_remote();

private:
    struct Task {
        enum Kind { Connect, List, Upload, Download, UploadTree, DownloadTree, Mkdir, Rename, Delete } kind = List;
        std::string local;
        std::string remote;
        std::string remote2;
        int retry_count = 0;
    };

    struct TransferRecord {
        std::string time;
        std::string dir;      // 上传 / 下载
        std::string name;
        std::string result;   // 成功 / 失败 / 已取消
        std::string detail;
        std::uint64_t bytes = 0;
        std::uint64_t total = 0;
        std::uint64_t elapsed_ms = 0;
        double speed_bps = 0;
        int retries = 0;
        bool tree = false;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK HistoryWndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK SettingsWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    LRESULT handle_history(UINT, WPARAM, LPARAM);
    LRESULT handle_settings(UINT, WPARAM, LPARAM);
    void build(const std::string& channel);
    void layout();
    void on_command(int id, int code);
    void show_history();
    void refresh_history_list();
    void clear_history();
    void show_settings();
    void layout_secondary_windows();
    void update_settings_note();
    void apply_settings();
    void append_history(bool upload, bool tree, const std::string& name,
                        std::uint64_t bytes, std::uint64_t total,
                        std::uint64_t elapsed_ms, bool ok, bool cancelled,
                        const std::string& detail, int retries);
    std::vector<TransferRecord> history_snapshot() const;
    void start_worker();
    void stop_worker();
    void enqueue(Task t);
    void worker_loop();
    void do_connect(const std::string& initial_path);
    void do_list(const std::string& path, bool announce = true);
    void do_transfer(const Task& t);
    void do_transfer_tree(const Task& t, bool upload);
    void do_remote_op(const Task& t);
    void reset_delete_confirm();
    void post_event(int code);
    void apply_remote_snapshot();
    void update_progress_ui();
    void set_status(const std::wstring& text);
    std::wstring status_text() const;
    std::string channel_name();
    std::string local_dir() const;
    std::string remote_dir();
    std::string current_local_dir() const;
    bool selected_local(std::string* path, bool* is_dir) const;
    bool selected_remote(std::string* name, bool* is_dir) const;
    const SftpEntry* selected_remote_entry() const;
    void update_local_info();
    void update_remote_info();
    void ensure_target();

    AppCore*     core_ = nullptr;
    HINSTANCE    inst_ = nullptr;
    HWND         hwnd_ = nullptr;
    HWND         parent_ = nullptr;
    HFONT        font_ = nullptr;
    int          dpi_ = 96;

    SftpClient   client_;
    SshTarget    target_;
    std::string  home_;
    std::string  remote_path_;
    bool         local_ready_ = false;
    bool         delete_armed_ = false;
    DWORD        delete_arm_ms_ = 0;

    std::thread  worker_;
    std::mutex   queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<Task> queue_;
    bool         quit_ = false;
    int          busy_ = 0;            // 0/1：worker 正在处理任务
    int          pending_ = 0;         // 队列里等待的任务数

    mutable std::mutex snap_mu_;
    std::vector<SftpEntry> remote_items_;
    std::string  remote_error_;
    bool         remote_ready_ = false;

    std::atomic<bool>         cancel_{false};
    std::atomic<std::uint64_t> done_{0};
    std::atomic<std::uint64_t> total_{0};
    std::atomic<int>          phase_{0};   // 0空闲 1连接 2列目录 3上传 4下载 5完成 6失败 7取消 8远端操作
    std::string               note_;       // guarded by snap_mu_
    std::string               active_name_;
    std::string               active_kind_; // upload/download/tree
    int                       active_files_ = 0;  // 递归：本次文件序号
    int                       active_total_files_ = 0;
    std::vector<std::string>  active_plan_;       // 递归：本次待传文件相对路径
    int                       active_plan_pos_ = 0;
    Task                      last_failed_;       // 最近失败的传输任务（可重试）
    bool                      has_last_failed_ = false;
    std::vector<TransferRecord> records_;        // 最近 50 次传输（旧在前）
    std::atomic<bool>         history_dirty_{true};
    HWND                      history_ = nullptr;
    HWND                      settings_ = nullptr;
    int                       active_index_ = 0;
    int                       active_count_ = 0;
    bool         connected_ = false;
    std::wstring status_;
    std::wstring flash_;
    DWORD        flash_ms_ = 0;
    HBRUSH       brush_ = nullptr;
};

}  // namespace sb3

