#ifndef LNOTIFY_QUEUE_H
#define LNOTIFY_QUEUE_H

#include "lnotify.h"
#include <pthread.h>
#include <stddef.h>

// Maximum number of entries in a notification queue. When a push would exceed
// this limit, the oldest entry is dropped to make room (prevents memory
// exhaustion via the world-writable socket).
#define QUEUE_MAX_SIZE 1000

// Singly-linked list node holding a deep-copied notification
typedef struct notif_node {
    notification        notif;
    struct notif_node  *next;
} notif_node;

// Thread-safe notification queue (FIFO, linked list)
typedef struct {
    notif_node     *head;
    notif_node     *tail;
    size_t          count;
    pthread_mutex_t lock;
} notif_queue;

// Initialize a queue (must be called before use).
// Returns 0 on success, -1 if mutex init fails.
int queue_init(notif_queue *q);

// Destroy a queue, freeing all queued notifications
void queue_destroy(notif_queue *q);

// Push a notification (deep-copied). If the notification has a group_id
// that matches an existing entry, the existing entry is replaced in-place.
void queue_push(notif_queue *q, const notification *n);

// Pop the front notification (caller owns the returned malloc'd notification).
// Returns NULL if queue is empty.
notification *queue_pop(notif_queue *q);

// Pop the front live (non-expired) notification. Expired entries are freed
// and skipped. Returns NULL if all entries are expired or queue is empty.
notification *queue_pop_live(notif_queue *q);

// Return current queue size
size_t queue_size(notif_queue *q);

// Global notification queue (used by daemon and engines for fallback delivery).
// Must be initialized with queue_init() before use.
extern notif_queue g_queue;

#endif
