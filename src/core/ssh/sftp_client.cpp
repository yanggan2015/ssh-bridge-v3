// sftp_client.cpp — S7：独立 libssh SFTP 会话 + 分块传输（可取消）+ MD5 校验。
// 线程约定：实例只被传输线程使用；UI 只读原子进度。

#include "core/ssh/sftp_client.h"

#include <winsock2.h>
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <thread>

namespace sb3 {
namespace {

constexpr std::size_t kChunk = 64 * 1024;

std::string err_of(ssh_session s, const char* prefix) {
    std::string text = prefix ? prefix : "";
    if (!text.empty()) text += ": ";
    const char* e = s ? ssh_get_error(s) : nullptr;
    text += (e && *e) ? e : "unknown";
    return text;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// 按已发送字节把耗时补齐到 limit 允许的速度；20 ms 切片便于及时响应取消。
void throttle_to(std::uint64_t sent, std::chrono::steady_clock::time_point start,
                 std::uint64_t limit, std::atomic<bool>* cancel) {
    if (limit == 0 || sent == 0) return;
    const auto target = std::chrono::microseconds(
        static_cast<std::chrono::microseconds::rep>(sent * 1000000ull / limit));
    while (!(cancel && cancel->load(std::memory_order_relaxed))) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= target) break;
        auto left = target - elapsed;
        auto nap = left > std::chrono::milliseconds(20) ? std::chrono::milliseconds(20) : left;
        std::this_thread::sleep_for(nap);
    }
}

}  // namespace

struct SftpClient::Impl {
    ssh_session  session = nullptr;
    sftp_session sftp    = nullptr;
    std::string  error;
    std::atomic<bool> ready{false};
    std::atomic<std::uint64_t> speed_limit{0};
};

SftpClient::SftpClient() : impl_(new Impl()) {}
SftpClient::~SftpClient() {
    close();
    delete impl_;
}

bool SftpClient::connected() const { return impl_ && impl_->ready.load(std::memory_order_acquire); }
std::string SftpClient::last_error() const { return impl_->error; }
void SftpClient::set_speed_limit(std::uint64_t bytes_per_sec) {
    impl_->speed_limit.store(bytes_per_sec, std::memory_order_relaxed);
}

bool SftpClient::connect(const SshTarget& t, std::string* err) {
    close();
    if (!ssh_runtime_init()) {
        impl_->error = "libssh 初始化失败";
        if (err) *err = impl_->error;
        return false;
    }
    ssh_session s = ssh_new();
    if (!s) {
        impl_->error = "ssh_new 失败";
        if (err) *err = impl_->error;
        return false;
    }
    long timeout_s = static_cast<long>(std::max(1, (t.connect_timeout_ms + 999) / 1000));
    ssh_options_set(s, SSH_OPTIONS_HOST, t.host.c_str());
    ssh_options_set(s, SSH_OPTIONS_PORT, &t.port);
    if (!t.username.empty()) ssh_options_set(s, SSH_OPTIONS_USER, t.username.c_str());
    ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout_s);
    int verbosity = SSH_LOG_NOLOG;
    ssh_options_set(s, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);

    if (ssh_connect(s) != SSH_OK) {
        impl_->error = err_of(s, "连接失败");
        if (err) *err = impl_->error;
        ssh_free(s);
        return false;
    }
    int auth = SSH_AUTH_DENIED;
    if (!t.password.empty()) {
        auth = ssh_userauth_password(s, nullptr, t.password.c_str());
        if (auth != SSH_AUTH_SUCCESS && ssh_userauth_none(s, nullptr) == SSH_AUTH_SUCCESS)
            auth = SSH_AUTH_SUCCESS;
    } else {
        auth = ssh_userauth_none(s, nullptr);
    }
    if (auth != SSH_AUTH_SUCCESS) {
        impl_->error = err_of(s, "认证失败");
        if (err) *err = impl_->error;
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }
    sftp_session sftp = sftp_new(s);
    if (!sftp || sftp_init(sftp) != SSH_OK) {
        impl_->error = err_of(s, "SFTP 初始化失败");
        if (err) *err = impl_->error;
        if (sftp) sftp_free(sftp);
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }
    impl_->session = s;
    impl_->sftp = sftp;
    impl_->error.clear();
    impl_->ready.store(true, std::memory_order_release);
    return true;
}

void SftpClient::close() {
    if (!impl_) return;
    impl_->ready.store(false, std::memory_order_release);
    if (impl_->sftp) sftp_free(impl_->sftp);
    if (impl_->session) {
        ssh_disconnect(impl_->session);
        ssh_free(impl_->session);
    }
    impl_->sftp = nullptr;
    impl_->session = nullptr;
}

std::string SftpClient::home() const {
    if (!impl_->session) return std::string();
    char* canon = sftp_canonicalize_path(impl_->sftp, ".");
    std::string out;
    if (canon) {
        out = canon;
        ssh_string_free_char(canon);
    }
    if (out.empty()) out = "/";
    return out;
}

bool SftpClient::list(const std::string& path, std::vector<SftpEntry>* out, std::string* err) {
    if (!impl_->sftp) { if (err) *err = "SFTP 未连接"; return false; }
    sftp_dir dir = sftp_opendir(impl_->sftp, path.c_str());
    if (!dir) {
        impl_->error = err_of(impl_->session, "打开目录失败");
        if (err) *err = impl_->error;
        return false;
    }
    std::vector<SftpEntry> items;
    while (sftp_attributes a = sftp_readdir(impl_->sftp, dir)) {
        std::string name = a->name ? a->name : "";
        if (!name.empty() && name != "." && name != "..") {
            SftpEntry e;
            e.name = name;
            e.dir = (a->type == SSH_FILEXFER_TYPE_DIRECTORY);
            e.link = (a->type == SSH_FILEXFER_TYPE_SYMLINK);
            e.size = a->size;
            e.mode = a->permissions;
            e.uid = a->uid;
            e.gid = a->gid;
            e.mtime = static_cast<std::uint32_t>(a->mtime);
            items.push_back(e);
        }
        sftp_attributes_free(a);
    }
    sftp_closedir(dir);
    std::sort(items.begin(), items.end(), [](const SftpEntry& x, const SftpEntry& y) {
        if (x.dir != y.dir) return x.dir > y.dir;
        return x.name < y.name;
    });
    if (out) *out = std::move(items);
    return true;
}

bool SftpClient::mkdir(const std::string& path, std::string* err) {
    if (!impl_->sftp) { if (err) *err = "SFTP 未连接"; return false; }
    if (sftp_mkdir(impl_->sftp, path.c_str(), 0755) != SSH_OK) {
        impl_->error = err_of(impl_->session, "创建目录失败");
        if (err) *err = impl_->error;
        return false;
    }
    return true;
}

bool SftpClient::unlink(const std::string& path, std::string* err) {
    if (!impl_->sftp) { if (err) *err = "SFTP 未连接"; return false; }
    if (sftp_unlink(impl_->sftp, path.c_str()) != SSH_OK) {
        impl_->error = err_of(impl_->session, "删除远端文件失败");
        if (err) *err = impl_->error;
        return false;
    }
    return true;
}

bool SftpClient::rmdir(const std::string& path, std::string* err) {
    if (!impl_->sftp) { if (err) *err = "SFTP 未连接"; return false; }
    if (sftp_rmdir(impl_->sftp, path.c_str()) != SSH_OK) {
        impl_->error = err_of(impl_->session, "删除远端目录失败");
        if (err) *err = impl_->error;
        return false;
    }
    return true;
}

bool SftpClient::rename(const std::string& from, const std::string& to, std::string* err) {
    if (!impl_->sftp) { if (err) *err = "SFTP 未连接"; return false; }
    if (sftp_rename(impl_->sftp, from.c_str(), to.c_str()) != SSH_OK) {
        impl_->error = err_of(impl_->session, "远端重命名失败");
        if (err) *err = impl_->error;
        return false;
    }
    return true;
}

namespace {

bool hash_stream(EVP_MD_CTX* ctx, std::FILE* f, std::atomic<bool>* cancel,
                 std::atomic<std::uint64_t>* done, std::atomic<std::uint64_t>* total, std::string* err) {
    unsigned char buf[kChunk];
    std::size_t got = 0;
    std::uint64_t sent = 0;
    while ((got = std::fread(buf, 1, kChunk, f)) > 0) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            if (err) *err = "已取消";
            return false;
        }
        EVP_DigestUpdate(ctx, buf, got);
        sent += got;
        if (done) done->store(sent, std::memory_order_relaxed);
        if (total) total->store(sent, std::memory_order_relaxed);
    }
    return true;
}

std::string hex_digest(EVP_MD_CTX* ctx) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx, out, &len) != 1) return std::string();
    std::string hex;
    hex.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        char b[3];
        std::snprintf(b, sizeof(b), "%02x", out[i]);
        hex += b;
    }
    return hex;
}

std::string human_bytes(std::uint64_t n) {
    char b[48];
    if (n >= (1ull << 20)) std::snprintf(b, sizeof(b), "%.1f MB", double(n) / (1ull << 20));
    else if (n >= 1024) std::snprintf(b, sizeof(b), "%.1f KB", double(n) / 1024.0);
    else std::snprintf(b, sizeof(b), "%llu B", static_cast<unsigned long long>(n));
    return b;
}

}  // namespace

std::string md5_local_file(const std::string& path, bool* ok) {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, widen(path).c_str(), L"rb") != 0 || !f) {
        if (ok) *ok = false;
        return std::string();
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        std::fclose(f);
        if (ok) *ok = false;
        return std::string();
    }
    if (EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        std::fclose(f);
        if (ok) *ok = false;
        return std::string();
    }
    bool good = hash_stream(ctx, f, nullptr, nullptr, nullptr, nullptr);
    std::string hex = good ? hex_digest(ctx) : std::string();
    EVP_MD_CTX_free(ctx);
    std::fclose(f);
    if (ok) *ok = good && !hex.empty();
    return hex;
}

bool SftpClient::upload(const std::string& local, const std::string& remote,
                        std::atomic<bool>* cancel, std::atomic<std::uint64_t>* done,
                        std::atomic<std::uint64_t>* total, std::string* err, std::string* md5_note) {
    if (!impl_->sftp) { if (err) *err = "SFTP 未连接"; return false; }
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, widen(local).c_str(), L"rb") != 0 || !f) {
        if (err) *err = "打不开本地文件：" + local;
        return false;
    }
    std::uint64_t size = 0;
    {
        LARGE_INTEGER li{};
        if (GetFileSizeEx(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f))), &li))
            size = static_cast<std::uint64_t>(li.QuadPart);
    }
    if (total) total->store(size, std::memory_order_relaxed);

    std::string tmp = remote + ".part";
    std::uint64_t offset = 0;
    bool resumed = false;
    {
        sftp_attributes pa = sftp_stat(impl_->sftp, tmp.c_str());
        if (pa) {
            std::uint64_t ps = static_cast<std::uint64_t>(pa->size);
            std::time_t pm = pa->mtime;
            sftp_attributes_free(pa);
            struct _stat64 st {};
            bool local_known = (_wstat64(widen(local).c_str(), &st) == 0);
            // 本地文件在断点之后没有改过，才允许续传，避免把旧尾巴接在新文件上
            if (ps > 0 && ps <= size && local_known && pm > 0 &&
                static_cast<std::time_t>(st.st_mtime) < pm) {
                offset = ps;
                resumed = true;
            }
        }
    }
    if (done) done->store(offset, std::memory_order_relaxed);

    if (!(resumed && offset == size)) {
        sftp_file rf = sftp_open(impl_->sftp, tmp.c_str(),
                                 O_WRONLY | O_CREAT | (resumed ? 0 : O_TRUNC), 0644);
        if (!rf) {
            impl_->error = err_of(impl_->session, ("创建远端临时文件失败：" + tmp).c_str());
            if (err) *err = impl_->error;
            std::fclose(f);
            return false;
        }
        if (resumed) {
            if (sftp_seek64(rf, offset) != SSH_OK) {
                sftp_close(rf);
                std::fclose(f);
                if (err) *err = "续传定位失败（远端偏移 " + human_bytes(offset) + "）";
                return false;
            }
            if (_fseeki64(f, static_cast<__int64>(offset), SEEK_SET) != 0) {
                sftp_close(rf);
                std::fclose(f);
                if (err) *err = "续传定位失败（本地偏移 " + human_bytes(offset) + "）";
                return false;
            }
        }

        unsigned char buf[kChunk];
        std::uint64_t sent = offset;
        std::uint64_t rate_base = offset;
        auto rate_start = std::chrono::steady_clock::now();
        bool ok = true;
        std::string fail;
        while (true) {
            if (cancel && cancel->load(std::memory_order_relaxed)) { ok = false; fail = "已取消"; break; }
            std::size_t got = std::fread(buf, 1, kChunk, f);
            if (got == 0) break;
            std::size_t written = 0;
            while (written < got) {
                int n = sftp_write(rf, buf + written, got - written);
                if (n < 0) {
                    ok = false;
                    fail = err_of(impl_->session, "写入远端失败");
                    break;
                }
                if (n == 0) {
                    ok = false;
                    fail = "写入远端失败（服务器返回 0 字节）";
                    break;
                }
                written += static_cast<std::size_t>(n);
            }
            if (!ok) break;
            sent += got;
            if (done) done->store(sent, std::memory_order_relaxed);
            throttle_to(sent - rate_base, rate_start,
                        impl_->speed_limit.load(std::memory_order_relaxed), cancel);
        }
        std::fclose(f);
        sftp_close(rf);
        if (!ok) {
            // 保留 .part：下次上传可从断点继续
            if (err) *err = fail.empty() ? "上传失败（断点已保留）" : fail + "（断点已保留）";
            return false;
        }
        if (sent != size) {
            if (err) *err = "上传字节不完整（断点已保留）";
            return false;
        }
    } else {
        std::fclose(f);
    }
    bool renamed = sftp_rename(impl_->sftp, tmp.c_str(), remote.c_str()) == SSH_OK;
    if (!renamed) {
        // SFTP v3 的 RENAME 在多数服务器上不允许覆盖已存在目标；显式删旧文件后重试
        if (sftp_attributes existing = sftp_stat(impl_->sftp, remote.c_str())) {
            sftp_attributes_free(existing);
            if (sftp_unlink(impl_->sftp, remote.c_str()) == SSH_OK)
                renamed = sftp_rename(impl_->sftp, tmp.c_str(), remote.c_str()) == SSH_OK;
        }
    }
    if (!renamed) {
        impl_->error = err_of(impl_->session, "远端重命名失败（目标可能是目录或无权限；断点已保留）");
        if (err) *err = impl_->error;
        return false;
    }

    std::string notes;
    if (resumed) notes = "从 " + human_bytes(offset) + " 续传 · ";
    if (size > (4ull << 20)) {
        bool size_ok = false;
        if (sftp_attributes st = sftp_stat(impl_->sftp, remote.c_str())) {
            size_ok = (static_cast<std::uint64_t>(st->size) == size);
            sftp_attributes_free(st);
        }
        notes += size_ok ? "已写入 · 大小校验一致" : "写入后大小不一致";
    } else {
        bool local_ok = false;
        std::string local_md5 = md5_local_file(local, &local_ok);
        std::string remote_md5;
        if (sftp_file chk = sftp_open(impl_->sftp, remote.c_str(), O_RDONLY, 0)) {
            EVP_MD_CTX* rctx = EVP_MD_CTX_new();
            if (rctx && EVP_DigestInit_ex(rctx, EVP_md5(), nullptr) == 1) {
                unsigned char buf[kChunk];
                int n = 0;
                while ((n = sftp_read(chk, buf, kChunk)) > 0)
                    EVP_DigestUpdate(rctx, buf, static_cast<std::size_t>(n));
                remote_md5 = hex_digest(rctx);
            }
            if (rctx) EVP_MD_CTX_free(rctx);
            sftp_close(chk);
        }
        notes += (local_md5.empty() || remote_md5.empty())
                     ? "MD5 校验失败（无法读取）"
                     : (local_md5 == remote_md5 ? "MD5 一致" : "MD5 不一致");
    }
    if (md5_note) *md5_note = notes;
    return true;
}

bool SftpClient::download(const std::string& remote, const std::string& local,
                          std::atomic<bool>* cancel, std::atomic<std::uint64_t>* done,
                          std::atomic<std::uint64_t>* total, std::string* err, std::string* md5_note) {
    if (!impl_->sftp) { if (err) *err = "SFTP 未连接"; return false; }
    sftp_file rf = sftp_open(impl_->sftp, remote.c_str(), O_RDONLY, 0);
    if (!rf) {
        impl_->error = err_of(impl_->session, "打开远端文件失败");
        if (err) *err = impl_->error;
        return false;
    }
    std::uint64_t size = 0;
    std::time_t remote_mtime = 0;
    if (sftp_attributes a = sftp_fstat(rf)) {
        size = static_cast<std::uint64_t>(a->size);
        remote_mtime = a->mtime;
        sftp_attributes_free(a);
    }
    if (total) total->store(size, std::memory_order_relaxed);

    std::string tmp = local + ".part";
    std::uint64_t offset = 0;
    bool resumed = false;
    struct _stat64 st {};
    if (_wstat64(widen(tmp).c_str(), &st) == 0) {
        std::uint64_t ps = static_cast<std::uint64_t>(st.st_size);
        // 远端文件在断点之后没有改过，才允许续传
        if (ps > 0 && ps <= size &&
            (remote_mtime <= 0 || static_cast<std::time_t>(st.st_mtime) > remote_mtime)) {
            offset = ps;
            resumed = true;
        }
    }
    if (done) done->store(offset, std::memory_order_relaxed);

    std::FILE* f = nullptr;
    if (_wfopen_s(&f, widen(tmp).c_str(), resumed ? L"ab" : L"wb") != 0 || !f) {
        if (err) *err = "写不了本地临时文件：" + tmp;
        sftp_close(rf);
        return false;
    }
    if (resumed && sftp_seek64(rf, offset) != SSH_OK) {
        std::fclose(f);
        sftp_close(rf);
        if (err) *err = "续传定位远端失败（偏移 " + human_bytes(offset) + "）";
        return false;
    }

    unsigned char buf[kChunk];
    std::uint64_t got_total = offset;
    std::uint64_t rate_base = offset;
    auto rate_start = std::chrono::steady_clock::now();
    bool ok = true;
    std::string fail;
    while (true) {
        if (cancel && cancel->load(std::memory_order_relaxed)) { ok = false; fail = "已取消"; break; }
        int n = sftp_read(rf, buf, kChunk);
        if (n < 0) { ok = false; fail = err_of(impl_->session, "读取远端失败"); break; }
        if (n == 0) break;
        if (std::fwrite(buf, 1, static_cast<std::size_t>(n), f) != static_cast<std::size_t>(n)) {
            ok = false;
            fail = "写本地文件失败";
            break;
        }
        got_total += static_cast<std::uint64_t>(n);
        if (done) done->store(got_total, std::memory_order_relaxed);
        throttle_to(got_total - rate_base, rate_start,
                    impl_->speed_limit.load(std::memory_order_relaxed), cancel);
    }
    std::fclose(f);
    sftp_close(rf);
    if (!ok) {
        // 保留 .part：下次下载可从断点继续
        if (err) *err = fail.empty() ? "下载失败（断点已保留）" : fail + "（断点已保留）";
        return false;
    }
    if (got_total != size) {
        if (err) *err = "下载字节不完整（断点已保留）";
        return false;
    }
    if (!MoveFileExW(widen(tmp).c_str(), widen(local).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (err) *err = "重命名本地文件失败（目标可能被占用；断点已保留）";
        return false;
    }
    bool md5_ok = false;
    std::string md5 = md5_local_file(local, &md5_ok);
    if (md5_note) {
        *md5_note = (resumed ? "从 " + human_bytes(offset) + " 续传 · " : "") +
                    "本地 MD5 " + (md5_ok ? md5 : std::string("计算失败"));
    }
    return true;
}

}  // namespace sb3

