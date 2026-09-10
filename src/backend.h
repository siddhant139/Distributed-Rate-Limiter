#pragma once

#include <string>
#include <cstdint>

namespace ratelimiter {

// Storage abstraction for rate limiter state.
//
// KEY DESIGN POINT: each method performs the ENTIRE check-and-update as one
// ATOMIC operation. The previous design exposed separate read (getBucket) and
// write (setBucket) calls, which created a time-of-check-to-time-of-use (TOCTOU)
// race across distributed instances: two instances could both read "5 tokens",
// both allow a request, and both write "4" — letting 6 requests through a limit
// of 5. Folding the whole decision into one atomic operation eliminates that.
//
//   - MemoryBackend  performs each check under its exclusive lock (atomic within
//                    a single process).
//   - RedisBackend   performs each check as a Lua script via EVAL, which Redis
//                    executes single-threaded and atomically (atomic ACROSS all
//                    instances sharing that Redis — true distributed correctness).
class RateLimitBackend {
public:
    virtual ~RateLimitBackend() = default;

    struct BucketDecision {
        bool allowed;
        double tokens_remaining;
    };

    struct WindowDecision {
        bool allowed;
        int count;   // requests counted in the window (after this decision)
    };

    // Token Bucket: atomically refill by elapsed time, then consume one token if
    // available. capacity = bucket size, refill_per_sec = tokens added per second.
    virtual BucketDecision tokenBucketCheck(const std::string& key,
                                            double capacity,
                                            double refill_per_sec,
                                            int64_t now_ms) = 0;

    // Sliding Window Log: atomically prune timestamps older than the window,
    // count what remains, and record this request iff under the limit. Exact,
    // but stores one entry per request (O(N) memory per client).
    virtual WindowDecision slidingWindowLogCheck(const std::string& key,
                                                 int64_t now_ms,
                                                 int64_t window_ms,
                                                 int max_requests) = 0;

    // Sliding Window Counter: two fixed buckets (current + previous window),
    // weighting the previous bucket by how much of it still overlaps the sliding
    // window. Approximate, but O(1) memory per client.
    virtual WindowDecision slidingWindowCounterCheck(const std::string& key,
                                                     int64_t now_ms,
                                                     int64_t window_ms,
                                                     int max_requests) = 0;

    virtual void reset(const std::string& key) = 0;
    virtual bool isDistributed() const = 0;
    virtual std::string name() const = 0;
};

} // namespace ratelimiter
