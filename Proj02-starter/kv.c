#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "kv.h"


extern HashMap map = {0};
int bucketNum = 0;

void init_hashmap(HashMap *mp)
{
    mp->arr = calloc(bucketNum, sizeof(entry *));
    if (!mp->arr) { perror("calloc arr"); exit(EXIT_FAILURE); }
 
    mp->bucket_locks = malloc(bucketNum * sizeof(pthread_rwlock_t));
    if (!mp->bucket_locks) { perror("malloc locks"); exit(EXIT_FAILURE); }
 
    for (int i = 0; i < bucketNum; i++)
        pthread_rwlock_init(&mp->bucket_locks[i], NULL);
 
    mp->capacity = bucketNum;
 
    atomic_init(&mp->hits,   0);
    atomic_init(&mp->misses, 0);
    atomic_init(&mp->puts,   0);
    atomic_init(&mp->dels,   0);

}

void destroy_hashmap(HashMap *mp)
{
    for (int i = 0; i < mp->capacity; i++) {
        pthread_rwlock_wrlock(&mp->bucket_locks[i]);
        entry *curr = mp->arr[i];
        while (curr) {
            entry *next = curr->next;
            free(curr);
            curr = next;
        }
        mp->arr[i] = NULL;
        pthread_rwlock_unlock(&mp->bucket_locks[i]);
        pthread_rwlock_destroy(&mp->bucket_locks[i]);
    }
    free(mp->arr);
    free(mp->bucket_locks);
    mp->arr          = NULL;
    mp->bucket_locks = NULL;
}


int handle_client(int conn_fd){ 
    char line[MAX_LINE_LEN];
    FILE *stream = fdopen(conn_fd, "r");
    char *key, *value;
    char buffer[128];
    int ttl, len;

    //printf("Trying to Parse Line\n");
    while (fgets(line, sizeof(line), stream) != NULL) {
        // Process line
        enum commands cmd = parse_line(line, &key, &value, &ttl);

        switch (cmd) {
            case QUIT:
                //printf("Received QUIT\n");
                write(conn_fd, RESP_BYE, sizeof(RESP_BYE));
                fclose(stream);
                return 0;

            case GET: {
                //printf("Received GET");
                char *result = search(&map, key);
                if (result){
                    len = snprintf(buffer, sizeof(buffer), "VALUE %s\n", result);
                    write(conn_fd, buffer, len);
                }
                else{
                    write(conn_fd,  RESP_NOTFOUND, sizeof(RESP_NOTFOUND));
                }
                break;
            }
            case GETTTL: {
                //printf("Received GET");
                entry *result = find_entry(&map, key);
                if (!result){
                    write(conn_fd, RESP_NOTFOUND, sizeof(RESP_NOTFOUND));
                }
                else{
                    if (result->deathtime == 0 || result->deathtime == -1 ) {
                        len = snprintf(buffer, sizeof(buffer), "I WILL NEVER DIE\n");
                    } else {
                        int remaining = (int)(result->deathtime - time(NULL));
                        if (remaining < 0) remaining = -1; //FIXME get rid of once the sweeper is implemented
                        len = snprintf(buffer, sizeof(buffer), "TTL %d\n", remaining);
                    }

                    write(conn_fd, buffer, len);
                }
                break;
            }
            case PUT:
                //printf("Received PUT: ");
                if (!key || !value) {
                    printf("More information please!! \n");
                    break;
                }
                insert(&map, key, value, ttl);
                write(conn_fd,  RESP_OK_PUT, sizeof(RESP_OK_PUT));
                break;

            case DEL:
                printf("IM TRYING REALLY HARD TO DELETE\n");
                if (!key) {
                    printf("ERROR missing key\n");
                    break;
                }
                delete_key(&map, key);
                write(conn_fd,  RESP_OK_DEL, sizeof(RESP_OK_DEL));
                break;

            case STATS: {
                int live_keys = 0;
                time_t now = time(NULL);
                for (int i = 0; i < map.capacity; i++) {
                    pthread_rwlock_rdlock(&map.bucket_locks[i]);
                    for (entry *e = map.arr[i]; e; e = e->next)
                        if (e->deathtime == 0 || now <= e->deathtime)
                            live_keys++;
                    pthread_rwlock_unlock(&map.bucket_locks[i]);
                }

                len = snprintf(buffer, sizeof(buffer),
                    "STATS keys=%d hits=%d misses=%d puts=%d dels=%d\n", live_keys, atomic_load(&map.hits),
                    atomic_load(&map.misses), atomic_load(&map.puts), atomic_load(&map.dels));
                write(conn_fd, buffer, len);
                break;
            }
            default:
                write(conn_fd, RESP_ERROR, strlen(RESP_ERROR));
                break;
        }
    } 
    fclose(stream);
    return 0;         
}

enum commands parse_line(char *line, char **key, char **value, int *ttl)
{
    *key = NULL;
    *value = NULL;
    *ttl = -1;

    int len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1]))
        line[--len] = '\0';

    if (len == 0) return UNKNOWN;

    char *cmd = strtok(line, " ");
    if (!cmd) return UNKNOWN;

    if (strcmp(cmd, "QUIT") == 0) return QUIT;

    if (strcmp(cmd, "GET") == 0 || strcmp(cmd, "get") == 0 || strcmp(cmd, "Get") == 0) {
        *key = strtok(NULL, " ");
        return *key ? GET : UNKNOWN;
    }

     if (strcmp(cmd, "GETTTL") == 0 || strcmp(cmd, "getttl") == 0 || strcmp(cmd, "GetTTL") == 0) {
        *key = strtok(NULL, " ");
        return *key ? GETTTL : UNKNOWN;
    }

    if (strcmp(cmd, "DEL") == 0 || strcmp(cmd, "del") == 0 || strcmp(cmd, "Del") == 0) {
        *key = strtok(NULL, " ");
        return *key ? DEL : UNKNOWN;
    }

    if (strcmp(cmd, "PUT") == 0 || strcmp(cmd, "put") == 0 || strcmp(cmd, "Put") == 0) {
        *key = strtok(NULL, " ");
        *value = strtok(NULL, " ");
        char *ttl_str = strtok(NULL, " ");

        if (!*key || !*value) return UNKNOWN;

        if (ttl_str) {
            *ttl = atoi(ttl_str);
            //printf("Adding with a ttl of %d\n", *ttl);
        }

        return PUT;
    }

    if (strcmp(cmd, "STATS") == 0) return STATS;

    return UNKNOWN;
}



void setEntry(entry* input, const char* key, const char* value, int ttl)
{
    strncpy(input->key, key, MAX_KEY_LEN);
    input->key[MAX_KEY_LEN] = '\0';

    strncpy(input->value, value, MAX_VAL_LEN);
    input->value[MAX_VAL_LEN] = '\0';

    input->next = NULL;

    if (ttl <= 0) {
        input->deathtime = 0; // never expires
    } else {
        input->deathtime = time(NULL) + (time_t)ttl;
    }
}



unsigned long hashFunction(const char* key)
{
    unsigned long hash = 5381;
    int c;

    while ((c = *key++)) {
        hash = ((hash << 5) + hash) + c;
    }

    return hash;
}

static inline int bucket_of(HashMap *mp, const char *key)
{
    return (int)(hashFunction(key) % (unsigned long)mp->capacity);
}


void insert(HashMap* mp, char* key, char* value, int ttl)
{
    int idx = bucket_of(mp, key);
 
    entry *ne = malloc(sizeof(entry));
    if (!ne) { perror("malloc entry"); return; }
    *ne = (entry){0};
    setEntry(ne, key, value, ttl);
 
    pthread_rwlock_wrlock(&mp->bucket_locks[idx]);
    ne->next      = mp->arr[idx];
    mp->arr[idx]  = ne;
    pthread_rwlock_unlock(&mp->bucket_locks[idx]);
 
    atomic_fetch_add(&mp->puts, 1);
}

void delete_key(HashMap* mp, char* key)
{
    int idx = bucket_of(mp, key);
 
    pthread_rwlock_wrlock(&mp->bucket_locks[idx]);
 
    entry *prev = NULL, *curr = mp->arr[idx];
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            if (prev) prev->next      = curr->next;
            else      mp->arr[idx]    = curr->next;
            free(curr);
            atomic_fetch_add(&mp->dels, 1);
            pthread_rwlock_unlock(&mp->bucket_locks[idx]);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
 
    pthread_rwlock_unlock(&mp->bucket_locks[idx]);
}

char* search(HashMap* mp, char* key)
{
    int idx = bucket_of(mp, key);
 
    pthread_rwlock_rdlock(&mp->bucket_locks[idx]);
 
    entry *curr  = mp->arr[idx];
    char  *found = NULL;
 
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            if (curr->deathtime != 0 && time(NULL) > curr->deathtime)
                break;          /* expired — treat as miss */
            found = curr->value;
            break;
        }
        curr = curr->next;
    }
 
    pthread_rwlock_unlock(&mp->bucket_locks[idx]);
 
    if (found) atomic_fetch_add(&mp->hits,   1);
    else        atomic_fetch_add(&mp->misses, 1);
 
    return found;

}

entry* find_entry(HashMap *mp, char *key)
{
    int idx = bucket_of(mp, key);
 
    pthread_rwlock_rdlock(&mp->bucket_locks[idx]);
 
    entry *curr   = mp->arr[idx];
    entry *result = NULL;
 
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            if (curr->deathtime != 0 && time(NULL) > curr->deathtime)
                break;
            result = curr;
            break;
        }
        curr = curr->next;
    }
 
    pthread_rwlock_unlock(&mp->bucket_locks[idx]);
    return result;

}