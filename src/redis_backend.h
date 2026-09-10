#pragma once

#include "backend.h"
#include <string>
#include <vector>
#include <mutex>

namespace ratelimiter {

// Distributed backend backed by Redis over a raw TCP socket (RESP protocol).
//
// Every rate-limit decision is executed as a Lua script via EVAL. Redis runs
// Lua scripts SINGLE-THREADED and ATOMICALLY: no other command (from any
// instance) can interleave mid-script. This makes the check-and-decrement
// atomic across every rate-limiter instance sharing this Redis, which is what
// gives the limiter true distributed correctness (no TOCTOU race).
//
// A single synchronous connection guarded by a mutex; a production system would
// use a connection pool. Falls back gracefully if Redis is unavailable.
class RedisBackend : public RateLimitBackend {
public:
    RedisBackend(const std::string& host, int port);
    ~RedisBackend() override;

    bool connect();
    bool isConnected() const { return fd_ >= 0; }

    BucketDecision tokenBucketCheck(const std::string& key, double capacity,
                                    double refill_per_sec, int64_t now_ms) override;
    WindowDecision slidingWindowLogCheck(const std::string& key, int64_t now_ms,
                                         int64_t window_ms, int max_requests) override;
    WindowDecision slidingWindowCounterCheck(const std::string& key, int64_t now_ms,
                                             int64_t window_ms, int max_requests) override;
    void reset(const std::string& key) override;
    bool isDistributed() const override { return true; }
    std::string name() const override { return "redis"; }

private:
    std::string host_;
    int port_;
    int fd_;
    std::mutex conn_mutex_;
    uint64_t seq_ = 0;  // makes sliding-window-log members unique within a ms

    // RESP I/O
    std::string sendCommand(const std::vector<std::string>& args);
    std::string readReply();
    bool sendAll(const std::string& data);

    // EVAL a Lua script with one key + string args; returns reply lines.
    std::string eval(const std::string& script, const std::string& key,
                     const std::vector<std::string>& argv);
};

} // namespace ratelimiter
