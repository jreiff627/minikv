#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>

#define QUEUE_CAPACITY 16

typedef struct {
    int buf[QUEUE_CAPACITY];         
    int head, tail;    // read/write positions
    int count;         // current items in queue

    pthread_mutex_t lock;
    pthread_cond_t  not_empty;  // signal workers when fd is added
    pthread_cond_t  not_full;   // signal main when space opens up
} WorkQueue;


void queue_init(WorkQueue *q) {
    q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}


void queue_push(WorkQueue *q, int fd) {
    pthread_mutex_lock(&q->lock);

    // "Wait until there's room"
    while (q->count == QUEUE_CAPACITY)
        pthread_cond_wait(&q->not_full, &q->lock);

    // Put the fd in, advance tail
    q->buf[q->tail] = fd;
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;
    q->count++;

    // "Hey workers, something is in the queue"
    pthread_cond_signal(&q->not_empty);

    pthread_mutex_unlock(&q->lock);
}

int queue_pop(WorkQueue *q) {
    pthread_mutex_lock(&q->lock);

    // "Wait until there's something to grab"
    while (q->count == 0)
        pthread_cond_wait(&q->not_empty, &q->lock);

    // Grab the fd, advance head
    int fd = q->buf[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->count--;

    // "Hey main thread, there's room now"
    pthread_cond_signal(&q->not_full);

    pthread_mutex_unlock(&q->lock);
    return fd;
}

#endif
