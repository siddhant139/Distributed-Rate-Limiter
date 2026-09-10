#include <iostream>
#include <string>

#include "../src/http_parser.h"

int tests_passed = 0, tests_failed = 0;

void check(bool condition, const std::string& name) {
    if (condition) { std::cout << "  PASS: " << name << std::endl; tests_passed++; }
    else { std::cout << "  FAIL: " << name << std::endl; tests_failed++; }
}

int main() {
    std::cout << "=== HTTP Parser Tests ===" << std::endl;

    // --- Test: parse a GET request ---
    {
        std::cout << "\n[parse GET request]" << std::endl;
        std::string raw = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
        auto result = ratelimiter::HttpParser::parse(raw);
        check(result.has_value(), "GET request parsed successfully");
        if (result.has_value()) {
            check(result->method == "GET", "method is GET");
            check(result->path == "/health", "path is /health");
        }
    }

    // --- Test: parse POST with body and Content-Length ---
    {
        std::cout << "\n[parse POST with body]" << std::endl;
        std::string body = "{\"test\":\"x\"}";
        std::string raw = "POST /api/check HTTP/1.1\r\n"
                          "Content-Length: " + std::to_string(body.size()) + "\r\n"
                          "X-API-Key: user1\r\n"
                          "\r\n" + body;
        auto result = ratelimiter::HttpParser::parse(raw);
        check(result.has_value(), "POST request parsed successfully");
        if (result.has_value()) {
            check(result->method == "POST", "method is POST");
            check(result->path == "/api/check", "path is /api/check");
            check(result->client_id == "user1", "client_id is user1 from X-API-Key");
            check(result->body.size() == body.size(), "body length matches Content-Length");
        }
    }

    // --- Test: header extraction sets client_id ---
    {
        std::cout << "\n[header extraction - X-API-Key sets client_id]" << std::endl;
        std::string raw = "GET /test HTTP/1.1\r\nX-API-Key: mykey123\r\n\r\n";
        auto result = ratelimiter::HttpParser::parse(raw);
        check(result.has_value(), "request parsed");
        if (result.has_value()) {
            check(result->client_id == "mykey123", "client_id set from X-API-Key header");
            check(result->header("X-API-Key") == "mykey123", "header() returns X-API-Key value");
        }
    }

    // --- Test: client_id defaults to 'anonymous' when no key header ---
    {
        std::cout << "\n[client_id defaults to anonymous]" << std::endl;
        std::string raw = "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n";
        auto result = ratelimiter::HttpParser::parse(raw);
        check(result.has_value(), "request parsed");
        if (result.has_value()) {
            check(result->client_id == "anonymous", "client_id defaults to 'anonymous'");
        }
    }

    // --- Test: malformed request returns nullopt ---
    {
        std::cout << "\n[malformed request returns nullopt]" << std::endl;
        std::string raw = "GET /health HTTP/1.1\r\nHost: localhost";
        auto result = ratelimiter::HttpParser::parse(raw);
        check(!result.has_value(), "malformed request (no \\r\\n\\r\\n) returns nullopt");
    }

    // --- Test: requestLength ---
    {
        std::cout << "\n[requestLength]" << std::endl;
        std::string incomplete = "GET /health HTTP/1.1\r\nHost: localhost";
        std::string complete = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";

        size_t incomplete_len = ratelimiter::HttpParser::requestLength(incomplete);
        check(incomplete_len == 0, "incomplete buffer returns 0");

        size_t complete_len = ratelimiter::HttpParser::requestLength(complete);
        check(complete_len == complete.size(), "complete buffer returns full length");
    }

    // --- Test: headers map populated correctly ---
    {
        std::cout << "\n[headers map populated]" << std::endl;
        std::string raw = "GET /test HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Accept: text/html\r\n"
                          "X-Custom: foobar\r\n"
                          "\r\n";
        auto result = ratelimiter::HttpParser::parse(raw);
        check(result.has_value(), "request parsed");
        if (result.has_value()) {
            check(result->header("Host") == "localhost", "Host header present");
            check(result->header("Accept") == "text/html", "Accept header present");
            check(result->header("X-Custom") == "foobar", "X-Custom header present");
            check(result->headers.size() >= 3, "at least 3 headers in map");
        }
    }

    // --- Summary ---
    std::cout << "\n=== Results: " << tests_passed << " passed, " << tests_failed << " failed ===" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}
