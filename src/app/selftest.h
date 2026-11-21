// selftest.h — 无窗口自检入口（S1 起）
#pragma once

#include <string>
#include <vector>

namespace sb3 {

// 返回 0 = 全 PASS；1 = 有 FAIL。args[0] 为子命令。
int selftest_main(const std::vector<std::string>& args);

}  // namespace sb3
