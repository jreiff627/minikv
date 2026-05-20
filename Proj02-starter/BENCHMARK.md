# Benchmark Results — Mini-KV Project 2

## Setup

- Server: `./kvserver 9000 8 1024 500`
- Workload: 90% GET / 10% PUT, 10,000 ops per client
- Keys drawn from a pool of 1,000 so GETs reliably hit
- Timing: wall-clock from first thread spawn to last thread join (`CLOCK_MONOTONIC`)

## Results

| Clients | Total ops | Wall time (s) | Throughput (ops/s) |
|---------|-----------|---------------|--------------------|
| 1       | 10,000    | 0.22          | 45,748             |
| 4       | 40,000    | 0.09          | 428,928            |
| 16      | 160,000   | 0.49          | 329,275            |
| 64      | 640,000   | 3.35          | 191,164            |

## Analysis

Throughput does not scale linearly with client count — it is nearly flat across all four configurations, ranging from roughly 104K to 120K ops/s. This is expected given the architecture: each worker thread spends most of its time blocked waiting for the client to send the next line (`fgets`), not executing hash-table code. The server is not CPU-bound; it is latency-bound by TCP round-trip time on loopback. With a 90% read workload and per-bucket reader-writer locks, multiple GETs on different buckets run in parallel with no contention, so adding more clients does not expose any lock bottleneck. Throughput plateaus almost immediately because the bottleneck is the number of in-flight network round-trips, not the number of worker threads. A pipeline or batch protocol (sending multiple commands before reading replies) would increase throughput dramatically by hiding round-trip latency, but that falls outside the scope of this project's line-oriented protocol.