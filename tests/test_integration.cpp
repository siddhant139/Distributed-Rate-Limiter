/**
 * Integration tests for the rate limiter server.
 * Uses raw POSIX sockets to send HTTP requests — no external test framework.
 *
 * The server must be running separately before executing this binary.
 * Start it with:
 *   ./build/rate_limiter -p 8090 -a token_bucket -m 5
 *
 * Usage:
 *   ./test_integration [port]
 *   port defaults to 8090 if not provided.
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <sstream>

// ---------------------------------------------------------------------------
// HTTP helper
// ---------------------------------------------------------------------------

struct HttpResult { int status; std::string body; };

HttpResult httpRequest(const std::string& host, int port, const std::string& method,
                       const std::string& path, const std::string& apiKey,
                       const std::string& body = "") {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { close(fd); return {-1, ""}; }

    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << "\r\n";
    if (!apiKey.empty()) req << "X-API-Key: " << apiKey << "\r\n";
    req << "Content-Length: " << body.size() << "\r\n";
    req << "Connection: close\r\n";
    req << "\r\n" << body;
    std::string reqStr = req.str();
    send(fd, reqStr.c_str(), reqStr.size(), 0);

    std::string response;
    char buf[4096];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) response.append(buf, n);
    close(fd);

    // Parse status code from 'HTTP/1.1 XXX'
    int status = -1;
    size_t sp = response.find(' ');
    if (sp != std::string::npos) { try { status = std::stoi(response.substr(sp+1, 3)); } catch (...) {} }
    // Body after \r\n\r\n
    std::string respBody;
    size_t bodyPos = response.find("\r\n\r\n");
    if (bodyPos != std::string::npos) respBody = response.substr(bodyPos + 4);
    return {status, respBody};
}

// ---------------------------------------------------------------------------
// Test bookkeeping
// ---------------------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;

void check(const std::string& name, bool condition) {
    if (condition) {
        std::cout << "  [PASS] " << name << "\n";
        ++g_passed;
    } else {
        std::cout << "  [FAIL] " << name << "\n";
        ++g_failed;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    int port = 8090;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    const std::string host = "127.0.0.1";

    std::cout << "============================================================\n";
    std::cout << " Rate Limiter Integration Tests\n";
    std::cout << "============================================================\n";
    std::cout << "Make sure the server is running:\n";
    std::cout << "  ./build/rate_limiter -p " << port << " -a token_bucket -m 5\n";
    std::cout << "------------------------------------------------------------\n\n";

    // ------------------------------------------------------------------
    // Pre-flight: verify server is reachable
    // ------------------------------------------------------------------
    {
        HttpResult r = httpRequest(host, port, "GET", "/health", "");
        if (r.status == -1) {
            std::cerr << "ERROR: Could not connect to server at " << host << ":" << port << "\n";
            std::cerr << "Please start the server first:\n";
            std::cerr << "  ./build/rate_limiter -p " << port << " -a token_bucket -m 5\n";
            return 1;
        }
    }

    // ------------------------------------------------------------------
    // 1. Connection / health
    // ------------------------------------------------------------------
    std::cout << "[Test 1] Connection / health check\n";
    {
        HttpResult r = httpRequest(host, port, "GET", "/health", "");
        check("GET /health returns 200", r.status == 200);
        check("Body contains 'healthy'", r.body.find("healthy") != std::string::npos);
    }

    // ------------------------------------------------------------------
    // 2. First check allowed
    // ------------------------------------------------------------------
    std::cout << "[Test 2] First check allowed\n";
    {
        HttpResult r = httpRequest(host, port, "POST", "/api/check", "itest1");
        check("POST /api/check returns 200", r.status == 200);
        check("Body contains 'allowed\":true'", r.body.find("allowed\":true") != std::string::npos);
    }

    // ------------------------------------------------------------------
    // 3. Rate limit enforcement (fresh key, capacity 5)
    // ------------------------------------------------------------------
    std::cout << "[Test 3] Rate limit enforcement\n";
    {
        int count200 = 0;
        int count429 = 0;
        for (int i = 0; i < 6; ++i) {
            HttpResult r = httpRequest(host, port, "POST", "/api/check", "itest_burst");
            if (r.status == 200) ++count200;
            else if (r.status == 429) ++count429;
        }
        check("Exactly 5 requests return 200", count200 == 5);
        check("Exactly 1 request returns 429", count429 == 1);
    }

    // ------------------------------------------------------------------
    // 4. Per-client isolation
    // ------------------------------------------------------------------
    std::cout << "[Test 4] Per-client isolation\n";
    {
        // Exhaust itestA
        for (int i = 0; i < 6; ++i) {
            httpRequest(host, port, "POST", "/api/check", "itestA");
        }
        // itestB should still be allowed
        HttpResult r = httpRequest(host, port, "POST", "/api/check", "itestB");
        check("itestB first request returns 200 after itestA exhausted", r.status == 200);
    }

    // ------------------------------------------------------------------
    // 5. Stats endpoint
    // ------------------------------------------------------------------
    std::cout << "[Test 5] Stats endpoint\n";
    {
        HttpResult r = httpRequest(host, port, "GET", "/stats", "");
        check("GET /stats returns 200", r.status == 200);
        check("Body contains 'total_requests'", r.body.find("total_requests") != std::string::npos);
    }

    // ------------------------------------------------------------------
    // 6. Per-client stats
    // ------------------------------------------------------------------
    std::cout << "[Test 6] Per-client stats\n";
    {
        HttpResult r = httpRequest(host, port, "GET", "/stats/itest1", "");
        check("GET /stats/itest1 returns 200", r.status == 200);
        check("Body contains 'itest1'", r.body.find("itest1") != std::string::npos);
    }

    // ------------------------------------------------------------------
    // 7. Config get
    // ------------------------------------------------------------------
    std::cout << "[Test 7] Config get\n";
    {
        HttpResult r = httpRequest(host, port, "GET", "/config", "");
        check("GET /config returns 200", r.status == 200);
        check("Body contains 'token_bucket'", r.body.find("token_bucket") != std::string::npos);
    }

    // ------------------------------------------------------------------
    // 8. Reset
    // ------------------------------------------------------------------
    std::cout << "[Test 8] Reset\n";
    {
        HttpResult resetResult = httpRequest(host, port, "POST", "/api/reset", "itestA");
        check("POST /api/reset returns 200", resetResult.status == 200);

        HttpResult r = httpRequest(host, port, "POST", "/api/check", "itestA");
        check("POST /api/check after reset returns 200", r.status == 200);
        check("itestA allowed again after reset", r.body.find("allowed\":true") != std::string::npos);
    }

    // ------------------------------------------------------------------
    // 9. 404 for unknown route
    // ------------------------------------------------------------------
    std::cout << "[Test 9] 404 for unknown route\n";
    {
        HttpResult r = httpRequest(host, port, "GET", "/nonexistent", "");
        check("GET /nonexistent returns 404", r.status == 404);
    }

    // ------------------------------------------------------------------
    // 10. Concurrent clients
    // ------------------------------------------------------------------
    std::cout << "[Test 10] Concurrent clients\n";
    {
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        threads.reserve(10);

        for (int t = 0; t < 10; ++t) {
            threads.emplace_back([&, t]() {
                std::string key = "iconc_" + std::to_string(t);
                for (int i = 0; i < 3; ++i) {
                    HttpResult r = httpRequest(host, port, "POST", "/api/check", key);
                    if (r.status != 200) {
                        failures.fetch_add(1);
                    }
                }
            });
        }

        for (auto& th : threads) {
            th.join();
        }

        check("All concurrent requests (10 clients x 3 each) return 200", failures.load() == 0);
    }

    // ------------------------------------------------------------------
    // Summary
    // ------------------------------------------------------------------
    std::cout << "\n============================================================\n";
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed\n";
    std::cout << "============================================================\n";

    return g_failed == 0 ? 0 : 1;
}
