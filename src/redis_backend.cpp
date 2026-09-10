#include "redis_backend.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <iostream>
#include <stdexcept>

namespace ratelimiter {

// ---------------------------------------------------------------------------
// Atomic Lua scripts. Redis runs each of these single-threaded and atomically,
// so the entire read-decide-write sequence is indivisible across all instances.
// ---------------------------------------------------------------------------

// Token bucket. ARGV: capacity, refill_per_sec, now_ms.  Returns {allowed, tokens}.
static const char* TOKEN_BUCKET_LUA = R"LUA(
local key = KEYS[1]
local capacity = tonumber(ARGV[1])
local refill = tonumber(ARGV[2])
local now = tonumber(ARGV[3])
local d = redis.call('HMGET', key, 'tokens', 'ts')
local tokens = tonumber(d[1])
local ts = tonumber(d[2])
if tokens == nil then tokens = capacity; ts = now end
local elapsed = (now - ts) / 1000.0
if elapsed < 0 then elapsed = 0 end
tokens = math.min(capacity, tokens + elapsed * refill)
local allowed = 0
if tokens >= 1 then tokens = tokens - 1; allowed = 1 end
redis.call('HSET', key, 'tokens', tokens, 'ts', now)
if refill > 0 then redis.call('PEXPIRE', key, math.ceil((capacity / refill) * 1000) + 1000) end
return {allowed, tostring(tokens)}
)LUA";

// Sliding window log. ARGV: now_ms, window_ms, max, unique_member.  Returns {allowed, count}.
static const char* SLIDING_LOG_LUA = R"LUA(
local key = KEYS[1]
local now = tonumber(ARGV[1])
local window = tonumber(ARGV[2])
local max = tonumber(ARGV[3])
local member = ARGV[4]
redis.call('ZREMRANGEBYSCORE', key, 0, now - window)
local count = redis.call('ZCARD', key)
local allowed = 0
if count < max then
  redis.call('ZADD', key, now, member)
  count = count + 1
  allowed = 1
end
redis.call('PEXPIRE', key, window + 1000)
return {allowed, count}
)LUA";

// Sliding window counter (two buckets). ARGV: now_ms, window_ms, max.  Returns {allowed, estimate}.
static const char* SLIDING_COUNTER_LUA = R"LUA(
local key = KEYS[1]
local now = tonumber(ARGV[1])
local window = tonumber(ARGV[2])
local max = tonumber(ARGV[3])
local cur_win = now - (now % window)
local d = redis.call('HMGET', key, 'win', 'cur', 'prev')
local stored = tonumber(d[1])
local cur = tonumber(d[2])
local prev = tonumber(d[3])
if stored == nil then stored = cur_win; cur = 0; prev = 0 end
if cur_win == stored then
elseif cur_win == stored + window then prev = cur; cur = 0; stored = cur_win
else prev = 0; cur = 0; stored = cur_win end
local elapsed = now - stored
local weight = (window - elapsed) / window
if weight < 0 then weight = 0 end
local est = prev * weight + cur
local allowed = 0
if est < max then cur = cur + 1; allowed = 1 end
redis.call('HSET', key, 'win', stored, 'cur', cur, 'prev', prev)
redis.call('PEXPIRE', key, window * 2 + 1000)
return {allowed, math.floor(est) + allowed}
)LUA";

static std::string tbKey(const std::string& k) { return "tb:" + k; }
static std::string swKey(const std::string& k) { return "sw:" + k; }
static std::string scKey(const std::string& k) { return "sc:" + k; }

// Split "a\nb" reply into two fields.
static void splitTwo(const std::string& s, std::string& a, std::string& b) {
    size_t nl = s.find('\n');
    if (nl == std::string::npos) { a = s; b.clear(); return; }
    a = s.substr(0, nl);
    b = s.substr(nl + 1);
}

// ---------------------------------------------------------------------------
// Construction / connection
// ---------------------------------------------------------------------------

RedisBackend::RedisBackend(const std::string& host, int port)
    : host_(host), port_(port), fd_(-1) {}

RedisBackend::~RedisBackend() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool RedisBackend::connect() {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (::getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0) {
        return false;
    }
    for (struct addrinfo* p = res; p; p = p->ai_next) {
        int sock = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        if (::connect(sock, p->ai_addr, p->ai_addrlen) == 0) { fd_ = sock; break; }
        ::close(sock);
    }
    ::freeaddrinfo(res);
    return fd_ >= 0;
}

// ---------------------------------------------------------------------------
// RESP I/O
// ---------------------------------------------------------------------------

bool RedisBackend::sendAll(const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) { ::close(fd_); fd_ = -1; return false; }
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string RedisBackend::sendCommand(const std::vector<std::string>& args) {
    std::ostringstream os;
    os << '*' << args.size() << "\r\n";
    for (const auto& a : args) os << '$' << a.size() << "\r\n" << a << "\r\n";
    if (!sendAll(os.str())) return {};
    return readReply();
}

std::string RedisBackend::readReply() {
    auto readLine = [this]() -> std::string {
        std::string line;
        char c = 0;
        while (true) {
            ssize_t n = ::recv(fd_, &c, 1, 0);
            if (n <= 0) { ::close(fd_); fd_ = -1; return {}; }
            if (c == '\r') { ::recv(fd_, &c, 1, 0); return line; }  // consume \n
            line += c;
        }
    };

    std::string first = readLine();
    if (first.empty()) return {};
    char type = first[0];
    std::string payload = first.substr(1);

    switch (type) {
        case '+':
        case '-':
        case ':':
            return payload;
        case '$': {
            int len = 0;
            try { len = std::stoi(payload); } catch (...) { return {}; }
            if (len < 0) return {};
            std::string buf(static_cast<size_t>(len), '\0');
            size_t got = 0;
            while (got < static_cast<size_t>(len)) {
                ssize_t n = ::recv(fd_, &buf[got], static_cast<size_t>(len) - got, 0);
                if (n <= 0) { ::close(fd_); fd_ = -1; return {}; }
                got += static_cast<size_t>(n);
            }
            char crlf[2]; ::recv(fd_, crlf, 2, 0);
            return buf;
        }
        case '*': {
            int count = 0;
            try { count = std::stoi(payload); } catch (...) { return {}; }
            if (count < 0) return {};
            std::string result;
            for (int i = 0; i < count; ++i) {
                std::string elem = readReply();
                if (i > 0) result += '\n';
                result += elem;
            }
            return result;
        }
        default:
            return {};
    }
}

std::string RedisBackend::eval(const std::string& script, const std::string& key,
                               const std::vector<std::string>& argv) {
    std::vector<std::string> cmd;
    cmd.reserve(4 + argv.size());
    cmd.push_back("EVAL");
    cmd.push_back(script);
    cmd.push_back("1");        // numkeys
    cmd.push_back(key);
    for (const auto& a : argv) cmd.push_back(a);
    return sendCommand(cmd);
}

// ---------------------------------------------------------------------------
// Atomic checks (each is one EVAL = one indivisible Redis operation)
// ---------------------------------------------------------------------------

RateLimitBackend::BucketDecision
RedisBackend::tokenBucketCheck(const std::string& key, double capacity,
                               double refill_per_sec, int64_t now_ms) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    // Fail-open: if Redis is unreachable, allow the request rather than taking
    // down the protected service. (Documented tradeoff — a stricter deployment
    // might fail-closed.)
    if (fd_ < 0) return {true, capacity};

    std::string reply = eval(TOKEN_BUCKET_LUA, tbKey(key),
                             {std::to_string(capacity), std::to_string(refill_per_sec),
                              std::to_string(now_ms)});
    if (reply.empty() || fd_ < 0) return {true, capacity};

    std::string a, b;
    splitTwo(reply, a, b);
    try {
        return {std::stoi(a) == 1, std::stod(b)};
    } catch (...) {
        return {true, capacity};
    }
}

RateLimitBackend::WindowDecision
RedisBackend::slidingWindowLogCheck(const std::string& key, int64_t now_ms,
                                    int64_t window_ms, int max_requests) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    if (fd_ < 0) return {true, 0};

    std::string member = std::to_string(now_ms) + ":" + std::to_string(seq_++);
    std::string reply = eval(SLIDING_LOG_LUA, swKey(key),
                             {std::to_string(now_ms), std::to_string(window_ms),
                              std::to_string(max_requests), member});
    if (reply.empty() || fd_ < 0) return {true, 0};

    std::string a, b;
    splitTwo(reply, a, b);
    try {
        return {std::stoi(a) == 1, std::stoi(b)};
    } catch (...) {
        return {true, 0};
    }
}

RateLimitBackend::WindowDecision
RedisBackend::slidingWindowCounterCheck(const std::string& key, int64_t now_ms,
                                        int64_t window_ms, int max_requests) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    if (fd_ < 0) return {true, 0};

    std::string reply = eval(SLIDING_COUNTER_LUA, scKey(key),
                             {std::to_string(now_ms), std::to_string(window_ms),
                              std::to_string(max_requests)});
    if (reply.empty() || fd_ < 0) return {true, 0};

    std::string a, b;
    splitTwo(reply, a, b);
    try {
        return {std::stoi(a) == 1, std::stoi(b)};
    } catch (...) {
        return {true, 0};
    }
}

void RedisBackend::reset(const std::string& key) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    if (fd_ < 0) return;
    sendCommand({"DEL", tbKey(key)});
    sendCommand({"DEL", swKey(key)});
    sendCommand({"DEL", scKey(key)});
}

} // namespace ratelimiter
