// Tests for MemoryBackend's atomic check methods.
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include "../src/memory_backend.h"

using namespace ratelimiter;

int tests_passed = 0, tests_failed = 0;
void check(bool condition, const std::string& name) {
    if (condition) { std::cout << "  PASS: " << name << std::endl; tests_passed++; }
    else { std::cout << "  FAIL: " << name << std::endl; tests_failed++; }
}

int main() {
    std::cout << "=== MemoryBackend Atomic Check Tests ===\n";

    // ---- Token bucket ----
    {
        std::cout << "\n[token bucket: burst then deny]\n";
        MemoryBackend b;
        int64_t now = 1'000'000;
        // capacity 3, refill 1/sec. 3 allowed at the same instant, 4th denied.
        int allowed = 0;
        for (int i = 0; i < 5; ++i) {
            auto d = b.tokenBucketCheck("c1", 3.0, 1.0, now);
            if (d.allowed) allowed++;
        }
        check(allowed == 3, "exactly 3 of 5 rapid requests allowed (burst=3)");
    }
    {
        std::cout << "\n[token bucket: refill over time]\n";
        MemoryBackend b;
        int64_t now = 2'000'000;
        for (int i = 0; i < 2; ++i) b.tokenBucketCheck("c2", 2.0, 10.0, now);  // drain 2
        auto denied = b.tokenBucketCheck("c2", 2.0, 10.0, now);
        check(!denied.allowed, "bucket empty -> denied");
        // 10 tokens/sec => 1 token in 100ms. Advance 150ms.
        auto after = b.tokenBucketCheck("c2", 2.0, 10.0, now + 150);
        check(after.allowed, "allowed again after refill window elapsed");
    }
    {
        std::cout << "\n[token bucket: per-client isolation]\n";
        MemoryBackend b;
        int64_t now = 3'000'000;
        for (int i = 0; i < 3; ++i) b.tokenBucketCheck("A", 3.0, 1.0, now);  // exhaust A
        auto a = b.tokenBucketCheck("A", 3.0, 1.0, now);
        auto other = b.tokenBucketCheck("B", 3.0, 1.0, now);
        check(!a.allowed, "A exhausted");
        check(other.allowed, "B independent, allowed");
    }

    // ---- Sliding window log ----
    {
        std::cout << "\n[sliding window log: max then deny]\n";
        MemoryBackend b;
        int64_t now = 5'000'000;
        int allowed = 0;
        for (int i = 0; i < 5; ++i) {
            auto d = b.slidingWindowLogCheck("w1", now, 1000, 3);  // max 3 per 1s
            if (d.allowed) allowed++;
        }
        check(allowed == 3, "exactly 3 of 5 allowed in window (max=3)");
    }
    {
        std::cout << "\n[sliding window log: denied does not count]\n";
        MemoryBackend b;
        int64_t now = 6'000'000;
        b.slidingWindowLogCheck("w2", now, 1000, 2);
        b.slidingWindowLogCheck("w2", now, 1000, 2);           // now at max
        auto d3 = b.slidingWindowLogCheck("w2", now, 1000, 2); // denied
        auto d4 = b.slidingWindowLogCheck("w2", now, 1000, 2); // still denied, count unchanged
        check(!d3.allowed && !d4.allowed, "over-limit requests denied");
        check(d4.count == 2, "denied requests are not recorded (count stays 2)");
    }
    {
        std::cout << "\n[sliding window log: window expiry]\n";
        MemoryBackend b;
        int64_t now = 7'000'000;
        b.slidingWindowLogCheck("w3", now, 1000, 2);
        b.slidingWindowLogCheck("w3", now, 1000, 2);           // exhaust
        auto after = b.slidingWindowLogCheck("w3", now + 1100, 1000, 2);  // old entries expired
        check(after.allowed, "allowed after the window slides past old entries");
    }

    // ---- Sliding window counter ----
    {
        std::cout << "\n[sliding window counter: max then deny within a window]\n";
        MemoryBackend b;
        int64_t win_start = 10 * 60 * 1000;      // aligned to a window boundary
        int64_t now = win_start + 10;            // early in the window
        int allowed = 0;
        for (int i = 0; i < 6; ++i) {
            auto d = b.slidingWindowCounterCheck("cnt1", now, 60000, 4);  // max 4 per 60s
            if (d.allowed) allowed++;
        }
        check(allowed == 4, "exactly 4 of 6 allowed (max=4)");
    }
    {
        std::cout << "\n[sliding window counter: per-client isolation]\n";
        MemoryBackend b;
        int64_t now = 20 * 60 * 1000 + 10;
        for (int i = 0; i < 4; ++i) b.slidingWindowCounterCheck("X", now, 60000, 4);
        auto x = b.slidingWindowCounterCheck("X", now, 60000, 4);
        auto y = b.slidingWindowCounterCheck("Y", now, 60000, 4);
        check(!x.allowed, "X exhausted");
        check(y.allowed, "Y independent, allowed");
    }
    {
        std::cout << "\n[sliding window counter: weighted rollover reduces old window influence]\n";
        MemoryBackend b;
        int64_t window = 60000;
        int64_t w0 = 30 * 60 * 1000;             // window boundary
        // Fill the previous window heavily.
        for (int i = 0; i < 4; ++i) b.slidingWindowCounterCheck("roll", w0 + 100, window, 4);
        // At the exact start of the next window, the previous window is fully
        // weighted (weight=1) so estimate == prev == 4 == max -> denied.
        auto early = b.slidingWindowCounterCheck("roll", w0 + window, window, 4);
        // Near the end of the next window, the previous window's weight ~0 -> allowed.
        auto late = b.slidingWindowCounterCheck("roll", w0 + 2 * window - 1, window, 4);
        check(!early.allowed, "start of new window: prev window still counts -> denied");
        check(late.allowed, "end of new window: prev window decayed -> allowed");
    }

    // ---- reset ----
    {
        std::cout << "\n[reset clears all state for a key]\n";
        MemoryBackend b;
        int64_t now = 40 * 60 * 1000;
        for (int i = 0; i < 3; ++i) b.tokenBucketCheck("r", 3.0, 1.0, now);
        b.reset("r");
        auto after = b.tokenBucketCheck("r", 3.0, 1.0, now);
        check(after.allowed, "after reset, token bucket is full again");
    }

    // ---- thread safety ----
    {
        std::cout << "\n[thread safety: atomic decrement under contention]\n";
        MemoryBackend b;
        int64_t now = 50 * 60 * 1000;
        // Capacity 1000, refill 0 (no refill), so exactly 1000 requests should be
        // allowed no matter how many threads race — proving the decrement is atomic.
        const int threads = 10, per = 500;   // 5000 attempts, only 1000 tokens
        std::atomic<int> allowed{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < threads; ++t) {
            ts.emplace_back([&]() {
                for (int i = 0; i < per; ++i) {
                    if (b.tokenBucketCheck("shared", 1000.0, 0.0, now).allowed) allowed++;
                }
            });
        }
        for (auto& th : ts) th.join();
        check(allowed.load() == 1000,
              "exactly 1000 allowed across 5000 concurrent attempts (no over-count)");
    }

    std::cout << "\n=== Results: " << tests_passed << " passed, " << tests_failed << " failed ===\n";
    return tests_failed > 0 ? 1 : 0;
}
