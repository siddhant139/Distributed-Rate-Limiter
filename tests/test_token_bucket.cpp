#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include "../src/token_bucket.h"
#include "../src/memory_backend.h"
#include "../src/rate_limiter.h"

int tests_passed = 0, tests_failed = 0;

void check(bool condition, const std::string& name) {
    if (condition) { std::cout << "  PASS: " << name << std::endl; tests_passed++; }
    else { std::cout << "  FAIL: " << name << std::endl; tests_failed++; }
}

int main() {
    std::cout << "=== Token Bucket Limiter Tests ===" << std::endl;

    // --- Test: burst allows N then denies ---
    {
        std::cout << "\n[burst allows N then denies]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 10;
        config.window_seconds = 60;
        config.burst_size = 3;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::TokenBucketLimiter limiter(backend, config);

        auto r1 = limiter.checkLimit("c1");
        check(r1.allowed == true, "1st request allowed");

        auto r2 = limiter.checkLimit("c1");
        check(r2.allowed == true, "2nd request allowed");

        auto r3 = limiter.checkLimit("c1");
        check(r3.allowed == true, "3rd request allowed");

        auto r4 = limiter.checkLimit("c1");
        check(r4.allowed == false, "4th request denied (429 semantics)");
        check(r4.retry_after_ms > 0, "denied result has retry_after_ms > 0");
    }

    // --- Test: remaining decrements ---
    {
        std::cout << "\n[remaining decrements]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 10;
        config.window_seconds = 60;
        config.burst_size = 3;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::TokenBucketLimiter limiter(backend, config);

        auto r1 = limiter.checkLimit("c1");
        check(r1.remaining == 2, "after 1st request remaining == 2");

        auto r2 = limiter.checkLimit("c1");
        check(r2.remaining == 1, "after 2nd request remaining == 1");

        auto r3 = limiter.checkLimit("c1");
        check(r3.remaining == 0, "after 3rd request remaining == 0");
    }

    // --- Test: limit field equals burst_size ---
    {
        std::cout << "\n[limit field equals burst_size]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 10;
        config.window_seconds = 60;
        config.burst_size = 3;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::TokenBucketLimiter limiter(backend, config);

        auto r1 = limiter.checkLimit("c1");
        check(r1.limit == 3, "limit field equals burst_size (3)");
    }

    // --- Test: per-client isolation ---
    {
        std::cout << "\n[per-client isolation]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 10;
        config.window_seconds = 60;
        config.burst_size = 3;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::TokenBucketLimiter limiter(backend, config);

        // Exhaust c1
        limiter.checkLimit("c1");
        limiter.checkLimit("c1");
        limiter.checkLimit("c1");
        auto r_c1 = limiter.checkLimit("c1");
        check(r_c1.allowed == false, "c1 is exhausted");

        // c2 should still be allowed
        auto r_c2 = limiter.checkLimit("c2");
        check(r_c2.allowed == true, "c2 first request allowed (isolated from c1)");
    }

    // --- Test: refill after wait ---
    {
        std::cout << "\n[refill after wait]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 10;
        config.window_seconds = 60;
        config.burst_size = 2;
        config.refill_rate = 10.0; // 10 tokens/sec => 100ms per token

        ratelimiter::MemoryBackend backend;
        ratelimiter::TokenBucketLimiter limiter(backend, config);

        // Exhaust c3
        limiter.checkLimit("c3");
        limiter.checkLimit("c3");
        auto r_exhausted = limiter.checkLimit("c3");
        check(r_exhausted.allowed == false, "c3 exhausted after burst");

        // Sleep 250ms to allow refill of at least 2 tokens (10/sec * 0.25s = 2.5 tokens)
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        auto r_refilled = limiter.checkLimit("c3");
        check(r_refilled.allowed == true, "c3 allowed again after refill wait");
    }

    // --- Test: resetClient ---
    {
        std::cout << "\n[resetClient]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 10;
        config.window_seconds = 60;
        config.burst_size = 3;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::TokenBucketLimiter limiter(backend, config);

        // Exhaust c4
        limiter.checkLimit("c4");
        limiter.checkLimit("c4");
        limiter.checkLimit("c4");
        auto r_denied = limiter.checkLimit("c4");
        check(r_denied.allowed == false, "c4 exhausted before reset");

        limiter.resetClient("c4");

        auto r_after_reset = limiter.checkLimit("c4");
        check(r_after_reset.allowed == true, "c4 allowed after resetClient");
    }

    // --- Test: denied result has remaining 0 ---
    {
        std::cout << "\n[denied result has remaining 0]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 10;
        config.window_seconds = 60;
        config.burst_size = 3;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::TokenBucketLimiter limiter(backend, config);

        // Exhaust
        limiter.checkLimit("c5");
        limiter.checkLimit("c5");
        limiter.checkLimit("c5");

        auto r_denied = limiter.checkLimit("c5");
        check(r_denied.allowed == false, "c5 denied");
        check(r_denied.remaining == 0, "denied result has remaining == 0");
    }

    // --- Summary ---
    std::cout << "\n=== Results: " << tests_passed << " passed, " << tests_failed << " failed ===" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}
