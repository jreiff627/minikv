/*
 * kvserver.c -- Mini-KV server entry point
 *
 * Project 2, CprE 3080, Spring 2026
 *
 * Starter scaffolding: this file gives you a working TCP listener and an
 * argument parser. Everything else -- accept loop, protocol, hash table,
 * thread pool, RW locking, TTL sweeper -- is yours to write.
 *
 * Build: run `make` in this directory. See the provided Makefile.
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "kv.h"
#include "queue.h"
#include <pthread.h>

/* -------- Globals ------------------------------------------------------- */
static WorkQueue work_queue;
static pthread_t *workers;
static pthread_t janitor;
static int num_workers_global;

static volatile sig_atomic_t g_shutdown = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/* -------- Socket helpers ------------------------------------------------ */

/* Create a listening TCP socket bound to the given port. Returns fd or -1. */
static int make_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 64) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

/* -------- Entry point --------------------------------------------------- */

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s <port> <num_workers> <num_buckets> [sweeper_interval_ms]\n"
        "   port                TCP port to listen on (1-65535)\n"
        "   num_workers         number of worker threads (>=1)\n"
        "   num_buckets         hash-table bucket count (>=1)\n"
        "   sweeper_interval_ms default 500\n",
        prog);
}


void *worker_fn(void *arg) {
    WorkQueue *q = (WorkQueue *)arg;
    while (1) {
        int fd = queue_pop(q);   // blocks here until work arrives
        if (fd == -1) break;     // -1 is the shutdown signal
        handle_client(fd);
        close(fd);
    }
    return NULL;
}

static void *sweeper_fn(void *arg)
{
    SweeperArgs *a   = (SweeperArgs *)arg;
    HashMap     *mp  = a->map;
    long         ns  = (long)a->interval_ms * 1000000L;
 
    struct timespec sleep_ts = {
        .tv_sec  = ns / 1000000000L,
        .tv_nsec = ns % 1000000000L
    };
 
    while (!*(a->shutdown)) {
        /* sleep first — nothing is stale on the very first tick */
        nanosleep(&sleep_ts, NULL);
 
        if (*(a->shutdown)) break;
 
        time_t now = time(NULL);
 
        for (int i = 0; i < mp->capacity; i++) {
 
            /* --- lock only this bucket, sweep it, unlock --- */
            pthread_rwlock_wrlock(&mp->bucket_locks[i]);
 
            entry *prev = NULL;
            entry *curr = mp->arr[i];
 
            while (curr) {
                if (curr->deathtime != 0 && now > curr->deathtime) {
                    /* unlink the dead entry */
                    entry *dead = curr;
                    if (prev) prev->next   = curr->next;
                    else      mp->arr[i]   = curr->next;
                    curr = curr->next;
                    free(dead);
                } else {
                    prev = curr;
                    curr = curr->next;
                }
            }
 
            pthread_rwlock_unlock(&mp->bucket_locks[i]);
            /* ← all other buckets remain accessible to workers here */
        }
    }
 
    return NULL;
}




int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        usage(argv[0]);
        return 1;
    }
    int port         = atoi(argv[1]);
    int num_workers  = atoi(argv[2]);
    int num_buckets  = atoi(argv[3]);
    int sweeper_ms   = (argc == 5) ? atoi(argv[4]) : 500;

    if (port <= 0 || port > 65535 || num_workers < 1 ||
        num_buckets < 1 || sweeper_ms <= 0) {
        usage(argv[0]);
        return 1;
    }
    bucketNum = num_buckets;
    init_hashmap(&map);

    /* Install Ctrl-C handler for clean shutdown. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Ignore SIGPIPE: writes to closed sockets should fail with EPIPE, not
     * kill the server. */
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = make_listen_socket(port);
    if (listen_fd < 0) return 1;

    fprintf(stderr,
        "kvserver: listening on port %d "
        "(workers=%d, buckets=%d, sweeper=%dms)\n",
        port, num_workers, num_buckets, sweeper_ms);

    /* ================================================================
     * TODO (Stage 1): Sequential accept loop.
     *   while (!g_shutdown) {
     *       int conn = accept(listen_fd, NULL, NULL);
     *       if (conn < 0) { ...handle EINTR on signal, else perror... }
     *       handle_client(conn);
     *       close(conn);
     *   }
     *
     * TODO (Stage 2): Initialize work queue + spawn worker threads.
     *                 The accept loop now enqueues conn fds instead of
     *                 calling handle_client directly.
     *
     * TODO (Stage 3): Initialize the hash table's rwlock before the accept
     *                 loop starts.
     *
     * TODO (Stage 4): Spawn the sweeper thread; join it on shutdown.
     *
     * TODO (shutdown): drain queue, join all threads, free everything.
     * ================================================================ */

    //================================================================
    //                         STAGE 1
    //================================================================
    queue_init(&work_queue);

    num_workers_global = num_workers;
    workers = malloc(num_workers * sizeof(pthread_t));

    for (int i = 0; i < num_workers; i++){
        pthread_create(&workers[i], NULL, worker_fn, &work_queue);
    }

    SweeperArgs sweeper_args = {
        .map         = &map,
        .interval_ms = sweeper_ms,
        .shutdown    = &g_shutdown,
    };

    pthread_create(&janitor, NULL, sweeper_fn, &sweeper_args);


    while (!g_shutdown) {
            int conn = accept(listen_fd, NULL, NULL);

            if (conn < 0) 
            { 
                if(errno == EINTR){
                    //signal interruption
                    continue;
                }

                perror("Accept"); 
                break;
            }

            //printf("Handling it Chief\n");
            queue_push(&work_queue, conn);

    }



    for (int i = 0; i < num_workers; i++){
        queue_push(&work_queue, -1);
    }

    // Wait for all workers to finish
    for (int i = 0; i < num_workers; i++){
        pthread_join(workers[i], NULL);
    }

    free(workers);
    close(listen_fd);
    return 0;
}
