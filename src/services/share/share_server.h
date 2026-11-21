// share_server.h — S11 共享目录：和 TFTP/NFS 共用同一份文件，HTTP 浏览/上传/下载/删除/建目录。
#pragma once

#include <atomic>
#include <string>
#include <thread>

struct mg_mgr;
struct mg_connection;
struct mg_http_message;

namespace sb3 {

class ShareServer {
public:
    ~ShareServer();

    bool start(const std::string& root, int port, std::string* err);
    void stop();
    bool running() const { return running_.load(); }
    int port() const { return port_; }
    std::string root() const { return root_; }
    std::string base_url(const std::string& ip) const;

private:
    static void on_http(struct mg_connection* c, int ev, void* ev_data);
    void handle(struct mg_connection* c, struct mg_http_message* hm, int ev);
    bool safe_join(const std::string& rel, std::string* abs) const;
    std::string list_page(const std::string& rel, const std::string& notice) const;

    mg_mgr* mgr_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    int port_ = 0;
    std::string root_;
};

}  // namespace sb3
