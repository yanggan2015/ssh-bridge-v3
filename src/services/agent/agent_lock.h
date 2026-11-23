// agent_lock.h — C5：按通道的租约锁。仅内存态，进程退出即释放。
#ifndef SSH_BRIDGE_AGENT_LOCK_H
#define SSH_BRIDGE_AGENT_LOCK_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace sb3 {
namespace agent {

struct LeaseInfo {
    bool active = false;
    std::string channel;
    std::string owner;
    std::string session;
    int ttl_ms = 0;
    std::int64_t acquired_ms = 0;
    std::int64_t expires_ms = 0;
    std::int64_t remaining_ms = 0;
    std::string last_takeover_reason;
    std::string last_takeover_actor;
    std::string last_takeover_previous_owner;
    std::int64_t last_takeover_ms = 0;
};

class LeaseLocks {
public:
    // 获取或续租；被他人持有返回 false 并填 blocker。
    bool acquire(const std::string& channel, const std::string& session, int ttl_ms,
                 LeaseInfo* out, LeaseInfo* blocker);
    // 0=允许，1=被他人锁定（blocker 有值）。
    int check(const std::string& channel, const std::string& session, LeaseInfo* blocker);
    // 只释放自己的锁；no lock 时 released=false；他人锁返回 false。
    bool release(const std::string& channel, const std::string& session,
                 bool* released, LeaseInfo* blocker);
    // 人工显式接管：清掉旧锁并记录 last_takeover。
    bool takeover(const std::string& channel, const std::string& actor,
                  const std::string& reason, LeaseInfo* previous);
    // 本地键盘/鼠标输入被视为人工接管；无锁时不写审计。
    bool human_input(const std::string& channel, const std::string& reason);
    LeaseInfo get(const std::string& channel);
    std::vector<LeaseInfo> list();
    void clear_all();

private:
    void expire_locked(const std::string& channel);
    mutable std::mutex mu_;
    std::map<std::string, LeaseInfo> leases_;
};

LeaseLocks& lease_locks();

}  // namespace agent
}  // namespace sb3

#endif
