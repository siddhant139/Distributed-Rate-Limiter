#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include "../src/sliding_window.h"
#include "../src/memory_backend.h"

int tests_passed = 0, tests_failed = 0;

void check(bool condition, const std::string& name) {
    if (condition) { std::cout << "  PASS: " << name << std::endl; tests_passed++; }
    else { std::cout << "  FAIL: " << name << std::endl; tests_failed++; }
}

int main() {
    std::cout << "=== Sliding Window Limiter Tests ===" << std::endl;

    // --- Test: max requests then deny ---
    {
        std::cout << "\n[max requests then deny]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 3;
        config.window_seconds = 60;
        config.burst_size = 3;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::SlidingWindowLimiter limiter(backend, config);

        auto r1 = limiter.checkLimit("c1");
        check(r1.allowed == true, "1st request allowed");

        auto r2 = limiter.checkLimit("c1");
        check(r2.allowed == true, "2nd request allowed");

        auto r3 = limiter.checkLimit("c1");
        check(r3.allowed == true, "3rd request allowed");

        auto r4 = limiter.checkLimit("c1");
        check(r4.allowed == false, "4th request denied");
    }

    // --- Test: remaining decrements correctly ---
    {
        std::cout << "\n[remaining decrements correctly]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 3;
        config.window_seconds = 60;
        config.burst_size = 3;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::SlidingWindowLimiter limiter(backend, config);

        auto r1 = limiter.checkLimit("c1");
        check(r1.remaining == 2, "after 1st request remaining == 2");

        auto r2 = limiter.checkLimit("c1");
        check(r2.remaining == 1, "after 2nd request remaining == 1");

        auto r3 = limiter.checkLimit("c1");
        check(r3.remaining == 0, "after 3rd request remaining == 0");
    }

    // --- Test: per-client isolation ---
    {
        std::cout << "\n[per-client isolation]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 3;
        config.window_seconds = 60;
        config.burst_size = 3;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::SlidingWindowLimiter limiter(backend, config);

        // Exhaust c1
        limiter.checkLimit("c1");
        limiter.checkLimit("c1");
        limiter.checkLimit("c1");
        auto r_c1 = limiter.checkLimit("c1");
        check(r_c1.allowed == false, "c1 exhausted");

        // c2 should still be allowed
        auto r_c2 = limiter.checkLimit("c2");
        check(r_c2.allowed == true, "c2 first request allowed (isolated from c1)");
    }

    // --- Test: denied does NOT count against window ---
    {
        std::cout << "\n[denied does NOT count against window]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 3;
        config.window_seconds = 60;
        config.burst_size = 3;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::SlidingWindowLimiter limiter(backend, config);

        // Use all 3 slots
        limiter.checkLimit("c1");
        limiter.checkLimit("c1");
        limiter.checkLimit("c1");

        // 4th and 5th should both be denied, count stays at 3
        auto r4 = limiter.checkLimit("c1");
        check(r4.allowed == false, "4th request denied");

        auto r5 = limiter.checkLimit("c1");
        check(r5.allowed == false, "5th request denied (denied requests don't count)");
    }

    // --- Test: resetClient clears the window ---
    {
        std::cout << "\n[resetClient clears the window]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 3;
        config.window_seconds = 60;
        config.burst_size = 3;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::SlidingWindowLimiter limiter(backend, config);

        // Exhaust c3
        limiter.checkLimit("c3");
        limiter.checkLimit("c3");
        limiter.checkLimit("c3");
        auto r_denied = limiter.checkLimit("c3");
        check(r_denied.allowed == false, "c3 exhausted before reset");

        limiter.resetClient("c3");

        auto r_after_reset = limiter.checkLimit("c3");
        check(r_after_reset.allowed == true, "c3 allowed after resetClient");
    }

    // --- Test: limit field equals max_requests ---
    {
        std::cout << "\n[limit field equals max_requests]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 3;
        config.window_seconds = 60;
        config.burst_size = 3;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::SlidingWindowLimiter limiter(backend, config);

        auto r1 = limiter.checkLimit("c1");
        check(r1.limit == 3, "limit field equals max_requests (3)");
    }

    // --- Test: window expiry ---
    {
        std::cout << "\n[window expiry]" << std::endl;
        ratelimiter::RateLimitConfig config;
        config.max_requests = 2;
        config.window_seconds = 1;
        config.burst_size = 2;
        config.refill_rate = 1.0;

        ratelimiter::MemoryBackend backend;
        ratelimiter::SlidingWindowLimiter limiter(backend, config);

        // Exhaust c4 with 2 requests
        auto r1 = limiter.checkLimit("c4");
        check(r1.allowed == true, "c4 1st request allowed");

        auto r2 = limiter.checkLimit("c4");
        check(r2.allowed == true, "c4 2nd request allowed");

        auto r3 = limiter.checkLimit("c4");
        check(r3.allowed == false, "c4 3rd request denied (window full)");

        // Sleep 1100ms so the 1-second window expires
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));

        auto r4 = limiter.checkLimit("c4");
        check(r4.allowed == true, "c4 allowed after window expiry");
    }

    // --- Summary ---
    std::cout << "\n=== Results: " << tests_passed << " passed, " << tests_failed << " failed ===" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}
