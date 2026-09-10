#include "sliding_window.h"
#include "backend.h"
#include <chrono>

namespace ratelimiter {

int64_t SlidingWindowLimiter::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

SlidingWindowLimiter::SlidingWindowLimiter(RateLimitBackend& backend, const RateLimitConfig& config)
    : backend_(backend), config_(config) {}

RateLimitResult SlidingWindowLimiter::checkLimit(const std::string& client_id) {
    RateLimitConfig cfg;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        cfg = config_;
    }

    int64_t now = nowMs();
    int64_t window_ms = static_cast<int64_t>(cfg.window_seconds) * 1000;

    // One atomic call prunes + counts + records-if-allowed (no peek-then-add race).
    auto decision = backend_.slidingWindowLogCheck(client_id, now, window_ms, cfg.max_requests);

    RateLimitResult result;
    result.limit = cfg.max_requests;
    result.allowed = decision.allowed;
    if (decision.allowed) {
        result.remaining = cfg.max_requests - decision.count;
        if (result.remaining < 0) result.remaining = 0;
        result.retry_after_ms = 0;
    } else {
        result.remaining = 0;
        result.retry_after_ms = 1000;  // conservative: retry in ~1s
    }
    return result;
}

void SlidingWindowLimiter::resetClient(const std::string& client_id) {
    backend_.reset(client_id);
}

void SlidingWindowLimiter::updateConfig(const RateLimitConfig& config) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = config;
}

RateLimitConfig SlidingWindowLimiter::getConfig() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

} // namespace ratelimiter
