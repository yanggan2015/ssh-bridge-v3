// selftest_ssh.cpp — S2 SSH 出站链路自检（对着本机可编排的测试服务器跑）。
// 每组子模式只做一件事，服务器生命周期由 tests\step2_check.ps1 控制。

#include "app/selftest_ssh.h"

#include "core/bridge/ssh/ssh_bridge.h"
#include "core/config/config.h"
#include "core/util/util.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace sb3 {
namespace {

int g_fail = 0;

void report(const char* id, const char* name, bool ok, const std::string& detail) {
    std::printf("%s %s %s %s\n", ok ? "PASS" : "FAIL", id, name, detail.c_str());
    if (!ok) ++g_fail;
}

SSHLinkConfig make_cfg(const std::string& host, const std::string& port,
                       const std::string& user, const std::string& pass,
                       const std::string& name) {
    SSHLinkConfig cfg;
    cfg.name = name;
    cfg.host = host;
    cfg.port = std::atoi(port.c_str());
    cfg.username = user;
    cfg.password = pass;
    cfg.password_present = !pass.empty();
    cfg.connect_timeout_ms = 5000;
    cfg.keepalive_s = 2;
    cfg.buffer_size = 256 * 1024;
    cfg.log_enabled = false;
    return cfg;
}

std::string collect(SshBridge* b, std::uint64_t* epoch, std::uint64_t* seen,
                    unsigned ms, const std::vector<std::string>& want) {
    std::string all;
    std::uint64_t t0 = util::now_ms();
    while (util::now_ms() - t0 < ms) {
        all += b->read_since(epoch, seen);
        bool complete = true;
        for (const std::string& w : want) {
            if (all.find(w) == std::string::npos) complete = false;
        }
        if (complete) break;
        Sleep(20);
    }
    return all;
}

bool send_and_expect(SshBridge* b, const std::string& line) {
    std::uint64_t epoch = 0, seen = 0;
    b->read_since(&epoch, &seen);
    b->write(line.data(), line.size());
    std::string got;
    std::uint64_t t0 = util::now_ms();
    while (util::now_ms() - t0 < 3000) {
        Sleep(20);
        got += b->read_since(&epoch, &seen);
        if (got.find(line.substr(0, line.find('\n'))) != std::string::npos) return true;
    }
    return false;
}

// ---------------- T2-1 / T2-2 ----------------

bool run_echo(const std::vector<std::string>& a) {
    std::string dir = util::join_path("output\\selftest", "ssh");
    util::ensure_dir(dir);
    SshBridge bridge(make_cfg(a[1], a[2], a[3], a[4], "ssh-echo"), dir);
    if (!bridge.start()) {
        report("T2-1", "连接成功且拿到 shell", false, "start failed");
        return false;
    }
    bool up = bridge.wait_connected(8000);
    report("T2-1", "连接成功且拿到 shell", up, up ? "" : bridge.last_error());
    if (!up) {
        bridge.stop();
        return false;
    }

    std::uint64_t epoch = 0, seen = 0;
    bridge.read_since(&epoch, &seen);
    std::string a1 = "SB3-T2-A\n";
    std::string a2 = "SB3-T2-B\n";
    bridge.write(a1.data(), a1.size());
    Sleep(120);
    bridge.write(a2.data(), a2.size());
    std::string got = collect(&bridge, &epoch, &seen, 3000, {"SB3-T2-A", "SB3-T2-B"});
    std::size_t pa = got.find("SB3-T2-A");
    std::size_t pb = got.find("SB3-T2-B");
    bool ok = pa != std::string::npos && pb != std::string::npos && pa < pb;
    report("T2-2", "发命令回显顺序正确", ok,
           "A=" + std::to_string(pa) + " B=" + std::to_string(pb) + " rx=" +
               std::to_string(got.size()));
    bridge.stop();
    return ok;
}

// ---------------- T2-6 ----------------

bool run_auth_fail(const std::vector<std::string>& a) {
    std::string dir = util::join_path("output\\selftest", "ssh");
    util::ensure_dir(dir);
    SshBridge bridge(make_cfg(a[1], a[2], a[3], a[4], "ssh-auth"), dir);
    if (!bridge.start()) return false;
    bool up = bridge.wait_connected(6000);
    std::string err = bridge.last_error();
    bool readable = !up && !err.empty() &&
                    (err.find("认证") != std::string::npos ||
                     err.find("auth") != std::string::npos ||
                     err.find("denied") != std::string::npos ||
                     err.find("拒绝") != std::string::npos);
    report("T2-6", "密码错误给出可读错误", readable, "up=" + std::to_string(up) + " err=" + err);
    bridge.stop();
    return readable;
}

// ---------------- T2-3 ----------------

bool run_drop(const std::vector<std::string>& a) {
    std::string dir = util::join_path("output\\selftest", "ssh");
    util::ensure_dir(dir);
    SshBridge bridge(make_cfg(a[1], a[2], a[3], a[4], "ssh-drop"), dir);
    if (!bridge.start()) return false;
    if (!bridge.wait_connected(8000)) {
        report("T2-3", "拔网 5s 内标记断开", false, "未连上: " + bridge.last_error());
        bridge.stop();
        return false;
    }
    std::uint64_t t0 = util::now_ms();
    bool down = bridge.wait_disconnected(9000);
    std::uint64_t ms = util::now_ms() - t0;
    bool ok = down && ms <= 5000;
    report("T2-3", "拔网 5s 内标记断开", ok, "down=" + std::to_string(down) + " drop_ms=" +
                                                    std::to_string(ms));
    bridge.stop();
    return ok;
}

// ---------------- T2-4 ----------------

bool run_reconnect(const std::vector<std::string>& a) {
    std::string dir = util::join_path("output\\selftest", "ssh");
    util::ensure_dir(dir);
    SshBridge bridge(make_cfg(a[1], a[2], a[3], a[4], "ssh-reconnect"), dir);
    if (!bridge.start()) return false;
    if (!bridge.wait_connected(8000)) {
        report("T2-4", "自动重连成功", false, "首连失败: " + bridge.last_error());
        bridge.stop();
        return false;
    }
    bool down = bridge.wait_disconnected(9000);
    std::uint64_t reconnects_before = bridge.reconnect_count();
    bool up = bridge.wait_connected(30000);
    bool again = up && send_and_expect(&bridge, "SB3-T2-RECONNECT\n");
    report("T2-4", "自动重连成功", down && up && again,
           "down=" + std::to_string(down) + " up=" + std::to_string(up) +
               " echo=" + std::to_string(again) + " reconnects=" +
               std::to_string(bridge.reconnect_count() - reconnects_before));
    bridge.stop();
    return down && up && again;
}

// ---------------- T2-5 ----------------

bool run_two(const std::vector<std::string>& a) {
    std::string dir = util::join_path("output\\selftest", "ssh");
    util::ensure_dir(dir);
    SshBridge b1(make_cfg(a[1], a[2], a[4], a[5], "ssh-two-1"), dir);
    SshBridge b2(make_cfg(a[1], a[3], a[4], a[5], "ssh-two-2"), dir);
    bool s1 = b1.start();
    bool s2 = b2.start();
    bool up1 = s1 && b1.wait_connected(8000);
    bool up2 = s2 && b2.wait_connected(8000);
    bool echo1 = up1 && send_and_expect(&b1, "SB3-T2-L1\n");
    bool echo2 = up2 && send_and_expect(&b2, "SB3-T2-L2\n");

    b1.stop();  // 停掉一条链路
    Sleep(50);
    bool echo2_after = up2 && !b1.running() && send_and_expect(&b2, "SB3-T2-L2B\n");
    bool ok = echo1 && echo2 && echo2_after;
    report("T2-5", "两条链路互不影响", ok,
           "up1=" + std::to_string(up1) + " up2=" + std::to_string(up2) +
               " echo1=" + std::to_string(echo1) + " echo2=" + std::to_string(echo2) +
               " survive=" + std::to_string(echo2_after));
    b2.stop();
    return ok;
}

}  // namespace

int ssh_selftest_main(const std::vector<std::string>& args) {
    std::string mode = args.empty() ? std::string() : args[0];
    bool ok = false;
    std::size_t need = 5;
    if (mode == "ssh-two") need = 6;
    if (args.size() < need) {
        std::printf("FAIL T2 usage: ssh-<mode> needs %u args\n", static_cast<unsigned>(need));
        return 1;
    }
    if (mode == "ssh") {
        ok = run_echo(args);
    } else if (mode == "ssh-auth") {
        ok = run_auth_fail(args);
    } else if (mode == "ssh-drop") {
        ok = run_drop(args);
    } else if (mode == "ssh-reconnect") {
        ok = run_reconnect(args);
    } else if (mode == "ssh-two") {
        ok = run_two(args);
    } else {
        std::printf("FAIL T2 unknown mode %s\n", mode.c_str());
        return 1;
    }
    std::printf("RESULT: %s\n", (ok && g_fail == 0) ? "ALL PASS" : "FAIL");
    return (ok && g_fail == 0) ? 0 : 1;
}

}  // namespace sb3
