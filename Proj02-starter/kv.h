/*
 * kv.h -- Mini-KV server: shared declarations
 *
 * Project 2, CprE 3080, Spring 2026
 *
 * You may modify this file. It is provided as a starting point, not a rigid
 * interface. If your design benefits from additional fields or types, add them.
 */
#ifndef KV_H
#define KV_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <stdlib.h>
#include <signal.h>

/* -------- Protocol constants (do NOT change) ---------------------------- */

#define MAX_KEY_LEN    256
#define MAX_VAL_LEN    256
#define MAX_LINE_LEN   (MAX_KEY_LEN + MAX_VAL_LEN + 64)  /* + command + ttl */

/* Response strings. Each response is one line ending in '\n'. */
#define RESP_OK_PUT     "You got it dude, its added\n"
#define RESP_OK_DEL    "That Entry has been nuked\n"
#define RESP_BYE       "BYE Felicia\n"
#define RESP_NOTFOUND  "NOT_FOUND\n"
#define RESP_ERROR      "Unknown Command, try again\n"
#define MAX_HASHMAP_ENTRIES 100


/* -------- Your types go here -------------------------------------------- */
enum commands {
    GET,
    GETTTL,    
    PUT,
    DEL,
    STATS,
    QUIT,
    UNKNOWN    
};

typedef struct entry{
    char key[MAX_KEY_LEN + 1];
    char value[MAX_VAL_LEN + 1];
    struct entry* next;
    //int lifespan; // If lifespan = -1, never expires
    time_t deathtime;
}entry;

typedef struct{
    int capacity;
    entry **arr;
    pthread_rwlock_t *bucket_locks;
    atomic_int  hits, misses, puts, dels;
} HashMap;


typedef struct {
    HashMap              *map;
    int                   interval_ms;
    volatile sig_atomic_t *shutdown;
} SweeperArgs;

extern int bucketNum;
extern HashMap map;

/*
 * TODO (Stage 1): Define your hash-table entry and bucket types.
 *
 * TODO (Stage 2): Define your work-queue type (bounded FIFO of int fds).
 *
 * TODO (Stage 3): Add an rwlock to your table type.
 *
 * TODO (Stage 4): Add expiration timestamp to entries; declare the sweeper
 *                 thread function.
 */

/* -------- Function prototypes you will likely want ---------------------- */

/* Protocol / connection handling (Stage 1) */
void init_hashmap(HashMap *mp);
void destroy_hashmap(HashMap *mp);
int handle_client(int conn_fd);        /* loop: read line, parse, reply */
enum commands parse_line(char *line, char **key, char **value, int *ttl);
void setEntry(entry* e, const char* key, const char* value, int ttl);
entry* find_entry(HashMap *mp, char *key);
unsigned long hashFunction(const char* key);
static inline int bucket_of(HashMap *mp, const char *key);
void insert(HashMap* mp, char* key, char* value, int ttl);
char* search(HashMap* mp, char* key);
void delete_key(HashMap* mp, char* key);
/* Hash-table operations (Stage 1, made thread-safe in Stage 3) */
/*   Return 0 on success, -1 on not-found / error. */
/*   You design the full signatures -- these are just suggestions. */
/* int  kv_get(const char *key, char *out_val, size_t out_cap); */
/* int  kv_put(const char *key, const char *val, int ttl_seconds); */
/* int  kv_del(const char *key); */

#endif /* KV_H */
