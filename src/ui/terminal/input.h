// input.h — 键盘/粘贴 → 上行字节的映射（S4）。
// 不依赖窗口：传 VK/字符进来，出 UTF-8 字节串，便于单测与脚本复用。

#pragma once

#include <windows.h>

#include <string>

namespace sb3 {
namespace term {

// 需要按"功能键"发给远端、而不是当普通字符的键
bool is_special_key(UINT vk);

// 功能键映射（方向键 / Home / End / Ins / Del / PgUp / PgDn / F1–F12 / Esc / Tab / Back）
// shift 参与时对方向键发 CSI 1;2X（与 xterm 一致）
std::string encode_key(UINT vk, bool ctrl, bool alt, bool shift);

// 把 Unicode 码点直接编码为 UTF-8；供桥接文本输入保留 BMP 外字符（emoji 等）
std::string encode_codepoint(unsigned int cp);

// WM_CHAR 走这里：可见字符 → UTF-8；Ctrl+字母 → 控制字节；Enter → CR
std::string encode_char(unsigned int ch, bool ctrl, bool alt);

// 粘贴：原样写（换行统一成 CR），不做命令行补行尾
std::string encode_paste(const std::string& utf8);

// UTF-16 → UTF-8（代理对合并）
std::string utf16_to_utf8(const std::wstring& text);

// UTF-8 → UTF-16（供剪贴板）
std::wstring utf8_to_utf16(const std::string& text);

}  // namespace term
}  // namespace sb3
