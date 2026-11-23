#include "services/share/share_server.h"

#include <mongoose.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace sb3 {
namespace {

// std::fopen 在 Windows 上按 ANSI 解析路径，中文/空格路径会失败；统一走宽字符 API。
std::FILE* fopen_utf8(const std::string& path, const char* mode) {
    int need = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (need <= 0) return nullptr;
    std::wstring wpath(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), need);
    std::wstring wmode;
    for (const char* p = mode; *p; ++p) wmode.push_back(*p);
    return _wfopen(wpath.c_str(), wmode.c_str());
}

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string url_encode(const std::string& s) {
    char buf[2048];
    size_t n = mg_url_encode(s.c_str(), s.size(), buf, sizeof(buf));
    return std::string(buf, n);
}

std::string human_size(unsigned long long n) {
    char b[64];
    if (n >= 1024ull * 1024 * 1024)
        std::snprintf(b, sizeof(b), "%.2f GB", n / 1024.0 / 1024.0 / 1024.0);
    else if (n >= 1024ull * 1024)
        std::snprintf(b, sizeof(b), "%.2f MB", n / 1024.0 / 1024.0);
    else if (n >= 1024ull)
        std::snprintf(b, sizeof(b), "%.1f KB", n / 1024.0);
    else
        std::snprintf(b, sizeof(b), "%llu B", n);
    return b;
}

bool is_post(struct mg_http_message* hm) {
    return hm->method.len == 4 && _strnicmp(hm->method.buf, "POST", 4) == 0;
}

}  // namespace

ShareServer::~ShareServer() { stop(); }

bool ShareServer::start(const std::string& root, int port, std::string* err) {
    stop();
    if (err) err->clear();
    // 重复启动时先彻底停止旧实例，避免旧 mgr/线程泄漏并持有 this
    if (mgr_ || thread_.joinable()) stop();
    if (root.empty()) {
        if (err) *err = "共享目录为空";
        return false;
    }
    try {
        fs::create_directories(fs::u8path(root));
    } catch (...) {
        if (err) *err = "无法创建共享目录：" + root;
        return false;
    }
    root_ = fs::absolute(fs::u8path(root)).lexically_normal().u8string();
    port_ = port > 0 ? port : 18082;
    mgr_ = new mg_mgr();
    mg_mgr_init(mgr_);
    mgr_->userdata = this;
    char url[128];
    std::snprintf(url, sizeof(url), "http://0.0.0.0:%d", port_);
    mg_connection* c = mg_http_listen(mgr_, url, on_http, this);
    if (!c) {
        mg_mgr_free(mgr_);
        delete mgr_;
        mgr_ = nullptr;
        if (err) *err = "共享 HTTP 端口 " + std::to_string(port_) + " 绑定失败（可能被占用）";
        return false;
    }
    stop_ = false;
    running_ = true;
    thread_ = std::thread([this]() {
        while (!stop_.load()) mg_mgr_poll(mgr_, 40);
        running_ = false;
    });
    return true;
}

void ShareServer::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
    if (mgr_) {
        mg_mgr_free(mgr_);
        delete mgr_;
        mgr_ = nullptr;
    }
}

std::string ShareServer::base_url(const std::string& ip) const {
    char b[128];
    std::snprintf(b, sizeof(b), "http://%s:%d/", ip.empty() ? "127.0.0.1" : ip.c_str(), port_);
    return b;
}

bool ShareServer::safe_join(const std::string& rel, std::string* abs) const {
    if (!abs) return false;
    std::string clean = rel;
    std::replace(clean.begin(), clean.end(), '\\', '/');
    while (!clean.empty() && clean.front() == '/') clean.erase(clean.begin());
    if (clean.find("..") != std::string::npos) return false;
    if (clean.size() > 1 && clean[1] == ':') return false;
    fs::path p = fs::u8path(root_);
    if (!clean.empty()) p /= fs::u8path(clean);
    std::string norm = p.lexically_normal().u8string();
    std::string rootnorm = fs::u8path(root_).lexically_normal().u8string();
    if (norm.size() < rootnorm.size()) return false;
    if (_strnicmp(norm.c_str(), rootnorm.c_str(), rootnorm.size()) != 0) return false;
    if (norm.size() > rootnorm.size()) {
        char sep = norm[rootnorm.size()];
        if (sep != '\\' && sep != '/') return false;
    }
    *abs = norm;
    return true;
}

std::string ShareServer::list_page(const std::string& rel, const std::string& notice) const {
    std::string abs;
    if (!safe_join(rel, &abs)) return "<html><body>非法路径</body></html>";
    std::vector<fs::directory_entry> entries;
    try {
        for (const auto& e : fs::directory_iterator(fs::u8path(abs))) entries.push_back(e);
    } catch (...) {
        return "<html><body>目录不可读：" + html_escape(rel) + "</body></html>";
    }
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a,
                                                 const fs::directory_entry& b) {
        std::error_code ea, eb;
        bool da = a.is_directory(ea);
        bool db = b.is_directory(eb);
        if (da != db) return da > db;
        return _stricmp(a.path().filename().u8string().c_str(),
                        b.path().filename().u8string().c_str()) < 0;
    });

    std::ostringstream html;
    html << "<!doctype html><html lang=zh><meta charset='utf-8'>"
         << "<meta name=viewport content='width=device-width,initial-scale=1'>"
         << "<title>ssh-bridge v3 共享目录</title><style>"
         << "body{font:14px/1.6 'Segoe UI','Microsoft YaHei',sans-serif;margin:0;background:#f5f6f8;color:#222}"
         << "main{max-width:980px;margin:24px auto;background:#fff;border:1px solid #dfe3e8;border-radius:10px;padding:22px}"
         << "h1{font-size:20px;margin:0 0 6px} .muted{color:#667085;font-size:12px}"
         << "table{width:100%;border-collapse:collapse;margin:14px 0} th,td{padding:8px 10px;border-bottom:1px solid #eef0f3;text-align:left}"
         << "th{color:#667085;font-weight:600;background:#fafbfc} a{color:#175cd3;text-decoration:none} a:hover{text-decoration:underline}"
         << ".row{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin:12px 0}"
         << "input[type=text]{flex:1;min-width:220px;padding:8px 10px;border:1px solid #cfd6df;border-radius:6px}"
         << "button{padding:7px 12px;border:1px solid #175cd3;background:#175cd3;color:#fff;border-radius:6px;cursor:pointer}"
         << "button.ghost{background:#fff;color:#175cd3}.danger{color:#b42318;border-color:#fda29b;background:#fff}"
         << ".path{font-family:Consolas,monospace;background:#f8fafc;padding:8px 10px;border-radius:6px}"
         << ".note{padding:8px 10px;border-radius:6px;background:#ecfdf3;color:#067647;margin:10px 0}"
         << ".ico{display:inline-block;width:18px}"
         << "</style><main><h1>ssh-bridge v3 共享目录</h1>"
         << "<div class=muted>TFTP、NFS 与 HTTP 共用这一份文件；所有传输都是本机文件，无需另存副本。</div>";
    if (!notice.empty()) html << "<div class=note>" << html_escape(notice) << "</div>";
    html << "<div class=path>根目录：" << html_escape(root_) << "　当前：/"
         << html_escape(rel) << "</div>";

    html << "<div class=row><form method=post enctype=multipart/form-data action='/upload?dir="
         << url_encode(rel) << "'><input type=file name=file required> "
         << "<button type=submit>上传到当前目录</button></form></div>";

    html << "<div class=row><form method=post action='/mkdir?dir=" << url_encode(rel)
         << "'><input type=text name=name placeholder='新目录名（例如 ok3568）' required>"
         << "<button class=ghost type=submit>新建目录</button></form></div>";

    html << "<table><thead><tr><th>名称</th><th>大小</th><th>修改时间</th><th>操作</th></tr></thead><tbody>";
    if (!rel.empty()) {
        std::string parent = rel;
        size_t p = parent.find_last_of('/');
        parent = p == std::string::npos ? "" : parent.substr(0, p);
        html << "<tr><td><span class=ico>↑</span><a href='/browse?path=" << url_encode(parent)
             << "'>返回上级</a></td><td>—</td><td>—</td><td>—</td></tr>";
    }
    for (const auto& e : entries) {
        std::string name = e.path().filename().u8string();
        std::string child = rel.empty() ? name : rel + "/" + name;
        std::error_code dec;
        bool isdir = e.is_directory(dec);
        std::string size = "—";
        std::string mtime = "—";
        try {
            if (!isdir) size = human_size(e.file_size());
            auto t = fs::last_write_time(e);
            auto delta = std::chrono::duration_cast<std::chrono::system_clock::duration>(
                t - fs::file_time_type::clock::now());
            auto sys_t = std::chrono::system_clock::now() + delta;
            std::time_t sec = std::chrono::system_clock::to_time_t(sys_t);
            std::tm local{};
            if (localtime_s(&local, &sec) == 0) {
                char tb[64];
                std::strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M", &local);
                mtime = tb;
            }
        } catch (...) {}
        html << "<tr><td>" << (isdir ? "<span class=ico>▸</span>" : "<span class=ico>·</span>");
        if (isdir)
            html << "<a href='/browse?path=" << url_encode(child) << "'>" << html_escape(name) << "/</a>";
        else
            html << "<a href='/files?path=" << url_encode(child) << "'>" << html_escape(name) << "</a>";
        html << "</td><td>" << size << "</td><td>" << mtime << "</td><td>"
             << "<form method=post action='/delete?path=" << url_encode(child)
             << "' onsubmit=\"return confirm('确认删除该条目？')\" style='display:inline'><button class='danger' type=submit>删除</button></form>"
             << "</td></tr>";
    }
    html << "</tbody></table><div class=muted>HTTP 端口 " << port_
         << "　·　服务随应用启停；关闭应用后端口立即释放。</div></main></html>";
    return html.str();
}

void ShareServer::on_http(mg_connection* c, int ev, void* ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;
    static_cast<ShareServer*>(c->mgr->userdata)->handle(c, static_cast<mg_http_message*>(ev_data), ev);
}

void ShareServer::handle(mg_connection* c, mg_http_message* hm, int ev) {
    if (ev != MG_EV_HTTP_MSG || !hm) return;
    std::string uri(hm->uri.buf, hm->uri.len);

    char pathbuf[1024] = "";
    mg_http_get_var(&hm->query, "path", pathbuf, sizeof(pathbuf));
    std::string rel = pathbuf;

    if (uri == "/api/status") {
        std::ostringstream j;
        j << "{\"running\":" << (running_.load() ? "true" : "false") << ",\"port\":" << port_
          << ",\"root\":\"" << json_escape(root_) << "\"}";
        mg_http_reply(c, 200, "Content-Type: application/json; charset=utf-8\r\n", "%s",
                      j.str().c_str());
        return;
    }

    if (uri == "/" || uri == "/index.html" || uri == "/browse") {
        char nb[512] = "";
        mg_http_get_var(&hm->query, "notice", nb, sizeof(nb));
        std::string html = list_page(rel, nb);
        mg_http_reply(c, 200,
                      "Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n",
                      "%s", html.c_str());
        return;
    }

    if (uri.rfind("/tools/", 0) == 0 || uri.rfind("/skills/", 0) == 0) {
        std::string asset_rel = uri.substr(1);
        std::string abs;
        std::error_code asset_ec;
        if (!safe_join(asset_rel, &abs) || !fs::is_regular_file(fs::u8path(abs), asset_ec)) {
            mg_http_reply(c, 404, "Content-Type: text/plain; charset=utf-8\r\n", "文件不存在");
            return;
        }
        auto sz = fs::file_size(fs::u8path(abs), asset_ec);
        if (asset_ec || sz > 64ull * 1024 * 1024) {
            mg_http_reply(c, 413, "Content-Type: text/plain; charset=utf-8\r\n", "文件过大");
            return;
        }
        std::ifstream in(fs::u8path(abs), std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string ext = fs::u8path(abs).extension().u8string();
        const char* ct = (ext == ".json") ? "application/json; charset=utf-8"
                        : (ext == ".py" || ext == ".md" || ext == ".txt")
                              ? "text/plain; charset=utf-8"
                              : "application/octet-stream";
        mg_http_reply(c, 200, ("Content-Type: " + std::string(ct) +
                              "\r\nCache-Control: no-store\r\n").c_str(), "%s", data.c_str());
        return;
    }

    if (uri == "/files") {
        std::string abs;
        std::error_code ec;
        if (!safe_join(rel, &abs) || !fs::is_regular_file(fs::u8path(abs), ec)) {
            mg_http_reply(c, 404, "Content-Type: text/plain; charset=utf-8\r\n", "文件不存在");
            return;
        }
        struct mg_http_serve_opts opts {};
        opts.root_dir = root_.c_str();
        opts.mime_types = ".txt=text/plain,.log=text/plain,.json=application/json,.bin=application/octet-stream";
        opts.fs = &mg_fs_posix;
        mg_http_serve_file(c, hm, abs.c_str(), &opts);
        return;
    }

    if (uri == "/upload" && is_post(hm)) {
        char dirbuf[1024] = "";
        mg_http_get_var(&hm->query, "dir", dirbuf, sizeof(dirbuf));
        std::string abs;
        if (!safe_join(dirbuf, &abs)) {
            mg_http_reply(c, 400, "Content-Type: text/plain; charset=utf-8\r\n", "非法目录");
            return;
        }
        std::error_code ec;
        fs::create_directories(fs::u8path(abs), ec);
        if (ec) {
            mg_http_reply(c, 400, "Content-Type: text/plain; charset=utf-8\r\n", "目录不可用");
            return;
        }
        if (hm->body.len > 256ull * 1024 * 1024) {
            mg_http_reply(c, 413, "Content-Type: text/plain; charset=utf-8\r\n",
                          "上传内容超过 256 MB");
            return;
        }
        long written = 0;
        std::string error;
        bool multipart = false;
        struct mg_str *ct = mg_http_get_header(hm, "Content-Type");
        if (ct && ct->len >= 19 && _strnicmp(ct->buf, "multipart/form-data", 19) == 0)
            multipart = true;
        if (multipart) {
            size_t off = 0;
            struct mg_http_part part;
            while ((off = mg_http_next_multipart(hm->body, off, &part)) != 0) {
                if (part.filename.len == 0) continue;
                std::string name(part.filename.buf, part.filename.len);
                std::string base = fs::u8path(name).filename().u8string();
                if (base.empty() || base == "." || base == ".." ||
                    base.find('/') != std::string::npos || base.find('\\') != std::string::npos) {
                    error = "非法文件名";
                    continue;
                }
                std::string target = (fs::u8path(abs) / fs::u8path(base)).u8string();
                std::FILE* f = fopen_utf8(target, "wb");
                if (!f) {
                    error = "无法写入 " + base;
                    continue;
                }
                size_t body_len = part.body.len;
                bool write_ok = body_len == 0 ||
                                std::fwrite(part.body.buf, 1, body_len, f) == body_len;
                if (std::fclose(f) != 0) write_ok = false;
                if (!write_ok) {
                    error = "写入失败：" + base;
                    std::error_code remove_ec;
                    fs::remove(fs::u8path(target), remove_ec);
                    continue;
                }
                written += static_cast<long>(body_len);
            }
        } else {
            char filebuf[512] = "";
            mg_http_get_var(&hm->query, "file", filebuf, sizeof(filebuf));
            std::string base = fs::u8path(std::string(filebuf)).filename().u8string();
            if (base.empty()) {
                error = "缺少 file 参数或 multipart 表单";
            } else if (hm->body.len > 256ull * 1024 * 1024) {
                error = "文件超过 256 MB";
            } else {
                std::string target = (fs::u8path(abs) / fs::u8path(base)).u8string();
                std::FILE* f = fopen_utf8(target, "wb");
                if (!f)
                    error = "无法写入 " + base;
                else {
                    size_t body_len = hm->body.len;
                    bool write_ok = body_len == 0 ||
                                    std::fwrite(hm->body.buf, 1, body_len, f) == body_len;
                    if (std::fclose(f) != 0) write_ok = false;
                    if (!write_ok) {
                        error = "写入失败：" + base;
                        std::error_code remove_ec;
                        fs::remove(fs::u8path(target), remove_ec);
                    } else {
                        written = static_cast<long>(body_len);
                    }
                }
            }
        }
        char loc[8192];
        if (!error.empty())
            std::snprintf(loc, sizeof(loc), "/browse?path=%s&notice=%s", url_encode(dirbuf).c_str(),
                          url_encode("上传失败：" + error).c_str());
        else {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "已上传，写入 %ld 字节", written);
            std::snprintf(loc, sizeof(loc), "/browse?path=%s&notice=%s", url_encode(dirbuf).c_str(),
                          url_encode(msg).c_str());
        }
        mg_http_reply(c, 302, ("Location: " + std::string(loc) + "\r\n").c_str(), "%s", "");
        return;
    }

    if (uri == "/mkdir" && is_post(hm)) {
        char dirbuf[1024] = "";
        char name[256] = "";
        mg_http_get_var(&hm->query, "dir", dirbuf, sizeof(dirbuf));
        mg_http_get_var(&hm->body, "name", name, sizeof(name));
        std::string rel_dir = dirbuf;
        std::string child = rel_dir.empty() ? std::string(name) : rel_dir + "/" + name;
        std::string abs;
        std::string notice = "目录已创建";
        if (name[0] && safe_join(child, &abs)) {
            std::error_code ec;
            fs::create_directory(fs::u8path(abs), ec);
            if (ec) notice = "新建失败：" + ec.message();
        } else if (!name[0] || !safe_join(child, &abs)) {
            notice = "新建失败：目录名非法";
        }
        char loc[8192];
        std::snprintf(loc, sizeof(loc), "/browse?path=%s&notice=%s", url_encode(rel_dir).c_str(),
                      url_encode(notice).c_str());
        mg_http_reply(c, 302, ("Location: " + std::string(loc) + "\r\n").c_str(), "%s", "");
        return;
    }

    if (uri == "/delete" && is_post(hm)) {
        std::string abs;
        // 根目录不允许删除：否则一个 POST /delete?path= 就能清空整个共享目录
        if (rel.empty() || !safe_join(rel, &abs) ||
            (_strnicmp(abs.c_str(), root_.c_str(), root_.size()) == 0 &&
             abs.size() == root_.size())) {
            mg_http_reply(c, 400, "Content-Type: text/plain; charset=utf-8\r\n",
                          "不能删除共享根目录");
            return;
        }
        std::error_code ec;
        fs::remove_all(fs::u8path(abs), ec);
        std::string parent = rel;
        size_t p = parent.find_last_of('/');
        parent = p == std::string::npos ? "" : parent.substr(0, p);
        char loc[8192];
        std::snprintf(loc, sizeof(loc), "/browse?path=%s&notice=%s", url_encode(parent).c_str(),
                      url_encode(ec ? "删除失败：" + ec.message() : "已删除").c_str());
        mg_http_reply(c, 302, ("Location: " + std::string(loc) + "\r\n").c_str(), "%s", "");
        return;
    }

    mg_http_reply(c, 404, "Content-Type: text/plain; charset=utf-8\r\n", "Not found");
}

}  // namespace sb3

