// selftest_term.h — 终端模型自检入口（S4）。
#pragma once

#include <string>
#include <vector>

namespace sb3 {

// 返回 0 = 全 PASS；1 = 有 FAIL。args[0] 形如 term / term-scroll
int term_selftest_main(const std::vector<std::string>& args);

}  // namespace sb3
