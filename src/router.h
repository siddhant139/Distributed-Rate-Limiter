#pragma once

#include "http_types.h"

namespace ratelimiter {

class RateLimiter;
class StatsCollector;

// Maps HTTP routes to rate limiter operations.
class Router {
public:
    Router(RateLimiter& limiter, StatsCollector& stats);

    // POST /api/check      -> check rate limit for the request's client_id
    HttpResponse handleCheck(const HttpRequest& req);
    // GET  /stats           -> global stats
    HttpResponse handleGlobalStats(const HttpRequest& req);
    // GET  /stats/{id}       -> per-client stats
    HttpResponse handleClientStats(const HttpRequest& req);
    // GET  /config           -> current config
    HttpResponse handleGetConfig(const HttpRequest& req);
    // POST /config           -> update config (JSON body)
    HttpResponse handleUpdateConfig(const HttpRequest& req);
    // POST /api/reset        -> reset a client's limit
    HttpResponse handleReset(const HttpRequest& req);
    // GET  /health           -> health check
    HttpResponse handleHealth(const HttpRequest& req);

private:
    RateLimiter& limiter_;
    StatsCollector& stats_;
};

} // namespace ratelimiter
