#include "http_server.h"
#include "http_parser.h"
#include "thread_pool.h"
#include "json.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

namespace ratelimiter {

HttpServer::HttpServer(int port, ThreadPool& pool)
    : port_(port), server_fd_(-1), running_(false), pool_(pool) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::addRoute(const std::string& method, const std::string& path, RouteHandler handler) {
    routes_[method + " " + path] = std::move(handler);
}

void HttpServer::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) throw std::runtime_error("Failed to create socket");

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(server_fd_);
        throw std::runtime_error("Failed to bind to port " + std::to_string(port_));
    }
    if (listen(server_fd_, SOMAXCONN) < 0) {
        close(server_fd_);
        throw std::runtime_error("Failed to listen");
    }

    running_.store(true);
    std::cout << "[rate-limiter] HTTP server listening on port " << port_ << std::endl;
    acceptLoop();
}

void HttpServer::stop() {
    running_.store(false);
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (int fd : client_fds_) close(fd);
    client_fds_.clear();
}

void HttpServer::acceptLoop() {
    while (running_.load()) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(server_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            if (running_.load()) std::cerr << "[rate-limiter] accept failed" << std::endl;
            continue;
        }
        int one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            client_fds_.insert(client_fd);
        }
        pool_.submit([this, client_fd]() { handleClient(client_fd); });
    }
}

void HttpServer::handleClient(int client_fd) {
    std::string buffer;
    char chunk[8192];

    while (running_.load()) {
        // Read until we have a complete request
        size_t req_len = HttpParser::requestLength(buffer);
        while (req_len == 0) {
            ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
            if (n <= 0) { removeClient(client_fd); return; }
            buffer.append(chunk, n);
            req_len = HttpParser::requestLength(buffer);
        }

        std::string raw_request = buffer.substr(0, req_len);
        buffer.erase(0, req_len);

        auto parsed = HttpParser::parse(raw_request);
        HttpResponse resp;
        bool close_conn = false;
        if (!parsed.has_value()) {
            resp = HttpResponse::json(400, "{\"error\":\"malformed request\"}");
            close_conn = true;
        } else {
            resp = route(parsed.value());
            // Honor the client's Connection: close request header
            std::string conn = parsed->header("Connection");
            if (conn.empty()) conn = parsed->header("connection");
            if (conn == "close" || conn == "Close") close_conn = true;
        }

        std::string out = resp.serialize(close_conn);
        ssize_t sent = send(client_fd, out.c_str(), out.size(), 0);
        if (sent < 0) { removeClient(client_fd); return; }

        if (close_conn) { removeClient(client_fd); return; }
    }
    removeClient(client_fd);
}

HttpResponse HttpServer::route(const HttpRequest& req) {
    // Exact match first
    auto it = routes_.find(req.method + " " + req.path);
    if (it != routes_.end()) return it->second(req);

    // Wildcard/prefix match: look for "METHOD /prefix/*"
    for (const auto& [key, handler] : routes_) {
        size_t space = key.find(' ');
        std::string m = key.substr(0, space);
        std::string p = key.substr(space + 1);
        if (m != req.method) continue;
        if (p.size() >= 2 && p.compare(p.size() - 2, 2, "/*") == 0) {
            std::string prefix = p.substr(0, p.size() - 1);  // keep trailing slash
            if (req.path.compare(0, prefix.size(), prefix) == 0) {
                return handler(req);
            }
        }
    }

    return HttpResponse::json(404, "{\"error\":\"not found\"}");
}

void HttpServer::removeClient(int client_fd) {
    close(client_fd);
    std::lock_guard<std::mutex> lock(clients_mutex_);
    client_fds_.erase(client_fd);
}

} // namespace ratelimiter
