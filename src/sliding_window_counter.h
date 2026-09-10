#pragma once

#include "rate_limiter.h"
#include <mutex>

namespace ratelimiter {

class RateLimitBackend;

// Sliding Window Counter: an O(1)-memory approximation of the sliding window.
//
// Instead of storing every request timestamp (the log approach), it keeps just
// two counters — the current fixed window and the previous one — and estimates
// the rolling count by weighting the previous window by how much of it still
// overlaps the sliding window:
//
//     estimated = prev_count * (1 - elapsed/window) + cur_count
//
// This trades a small amount of accuracy at window boundaries for constant
// memory per client, which is how large-scale API gateways (e.g. Cloudflare,
// Kong) actually implement rate limiting.
class SlidingWindowCounterLimiter : public RateLimiter {
public:
    SlidingWindowCounterLimiter(RateLimitBackend& backend, const RateLimitConfig& config);

    RateLimitResult checkLimit(const std::string& client_id) override;
    void resetClient(const std::string& client_id) override;
    void updateConfig(const RateLimitConfig& config) override;
    RateLimitConfig getConfig() const override;
    std::string algorithmName() const override { return "sliding_window_counter"; }

private:
    RateLimitBackend& backend_;
    RateLimitConfig config_;
    mutable std::mutex config_mutex_;

    static int64_t nowMs();
};

} // namespace ratelimiter
