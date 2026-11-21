// util.cpp — Win32 实现。注意：只在 core 内使用，不碰 UI。

#include "core/util/util.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace sb3 {
namespace util {
namespace {

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string from_wide(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// 通用代码页 -> UTF-8
std::string cp_to_utf8(const std::string& in, UINT cp) {
    if (in.empty()) return std::string();
    int n = MultiByteToWideChar(cp, 0, in.c_str(), static_cast<int>(in.size()), nullptr, 0);
    if (n <= 0) return std::string();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(cp, 0, in.c_str(), static_cast<int>(in.size()), w.data(), n);
    return from_wide(w);
}

// UTF-8 -> 通用代码页
std::string utf8_to_cp(const std::string& in, UINT cp) {
    if (in.empty()) return std::string();
    std::wstring w = to_wide(in);
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(cp, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(cp, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(GetTickCount64());
}

std::string log_time_stamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buf);
}

std::string log_time_text() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return std::string(buf);
}

bool ensure_dir(const std::string& path) {
    if (path.empty()) return false;
    std::wstring w = to_wide(path);
    if (w.empty()) return false;
    DWORD attr = GetFileAttributesW(w.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    // 逐级创建，避免每层都调一次本函数
    std::wstring cur;
    for (std::size_t i = 0; i < w.size(); ++i) {
        wchar_t c = w[i];
        cur.push_back(c);
        bool slash = (c == L'\\' || c == L'/');
        if (slash && cur.size() > 1 && cur[cur.size() - 2] != L':') {
            CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryW(w.c_str(), nullptr);
    attr = GetFileAttributesW(w.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string dir_of(const std::string& path) {
    std::size_t p = path.find_last_of("\\/");
    if (p == std::string::npos) return std::string(".");
    if (p == 0) return path.substr(0, 1);
    return path.substr(0, p);
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a.back();
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

std::string gbk_to_utf8(const std::string& in) {
    return cp_to_utf8(in, 936);
}

std::string utf8_to_gbk(const std::string& in) {
    return utf8_to_cp(in, 936);
}

std::string apply_line_ending(const std::string& text, const std::string& mode) {
    if (text.empty()) return text;
    if (mode == "none") return text;
    if (mode == "cr") return text + "\r";
    if (mode == "lf") return text + "\n";
    if (mode == "crlf") return text + "\r\n";
    return text;
}

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool to_bool(const std::string& s, bool fallback) {
    std::string v = lower(trim(s));
    if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "off" || v == "no") return false;
    return fallback;
}

}  // namespace util
}  // namespace sb3
