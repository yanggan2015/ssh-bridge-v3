// agent_lock.cpp — C5：按通道租约锁。
#include "services/agent/agent_lock.h"

#include <algorithm>
#include <chrono>

namespace sb3 {
namespace agent {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void LeaseLocks::expire_locked(const std::string& channel) {
    auto it = leases_.find(channel);
    if (it == leases_.end()) return;
    if (it->second.active && it->second.expires_ms <= now_ms()) {
        it->second.active = false;
        it->second.remaining_ms = 0;
        it->second.owner.clear();
        it->second.session.clear();
    }
}

bool LeaseLocks::acquire(const std::string& channel, const std::string& session, int ttl_ms,
                         LeaseInfo* out, LeaseInfo* blocker) {
    std::lock_guard<std::mutex> g(mu_);
    expire_locked(channel);
    auto it = leases_.find(channel);
    if (it != leases_.end() && it->second.active && it->second.owner != session) {
        if (blocker) *blocker = it->second;
        if (blocker) blocker->remaining_ms = std::max<std::int64_t>(0, blocker->expires_ms - now_ms());
        return false;
    }
    LeaseInfo li;
    if (it != leases_.end()) li = it->second;  // 保留 last_takeover
    std::int64_t t = now_ms();
    li.active = true;
    li.channel = channel;
    li.owner = session;
    li.session = session;
    li.ttl_ms = ttl_ms;
    li.acquired_ms = t;
    li.expires_ms = t + ttl_ms;
    li.remaining_ms = ttl_ms;
    leases_[channel] = li;
    if (out) {
        *out = li;
        out->remaining_ms = ttl_ms;
    }
    return true;
}

int LeaseLocks::check(const std::string& channel, const std::string& session, LeaseInfo* blocker) {
    std::lock_guard<std::mutex> g(mu_);
    expire_locked(channel);
    auto it = leases_.find(channel);
    if (it == leases_.end() || !it->second.active) return 0;
    if (it->second.owner == session && !session.empty()) return 0;
    if (blocker) {
        *blocker = it->second;
        blocker->remaining_ms = std::max<std::int64_t>(0, blocker->expires_ms - now_ms());
    }
    return 1;
}

bool LeaseLocks::release(const std::string& channel, const std::string& session,
                         bool* released, LeaseInfo* blocker) {
    std::lock_guard<std::mutex> g(mu_);
    if (released) *released = false;
    expire_locked(channel);
    auto it = leases_.find(channel);
    if (it == leases_.end() || !it->second.active) return true;
    if (it->second.owner != session) {
        if (blocker) {
            *blocker = it->second;
            blocker->remaining_ms = std::max<std::int64_t>(0, blocker->expires_ms - now_ms());
        }
        return false;
    }
    it->second.active = false;
    it->second.owner.clear();
    it->second.session.clear();
    it->second.remaining_ms = 0;
    if (released) *released = true;
    return true;
}

bool LeaseLocks::takeover(const std::string& channel, const std::string& actor,
                          const std::string& reason, LeaseInfo* previous) {
    std::lock_guard<std::mutex> g(mu_);
    expire_locked(channel);
    LeaseInfo li;
    auto it = leases_.find(channel);
    if (it != leases_.end()) li = it->second;
    if (previous && li.active) *previous = li;
    li.channel = channel;
    li.active = false;
    li.owner.clear();
    li.session.clear();
    li.remaining_ms = 0;
    li.last_takeover_reason = reason;
    li.last_takeover_actor = actor;
    li.last_takeover_previous_owner = previous && previous->active ? previous->owner : std::string();
    li.last_takeover_ms = now_ms();
    leases_[channel] = li;
    return true;
}

bool LeaseLocks::human_input(const std::string& channel, const std::string& reason) {
    std::lock_guard<std::mutex> g(mu_);
    expire_locked(channel);
    auto it = leases_.find(channel);
    if (it == leases_.end() || !it->second.active) return false;
    LeaseInfo prev = it->second;
    LeaseInfo li = it->second;
    li.active = false;
    li.owner.clear();
    li.session.clear();
    li.remaining_ms = 0;
    li.last_takeover_reason = reason;
    li.last_takeover_actor = "local-human";
    li.last_takeover_previous_owner = prev.owner;
    li.last_takeover_ms = now_ms();
    leases_[channel] = li;
    return true;
}

LeaseInfo LeaseLocks::get(const std::string& channel) {
    std::lock_guard<std::mutex> g(mu_);
    expire_locked(channel);
    auto it = leases_.find(channel);
    if (it == leases_.end()) return LeaseInfo();
    LeaseInfo li = it->second;
    if (li.active) li.remaining_ms = std::max<std::int64_t>(0, li.expires_ms - now_ms());
    return li;
}

std::vector<LeaseInfo> LeaseLocks::list() {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<LeaseInfo> out;
    for (auto& kv : leases_) {
        expire_locked(kv.first);
        if (kv.second.active) {
            LeaseInfo li = kv.second;
            li.remaining_ms = std::max<std::int64_t>(0, li.expires_ms - now_ms());
            out.push_back(li);
        }
    }
    return out;
}

void LeaseLocks::clear_all() {
    std::lock_guard<std::mutex> g(mu_);
    leases_.clear();
}

LeaseLocks& lease_locks() {
    static LeaseLocks locks;
    return locks;
}

}  // namespace agent
}  // namespace sb3
