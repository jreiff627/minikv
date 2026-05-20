# Mini-KV — Multi-Threaded In-Memory Key-Value Store

A concurrent TCP key-value store server built in C for CprE 3080 (Operating Systems).
Inspired by systems like Redis and Memcached, Mini-KV composes core OS primitives:
thread pools, bounded queues, reader-writer locks, and condition variables; into a
working network service.

---

## Build & Run

```bash
make                          # builds kvserver and bench_client
make clean                    # removes binaries and object files
make bench                    # builds bench_client only

./kvserver <port> <num_workers> <num_buckets> [sweeper_interval_ms]

# Example
./kvserver 9000 8 1024 500
```

**Requirements:** Linux, GCC, POSIX threads. Tested on Iowa State Coover 2061 lab workstations.

---

## Protocol

Clients connect via TCP and send line-delimited text commands (`\n` terminated).

| Command | Description | Response |
|---|---|---|
| `GET <key>` | Retrieve value | `VALUE <value>` or `NOT_FOUND` |
| `PUT <key> <value> [ttl]` | Store key, optional TTL in seconds | `OK` |
| `DEL <key>` | Delete key | `OK` or `NOT_FOUND` |
| `GETTTL <key>` | Return remaining TTL in seconds | `TTL <seconds>` or `NOT_FOUND` |
| `STATS` | Server statistics | See below |
| `QUIT` | Close connection | `BYE` |

**All commands are case-insensitive** — `get`, `GET`, and `Get` are all valid.

`STATS` returns a single line:
```
STATS keys=<n> hits=<n> misses=<n> puts=<n> dels=<n> active_conns=<n> uptime_s=<n>
```

### Example session

```
PUT color red         -> OK
PUT temp 72 5         -> OK  (expires in 5 seconds)
GET color             -> VALUE red
GETTTL temp           -> TTL 3
get missing           -> NOT_FOUND
STATS                 -> STATS keys=2 hits=1 misses=1 puts=2 dels=0 active_conns=1 uptime_s=8
QUIT                  -> BYE
```

---

## Stage Status

| Stage | Description | Status |
|---|---|---|
| 1 | Sequential TCP server, hash table, full protocol | ✅ Complete |
| 2 | Thread pool, bounded work queue, producer-consumer sync | ✅ Complete |
| 3 | Reader-writer locking, concurrent hash table access | ✅ Complete |
| 4 | TTL sweeper thread, STATS command, atomic counters | ✅ Complete |
| Bonus | Persistence / write-ahead log | — |

No known bugs or limitations.

---

## Design Decisions

### 1. Lock granularity — per-bucket reader-writer locks

Rather than a single table-wide `pthread_rwlock_t`, Mini-KV uses one `pthread_rwlock_t`
per hash bucket. GET acquires only the lock for the target bucket in read mode; PUT and
DEL acquire only the target bucket's lock in write mode. This means concurrent GETs on
*different* keys never block each other even if they land in different buckets, and a
PUT to bucket 7 does not stall a GET on bucket 42.

The tradeoff is memory and initialization overhead: 1,024 locks instead of one. In
practice this is negligible (~40 KB). The benchmark results support the choice — under
a 90% read workload at 4 concurrent clients, throughput reached ~429K ops/s with no
measurable lock contention. The bottleneck was TCP round-trip latency, not the hash
table. A single table-wide lock would have serialized all writes against all reads and
degraded write throughput at higher client counts.

### 2. Worker pool sizing

Benchmarked at 1, 4, 16, and 64 concurrent clients with 8 workers and 10,000 ops per
client (90% GET / 10% PUT):

| Clients | Throughput (ops/s) |
|---|---|
| 1 | 45,748 |
| 4 | 428,928 |
| 16 | 329,275 |
| 64 | 191,164 |

Throughput peaks at 4 clients and then declines. The server is latency-bound, not
CPU-bound: each worker spends most of its time blocked on `fgets` waiting for the next
line over TCP, not executing hash-table code. Adding more clients beyond the worker
count increases queue depth and context-switching overhead without adding useful
parallelism. 8 workers matched the test machine's core count; beyond that, adding
workers stopped helping. A batch or pipelined protocol would expose more CPU-side
throughput by hiding round-trip latency, but falls outside the scope of this project's
line-oriented protocol.

### 3. Sweeper coordination

The sweeper thread wakes on a configurable interval (`sweeper_interval_ms`, default
500 ms) and scans the hash table for expired entries. It acquires the write lock
**per bucket** : locking, scanning, removing expired keys, and unlocking before moving
to the next bucket. This keeps the write-lock window as small as possible: workers
serving other buckets are never blocked during the sweep.

Holding the write lock across the entire sweep pass would freeze all GET/PUT/DEL
operations for the full duration of the scan: unacceptable under load with 1,024
buckets. The per-bucket approach means a GET in flight on bucket 5 is only blocked
if the sweeper is currently processing bucket 5, and only for the time it takes to
scan that one bucket's chain. Races where a GET observes a key the sweeper is
deleting are prevented because the sweeper holds the write lock for that bucket before
touching any entry.

---

## Extra Features

- **`GETTTL <key>`** : returns the remaining TTL in seconds for any key with an active
  expiration. Useful for debugging TTL behavior without waiting for the sweeper.
- **Case-insensitive commands** : `get`, `GET`, `Get` are all accepted. The parser
  normalizes the command token to uppercase before dispatch.

---

## Running the Benchmark

```bash
make bench
./bench_client <host> <port> <num_clients> <ops_per_client> <read_pct>

# Example: 16 clients, 10000 ops each, 90% reads
./bench_client localhost 9000 16 10000 90
```

Reports total wall-clock time and overall throughput in ops/s.

---

## Memory Safety

Validated with Valgrind — no leaks on clean shutdown (SIGINT):

```bash
valgrind --leak-check=full --show-leak-kinds=all ./kvserver 9000 4 128
```
