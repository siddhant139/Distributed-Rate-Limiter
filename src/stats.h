#pragma once

#include <string>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace ratelimiter {

class StatsCollector {
public:
    void recordAllowed(const std::string& client_id);
    void recordDenied(const std::string& client_id);

    uint64_t totalAllowed() const;
    uint64_t totalDenied() const;
    uint64_t totalRequests() const;
    double denyRate() const;

    struct ClientStats {
        uint64_t allowed = 0;
        uint64_t denied = 0;
        int64_t last_request_ms = 0;
    };
    ClientStats getClientStats(const std::string& client_id) const;

    // Serialize global stats to JSON
    std::string globalJson() const;
    // Serialize a single client's stats to JSON
    std::string clientJson(const std::string& client_id) const;

private:
    std::atomic<uint64_t> total_allowed_{0};
    std::atomic<uint64_t> total_denied_{0};

    mutable std::mutex clients_mutex_;
    std::unordered_map<std::string, ClientStats> clients_;
};

} // namespace ratelimiter
