#pragma once

#include "http_types.h"
#include <string>
#include <optional>

namespace ratelimiter {

class HttpParser {
public:
    // Parse a complete HTTP request from raw text.
    // Returns nullopt if the request is incomplete or malformed.
    static std::optional<HttpRequest> parse(const std::string& raw);

    // Given accumulated buffer data, determine if a full request is present.
    // Returns the byte length of the complete request (headers + body), or 0 if incomplete.
    static size_t requestLength(const std::string& buffer);
};

} // namespace ratelimiter
