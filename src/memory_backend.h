#pragma once

#include "backend.h"
#include <string>
#include <unordered_map>
#include <deque>
#include <mutex>

namespace ratelimiter {

// Single-instance backend. Each check runs under an exclusive lock, so the
// whole read-decide-write sequence is atomic within this process.
class MemoryBackend : public RateLimitBackend {
public:
    MemoryBackend() = default;

    BucketDecision tokenBucketCheck(const std::string& key, double capacity,
                                    double refill_per_sec, int64_t now_ms) override;
    WindowDecision slidingWindowLogCheck(const std::string& key, int64_t now_ms,
                                         int64_t window_ms, int max_requests) override;
    WindowDecision slidingWindowCounterCheck(const std::string& key, int64_t now_ms,
                                             int64_t window_ms, int max_requests) override;
    void reset(const std::string& key) override;
    bool isDistributed() const override { return false; }
    std::string name() const override { return "memory"; }

private:
    // Token bucket state per key.
    struct Bucket { double tokens; int64_t last_refill_ms; };
    std::unordered_map<std::string, Bucket> buckets_;

    // Sliding window log: timestamps per key.
    std::unordered_map<std::string, std::deque<int64_t>> windows_;

    // Sliding window counter: two-bucket state per key.
    struct Counter { int64_t window_start_ms; int cur_count; int prev_count; };
    std::unordered_map<std::string, Counter> counters_;

    std::mutex mutex_;  // one lock guards all maps; each op is fully atomic
};

} // namespace ratelimiter
