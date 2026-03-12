#ifndef LNOTIFY_H
#define LNOTIFY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    uint32_t    id;
    char       *title;          // optional (NULL = no title)
    char       *body;           // required
    uint8_t     priority;       // 0=low, 1=normal, 2=critical
    int32_t     timeout_ms;     // -1 = use config default
    char       *app;            // optional
    char       *group_id;       // optional, for dedup
    uint32_t    origin_uid;     // daemon-captured via SO_PEERCRED
    uint64_t    ts_sent;        // wall clock ms
    uint64_t    ts_received;    // wall clock ms
    uint64_t    ts_mono;        // monotonic ms (internal)
} notification;

// Get current monotonic time in milliseconds
uint64_t monotonic_ms(void);

// Get current wall clock time in milliseconds (Unix epoch)
uint64_t wallclock_ms(void);

// Initialize a notification with defaults. Copies title and body strings.
// Returns 0 on success, -1 on allocation failure.
int notification_init(notification *n, const char *title, const char *body);

// Deep-copy a notification. Returns 0 on success, -1 on failure.
int notification_copy(notification *dst, const notification *src);

// Free owned strings in a notification.
void notification_free(notification *n);

// Check if notification has expired relative to a monotonic timestamp.
bool notification_expired(const notification *n, uint64_t now_mono);

// Get remaining timeout in ms. Returns 0 if expired.
int32_t notification_remaining_ms(const notification *n, uint64_t now_mono);

// Check if two notifications should dedup (both non-NULL group_ids match).
bool notification_group_matches(const notification *a, const notification *b);

#endif
