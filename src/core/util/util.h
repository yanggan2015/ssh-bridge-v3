// util.h — 与界面无关的小工具：路径、时间、编码、行尾处理。
// 约定：所有对外字符串一律 UTF-8；Win32 调用前在这里转宽字符。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sb3 {
namespace util {

// 环形缓冲下标：绝对偏移 -> 物理下标
inline std::size_t ring_index(std::uint64_t pos, std::size_t cap) {
    return cap ? static_cast<std::size_t>(pos % cap) : 0;
}

// 毫秒级单调时钟（GetTickCount64），用于超时与状态刷新
std::uint64_t now_ms();

// 本地时间 "HH:MM:SS.mmm "，用于行首时间戳
std::string log_time_stamp();

// "YYYY-MM-DD HH:MM:SS" 便于日志文件名/状态展示
std::string log_time_text();

// 递归创建目录（UTF-8 路径），已存在返回 true
bool ensure_dir(const std::string& path);

// 取文件所在目录；无分隔符返回 "."
std::string dir_of(const std::string& path);

// 拼接路径，自动补一个分隔符
std::string join_path(const std::string& a, const std::string& b);

// UTF-8 <-> GBK（CP936）；转换失败返回空串
std::string gbk_to_utf8(const std::string& in);
std::string utf8_to_gbk(const std::string& in);

// 依配置把下行文本末尾补行尾：none / cr / lf / crlf
std::string apply_line_ending(const std::string& text, const std::string& mode);

// 去首尾空白；小写化（仅 ASCII）
std::string trim(const std::string& s);
std::string lower(const std::string& s);

// 宽松布尔：1/true/on/yes 为真（大小写无关）
bool to_bool(const std::string& s, bool fallback);

}  // namespace util
}  // namespace sb3
