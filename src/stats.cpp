#include "stats.h"
#include "json.h"
#include <chrono>

namespace ratelimiter {

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void StatsCollector::recordAllowed(const std::string& client_id) {
    total_allowed_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto& cs = clients_[client_id];
    cs.allowed++;
    cs.last_request_ms = nowMs();
}

void StatsCollector::recordDenied(const std::string& client_id) {
    total_denied_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto& cs = clients_[client_id];
    cs.denied++;
    cs.last_request_ms = nowMs();
}

uint64_t StatsCollector::totalAllowed() const { return total_allowed_.load(std::memory_order_relaxed); }
uint64_t StatsCollector::totalDenied() const { return total_denied_.load(std::memory_order_relaxed); }
uint64_t StatsCollector::totalRequests() const { return totalAllowed() + totalDenied(); }

double StatsCollector::denyRate() const {
    uint64_t total = totalRequests();
    if (total == 0) return 0.0;
    return static_cast<double>(totalDenied()) / static_cast<double>(total);
}

StatsCollector::ClientStats StatsCollector::getClientStats(const std::string& client_id) const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = clients_.find(client_id);
    if (it == clients_.end()) return ClientStats{};
    return it->second;
}

std::string StatsCollector::globalJson() const {
    JsonBuilder jb;
    jb.add("total_requests", (unsigned long long)totalRequests())
      .add("total_allowed", (unsigned long long)totalAllowed())
      .add("total_denied", (unsigned long long)totalDenied())
      .add("deny_rate", denyRate());
    size_t client_count;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        client_count = clients_.size();
    }
    jb.add("unique_clients", (unsigned long long)client_count);
    return jb.build();
}

std::string StatsCollector::clientJson(const std::string& client_id) const {
    ClientStats cs = getClientStats(client_id);
    JsonBuilder jb;
    jb.add("client_id", client_id)
      .add("allowed", (unsigned long long)cs.allowed)
      .add("denied", (unsigned long long)cs.denied)
      .add("last_request_ms", (long long)cs.last_request_ms);
    return jb.build();
}

} // namespace ratelimiter
