#include "queue.h"
#include <string.h>

// Deep-copy a notification into a freshly allocated node
static notif_node *node_from_notif(const notification *n) {
    notif_node *node = calloc(1, sizeof(*node));
    if (!node) return NULL;

    node->notif.id          = n->id;
    node->notif.title       = n->title    ? strdup(n->title)    : NULL;
    node->notif.body        = n->body     ? strdup(n->body)     : NULL;
    node->notif.app         = n->app      ? strdup(n->app)      : NULL;
    node->notif.group_id    = n->group_id ? strdup(n->group_id) : NULL;
    node->notif.priority    = n->priority;
    node->notif.timeout_ms  = n->timeout_ms;
    node->notif.origin_uid  = n->origin_uid;
    node->notif.ts_sent     = n->ts_sent;
    node->notif.ts_received = n->ts_received;
    node->notif.ts_mono     = n->ts_mono;
    node->next = NULL;
    return node;
}

// Replace an existing node's notification fields with a new notification's
// values (deep-copies strings, frees old ones).
static void node_replace(notif_node *node, const notification *n) {
    // Free old strings
    free(node->notif.title);
    free(node->notif.body);
    free(node->notif.app);
    // Keep group_id — it already matches

    node->notif.id          = n->id;
    node->notif.title       = n->title ? strdup(n->title) : NULL;
    node->notif.body        = n->body  ? strdup(n->body)  : NULL;
    node->notif.app         = n->app   ? strdup(n->app)   : NULL;
    node->notif.priority    = n->priority;
    node->notif.timeout_ms  = n->timeout_ms;
    node->notif.origin_uid  = n->origin_uid;
    node->notif.ts_sent     = n->ts_sent;
    node->notif.ts_received = n->ts_received;
    node->notif.ts_mono     = n->ts_mono;
}

void queue_init(notif_queue *q) {
    q->head  = NULL;
    q->tail  = NULL;
    q->count = 0;
    pthread_mutex_init(&q->lock, NULL);
}

void queue_destroy(notif_queue *q) {
    pthread_mutex_lock(&q->lock);
    notif_node *cur = q->head;
    while (cur) {
        notif_node *next = cur->next;
        notification_free(&cur->notif);
        free(cur);
        cur = next;
    }
    q->head  = NULL;
    q->tail  = NULL;
    q->count = 0;
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
}

void queue_push(notif_queue *q, const notification *n) {
    pthread_mutex_lock(&q->lock);

    // Dedup: if incoming has a group_id, scan for a match and replace in-place
    if (n->group_id) {
        for (notif_node *cur = q->head; cur; cur = cur->next) {
            if (cur->notif.group_id && strcmp(cur->notif.group_id, n->group_id) == 0) {
                node_replace(cur, n);
                pthread_mutex_unlock(&q->lock);
                return;
            }
        }
    }

    // No dedup match (or no group_id) — append to tail
    notif_node *node = node_from_notif(n);
    if (!node) {
        pthread_mutex_unlock(&q->lock);
        return;
    }

    if (q->tail) {
        q->tail->next = node;
    } else {
        q->head = node;
    }
    q->tail = node;
    q->count++;

    pthread_mutex_unlock(&q->lock);
}

notification *queue_pop(notif_queue *q) {
    pthread_mutex_lock(&q->lock);

    if (!q->head) {
        pthread_mutex_unlock(&q->lock);
        return NULL;
    }

    notif_node *node = q->head;
    q->head = node->next;
    if (!q->head) {
        q->tail = NULL;
    }
    q->count--;

    pthread_mutex_unlock(&q->lock);

    // Transfer ownership: copy notification to heap, free the node shell
    notification *out = malloc(sizeof(*out));
    if (out) {
        *out = node->notif;
    } else {
        notification_free(&node->notif);
    }
    free(node);
    return out;
}

notification *queue_pop_live(notif_queue *q) {
    pthread_mutex_lock(&q->lock);

    uint64_t now = monotonic_ms();

    while (q->head) {
        notif_node *node = q->head;

        if (!notification_expired(&node->notif, now)) {
            // Not expired — pop and return
            q->head = node->next;
            if (!q->head) q->tail = NULL;
            q->count--;
            pthread_mutex_unlock(&q->lock);

            notification *out = malloc(sizeof(*out));
            if (out) {
                *out = node->notif;
            } else {
                notification_free(&node->notif);
            }
            free(node);
            return out;
        }

        // Expired — free and advance
        q->head = node->next;
        if (!q->head) q->tail = NULL;
        q->count--;
        notification_free(&node->notif);
        free(node);
    }

    pthread_mutex_unlock(&q->lock);
    return NULL;
}

size_t queue_size(notif_queue *q) {
    pthread_mutex_lock(&q->lock);
    size_t s = q->count;
    pthread_mutex_unlock(&q->lock);
    return s;
}
