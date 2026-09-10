# rate-limiter — Distributed Rate Limiting Service

An HTTP rate limiting service built from scratch in **C++17**, featuring three algorithms — **Token Bucket**, **Sliding Window Log**, and **Sliding Window Counter** — with pluggable backends: in-memory (single node) or **Redis-backed distributed state** with **atomic check-and-decrement via Lua scripts**. The entire stack is hand-rolled: POSIX sockets, a custom HTTP/1.1 request parser, a thread pool, a raw-TCP RESP Redis client, and per-client statistics — no frameworks, no dependencies beyond the standard library and POSIX.

**Distributed correctness is the headline:** a naive read-then-write across instances is a TOCTOU race (two instances both read "1 left" and both allow). Every decision is executed as a single atomic Redis Lua script, so the limit holds exactly across all instances — verified with 2 instances sharing 1 Redis under 400 concurrent requests against a limit of 100 → **exactly 100 allowed**.

---

## Architecture

```
                        ┌─────────────────────────────────────────────────┐
                        │               rate-limiter  (C++17)             │
                        │                                                 │
  ┌──────────┐   TCP    │  ┌──────────────┐    ┌────────────────────┐     │
  │  HTTP     │────────▶│  │  HTTP Server  │───▶│    Thread Pool     │     │
  │  Clients  │         │  │  (POSIX       │    │  (producer /       │     │
  │  (curl,   │◀────────│  │   sockets,    │◀───│   consumer queue)  │     │
  │  browser, │  HTTP   │  │   custom      │    └─────────┬──────────┘     │
  │  bench)   │  resp.  │  │   parser)     │              │                │
  └──────────┘         │  └──────────────┘              │                │
                        │                      ┌─────────▼──────────┐     │
                        │                      │  Rate Limiter      │     │
                        │                      │  Engine             │     │
                        │                      │  ┌───────────────┐ │     │
                        │                      │  │ Token Bucket  │ │     │
                        │                      │  │ Sliding Window│ │     │
                        │                      │  └───────┬───────┘ │     │
                        │                      └──────────┼─────────┘     │
                        │                                 │               │
                        │               ┌─────────────────┼────────┐      │
                        │               │     Backend     │        │      │
                        │               │   ┌─────────┐ ┌─▼──────┐│      │
                        │               │   │ Memory  │ │ Redis  ││      │
                        │               │   │(shared_ │ │(RESP / ││      │
                        │               │   │ mutex)  │ │raw TCP)││      │
                        │               │   └─────────┘ └────────┘│      │
                        │               └──────────────────────────┘      │
                        └─────────────────────────────────────────────────┘
```

---

## Features

| Feature | Description |
|---|---|
| **Token Bucket algorithm** | Burst-tolerant rate limiting with steady token refill (O(1) memory) |
| **Sliding Window Log algorithm** | Exact request counting within a rolling window (O(N) memory) |
| **Sliding Window Counter algorithm** | Two-bucket weighted approximation — O(1) memory, no boundary burst |
| **Atomic distributed decisions** | Each check runs as a single Redis **Lua script** — no cross-instance TOCTOU race |
| **Pluggable backends** | In-memory (single node) or Redis-backed distributed state |
| **Redis via raw TCP** | Custom RESP protocol client + `EVAL` — no `hiredis` dependency |
| **HTTP/1.1 server** | Hand-rolled request parser, POSIX sockets |
| **Thread pool** | Producer-consumer work queue with configurable worker count |
| **Per-client statistics** | Track allowed / denied counts and last-seen time per API key |
| **Dynamic configuration** | Change algorithm, limits, and window size at runtime via `POST /config` |
| **HTTP 429 + Retry-After** | Standards-compliant rate limit response with retry guidance |
| **Graceful shutdown** | `SIGINT` / `SIGTERM` handler drains the thread pool cleanly |

---

## Algorithms

### Token Bucket

Each client gets a bucket of **B** tokens (the burst capacity). Tokens refill at a steady rate of **r tokens/sec**. A request consumes one token; if the bucket is empty the request is denied.

```
  Bucket capacity B = 5,  refill rate r = 1 tok/sec

  Time 0s   ●●●●●  (5 tokens — full)
  Req  →    ●●●●○  (4 remaining)
  Req  →    ●●●○○  (3 remaining)
  Req  →    ●●○○○  (2 remaining)
  ...1 sec passes, +1 refill...
  Time 1s   ●●●○○  (3 tokens)
  Req  →    ●●○○○  (2 remaining)
```

**Tradeoff:** allows short bursts up to B, but averages out to r req/sec over time.

### Sliding Window Log

Maintain a log of every request timestamp within the last **W** seconds. A new request is allowed only if the log contains fewer than **L** entries.

```
  Window W = 10s,  limit L = 3

  t=0s  [req]                  log: [0]           → ALLOWED (1 < 3)
  t=2s  [req]                  log: [0, 2]        → ALLOWED (2 < 3)
  t=4s  [req]                  log: [0, 2, 4]     → ALLOWED (3 = 3)
  t=5s  [req]                  log: [0, 2, 4]     → DENIED  (3 ≥ 3)
  t=11s [req]  evict t=0       log: [2, 4, 11]    → ALLOWED (3 = 3)
```

**Tradeoff:** strict fairness (no burst tolerance), but requires storing every timestamp (more memory).

### Sliding Window Counter

An **O(1)-memory approximation** of the log. Keep two counters — the current fixed window and the
previous one — and estimate the rolling count by weighting the previous window by how much of it
still overlaps the sliding window:

```
  estimated = prev_count * (1 - elapsed/window) + cur_count

  window W = 60s, limit L = 100
  At 15s into the current window, with prev=80, cur=30:
    weight    = 1 - 15/60 = 0.75
    estimated = 80 * 0.75 + 30 = 90   → ALLOWED (90 < 100)
```

**Tradeoff:** slight inaccuracy near window boundaries, but **constant memory per client** regardless
of request volume — which is why large API gateways (Cloudflare, Kong) use it for millions of clients.

---

## API Reference

| Method | Endpoint | Description |
|---|---|---|
| `POST` | `/api/check` | Check rate limit for the API key in `X-API-Key` header |
| `POST` | `/api/reset` | Reset rate limit state for the key in `X-API-Key` header |
| `GET` | `/stats` | Aggregate statistics across all clients |
| `GET` | `/stats/{id}` | Per-client statistics for a specific API key |
| `GET` | `/config` | Current server configuration (algorithm, limits, etc.) |
| `POST` | `/config` | Update configuration dynamically (JSON body) |
| `GET` | `/health` | Health check — returns `200 OK` if server is running |

### Example: `POST /api/check` — Allowed (200)

```json
{
  "allowed": true,
  "remaining": 9,
  "limit": 10,
  "retry_after": 0
}
```

### Example: `POST /api/check` — Denied (429)

```json
{
  "allowed": false,
  "remaining": 0,
  "limit": 10,
  "retry_after": 1.234
}
```

Response includes the `Retry-After` HTTP header (seconds until the client may retry).

---

## Build & Run

### Prerequisites

- C++17 compiler (GCC 9+, Clang 10+, Apple Clang 12+)
- POSIX-compliant OS (Linux, macOS)
- Redis (optional, for distributed mode)
- CMake 3.14+ (optional)

### Compile with g++

```bash
mkdir -p build
g++ -std=c++17 -O2 -pthread \
    src/main.cpp src/server.cpp src/http_parser.cpp src/thread_pool.cpp \
    src/rate_limiter.cpp src/token_bucket.cpp src/sliding_window.cpp \
    src/memory_backend.cpp src/redis_backend.cpp src/redis_client.cpp \
    src/config.cpp src/stats.cpp \
    -o build/rate_limiter
```

### Compile with CMake

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Run

```bash
# Token Bucket (default): 10 req/sec, burst of 10, port 8080
./build/rate_limiter -p 8080 -a token_bucket -m 10 -b 10

# Sliding Window: 100 requests per 60-second window
./build/rate_limiter -p 8080 -a sliding_window -m 100 -w 60

# Sliding Window Counter: O(1)-memory approximation, 100 per 60s
./build/rate_limiter -p 8080 -a sliding_window_counter -m 100 -w 60

# Distributed mode (Redis backend) — atomic Lua, correct across instances
./build/rate_limiter -p 8080 -a token_bucket -m 10 --redis localhost:6379

# High limit for benchmarking
./build/rate_limiter -p 8090 -m 1000000
```

> **Interview prep:** see [`docs/INTERVIEW_GUIDE.md`](docs/INTERVIEW_GUIDE.md) for a full walkthrough
> of the algorithms, the atomicity fix (TOCTOU race → Lua), design tradeoffs, and a Q&A bank.

---

## Usage Examples

### Health check

```bash
curl http://localhost:8080/health
# {"status":"healthy","uptime_seconds":42}
```

### Rate limit check (allowed → denied)

```bash
# First request — allowed
curl -X POST -H "X-API-Key: user_123" http://localhost:8080/api/check
# {"allowed":true,"remaining":9,"limit":10,"retry_after":0}

# ... after exhausting the limit ...
curl -X POST -H "X-API-Key: user_123" http://localhost:8080/api/check
# HTTP/1.1 429 Too Many Requests
# Retry-After: 1
# {"allowed":false,"remaining":0,"limit":10,"retry_after":1.000}
```

### View statistics

```bash
# All clients
curl http://localhost:8080/stats
# {"total_requests":1500,"total_allowed":1480,"total_denied":20,"active_clients":3}

# Single client
curl http://localhost:8080/stats/user_123
# {"api_key":"user_123","allowed":98,"denied":12,"last_seen":"2026-09-09T10:15:30Z"}
```

### Update configuration at runtime

```bash
curl -X POST -H "Content-Type: application/json" \
     -d '{"algorithm":"sliding_window","max_requests":50,"window_seconds":30}' \
     http://localhost:8080/config
# {"status":"updated","algorithm":"sliding_window","max_requests":50,"window_seconds":30}
```

---

## Distributed Mode (Redis)

When launched with `--redis host:port`, multiple rate-limiter instances share state through a single Redis server, enforcing a **global** rate limit across all nodes.

### Redis Key Schemes

| Algorithm | Key Pattern | Redis Structure | Operations (inside one Lua script) |
|---|---|---|---|
| **Token Bucket** | `tb:{api_key}` | Hash (`tokens`, `ts`) | `HMGET` → compute → `HSET` + `PEXPIRE` |
| **Sliding Window Log** | `sw:{api_key}` | Sorted Set (score = timestamp) | `ZREMRANGEBYSCORE` → `ZCARD` → `ZADD` |
| **Sliding Window Counter** | `sc:{api_key}` | Hash (`win`, `cur`, `prev`) | `HMGET` → roll + weight → `HSET` |

### How It Works — Atomic Check-and-Decrement

The **entire** decision for each algorithm runs inside a single **Lua script** executed via `EVAL`.
Redis runs Lua scripts **single-threaded and atomically**: no command from any other instance can
interleave mid-script. This makes the read-decide-write indivisible **across all instances**, which
is what gives true distributed correctness.

1. **Token Bucket:** the script `HMGET`s tokens+timestamp, refills by elapsed time, decrements if a
   token is available, `HSET`s the new state, and sets an idle-key TTL — all atomically.
2. **Sliding Window Log:** the script prunes expired entries (`ZREMRANGEBYSCORE`), counts (`ZCARD`),
   and adds the request (`ZADD`) only if under the limit — all atomically.
3. **Sliding Window Counter:** the script rolls the two fixed-window counters, computes the weighted
   estimate, and increments if allowed — all atomically.

**Why this matters (the bug it fixes):** the naive approach reads the counter, decides in the client,
then writes back. Between the read and the write, another instance can read the same value — a
**TOCTOU race** that lets more requests through than the limit. Folding everything into one atomic
Lua script eliminates it. **Verified:** 2 instances / 1 Redis / 400 concurrent requests / limit 100
→ exactly **100 allowed, 300 denied**.

All Redis communication uses a hand-rolled **RESP protocol** client over raw TCP — no external libraries.

---

## Design Decisions

| Decision | Rationale |
|---|---|
| **Atomic Lua over `WATCH`/`MULTI`** | The allow/deny decision depends on a value read mid-operation. `WATCH` optimistic locking retries on conflict (wasteful under contention); `MULTI/EXEC` can't branch on a read. A Lua script does read + branch + write atomically server-side in one round-trip. |
| **Three algorithms** | They sit at different points on the accuracy/memory/burst tradeoff — Token Bucket (burst-friendly, O(1)), Sliding Window Log (exact, O(N)), Sliding Window Counter (approximate, O(1)). |
| **Pluggable backend interface** | The engine doesn't know whether state is in-process or distributed. Swap backends with a single flag (`--redis`). |
| **Fail-open on Redis outage** | A rate limiter should protect a service, not become a SPOF that takes it down. Documented tradeoff; a security-critical limiter might fail-closed. |
| **HTTP 429 + `Retry-After`** | Industry standard (RFC 6585). Clients can auto-back-off without custom logic. |
| **Raw POSIX sockets + custom HTTP/RESP parsers** | Educational goal: understand the stack from `socket()` to `recv()`, and the Redis wire protocol, without frameworks. |

---

## Design Tradeoffs & Limitations

Honest engineering notes — known limitations and how I'd address them (interview-ready self-critique).

| # | Limitation | How I'd improve it |
|---|---|---|
| 1 | Single Redis connection guarded by a mutex | Connection pool so backend calls don't serialize on one socket |
| 2 | Fail-open when Redis is down (no limiting during outage) | Make fail-open/closed configurable per deployment |
| 3 | Sliding Window Log is O(N) memory per client | Use the Counter algorithm for high-cardinality clients (already implemented) |
| 4 | Ships the full Lua script on every `EVAL` | `SCRIPT LOAD` once + `EVALSHA` by hash to save bandwidth |
| 5 | Each instance uses its own clock (`now`) | Use Redis server `TIME` inside the script for a single authoritative clock |
| 6 | It's a check API, not a reverse proxy | Add proxy mode: forward allowed requests upstream, 429 the rest |
| 7 | Admin endpoints (`/config`, `/api/reset`) unauthenticated | Protect with an admin token |

---

## Benchmarks

Run the benchmark suite against a server started with a high limit (`-m 1000000`):

```bash
./build/rate_limiter -p 8090 -m 1000000 &
python3 benchmarks/benchmark.py 8090
```

### Throughput (POST /api/check)

| Threads | Requests/sec |
|---|---|
| 1 | ~4,100 |
| 4 | ~8,800 |
| 8 | ~9,700 |
| 16 | ~9,800 |

### Latency (single-threaded, 500 requests)

| Percentile | Latency (µs) |
|---|---|
| p50 | 216 |
| p95 | 236 |
| p99 | 338 |
| avg | 227 |

_Measured on macOS, Apple Silicon, in-memory backend. Throughput is bound by the Python client's per-request TCP round-trip (Connection: close); server-side rate-limit checks are sub-microsecond._

---

## Testing

### Unit Tests

```bash
# Token Bucket
g++ -std=c++17 -pthread tests/test_token_bucket.cpp src/token_bucket.cpp -o build/test_token_bucket
./build/test_token_bucket

# Sliding Window
g++ -std=c++17 -pthread tests/test_sliding_window.cpp src/sliding_window.cpp -o build/test_sliding_window
./build/test_sliding_window

# HTTP Parser
g++ -std=c++17 -pthread tests/test_http_parser.cpp src/http_parser.cpp -o build/test_http_parser
./build/test_http_parser

# Thread Pool
g++ -std=c++17 -pthread tests/test_thread_pool.cpp src/thread_pool.cpp -o build/test_thread_pool
./build/test_thread_pool

# Redis Client
g++ -std=c++17 -pthread tests/test_redis_client.cpp src/redis_client.cpp -o build/test_redis_client
./build/test_redis_client
```

### Integration Tests

```bash
# End-to-end: starts server, sends requests, validates responses
g++ -std=c++17 -pthread tests/test_integration.cpp \
    src/server.cpp src/http_parser.cpp src/thread_pool.cpp \
    src/rate_limiter.cpp src/token_bucket.cpp src/sliding_window.cpp \
    src/memory_backend.cpp src/redis_backend.cpp src/redis_client.cpp \
    src/config.cpp src/stats.cpp \
    -o build/test_integration
./build/test_integration

# Redis integration (requires running Redis on localhost:6379)
g++ -std=c++17 -pthread tests/test_redis_integration.cpp \
    src/redis_client.cpp src/redis_backend.cpp \
    -o build/test_redis_integration
./build/test_redis_integration
```

---

## Project Structure

```
rate-limiter/
├── CMakeLists.txt                  # CMake build configuration
├── README.md                       # This file
├── .gitignore                      # Build artifacts, IDE files
├── benchmarks/
│   └── benchmark.py                # Python load-testing benchmark (stdlib only)
├── src/
│   ├── main.cpp                    # Entry point, CLI argument parsing, signal handling
│   ├── server.h / server.cpp       # HTTP server: accept loop, socket management
│   ├── http_parser.h / .cpp        # State-machine HTTP/1.1 request parser
│   ├── thread_pool.h / .cpp        # Fixed-size thread pool with work queue
│   ├── rate_limiter.h / .cpp       # Engine: dispatches to algorithm + backend
│   ├── token_bucket.h / .cpp       # Token Bucket algorithm implementation
│   ├── sliding_window.h / .cpp     # Sliding Window Log algorithm implementation
│   ├── backend.h                   # Abstract backend interface
│   ├── memory_backend.h / .cpp     # In-memory backend (shared_mutex)
│   ├── redis_backend.h / .cpp      # Redis-backed distributed backend
│   ├── redis_client.h / .cpp       # RESP protocol client over raw TCP
│   ├── config.h / .cpp             # Runtime configuration management
│   └── stats.h / .cpp              # Per-client and aggregate statistics
└── tests/
    ├── test_token_bucket.cpp       # Unit tests: token bucket logic
    ├── test_sliding_window.cpp     # Unit tests: sliding window logic
    ├── test_http_parser.cpp        # Unit tests: HTTP parsing edge cases
    ├── test_thread_pool.cpp        # Unit tests: pool lifecycle, task execution
    ├── test_redis_client.cpp       # Unit tests: RESP serialization / parsing
    ├── test_integration.cpp        # End-to-end: server + HTTP + rate limiting
    └── test_redis_integration.cpp  # Integration: Redis backend with real Redis
```

---

## CS Fundamentals Demonstrated

| Concept | Where |
|---|---|
| **HTTP/1.1 parsing** | `http_parser.cpp` — state machine over raw byte stream |
| **POSIX sockets** | `server.cpp` — `socket()`, `bind()`, `listen()`, `accept()`, `recv()`, `send()` |
| **Thread pool / producer-consumer** | `thread_pool.cpp` — `condition_variable` + `mutex` + task queue |
| **Reader-writer lock** | `memory_backend.cpp` — `shared_mutex` for concurrent reads |
| **Token Bucket algorithm** | `token_bucket.cpp` — burst capacity + steady-state refill |
| **Sliding Window Log algorithm** | `sliding_window.cpp` — timestamp log with expiration |
| **RESP protocol client** | `redis_client.cpp` — serialize/deserialize Redis wire protocol |
| **Atomic counters** | `stats.cpp` — lock-free request counting with `std::atomic` |
| **Distributed state coordination** | `redis_backend.cpp` — shared state across multiple processes via Redis |
| **Graceful shutdown** | `main.cpp` — `SIGINT`/`SIGTERM` handling, thread pool drain |

---

## Future Extensions

- [ ] **Redis Lua scripts** for atomic token bucket operations (compare-and-swap in one round trip)
- [ ] **Sliding Window Counter** algorithm (approximate, memory-efficient hybrid)
- [ ] **Leaky Bucket** algorithm (smooth output rate, queue bursts)
- [ ] **Connection pooling** for Redis (reuse TCP connections across requests)
- [ ] **Prometheus metrics endpoint** (`/metrics` in exposition format)
- [ ] **Config file support** (YAML/JSON config file loaded at startup)
- [ ] **IP-based rate limiting** (extract client IP from socket or `X-Forwarded-For`)
- [ ] **TLS support** (OpenSSL integration for HTTPS)

---

*Built from scratch as a systems programming portfolio project — no frameworks, no external dependencies.*
