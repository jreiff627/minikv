/*
 * bench_client.c -- Mini-KV concurrent benchmark client
 *
 * Project 2, CprE 3080, Spring 2026
 *
 * Usage:
 *   ./bench_client <host> <port> <num_clients> <ops_per_client> <read_pct>
 *
 * Requirements (from the spec):
 *   - Spawn <num_clients> threads.
 *   - Each thread opens its own TCP connection to <host>:<port>.
 *   - Each thread issues <ops_per_client> operations.
 *   - <read_pct> percent of ops are GETs; the rest are PUTs.
 *   - Keys drawn from a small pool (~1000 keys) so GETs actually hit.
 *   - Report total wall-clock time and overall throughput (ops/sec).
 */

//  chmod +x run_bench.sh
// ./run_bench.sh         # uses defaults: port 9000, 8 workers, 1024 buckets
// ./run_bench.sh 9000 4 512   # custom workers/buckets
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define KEY_POOL_SIZE 1000

/* ── Per-thread arguments / results ──────────────────────── */
typedef struct {
    const char *host;
    int         port;
    int         ops_per_client;
    int         read_pct;
    long        completed;   /* filled in after thread finishes */
    int         errors;
} ThreadArgs;

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s <host> <port> <num_clients> <ops_per_client> <read_pct>\n",
        prog);
}

/* ── Connect helper ──────────────────────────────────────── */
static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints = {0}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* ── Thread body ─────────────────────────────────────────── *
 *
 * Each thread:
 *   1. Opens its own TCP connection.
 *   2. Issues ops_per_client operations (GETs or PUTs based on read_pct).
 *   3. Keys are drawn from a pool of KEY_POOL_SIZE so GETs actually hit.
 *   4. Uses rand_r() with a per-thread seed -- avoids serialization on
 *      the global rand() lock that would skew benchmark results.
 *   5. Reads the server reply line-by-line via fgets; does not assume
 *      one TCP packet per command.
 */
static void *bench_thread(void *arg)
{
    ThreadArgs *a = (ThreadArgs *)arg;

    int fd = tcp_connect(a->host, a->port);
    if (fd < 0) {
        perror("connect");
        a->errors++;
        return NULL;
    }

    /* Wrap a FILE* around a dup so fgets can do its own buffering
     * without closing the original fd out from under us. */
    FILE *rx = fdopen(dup(fd), "r");
    if (!rx) { close(fd); a->errors++; return NULL; }

    /* Per-thread seed: mix thread pointer with current time so every
     * thread gets an independent RNG stream. */
    unsigned int seed = (unsigned int)((size_t)arg ^ (unsigned int)time(NULL));

    char req[256], resp[256];

    for (int i = 0; i < a->ops_per_client; i++) {
        int key_id  = (int)((unsigned int)rand_r(&seed) % KEY_POOL_SIZE);
        int is_read = ((int)((unsigned int)rand_r(&seed) % 100)) < a->read_pct;
        int len;

        if (is_read) {
            len = snprintf(req, sizeof(req), "GET key%d\n", key_id);
        } else {
            len = snprintf(req, sizeof(req), "PUT key%d val%d\n", key_id, key_id);
        }

        if (write(fd, req, len) < 0) { a->errors++; break; }

        /* Read the reply line-by-line -- do NOT assume one packet per op. */
        if (!fgets(resp, sizeof(resp), rx)) { a->errors++; break; }

        a->completed++;
    }

    /* Clean protocol close. */
    write(fd, "QUIT\n", 5);
    fgets(resp, sizeof(resp), rx);   /* consume BYE */

    fclose(rx);
    close(fd);
    return NULL;
}

/* ── Main ─────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    if (argc != 6) {
        usage(argv[0]);
        return 1;
    }

    const char *host           = argv[1];
    int         port           = atoi(argv[2]);
    int         num_clients    = atoi(argv[3]);
    int         ops_per_client = atoi(argv[4]);
    int         read_pct       = atoi(argv[5]);

    if (port <= 0 || num_clients < 1 || ops_per_client < 1 ||
        read_pct < 0 || read_pct > 100) {
        usage(argv[0]);
        return 1;
    }

    pthread_t  *tids = malloc(num_clients * sizeof(pthread_t));
    ThreadArgs *args = malloc(num_clients * sizeof(ThreadArgs));
    if (!tids || !args) { perror("malloc"); return 1; }

    /* -- Seed the server with the full key pool so GETs hit from op 1.
     *    Write all PUTs in one shot, then drain all OK responses.
     *    Batching like this avoids 1000 sequential round-trips. -- */
    {
        int fd = tcp_connect(host, port);
        if (fd < 0) { perror("seed connect"); return 1; }

        FILE *f = fdopen(fd, "r+");
        if (!f) { perror("fdopen seed"); close(fd); return 1; }

        char buf[64];
        for (int i = 0; i < KEY_POOL_SIZE; i++) {
            fprintf(f, "PUT key%d val%d\n", i, i);
        }
        fflush(f);

        /* drain all KEY_POOL_SIZE "OK\n" responses */
        for (int i = 0; i < KEY_POOL_SIZE; i++) {
            if (!fgets(buf, sizeof(buf), f)) break;
        }

        fprintf(f, "QUIT\n");
        fflush(f);
        fgets(buf, sizeof(buf), f);  /* consume BYE */
        fclose(f);
    }

    /* -- Spawn all threads, then start the clock -- */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < num_clients; i++) {
        args[i] = (ThreadArgs){
            .host           = host,
            .port           = port,
            .ops_per_client = ops_per_client,
            .read_pct       = read_pct,
            .completed      = 0,
            .errors         = 0,
        };
        pthread_create(&tids[i], NULL, bench_thread, &args[i]);
    }

    /* -- Join all threads, then stop the clock -- */
    for (int i = 0; i < num_clients; i++)
        pthread_join(tids[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    /* -- Compute and print results -- */
    double elapsed = (t1.tv_sec  - t0.tv_sec) +
                     (t1.tv_nsec - t0.tv_nsec) / 1e9;

    long total_ops = 0, total_err = 0;
    for (int i = 0; i < num_clients; i++) {
        total_ops += args[i].completed;
        total_err += args[i].errors;
    }

    double throughput = total_ops / elapsed;

    printf("clients=%d  ops/client=%d  read_pct=%d%%\n",
           num_clients, ops_per_client, read_pct);
    printf("total_ops=%ld  errors=%ld  wall=%.2fs  throughput=%.0f\n",
           total_ops, total_err, elapsed, throughput);

    free(tids);
    free(args);
    return 0;
}