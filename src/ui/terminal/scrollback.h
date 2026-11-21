// scrollback.h — 回看缓冲：按行环形保存滚出屏幕的行，绝对行号寻址。
// 容量按行数（默认 5000），超出丢最老；行号单调递增，便于"按行定位"。

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

#include "ui/terminal/term_types.h"

namespace sb3 {
namespace term {

class Scrollback {
public:
    explicit Scrollback(std::size_t cap = 5000) { set_capacity(cap); }

    void set_capacity(std::size_t cap);
    std::size_t capacity() const { return cap_; }

    void push(Line&& line);
    void clear();

    bool empty() const { return lines_.empty(); }
    std::size_t count() const { return lines_.size(); }
    std::uint64_t base() const { return base_; }     // 最老一行绝对行号
    std::uint64_t end() const { return base_ + lines_.size(); }

    const Line* at(std::uint64_t abs) const;
    std::string text(std::uint64_t abs) const;

    struct Hit {
        std::uint64_t abs = 0;
        int           col = 0;    // 起点列（按格）
        int           len = 0;    // 命中字符数（按格）
    };

    // 在回看范围内查找；start 是绝对行号，forward=false 表示向上找
    bool find(const std::string& needle, std::uint64_t start, bool forward, Hit* hit) const;

private:
    std::deque<Line> lines_;
    std::size_t      cap_ = 5000;
    std::uint64_t    base_ = 0;
};

}  // namespace term
}  // namespace sb3
