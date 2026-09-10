#!/usr/bin/env python3
"""Benchmark suite for the Distributed Rate Limiter"""

import socket
import threading
import time
import sys
import statistics

DEFAULT_HOST = "localhost"
DEFAULT_PORT = 8090

def http_check(host, port, api_key):
    """Send POST /api/check, return (status_code, elapsed_us)"""
    start = time.perf_counter()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect((host, port))
    body = ""
    req = (f"POST /api/check HTTP/1.1\r\n"
           f"Host: {host}\r\n"
           f"X-API-Key: {api_key}\r\n"
           f"Content-Length: 0\r\n"
           f"Connection: close\r\n\r\n")
    s.sendall(req.encode())
    resp = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        resp += chunk
    s.close()
    elapsed = (time.perf_counter() - start) * 1_000_000
    status = -1
    try:
        status = int(resp.split(b" ")[1][:3])
    except Exception:
        pass
    return status, elapsed

def benchmark_throughput(host, port, num_threads, reqs_per_thread):
    """Multi-threaded throughput: each thread uses a unique key with a high limit."""
    results = [0] * num_threads
    barrier = threading.Barrier(num_threads + 1)

    def worker(tid):
        barrier.wait()
        start = time.perf_counter()
        for i in range(reqs_per_thread):
            http_check(host, port, f"bench_thread_{tid}")
        elapsed = time.perf_counter() - start
        results[tid] = reqs_per_thread / elapsed

    threads = [threading.Thread(target=worker, args=(t,)) for t in range(num_threads)]
    for t in threads:
        t.start()
    barrier.wait()
    for t in threads:
        t.join()
    return sum(results)

def benchmark_latency(host, port, num_reqs):
    """Single-threaded latency measurement."""
    latencies = []
    for i in range(num_reqs):
        _, elapsed = http_check(host, port, "latency_bench")
        latencies.append(elapsed)
    return latencies

def main():
    host = DEFAULT_HOST
    port = DEFAULT_PORT
    if len(sys.argv) > 1:
        port = int(sys.argv[1])

    print("=" * 65)
    print("  Distributed Rate Limiter - Benchmark Suite")
    print("=" * 65)
    print(f"  Server: {host}:{port}")
    print()

    # Verify connection
    try:
        status, _ = http_check(host, port, "ping")
        if status < 0:
            raise Exception("no response")
    except Exception:
        print(f"ERROR: Cannot connect to server at {host}:{port}")
        print(f"Start server first: ./build/rate_limiter -p {port} -m 1000000")
        sys.exit(1)

    print("--- Throughput (POST /api/check) ---")
    print("  NOTE: run server with a high limit (-m 1000000) to avoid 429s skewing results")
    print()
    for nthreads in [1, 4, 8, 16]:
        rps = benchmark_throughput(host, port, nthreads, 500)
        print(f"  {nthreads:>2} threads x 500 reqs:   {rps:>10,.0f} req/sec")

    print()
    print("--- Latency (single-threaded, 500 reqs) ---")
    print()
    lat = benchmark_latency(host, port, 500)
    lat.sort()
    print(f"    p50: {statistics.median(lat):>8.0f} us")
    print(f"    p95: {lat[int(len(lat)*0.95)]:>8.0f} us")
    print(f"    p99: {lat[int(len(lat)*0.99)]:>8.0f} us")
    print(f"    avg: {statistics.mean(lat):>8.0f} us")

    print()
    print("=" * 65)
    print("  Benchmark complete.")
    print("=" * 65)

if __name__ == "__main__":
    main()
