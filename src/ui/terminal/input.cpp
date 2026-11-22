// input.cpp — 键序列与 UTF-8 转换实现（S4）。

#include "ui/terminal/input.h"

namespace sb3 {
namespace term {
namespace {

std::string arrow(char final_byte, bool ctrl, bool alt, bool shift) {
    std::string prefix = "\x1b[";
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    if (mod > 1) {
        prefix += "1;";
        prefix += std::to_string(mod);
    }
    prefix.push_back(final_byte);
    return prefix;
}

std::string tilde(int code, bool ctrl, bool alt, bool shift) {
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    if (mod > 1) return "\x1b[" + std::to_string(code) + ";" + std::to_string(mod) + "~";
    return "\x1b[" + std::to_string(code) + "~";
}

}  // namespace

bool is_special_key(UINT vk) {
    switch (vk) {
    case VK_UP:
    case VK_DOWN:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_HOME:
    case VK_END:
    case VK_INSERT:
    case VK_DELETE:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_ESCAPE:
    case VK_TAB:
    case VK_BACK:
    case VK_F1:
    case VK_F2:
    case VK_F3:
    case VK_F4:
    case VK_F5:
    case VK_F6:
    case VK_F7:
    case VK_F8:
    case VK_F9:
    case VK_F10:
    case VK_F11:
    case VK_F12:
        return true;
    default:
        return false;
    }
}

std::string encode_key(UINT vk, bool ctrl, bool alt, bool shift) {
    switch (vk) {
    case VK_UP:
        return arrow('A', ctrl, alt, shift);
    case VK_DOWN:
        return arrow('B', ctrl, alt, shift);
    case VK_RIGHT:
        return arrow('C', ctrl, alt, shift);
    case VK_LEFT:
        return arrow('D', ctrl, alt, shift);
    case VK_HOME:
        return arrow('H', ctrl, alt, shift);
    case VK_END:
        return arrow('F', ctrl, alt, shift);
    case VK_INSERT:
        return tilde(2, ctrl, alt, shift);
    case VK_DELETE:
        return tilde(3, ctrl, alt, shift);
    case VK_PRIOR:
        return tilde(5, ctrl, alt, shift);
    case VK_NEXT:
        return tilde(6, ctrl, alt, shift);
    case VK_ESCAPE:
        return "\x1b";
    case VK_TAB:
        return shift ? "\x1b[Z" : "\t";
    case VK_BACK:
        return "\x7f";
    case VK_F1:
        return tilde(11, ctrl, alt, shift);
    case VK_F2:
        return tilde(12, ctrl, alt, shift);
    case VK_F3:
        return tilde(13, ctrl, alt, shift);
    case VK_F4:
        return tilde(14, ctrl, alt, shift);
    case VK_F5:
        return tilde(15, ctrl, alt, shift);
    case VK_F6:
        return tilde(17, ctrl, alt, shift);
    case VK_F7:
        return tilde(18, ctrl, alt, shift);
    case VK_F8:
        return tilde(19, ctrl, alt, shift);
    case VK_F9:
        return tilde(20, ctrl, alt, shift);
    case VK_F10:
        return tilde(21, ctrl, alt, shift);
    case VK_F11:
        return tilde(23, ctrl, alt, shift);
    case VK_F12:
        return tilde(24, ctrl, alt, shift);
    default:
        return std::string();
    }
}

std::string encode_codepoint(unsigned int cp) {
    if (cp == '\r') return "\r";
    if (cp == '\n') return "\n";
    if (cp == '\t') return "\t";
    if (cp == 0x08) return "\x7f";
    if (cp < 0x20 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return std::string();
    if (cp < 0x80) return std::string(1, static_cast<char>(cp));
    if (cp < 0x800) {
        char out[2] = {static_cast<char>(0xC0 | (cp >> 6)),
                       static_cast<char>(0x80 | (cp & 0x3F))};
        return std::string(out, 2);
    }
    if (cp < 0x10000) {
        char out[3] = {static_cast<char>(0xE0 | (cp >> 12)),
                       static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                       static_cast<char>(0x80 | (cp & 0x3F))};
        return std::string(out, 3);
    }
    char out[4] = {static_cast<char>(0xF0 | (cp >> 18)),
                   static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
                   static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                   static_cast<char>(0x80 | (cp & 0x3F))};
    return std::string(out, 4);
}

std::string encode_char(unsigned int ch, bool ctrl, bool alt) {
    if (ch == '\r') return "\r";
    if (ch == '\n') return "\n";
    if (ch == '\t') return "\t";
    if (ch == 0x08) return "\x7f";
    if (ctrl && !alt) {
        if (ch >= 'a' && ch <= 'z') return std::string(1, static_cast<char>(ch - 'a' + 1));
        if (ch >= 'A' && ch <= 'Z') return std::string(1, static_cast<char>(ch - 'A' + 1));
        if (ch == ' ' || ch == '@' || ch == '2') return std::string(1, '\0');
        if (ch == '[') return "\x1b";
        if (ch == '\\') return std::string(1, static_cast<char>(0x1c));
        if (ch == ']') return std::string(1, static_cast<char>(0x1d));
        if (ch == '^' || ch == '6') return std::string(1, static_cast<char>(0x1e));
        if (ch == '_' || ch == '-') return std::string(1, static_cast<char>(0x1f));
    }
    return encode_codepoint(ch);
}

std::string encode_paste(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() + 8);
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        char c = utf8[i];
        if (c == '\r') {
            out.push_back('\r');
            if (i + 1 < utf8.size() && utf8[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            out.push_back('\r');
        } else if (c == '\t' || c >= 0x20) {
            out.push_back(c);
        }
    }
    return out;
}

std::string utf16_to_utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

std::wstring utf8_to_utf16(const std::string& text) {
    if (text.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                   nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need);
    return out;
}

}  // namespace term
}  // namespace sb3
