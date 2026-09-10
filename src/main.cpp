#include "http_server.h"
#include "thread_pool.h"
#include "memory_backend.h"
#include "redis_backend.h"
#include "token_bucket.h"
#include "sliding_window.h"
#include "sliding_window_counter.h"
#include "router.h"
#include "stats.h"
#include "rate_limiter.h"

#include <iostream>
#include <memory>
#include <csignal>
#include <string>

using namespace ratelimiter;

static HttpServer* g_server = nullptr;

void signalHandler(int signum) {
    std::cout << "\n[rate-limiter] Shutting down (signal " << signum << ")..." << std::endl;
    if (g_server) g_server->stop();
}

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -p PORT        HTTP port (default: 8080)\n"
              << "  -t THREADS     Thread pool size (default: 8)\n"
              << "  -a ALGORITHM   token_bucket | sliding_window | sliding_window_counter (default: token_bucket)\n"
              << "  -m MAX_REQ     Max requests per window / burst size (default: 10)\n"
              << "  -w WINDOW      Window seconds (default: 60)\n"
              << "  -r REFILL      Token refill rate/sec (default: 1.0)\n"
              << "  --redis HOST:PORT   Use Redis backend (default: in-memory)\n"
              << "  -h             Show help\n";
}

int main(int argc, char* argv[]) {
    int port = 8080;
    size_t threads = 8;
    std::string algorithm = "token_bucket";
    std::string redis_addr;
    RateLimitConfig config;
    config.max_requests = 10;
    config.window_seconds = 60;
    config.burst_size = 10;
    config.refill_rate = 1.0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-p" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (arg == "-t" && i + 1 < argc) threads = std::stoul(argv[++i]);
        else if (arg == "-a" && i + 1 < argc) algorithm = argv[++i];
        else if (arg == "-m" && i + 1 < argc) { config.max_requests = std::stoi(argv[++i]); config.burst_size = config.max_requests; }
        else if (arg == "-w" && i + 1 < argc) config.window_seconds = std::stoi(argv[++i]);
        else if (arg == "-r" && i + 1 < argc) config.refill_rate = std::stod(argv[++i]);
        else if (arg == "--redis" && i + 1 < argc) redis_addr = argv[++i];
        else if (arg == "-h") { printUsage(argv[0]); return 0; }
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Set up backend
    std::unique_ptr<RateLimitBackend> backend;
    if (!redis_addr.empty()) {
        size_t colon = redis_addr.find(':');
        std::string host = redis_addr.substr(0, colon);
        int rport = (colon != std::string::npos) ? std::stoi(redis_addr.substr(colon + 1)) : 6379;
        auto redis = std::make_unique<RedisBackend>(host, rport);
        if (redis->connect()) {
            std::cout << "[rate-limiter] Connected to Redis at " << redis_addr << std::endl;
            backend = std::move(redis);
        } else {
            std::cout << "[rate-limiter] WARNING: Redis connection failed, falling back to in-memory" << std::endl;
            backend = std::make_unique<MemoryBackend>();
        }
    } else {
        backend = std::make_unique<MemoryBackend>();
    }

    // Set up rate limiter algorithm
    std::unique_ptr<RateLimiter> limiter;
    if (algorithm == "sliding_window") {
        limiter = std::make_unique<SlidingWindowLimiter>(*backend, config);
    } else if (algorithm == "sliding_window_counter") {
        limiter = std::make_unique<SlidingWindowCounterLimiter>(*backend, config);
    } else {
        algorithm = "token_bucket";
        limiter = std::make_unique<TokenBucketLimiter>(*backend, config);
    }

    StatsCollector stats;
    Router router(*limiter, stats);

    std::cout << "[rate-limiter] Distributed Rate Limiter\n";
    std::cout << "[rate-limiter] Port: " << port << "\n";
    std::cout << "[rate-limiter] Algorithm: " << algorithm << "\n";
    std::cout << "[rate-limiter] Backend: " << backend->name() << "\n";
    std::cout << "[rate-limiter] Threads: " << threads << "\n";

    ThreadPool pool(threads);
    HttpServer server(port, pool);
    g_server = &server;

    // Register routes
    server.addRoute("POST", "/api/check", [&router](const HttpRequest& r) { return router.handleCheck(r); });
    server.addRoute("POST", "/api/reset", [&router](const HttpRequest& r) { return router.handleReset(r); });
    server.addRoute("GET",  "/stats", [&router](const HttpRequest& r) { return router.handleGlobalStats(r); });
    server.addRoute("GET",  "/stats/*", [&router](const HttpRequest& r) { return router.handleClientStats(r); });
    server.addRoute("GET",  "/config", [&router](const HttpRequest& r) { return router.handleGetConfig(r); });
    server.addRoute("POST", "/config", [&router](const HttpRequest& r) { return router.handleUpdateConfig(r); });
    server.addRoute("GET",  "/health", [&router](const HttpRequest& r) { return router.handleHealth(r); });

    try {
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "[rate-limiter] Fatal: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[rate-limiter] Server stopped." << std::endl;
    return 0;
}
