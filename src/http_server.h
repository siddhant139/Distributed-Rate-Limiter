#pragma once

#include "http_types.h"
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

namespace ratelimiter {

class ThreadPool;

class HttpServer {
public:
    using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer(int port, ThreadPool& pool);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Register a handler for METHOD + PATH (exact match; supports /prefix/* wildcard)
    void addRoute(const std::string& method, const std::string& path, RouteHandler handler);

    void start();  // blocking accept loop
    void stop();

private:
    int port_;
    int server_fd_;
    std::atomic<bool> running_;
    ThreadPool& pool_;

    std::unordered_map<std::string, RouteHandler> routes_;  // key: "METHOD PATH"

    mutable std::mutex clients_mutex_;
    std::unordered_set<int> client_fds_;

    void acceptLoop();
    void handleClient(int client_fd);
    HttpResponse route(const HttpRequest& req);
    void removeClient(int client_fd);
};

} // namespace ratelimiter
