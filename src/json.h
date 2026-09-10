#pragma once

#include <string>
#include <sstream>
#include <vector>
#include <utility>

namespace ratelimiter {

// Minimal JSON object builder for serializing responses.
class JsonBuilder {
public:
    JsonBuilder& add(const std::string& key, const std::string& value) {
        addKey(key);
        oss_ << '"' << escape(value) << '"';
        return *this;
    }
    JsonBuilder& add(const std::string& key, bool value) {
        addKey(key);
        oss_ << (value ? "true" : "false");
        return *this;
    }
    JsonBuilder& add(const std::string& key, int value) {
        addKey(key);
        oss_ << value;
        return *this;
    }
    JsonBuilder& add(const std::string& key, long long value) {
        addKey(key);
        oss_ << value;
        return *this;
    }
    JsonBuilder& add(const std::string& key, unsigned long long value) {
        addKey(key);
        oss_ << value;
        return *this;
    }
    JsonBuilder& add(const std::string& key, double value) {
        addKey(key);
        oss_ << value;
        return *this;
    }
    // Add a raw (already-JSON) value, e.g. a nested object
    JsonBuilder& addRaw(const std::string& key, const std::string& raw_json) {
        addKey(key);
        oss_ << raw_json;
        return *this;
    }

    std::string build() const {
        return "{" + oss_.str() + "}";
    }

private:
    std::ostringstream oss_;
    bool first_ = true;

    void addKey(const std::string& key) {
        if (!first_) oss_ << ",";
        first_ = false;
        oss_ << '"' << key << "\":";
    }

    static std::string escape(const std::string& s) {
        std::string out;
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += c;
            }
        }
        return out;
    }
};

// Extract a string field from a simple flat JSON body (no nesting).
// Returns empty string if not found. Minimal parser for request bodies.
inline std::string jsonGetString(const std::string& body, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return "";
    pos = body.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    // skip whitespace
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) pos++;
    if (pos >= body.size()) return "";
    if (body[pos] == '"') {
        pos++;
        size_t end = body.find('"', pos);
        if (end == std::string::npos) return "";
        return body.substr(pos, end - pos);
    }
    // numeric / bool value
    size_t end = pos;
    while (end < body.size() && body[end] != ',' && body[end] != '}' &&
           body[end] != ' ' && body[end] != '\n') end++;
    return body.substr(pos, end - pos);
}

} // namespace ratelimiter
