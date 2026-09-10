#pragma once

#include "rate_limiter.h"
#include <mutex>

namespace ratelimiter {

class RateLimitBackend;

// Sliding Window Log: track timestamps of each request per client.
// Count requests within [now - window, now]. If count > max_requests, deny.
class SlidingWindowLimiter : public RateLimiter {
public:
    SlidingWindowLimiter(RateLimitBackend& backend, const RateLimitConfig& config);

    RateLimitResult checkLimit(const std::string& client_id) override;
    void resetClient(const std::string& client_id) override;
    void updateConfig(const RateLimitConfig& config) override;
    RateLimitConfig getConfig() const override;
    std::string algorithmName() const override { return "sliding_window"; }

private:
    RateLimitBackend& backend_;
    RateLimitConfig config_;
    mutable std::mutex config_mutex_;

    static int64_t nowMs();
};

} // namespace ratelimiter
