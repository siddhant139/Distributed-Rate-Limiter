#pragma once

#include <string>

namespace ratelimiter {

struct RateLimitResult {
    bool allowed = false;
    int remaining = 0;      // requests remaining in current window
    int limit = 0;          // max requests allowed
    int retry_after_ms = 0; // 0 if allowed, else suggested wait time
};

struct RateLimitConfig {
    int max_requests = 100;    // for sliding window: max in window
    int window_seconds = 60;   // window size
    int burst_size = 10;       // token bucket capacity
    double refill_rate = 1.0;  // token bucket refill tokens/sec
};

class RateLimiter {
public:
    virtual ~RateLimiter() = default;
    virtual RateLimitResult checkLimit(const std::string& client_id) = 0;
    virtual void resetClient(const std::string& client_id) = 0;
    virtual void updateConfig(const RateLimitConfig& config) = 0;
    virtual RateLimitConfig getConfig() const = 0;
    virtual std::string algorithmName() const = 0;
};

} // namespace ratelimiter
