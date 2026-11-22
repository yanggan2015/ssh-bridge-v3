// selftest.cpp — 无窗口自检：ring / 配置 / 串口回环。
// 用法： ssh_bridge3.exe --selftest ring|config|serial A B|all [A] [B]
// 成功打印 PASS 行并返回 0；失败打印 FAIL 行并返回 1。

#include "app/selftest.h"

#include "app/app_core.h"
#include "app/selftest_ssh.h"
#include "app/selftest_term.h"
#include "core/bridge/serial/serial_bridge.h"
#include "core/config/config.h"
#include "core/logbuf/logbuf.h"
#include "core/util/util.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sb3 {
namespace {

int g_fail = 0;

void report(const char* id, const char* name, bool ok, const std::string& detail) {
    std::printf("%s %s %s %s\n", ok ? "PASS" : "FAIL", id, name, detail.c_str());
    if (!ok) ++g_fail;
}

std::string temp_root(const char* sub) {
    std::string dir = util::join_path("output\\selftest", sub);
    util::ensure_dir(dir);
    return dir;
}

// ---------- ring ----------

bool ring_basic() {
    std::string dir = temp_root("ring");
    LogBuffer log;
    if (!log.init(256 * 1024, false, false, dir, "ring")) return false;

    std::vector<char> chunk(4096);
    for (std::size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<char>('a' + (i % 26));
    std::vector<char> all;
    const std::size_t total_bytes = 1u << 20;
    for (std::size_t sent = 0; sent < total_bytes; sent += chunk.size()) {
        log.append(chunk.data(), chunk.size());
        if (all.size() < total_bytes) all.insert(all.end(), chunk.begin(), chunk.end());
    }
    if (log.total() != total_bytes) return false;

    std::uint64_t epoch = 0, seen = 0;
    bool dropped = false;
    std::string data = log.read_since(&epoch, &seen, &dropped);
    if (!data.empty() || seen != total_bytes) return false;

    // 再追加 1 MiB，读者必然落后，只应拿到最后 cap 字节
    for (std::size_t sent = 0; sent < total_bytes; sent += chunk.size()) {
        log.append(chunk.data(), chunk.size());
    }
    data = log.read_since(&epoch, &seen, &dropped);
    if (!dropped) return false;
    if (data.size() != log.capacity()) return false;
    if (log.notify_seq() < 2) return false;
    log.shutdown();
    return true;
}

bool ring_wrap() {
    std::string dir = temp_root("ring");
    LogBuffer log;
    if (!log.init(64 * 1024, false, false, dir, "ring")) return false;

    std::uint64_t epoch = 0, seen = 0;
    log.read_since(&epoch, &seen, nullptr);

    std::string stream;          // 累计写入的全部内容
    bool ok = true;
    for (int round = 0; round < 6; ++round) {
        std::string part(12000, static_cast<char>('0' + round));
        log.append(part.data(), part.size());
        stream += part;

        bool dropped = false;
        std::string got = log.read_since(&epoch, &seen, &dropped);
        if (dropped) {            // 落后超容量：只应拿到最后 cap 字节
            std::string tail = stream.substr(stream.size() - log.capacity());
            if (got != tail) ok = false;
        } else if (got != part) {
            ok = false;
        }
    }
    log.shutdown();
    return ok;
}

bool ring_clear() {
    std::string dir = temp_root("ring");
    LogBuffer log;
    if (!log.init(64 * 1024, false, false, dir, "ring")) return false;

    std::string payload(8192, 'x');
    log.append(payload.data(), payload.size());
    std::uint64_t epoch = log.epoch();
    std::uint64_t seen = log.total();

    log.clear();
    if (log.epoch() == epoch) return false;
    if (log.total() != 0) return false;

    bool dropped = false;
    std::string got = log.read_since(&epoch, &seen, &dropped);
    if (!got.empty()) return false;
    if (seen > log.total()) return false;

    log.append("after-clear", 11);
    got = log.read_since(&epoch, &seen, &dropped);
    if (got != "after-clear") return false;
    log.shutdown();
    return true;
}

bool ring_disk() {
    std::string dir = temp_root("ring");
    std::string file = util::join_path(dir, "disk.log");
    std::remove(file.c_str());
    LogBuffer log;
    if (!log.init(64 * 1024, false, true, dir, "disk")) return false;
    std::string line = "line-one\r\nline-two\r\nline-three\r\n";
    log.append(line.data(), line.size());
    log.flush_cr_hold();
    Sleep(400);
    log.shutdown();

    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return false;
    std::string got;
    char buf[512];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) got.append(buf, n);
    std::fclose(f);
    return got == line;
}

bool run_ring() {
    bool ok = true;
    bool basic = ring_basic();
    report("T1-2", "1MiB 无丢字节", basic, "");
    ok = ok && basic;
    bool wrap = ring_wrap();
    report("T1-3", "回绕后 read_since 连续", wrap, "");
    ok = ok && wrap;
    bool clr = ring_clear();
    report("T1-4", "clear 后 epoch 变化且游标夹紧", clr, "");
    ok = ok && clr;
    bool disk = ring_disk();
    report("T1-6", "落盘与 ring 一致", disk, "");
    ok = ok && disk;
    return ok;
}

// ---------- config ----------

std::string sample_json() {
    return std::string(
        "{\n"
        "  \"default\": {\"username\":\"root\",\"password\":\"defpw\",\"buffer_size\":131072,"
        "\"log_enabled\":true,\"log_dir\":\"\"},\n"
        "  \"boards\": [\n"
        "    {\"name\":\"b1\",\"serial_device\":\"COM3\",\"baudrate\":115200,\"databits\":8,"
        "\"parity\":\"N\",\"stopbits_x10\":10,\"flowcontrol\":\"none\",\"encoding\":\"utf-8\","
        "\"line_ending\":\"crlf\",\"timestamp\":true,"
        "\"quick_commands\":[{\"label\":\"Q1\",\"command\":\"ls -l\"}]},\n"
        "    {\"name\":\"b2\",\"serial_device\":\"COM4\",\"baudrate\":921600,\"password\":\"\"}\n"
        "  ],\n"
        "  \"ssh\": [{\"name\":\"s1\",\"target_host\":\"192.168.1.9\",\"target_port\":22}],\n"
        "  \"inbound_ssh\": {\"enable\":false,\"port\":2222},\n"
        "  \"ui\": {\"close_to_tray\":true}\n"
        "}\n");
}

bool write_sample(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::string s = sample_json();
    std::size_t n = std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    return n == s.size();
}

bool same_board(const BoardConfig& a, const BoardConfig& b) {
    bool quick = a.quick_commands.size() == b.quick_commands.size();
    for (std::size_t i = 0; quick && i < a.quick_commands.size(); ++i)
        quick = a.quick_commands[i].label == b.quick_commands[i].label &&
                a.quick_commands[i].command == b.quick_commands[i].command;
    return a.name == b.name && a.serial_device == b.serial_device &&
           a.baudrate == b.baudrate && a.databits == b.databits &&
           a.parity == b.parity && a.stopbits_x10 == b.stopbits_x10 &&
           a.flowcontrol == b.flowcontrol && a.encoding == b.encoding &&
           a.line_ending == b.line_ending && a.timestamp == b.timestamp &&
           a.password == b.password && a.username == b.username && quick;
}

bool run_config() {
    std::string dir = temp_root("config");
    std::string path = util::join_path(dir, "ssh_bridge_config.json");
    std::remove(path.c_str());
    if (!write_sample(path)) {
        report("T1-1", "配置往返一致", false, "cannot write sample");
        return false;
    }

    AppConfig cfg;
    bool load_ok = config_load(cfg, path);
    bool fields_ok = load_ok && cfg.def.buffer_size == 131072 && cfg.boards.size() == 2 &&
                     cfg.boards[0].serial_device == "COM3" && cfg.boards[1].baudrate == 921600;
    bool pass_ok = cfg.boards.size() == 2 &&
                   config_resolve_password(cfg, cfg.boards[0]) == "defpw" &&
                   config_resolve_password(cfg, cfg.boards[1]).empty();

    bool save_ok = load_ok && config_save(cfg);
    AppConfig again;
    bool reload_ok = save_ok && config_load(again, path);
    bool roundtrip = reload_ok && again.boards.size() == 2 &&
                     same_board(again.boards[0], cfg.boards[0]) &&
                     same_board(again.boards[1], cfg.boards[1]) &&
                     again.def.buffer_size == cfg.def.buffer_size;

    std::string err;
    bool patch_ok = config_patch_board(path, "b1", "{\"baudrate\":460800,\"timestamp\":false}", &err);
    AppConfig patched;
    config_load(patched, path);
    bool patch_ok2 = patch_ok && patched.boards.size() == 2 &&
                     patched.boards[0].baudrate == 460800 && !patched.boards[0].timestamp &&
                     patched.boards[1].baudrate == 921600;

    bool del_ok = config_delete_board(path, "b2", &err);
    AppConfig deleted;
    config_load(deleted, path);
    bool del_ok2 = del_ok && deleted.boards.size() == 1 && deleted.boards[0].name == "b1";

    bool ok = fields_ok && pass_ok && roundtrip && patch_ok2 && del_ok2;
    report("T1-1", "配置往返 + patch/delete", ok,
           "load=" + std::to_string(load_ok) + " rtt=" + std::to_string(roundtrip) +
           " patch=" + std::to_string(patch_ok2) + " del=" + std::to_string(del_ok2));
    return ok;
}

// 旧版 boards 是对象；patch/delete 必须无损转成数组，且新 relay_devices 不能被旧 relays 覆盖
bool run_config_migration() {
    std::string dir = temp_root("config_migration");
    std::string path = util::join_path(dir, "legacy.json");
    const std::string json =
        "{"
        "\"boards\":{"
        "\"b1\":{\"serial_device\":\"COM3\",\"baudrate\":115200},"
        "\"b2\":{\"serial_device\":\"COM4\",\"baudrate\":921600}},"
        "\"relays\":{\"old\":{\"windows_port\":\"COM9\",\"channels\":2}},"
        "\"relay_devices\":[{\"name\":\"new\",\"windows_port\":\"COM10\",\"channels\":4}]"
        "}\n";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    bool wrote = f && std::fwrite(json.data(), 1, json.size(), f) == json.size();
    if (f) std::fclose(f);
    if (!wrote) {
        report("T1-9", "旧配置迁移", false, "cannot write legacy json");
        return false;
    }

    AppConfig cfg;
    bool loaded = config_load(cfg, path);
    std::string err;
    bool patch = config_patch_board(path, "b1", "{\"baudrate\":460800}", &err);
    AppConfig patched;
    bool reloaded = config_load(patched, path);
    bool del = config_delete_board(path, "b2", &err);
    AppConfig deleted;
    bool del_reload = config_load(deleted, path);
    bool ok = loaded && cfg.boards.size() == 2 && cfg.boards[0].name == "b1" &&
              cfg.boards[1].name == "b2" && cfg.relays.size() == 1 &&
              cfg.relays[0].name == "new" &&
              patch && reloaded && patched.boards.size() == 2 &&
              patched.boards[0].baudrate == 460800 && patched.boards[1].name == "b2" &&
              del && del_reload && deleted.boards.size() == 1 && deleted.boards[0].name == "b1";
    report("T1-9", "旧配置对象/继电器迁移", ok,
           "load=" + std::to_string(loaded && cfg.boards.size() == 2) +
           " relay=" + std::to_string(cfg.relays.size() == 1 && cfg.relays[0].name == "new") +
           " patch=" + std::to_string(patch && patched.boards.size() == 2));
    return ok;
}

// 编辑串口参数时，对话框没暴露的密码/用户名/缓冲/日志/按钮/快捷指令必须保留
bool run_upsert_preserve() {
    std::string dir = temp_root("upsert_preserve");
    wchar_t old_cwd[MAX_PATH] = L"";
    GetCurrentDirectoryW(MAX_PATH, old_cwd);
    std::wstring wdir;
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()), nullptr, 0);
        if (n > 0) {
            wdir.resize(static_cast<std::size_t>(n));
            MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()), wdir.data(), n);
        }
    }
    if (wdir.empty() || !SetCurrentDirectoryW(wdir.c_str())) {
        report("T1-10", "通道编辑保留隐藏字段", false, "cannot chdir");
        return false;
    }

    AppConfig seed;
    seed.abs_path = "ssh_bridge_config.json";
    BoardConfig b;
    b.name = "b1";
    b.serial_device = "COM3";
    b.log_enabled = false;
    b.username = "root";
    b.password = "secret";
    b.password_present = true;
    b.buffer_size = 65536;
    b.log_dir = "logs";
    b.timestamp = false;
    b.quick_commands.push_back({"Q1", "ls -l"});
    BoardActionConfig btn;
    btn.id = "b1.btn";
    btn.label = "BTN";
    btn.commands = {"echo hi"};
    b.buttons.push_back(btn);
    seed.boards.push_back(b);
    bool wrote = config_save(seed);

    bool ok = false;
    std::string detail;
    if (!wrote) {
        detail = "seed save failed";
    } else {
        AppCore core;
        bool init_ok = core.init(dir);
        BoardConfig edit;
        edit.name = "b1";
        edit.serial_device = "COM6";
        edit.log_enabled = false;
        std::string err;
        bool upsert = init_ok && core.upsert_serial(edit, "b1", &err);
        const BoardConfig* after = core.board_of("b1");
        ok = upsert && after &&
             after->serial_device == "COM6" &&
             after->username == "root" && after->password == "secret" && after->password_present &&
             after->buffer_size == 65536 && after->log_dir == "logs" && !after->timestamp &&
             after->quick_commands.size() == 1 && after->quick_commands[0].command == "ls -l" &&
             after->buttons.size() == 1 && after->buttons[0].label == "BTN";
        detail = "init=" + std::to_string(init_ok) + " upsert=" + std::to_string(upsert) +
                 (err.empty() ? "" : " err=" + err);
    }
    SetCurrentDirectoryW(old_cwd);
    report("T1-10", "通道编辑保留隐藏字段", ok, detail);
    return ok;
}

// ---------- serial ----------

HANDLE open_raw(const std::string& dev, int baud) {
    std::wstring path = std::wstring(L"\\\\.\\") + std::wstring(dev.begin(), dev.end());
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return h;
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(h, &dcb);
    dcb.BaudRate = static_cast<DWORD>(baud);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    SetCommState(h, &dcb);
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 50;
    to.ReadTotalTimeoutConstant = 200;
    SetCommTimeouts(h, &to);
    return h;
}

bool read_raw(HANDLE h, std::string* out, unsigned total_wait_ms) {
    char buf[512];
    DWORD got = 0;
    std::uint64_t start = util::now_ms();
    while (util::now_ms() - start < total_wait_ms) {
        if (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
            out->append(buf, got);
            got = 0;
        } else {
            Sleep(20);
        }
    }
    return !out->empty();
}

bool loop_once(SerialBridge* b, const std::string& msg, const char* want) {
    std::uint64_t epoch = 0, seen = 0;
    b->read_since(&epoch, &seen);
    b->write(msg.data(), msg.size());
    std::string got;
    for (int i = 0; i < 20; ++i) {
        Sleep(50);
        got += b->read_since(&epoch, &seen);
        if (got.find(want) != std::string::npos) return true;
    }
    return false;
}

bool loop_selftest(const std::string& dev) {
    std::string dir = temp_root("serial");
    BoardConfig cfg;
    cfg.name = "selftest";
    cfg.serial_device = dev;
    cfg.baudrate = 115200;
    cfg.timestamp = false;
    cfg.log_enabled = false;
    cfg.buffer_size = 64 * 1024;

    SerialBridge bridge(cfg, dir);
    if (!bridge.start()) {
        report("T1-5", "回环收发一致", false, "start failed: " + bridge.last_error());
        return false;
    }
    bool rx1 = loop_once(&bridge, "SB3-LOOP-1\r\n", "SB3-LOOP-1");
    bool rx2 = loop_once(&bridge, "SB3-LOOP-2\r\n", "SB3-LOOP-2");
    bridge.stop();
    bool stopped = !bridge.running();

    bool restarted = bridge.start();
    bool rx3 = restarted && loop_once(&bridge, "SB3-LOOP-3\r\n", "SB3-LOOP-3");
    bridge.stop();
    bool ok = rx1 && rx2 && stopped && rx3;
    report("T1-5", "回环收发一致", ok,
           "rx1=" + std::to_string(rx1) + " rx2=" + std::to_string(rx2) +
           " stop=" + std::to_string(stopped) + " restart=" + std::to_string(rx3));
    return ok;
}

bool real_loop_test(const std::string& dev_a, const std::string& dev_b) {
    std::string dir = temp_root("serial");
    BoardConfig cfg;
    cfg.name = "selftest";
    cfg.serial_device = dev_a;
    cfg.baudrate = 115200;
    cfg.timestamp = false;
    cfg.log_enabled = false;
    cfg.buffer_size = 64 * 1024;

    SerialBridge bridge(cfg, dir);
    if (!bridge.start()) {
        report("T1-5", "回环收发一致", false, "open " + dev_a + ": " + bridge.last_error());
        return false;
    }
    HANDLE raw = open_raw(dev_b, cfg.baudrate);
    if (raw == INVALID_HANDLE_VALUE) {
        bridge.stop();
        report("T1-5", "回环收发一致", false, "open " + dev_b + " failed");
        return false;
    }
    std::uint64_t epoch = 0, seen = 0;
    bridge.read_since(&epoch, &seen);
    std::string ping = "SB3-PING\r\n";
    DWORD wrote = 0;
    WriteFile(raw, ping.data(), static_cast<DWORD>(ping.size()), &wrote, nullptr);
    Sleep(300);
    std::string rx = bridge.read_since(&epoch, &seen);
    bool rx_ok = rx.find("SB3-PING") != std::string::npos;

    std::string pong = "SB3-PONG\r\n";
    bridge.write(pong.data(), pong.size());
    std::string peer;
    bool tx_ok = read_raw(raw, &peer, 600) && peer.find("SB3-PONG") != std::string::npos;
    CloseHandle(raw);
    bridge.stop();
    report("T1-5", "回环收发一致", rx_ok && tx_ok,
           "rx=" + std::to_string(rx_ok) + " tx=" + std::to_string(tx_ok));
    return rx_ok && tx_ok;
}

bool busy_once(const std::string& dev) {
    std::string dir = temp_root("serial");
    BoardConfig cfg;
    cfg.name = "busy";
    cfg.serial_device = dev;
    cfg.timestamp = false;
    cfg.log_enabled = false;
    cfg.buffer_size = 64 * 1024;

    SerialBridge first(cfg, dir);
    if (!first.start()) return false;
    SerialBridge second(cfg, dir);
    bool ok = !second.start() && second.last_error().find("busy") != std::string::npos;
    second.stop();
    first.stop();
    return ok;
}

bool busy_any(const std::string& hint) {
    std::vector<std::string> ports = {"COM3", "COM4", "COM5"};
    if (!hint.empty() && hint.rfind("loop:", 0) != 0) ports.insert(ports.begin(), hint);
    for (const std::string& p : ports) {
        if (busy_once(p)) return true;
    }
    return false;
}

bool run_serial(const std::string& dev_a, const std::string& dev_b) {
    bool loop_mode = dev_a.rfind("loop:", 0) == 0;
    bool ok = loop_mode ? loop_selftest(dev_a) : real_loop_test(dev_a, dev_b);
    bool busy = busy_any(loop_mode ? std::string() : dev_a);
    report("T1-7", "端口被占用时错误明确", busy, busy ? "" : "没有可用的真实串口");
    return ok && busy;
}

}  // namespace

int selftest_main(const std::vector<std::string>& args) {
    std::string mode = args.empty() ? "all" : args[0];
    bool ok = true;
    if (mode.rfind("ssh", 0) == 0) {
        return ssh_selftest_main(args);
    }
    if (mode.rfind("term", 0) == 0) {
        return term_selftest_main(args);
    }
    if (mode == "ring" || mode == "all") ok = run_ring() && ok;
    if (mode == "config" || mode == "all") ok = run_config() && ok;
    if (mode == "config" || mode == "all") ok = run_config_migration() && ok;
    if (mode == "config" || mode == "all") ok = run_upsert_preserve() && ok;
    if (mode == "serial") {
        bool loop = args.size() >= 2 && args[1].rfind("loop:", 0) == 0;
        if (args.size() < 2 || (!loop && args.size() < 3)) {
            std::printf("FAIL T1-5 serial needs ports\n");
            return 1;
        }
        std::string b = args.size() >= 3 ? args[2] : std::string();
        ok = run_serial(args[1], b) && ok;
    }
    if (mode == "all") {
        bool loop = args.size() >= 2 && args[1].rfind("loop:", 0) == 0;
        if (loop || args.size() >= 3) {
            std::string b = args.size() >= 3 ? args[2] : std::string();
            ok = run_serial(args[1], b) && ok;
        }
    }
    std::printf("RESULT: %s\n", (ok && g_fail == 0) ? "ALL PASS" : "FAIL");
    return (ok && g_fail == 0) ? 0 : 1;
}

}  // namespace sb3
