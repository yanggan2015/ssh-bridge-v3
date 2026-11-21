// sftp_client.h — S7 SFTP 独立会话（不抢 shell 会话）：列目录、上传/下载、取消、MD5 校验。
// 一个实例只允许一个线程使用（传输线程独占）；取消靠原子标志 + 分块检查，真中断。

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/ssh/ssh_session.h"

namespace sb3 {

struct SftpEntry {
    std::string name;
    bool        dir  = false;
    bool        link = false;
    std::uint64_t size = 0;
    unsigned    mode = 0;     // POSIX 权限位（如 0755）
    unsigned    uid = 0;
    unsigned    gid = 0;
    std::uint32_t mtime = 0;  // 秒级时间戳（UTC）
};

class SftpClient {
public:
    SftpClient();
    ~SftpClient();
    SftpClient(const SftpClient&) = delete;
    SftpClient& operator=(const SftpClient&) = delete;

    bool connect(const SshTarget& t, std::string* err);
    void close();
    bool connected() const;
    const std::string& last_error() const;
    // 0 = 不限速；单位字节/秒，按 64 KB 块在传输线程内节流
    void set_speed_limit(std::uint64_t bytes_per_sec);

    bool list(const std::string& path, std::vector<SftpEntry>* out, std::string* err);
    bool mkdir(const std::string& path, std::string* err);
    bool unlink(const std::string& path, std::string* err);
    bool rmdir(const std::string& path, std::string* err);
    bool rename(const std::string& from, const std::string& to, std::string* err);
    std::string home() const;

    // done/total 每块更新；cancel=true 时立即停并清理临时文件，返回 false 且 err="已取消"
    bool upload(const std::string& local, const std::string& remote,
                std::atomic<bool>* cancel, std::atomic<std::uint64_t>* done,
                std::atomic<std::uint64_t>* total, std::string* err, std::string* md5_note);
    bool download(const std::string& remote, const std::string& local,
                  std::atomic<bool>* cancel, std::atomic<std::uint64_t>* done,
                  std::atomic<std::uint64_t>* total, std::string* err, std::string* md5_note);

private:
    struct Impl;
    Impl* impl_;
};

// 本地文件 MD5（16 进制小写）；失败返回空串
std::string md5_local_file(const std::string& path, bool* ok);

}  // namespace sb3
