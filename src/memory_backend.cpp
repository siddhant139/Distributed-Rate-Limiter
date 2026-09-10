#include "memory_backend.h"
#include <algorithm>
#include <cmath>

namespace ratelimiter {

RateLimitBackend::BucketDecision
MemoryBackend::tokenBucketCheck(const std::string& key, double capacity,
                                double refill_per_sec, int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);  // whole decision is atomic

    auto it = buckets_.find(key);
    double tokens;
    int64_t last;
    if (it == buckets_.end()) {
        tokens = capacity;      // new client starts with a full bucket
        last = now_ms;
    } else {
        tokens = it->second.tokens;
        last = it->second.last_refill_ms;
    }

    // Refill based on elapsed time.
    double elapsed_sec = (now_ms - last) / 1000.0;
    if (elapsed_sec < 0) elapsed_sec = 0;
    tokens = std::min(capacity, tokens + elapsed_sec * refill_per_sec);

    bool allowed = false;
    if (tokens >= 1.0) {
        tokens -= 1.0;
        allowed = true;
    }

    buckets_[key] = Bucket{tokens, now_ms};
    return {allowed, tokens};
}

RateLimitBackend::WindowDecision
MemoryBackend::slidingWindowLogCheck(const std::string& key, int64_t now_ms,
                                     int64_t window_ms, int max_requests) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& dq = windows_[key];
    int64_t window_start = now_ms - window_ms;
    while (!dq.empty() && dq.front() <= window_start) dq.pop_front();  // prune old

    bool allowed = false;
    if (static_cast<int>(dq.size()) < max_requests) {
        dq.push_back(now_ms);   // record only if under the limit
        allowed = true;
    }
    return {allowed, static_cast<int>(dq.size())};
}

RateLimitBackend::WindowDecision
MemoryBackend::slidingWindowCounterCheck(const std::string& key, int64_t now_ms,
                                         int64_t window_ms, int max_requests) {
    std::lock_guard<std::mutex> lock(mutex_);

    int64_t cur_window = now_ms - (now_ms % window_ms);  // start of current fixed window

    auto it = counters_.find(key);
    Counter c;
    if (it == counters_.end()) {
        c = Counter{cur_window, 0, 0};
    } else {
        c = it->second;
        if (cur_window == c.window_start_ms) {
            // same window — keep counts
        } else if (cur_window == c.window_start_ms + window_ms) {
            // advanced exactly one window: current becomes previous
            c.prev_count = c.cur_count;
            c.cur_count = 0;
            c.window_start_ms = cur_window;
        } else {
            // gap of >1 window: both buckets are stale
            c.prev_count = 0;
            c.cur_count = 0;
            c.window_start_ms = cur_window;
        }
    }

    // Weight the previous window by the fraction still inside the sliding window.
    double elapsed = static_cast<double>(now_ms - c.window_start_ms);
    double weight = (static_cast<double>(window_ms) - elapsed) / static_cast<double>(window_ms);
    if (weight < 0) weight = 0;
    double estimated = c.prev_count * weight + c.cur_count;

    bool allowed = false;
    if (estimated < max_requests) {
        c.cur_count += 1;
        allowed = true;
    }

    counters_[key] = c;
    int reported = static_cast<int>(std::floor(estimated)) + (allowed ? 1 : 0);
    return {allowed, reported};
}

void MemoryBackend::reset(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    buckets_.erase(key);
    windows_.erase(key);
    counters_.erase(key);
}

} // namespace ratelimiter
