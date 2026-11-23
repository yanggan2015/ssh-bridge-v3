// web_console.h — S9 网页端：单端口 HTTP + WS + 多页控制台。
#pragma once

#include <atomic>
#include <string>

namespace sb3 {

class AppCore;
class ServiceManager;

class WebConsole {
public:
    WebConsole();
    ~WebConsole();
    WebConsole(const WebConsole&) = delete;
    WebConsole& operator=(const WebConsole&) = delete;

    bool start(AppCore* core, ServiceManager* services, int port, const std::string& token,
               bool require_token, std::string* err);
    void stop();
    // Shell 主窗口句柄：配置保存后只 PostMessage，让 UI 线程自己 reload。
    void set_notify(void* hwnd);

    bool running() const;
    int port() const;
    std::string error() const;
    std::string token() const;
    bool require_token() const;
    // 未启动时也把配置端口/token 记进状态，便于 /state 显示“关但配置为 18082”。
    void configure(int port, const std::string& token, bool require_token);
    std::string status_json() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace sb3
