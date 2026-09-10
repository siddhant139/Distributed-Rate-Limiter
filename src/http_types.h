#pragma once

#include <string>
#include <unordered_map>
#include <sstream>

namespace ratelimiter {

struct HttpRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string client_id;  // extracted from X-API-Key header or IP

    std::string header(const std::string& key) const {
        auto it = headers.find(key);
        return it != headers.end() ? it->second : "";
    }
};

struct HttpResponse {
    int status_code = 200;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    static std::string statusText(int code) {
        switch (code) {
            case 200: return "OK";
            case 201: return "Created";
            case 400: return "Bad Request";
            case 404: return "Not Found";
            case 429: return "Too Many Requests";
            case 500: return "Internal Server Error";
            default:  return "Unknown";
        }
    }

    std::string serialize(bool close_connection = false) const {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status_code << " " << statusText(status_code) << "\r\n";
        oss << "Content-Length: " << body.size() << "\r\n";
        bool has_content_type = false;
        for (const auto& [k, v] : headers) {
            if (k == "Content-Type") has_content_type = true;
            oss << k << ": " << v << "\r\n";
        }
        if (!has_content_type) {
            oss << "Content-Type: application/json\r\n";
        }
        oss << "Connection: " << (close_connection ? "close" : "keep-alive") << "\r\n";
        oss << "\r\n";
        oss << body;
        return oss.str();
    }

    static HttpResponse json(int code, const std::string& json_body) {
        HttpResponse resp;
        resp.status_code = code;
        resp.headers["Content-Type"] = "application/json";
        resp.body = json_body;
        return resp;
    }
};

} // namespace ratelimiter
