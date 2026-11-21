// term_types.h — 终端行模型：一个单元格 / 一行 / 颜色表。
// 只放纯数据与纯函数，不碰 Win32，便于自检直接跑。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sb3 {
namespace term {

constexpr std::uint32_t kDefaultColor = 0xFFFFFFFFu;   // 用窗口默认前景/背景
constexpr std::uint32_t kColorMask    = 0x00FFFFFFu;

constexpr std::uint8_t kAttrBold      = 1u << 0;
constexpr std::uint8_t kAttrReverse   = 1u << 1;
constexpr std::uint8_t kAttrUnderline = 1u << 2;

struct Cell {
    char32_t      ch        = U' ';
    std::uint32_t fg        = kDefaultColor;
    std::uint32_t bg        = kDefaultColor;
    std::uint8_t  attr      = 0;
    // 宽字符（中文/表情）占两格：第二格置 wide_cont，绘制时跳过
    bool          wide_cont = false;
};

struct Line {
    std::vector<Cell> cells;
    bool              wrapped = false;   // 该行由自动换行产生（不是自然行尾）
};

// 行文本：默认裁掉行尾空白（复制/搜索都按这个口径）
std::string line_text(const Line& line, bool keep_trailing = false);

// 行宽（按字符计，宽字符算 2 格）
int line_cells(const Line& line);

// 16 色 / 256 色索引 → 0xRRGGBB；越界返回 kDefaultColor
std::uint32_t palette_color(int index);

// 该码位是否占两格
bool codepoint_wide(char32_t cp);

}  // namespace term
}  // namespace sb3
