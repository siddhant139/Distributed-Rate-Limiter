#include "http_parser.h"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace ratelimiter {

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    size_t end = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

size_t HttpParser::requestLength(const std::string& buffer) {
    size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) return 0;  // headers incomplete

    size_t body_start = header_end + 4;

    // Look for Content-Length
    size_t content_length = 0;
    std::string headers_part = buffer.substr(0, header_end);
    std::istringstream stream(headers_part);
    std::string line;
    while (std::getline(stream, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (key == "content-length") {
            try { content_length = std::stoul(val); } catch (...) { content_length = 0; }
        }
    }

    size_t total = body_start + content_length;
    if (buffer.size() < total) return 0;  // body incomplete
    return total;
}

std::optional<HttpRequest> HttpParser::parse(const std::string& raw) {
    HttpRequest req;

    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return std::nullopt;

    std::string headers_part = raw.substr(0, header_end);
    std::string body_part = raw.substr(header_end + 4);

    std::istringstream stream(headers_part);
    std::string request_line;
    if (!std::getline(stream, request_line)) return std::nullopt;

    // Parse request line: METHOD PATH HTTP/1.1
    std::istringstream rl(request_line);
    std::string version;
    if (!(rl >> req.method >> req.path >> version)) return std::nullopt;

    // Parse headers
    std::string line;
    while (std::getline(stream, line)) {
        if (line == "\r" || line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        req.headers[key] = val;
    }

    req.body = body_part;

    // Extract client_id: prefer X-API-Key header, fall back to X-Forwarded-For
    req.client_id = req.header("X-API-Key");
    if (req.client_id.empty()) req.client_id = req.header("X-Api-Key");
    if (req.client_id.empty()) req.client_id = req.header("X-Forwarded-For");
    if (req.client_id.empty()) req.client_id = "anonymous";

    return req;
}

} // namespace ratelimiter
