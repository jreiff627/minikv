# Viva Prep — Mini-KV Project 2

These are the questions most likely to come up in your 10-minute slot,
organized by topic. For each one: know the one-sentence answer cold,
then be ready to expand with the detail below it.

---

## The Work Queue (Stage 2)

**Q: Walk me through what happens when a client connects.**

Main thread returns from `accept()` with a new `conn` fd. It calls
`queue_push(&work_queue, conn)`, which locks the queue mutex, waits on
`not_full` if the queue is at capacity, writes the fd into
`buf[tail]`, increments `tail` and `count`, signals `not_empty`, then
unlocks. A worker blocked in `queue_pop` wakes up, grabs the fd, and
calls `handle_client(fd)`. When `handle_client` returns, the worker
closes the fd and loops back to `queue_pop`.

**Q: Why two condition variables instead of one?**

One CV for each direction of blocking: `not_empty` (workers wait here
when the queue is empty), `not_full` (main thread waits here when the
queue is full). If you used one CV, a signal meant to wake a worker
might wake the main thread instead, and vice versa — you'd need a
`broadcast` every time, which is wasteful and still technically
incorrect if you're not careful about the predicate.

**Q: What is the queue capacity and why does it matter?**

`QUEUE_CAPACITY` is 16 (defined in `queue.h`). This bounds memory
usage and applies backpressure: if all workers are busy and 16
connections are already queued, the main thread blocks in `queue_push`
rather than accepting more connections. This prevents the server from
accumulating unbounded file descriptors under overload.

**Q: How do you shut down cleanly?**

After the accept loop exits (on SIGINT), we push one `-1` sentinel per
worker into the queue. Each worker's `queue_pop` returns -1, the
worker checks `if (fd == -1) break` and exits its loop, then returns.
Main thread joins all workers, joins the sweeper, frees memory, closes
the listen fd, and calls `destroy_hashmap`.

---

## The Hash Table & Locking (Stage 3)

**Q: Why per-bucket locks instead of one table lock?**

The sweeper. With a single table-wide write lock, the sweeper would
freeze every GET, PUT, and DEL for the entire duration of a full scan.
Per-bucket locking lets the sweeper hold a write lock on one bucket at
a time; workers continue serving all other buckets in parallel. The
only time a worker stalls is if it happens to touch the exact bucket
the sweeper is currently inside.

**Q: Where is the lock acquired for a GET?**

Inside `search()`, not in `handle_client`. `search` computes the
bucket index, calls `pthread_rwlock_rdlock(&mp->bucket_locks[idx])`,
walks the chain, copies the result pointer, then
`pthread_rwlock_unlock`. `handle_client` never touches a lock
directly. This encapsulation means the locking is always correct
regardless of who calls the function.

**Q: Can two GETs run at the same time?**

Yes — if they hash to different buckets, they acquire different
read locks and run completely in parallel. If they hash to the same
bucket, they both acquire that bucket's read lock in read mode, which
`pthread_rwlock_rdlock` allows multiple holders simultaneously. Two
GETs on the same key run concurrently with no waiting.

**Q: Can a GET and a PUT run at the same time?**

Only if they hash to different buckets. If they hit the same bucket,
`insert` holds a write lock on that bucket, which excludes the GET's
read lock. The GET blocks until the write is complete, then proceeds
and sees the new value (or not, if a different key was written). This
is correct — there is no window where a GET can see a partially-written
entry.

**Q: What happens if the same key is PUT twice?**

`insert` always prepends to the bucket chain without checking for an
existing entry. The new entry goes at the head; `search` finds it
first and returns its value. The old entry is shadowed — it still
exists in the chain but is never returned. It will leak unless DEL is
called or the entry had a TTL (the sweeper frees it). For this project
that tradeoff is acceptable; the fix would be to walk the chain in
`insert` and update in place if the key exists.

---

## TTL and the Sweeper (Stage 4)

**Q: How does TTL work?**

`PUT key value 5` calls `insert` with `ttl=5`. Inside `setEntry`,
`deathtime = time(NULL) + 5`. On any subsequent `GET`, `search` reads
`time(NULL)` and checks `curr->deathtime != 0 && time(NULL) > curr->deathtime` — if expired, returns NULL (NOT FOUND) even if the
entry is still in the chain. The sweeper physically removes it on its
next pass.

**Q: Walk me through the sweeper loop.**

```
while (!shutdown):
    nanosleep(interval_ms)
    now = time(NULL)
    for i in 0..capacity:
        wrlock(bucket_locks[i])
        walk chain, free any entry where deathtime != 0 && now > deathtime
        unlock(bucket_locks[i])
```
One bucket at a time, write-locked only for the duration of that
bucket's scan. Between bucket unlocks, all workers are free to run.

**Q: Can a GET return an expired value?**

Technically yes — `search` reads `time(NULL)` inside the read lock,
but there is a tiny window between when the time check passes and when
`curr->value` is returned. In practice this cannot happen: the check
and the return are in the same critical section under the read lock.
What *can* happen is a GET that starts a fraction of a millisecond
before the TTL elapses returns the value, while a GET a fraction later
returns NOT FOUND. This is correct behaviour — TTL is defined as
"expires no later than deathtime", and the spec explicitly allows lazy
expiry on the GET path.

**Q: Could the sweeper and a GET race on the same entry?**

No. The sweeper holds the write lock on a bucket while modifying its
chain. A concurrent GET trying to read that bucket blocks at
`pthread_rwlock_rdlock` until the sweeper releases the lock. When the
GET proceeds, the deleted entry is already removed — there is no way
to observe a partially-unlinked or freed entry.

**Q: How does the sweeper shut down?**

`g_shutdown` is a `volatile sig_atomic_t`. The sweeper checks it at
the top of each loop iteration, after `nanosleep` returns. When
`g_shutdown` is set by the signal handler, the sweeper exits on its
next wakeup. Main thread then `pthread_join`s it before freeing the
hash table, so there is no use-after-free.

---

## STATS (Stage 4)

**Q: How are hit/miss/put/del counts kept thread-safe?**

They are `atomic_int` fields on the `HashMap` struct, updated with
`atomic_fetch_add`. No mutex needed — C11 atomics guarantee
indivisible read-modify-write. `atomic_load` is used when reading
them for the STATS response.

**Q: How does STATS count live keys without a table-wide lock?**

It iterates all buckets, read-locking each one briefly to count
non-expired entries, then unlocking before moving to the next. This is
an instantaneous snapshot — the count may be slightly stale by the
time the response is sent, which is acceptable for a stats command.

---

## Things to have ready for the demo

1. Start server: `./kvserver 9000 8 1024 500`
2. Manual session showing GET, PUT, PUT with TTL, DEL, STATS, QUIT via nc
3. Show TTL expiry: `PUT x hello 3`, wait 4 seconds, `GET x` → NOT FOUND
4. Show concurrent clients: run `./bench_client localhost 9000 16 1000 90` 
    while the server is live; show STATS before and after
5. Show clean shutdown: Ctrl-C the server; confirm "clean exit" prints