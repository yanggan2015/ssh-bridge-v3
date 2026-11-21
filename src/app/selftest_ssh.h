// selftest_ssh.h — S2 SSH 自检子模式入口。

#pragma once

#include <string>
#include <vector>

namespace sb3 {

// 支持：
//   ssh <host> <port> <user> <pass>             T2-1/T2-2
//   ssh-auth <host> <port> <user> <badpass>     T2-6
//   ssh-drop <host> <port> <user> <pass>        T2-3
//   ssh-reconnect <host> <port> <user> <pass>   T2-4
//   ssh-two <host> <p1> <p2> <user> <pass>      T2-5
int ssh_selftest_main(const std::vector<std::string>& args);

}  // namespace sb3
