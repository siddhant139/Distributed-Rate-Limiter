#pragma once

#include "rate_limiter.h"
#include <mutex>

namespace ratelimiter {

class RateLimitBackend;

// Token Bucket: each client has a bucket of `burst_size` tokens,
// refilled at `refill_rate` tokens/sec. Each request consumes 1 token.
// Allows bursts up to bucket capacity, then steady rate.
class TokenBucketLimiter : public RateLimiter {
public:
    TokenBucketLimiter(RateLimitBackend& backend, const RateLimitConfig& config);

    RateLimitResult checkLimit(const std::string& client_id) override;
    void resetClient(const std::string& client_id) override;
    void updateConfig(const RateLimitConfig& config) override;
    RateLimitConfig getConfig() const override;
    std::string algorithmName() const override { return "token_bucket"; }

private:
    RateLimitBackend& backend_;
    RateLimitConfig config_;
    mutable std::mutex config_mutex_;

    static int64_t nowMs();
};

} // namespace ratelimiter
