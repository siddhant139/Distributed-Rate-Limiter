#include "router.h"
#include "rate_limiter.h"
#include "stats.h"
#include "json.h"

namespace ratelimiter {

Router::Router(RateLimiter& limiter, StatsCollector& stats)
    : limiter_(limiter), stats_(stats) {}

HttpResponse Router::handleCheck(const HttpRequest& req) {
    RateLimitResult result = limiter_.checkLimit(req.client_id);

    if (result.allowed) stats_.recordAllowed(req.client_id);
    else stats_.recordDenied(req.client_id);

    JsonBuilder jb;
    jb.add("allowed", result.allowed)
      .add("remaining", result.remaining)
      .add("limit", result.limit)
      .add("retry_after_ms", result.retry_after_ms)
      .add("client_id", req.client_id)
      .add("algorithm", limiter_.algorithmName());

    int code = result.allowed ? 200 : 429;
    HttpResponse resp = HttpResponse::json(code, jb.build());
    if (!result.allowed) {
        resp.headers["Retry-After"] = std::to_string((result.retry_after_ms + 999) / 1000);
    }
    resp.headers["X-RateLimit-Limit"] = std::to_string(result.limit);
    resp.headers["X-RateLimit-Remaining"] = std::to_string(result.remaining);
    return resp;
}

HttpResponse Router::handleGlobalStats(const HttpRequest&) {
    return HttpResponse::json(200, stats_.globalJson());
}

HttpResponse Router::handleClientStats(const HttpRequest& req) {
    // path is /stats/{id}; extract id after last '/'
    std::string id = req.path.substr(req.path.find_last_of('/') + 1);
    if (id.empty() || id == "stats") {
        return HttpResponse::json(400, "{\"error\":\"missing client id\"}");
    }
    return HttpResponse::json(200, stats_.clientJson(id));
}

HttpResponse Router::handleGetConfig(const HttpRequest&) {
    RateLimitConfig cfg = limiter_.getConfig();
    JsonBuilder jb;
    jb.add("algorithm", limiter_.algorithmName())
      .add("max_requests", cfg.max_requests)
      .add("window_seconds", cfg.window_seconds)
      .add("burst_size", cfg.burst_size)
      .add("refill_rate", cfg.refill_rate);
    return HttpResponse::json(200, jb.build());
}

HttpResponse Router::handleUpdateConfig(const HttpRequest& req) {
    RateLimitConfig cfg = limiter_.getConfig();

    std::string mr = jsonGetString(req.body, "max_requests");
    std::string ws = jsonGetString(req.body, "window_seconds");
    std::string bs = jsonGetString(req.body, "burst_size");
    std::string rr = jsonGetString(req.body, "refill_rate");

    try {
        if (!mr.empty()) cfg.max_requests = std::stoi(mr);
        if (!ws.empty()) cfg.window_seconds = std::stoi(ws);
        if (!bs.empty()) cfg.burst_size = std::stoi(bs);
        if (!rr.empty()) cfg.refill_rate = std::stod(rr);
    } catch (...) {
        return HttpResponse::json(400, "{\"error\":\"invalid config values\"}");
    }

    limiter_.updateConfig(cfg);
    return handleGetConfig(req);
}

HttpResponse Router::handleReset(const HttpRequest& req) {
    std::string id = req.client_id;
    std::string body_id = jsonGetString(req.body, "client_id");
    if (!body_id.empty()) id = body_id;
    limiter_.resetClient(id);
    JsonBuilder jb;
    jb.add("reset", true).add("client_id", id);
    return HttpResponse::json(200, jb.build());
}

HttpResponse Router::handleHealth(const HttpRequest&) {
    return HttpResponse::json(200, "{\"status\":\"healthy\"}");
}

} // namespace ratelimiter
