#include "token_bucket.h"
#include "backend.h"
#include <chrono>
#include <algorithm>
#include <cmath>

namespace ratelimiter {

int64_t TokenBucketLimiter::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

TokenBucketLimiter::TokenBucketLimiter(RateLimitBackend& backend, const RateLimitConfig& config)
    : backend_(backend), config_(config) {}

RateLimitResult TokenBucketLimiter::checkLimit(const std::string& client_id) {
    RateLimitConfig cfg;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        cfg = config_;
    }

    int64_t now = nowMs();
    // One atomic call does refill + check + decrement (no TOCTOU race).
    auto decision = backend_.tokenBucketCheck(
        client_id, static_cast<double>(cfg.burst_size), cfg.refill_rate, now);

    RateLimitResult result;
    result.limit = cfg.burst_size;
    result.allowed = decision.allowed;

    if (decision.allowed) {
        result.remaining = static_cast<int>(std::floor(decision.tokens_remaining));
        result.retry_after_ms = 0;
    } else {
        result.remaining = 0;
        // Time until the bucket accrues one full token.
        double needed = 1.0 - decision.tokens_remaining;
        if (cfg.refill_rate > 0) {
            result.retry_after_ms = static_cast<int>(std::ceil((needed / cfg.refill_rate) * 1000.0));
        } else {
            result.retry_after_ms = 1000;
        }
    }
    return result;
}

void TokenBucketLimiter::resetClient(const std::string& client_id) {
    backend_.reset(client_id);
}

void TokenBucketLimiter::updateConfig(const RateLimitConfig& config) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = config;
}

RateLimitConfig TokenBucketLimiter::getConfig() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

} // namespace ratelimiter
