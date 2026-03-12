#ifndef LNOTIFY_PROTOCOL_H
#define LNOTIFY_PROTOCOL_H

#include "lnotify.h"
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

// Field mask bits — which optional fields are present in the wire message
#define FIELD_TITLE   (1 << 0)
#define FIELD_APP     (1 << 1)
#define FIELD_GROUP   (1 << 2)

// Transport-level flags (high bits, won't conflict with data fields)
#define FIELD_DRY_RUN (1 << 7)

// Fixed header size:
//   total_len(4) + field_mask(2) + priority(1) + timeout_ms(4) + ts_sent(8)
#define PROTOCOL_HEADER_SIZE 19

// Maximum wire-message size (64 KiB)
#define MAX_MSG_SIZE 65536

// Extract the raw field_mask from a serialized buffer without full deserialization.
// Returns the field_mask, or 0 if the buffer is too small.
static inline uint16_t protocol_peek_field_mask(const uint8_t *buf, size_t buflen) {
    if (buflen < PROTOCOL_HEADER_SIZE) return 0;
    uint16_t val;
    memcpy(&val, buf + 4, 2);  // field_mask starts at offset 4 (after total_len)
    return val;
}

// Serialize a notification into buf. Returns bytes written, or -1 on error.
// Body must be non-NULL. Title, app, and group_id are optional.
ssize_t protocol_serialize(const notification *n, uint8_t *buf, size_t buflen);

// Deserialize a notification from buf. Populates n (caller must notification_free).
// Returns bytes consumed, or -1 on error. Unknown field_mask bits are ignored.
ssize_t protocol_deserialize(const uint8_t *buf, size_t buflen, notification *n);

#endif
