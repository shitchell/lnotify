#include "protocol.h"
#include <string.h>

// Known data field_mask bits — used to parse optional fields during deserialization
#define FIELD_MASK_DATA (FIELD_TITLE | FIELD_APP | FIELD_GROUP)
// All known field_mask bits (data + transport flags)
#define FIELD_MASK_KNOWN (FIELD_MASK_DATA | FIELD_DRY_RUN)

// Helper: write a uint16 to buf (little-endian)
static void write_u16(uint8_t *buf, uint16_t val) {
    memcpy(buf, &val, 2);
}

// Helper: write a uint32 to buf (little-endian)
static void write_u32(uint8_t *buf, uint32_t val) {
    memcpy(buf, &val, 4);
}

// Helper: write a uint64 to buf (little-endian)
static void write_u64(uint8_t *buf, uint64_t val) {
    memcpy(buf, &val, 8);
}

// Helper: write an int32 to buf (little-endian)
static void write_i32(uint8_t *buf, int32_t val) {
    memcpy(buf, &val, 4);
}

// Helper: read a uint16 from buf (little-endian)
static uint16_t read_u16(const uint8_t *buf) {
    uint16_t val;
    memcpy(&val, buf, 2);
    return val;
}

// Helper: read a uint32 from buf (little-endian)
static uint32_t read_u32(const uint8_t *buf) {
    uint32_t val;
    memcpy(&val, buf, 4);
    return val;
}

// Helper: read a uint64 from buf (little-endian)
static uint64_t read_u64(const uint8_t *buf) {
    uint64_t val;
    memcpy(&val, buf, 8);
    return val;
}

// Helper: read an int32 from buf (little-endian)
static int32_t read_i32(const uint8_t *buf) {
    int32_t val;
    memcpy(&val, buf, 4);
    return val;
}

// Helper: write a length-prefixed string (uint16 len + bytes, no NUL)
// Returns bytes written, or -1 if it won't fit or string exceeds uint16 max.
static ssize_t write_string(uint8_t *buf, size_t remaining, const char *str) {
    size_t raw_len = strlen(str);
    if (raw_len > UINT16_MAX) return -1;
    uint16_t slen = (uint16_t)raw_len;
    size_t need = 2 + (size_t)slen;
    if (remaining < need) return -1;
    write_u16(buf, slen);
    memcpy(buf + 2, str, slen);
    return (ssize_t)need;
}

// Helper: read a length-prefixed string. Returns a strdup'd copy.
// Advances *pos by bytes consumed. Returns NULL on error.
static char *read_string(const uint8_t *buf, size_t buflen, size_t *pos) {
    if (*pos + 2 > buflen) return NULL;
    uint16_t slen = read_u16(buf + *pos);
    *pos += 2;
    if (*pos + slen > buflen) return NULL;
    char *s = malloc(slen + 1);
    if (!s) return NULL;
    memcpy(s, buf + *pos, slen);
    s[slen] = '\0';
    *pos += slen;
    return s;
}

ssize_t protocol_serialize(const notification *n, uint8_t *buf, size_t buflen) {
    if (!n || !n->body) return -1;

    // Build field_mask
    uint16_t field_mask = 0;
    if (n->title)    field_mask |= FIELD_TITLE;
    if (n->app)      field_mask |= FIELD_APP;
    if (n->group_id) field_mask |= FIELD_GROUP;

    // Calculate total size needed
    size_t total = PROTOCOL_HEADER_SIZE;

    // Body is always present (length-prefixed)
    total += 2 + strlen(n->body);

    // Optional fields
    if (n->title)    total += 2 + strlen(n->title);
    if (n->app)      total += 2 + strlen(n->app);
    if (n->group_id) total += 2 + strlen(n->group_id);

    if (buflen < total) return -1;

    // Write fixed header
    size_t pos = 0;

    write_u32(buf + pos, (uint32_t)total);  pos += 4;
    write_u16(buf + pos, field_mask);        pos += 2;
    buf[pos] = n->priority;                  pos += 1;
    write_i32(buf + pos, n->timeout_ms);     pos += 4;
    write_u64(buf + pos, n->ts_sent);        pos += 8;

    // Write optional title (before body, per spec order)
    if (n->title) {
        ssize_t w = write_string(buf + pos, buflen - pos, n->title);
        if (w < 0) return -1;
        pos += (size_t)w;
    }

    // Write body (always present)
    {
        ssize_t w = write_string(buf + pos, buflen - pos, n->body);
        if (w < 0) return -1;
        pos += (size_t)w;
    }

    // Write optional app
    if (n->app) {
        ssize_t w = write_string(buf + pos, buflen - pos, n->app);
        if (w < 0) return -1;
        pos += (size_t)w;
    }

    // Write optional group_id
    if (n->group_id) {
        ssize_t w = write_string(buf + pos, buflen - pos, n->group_id);
        if (w < 0) return -1;
        pos += (size_t)w;
    }

    return (ssize_t)pos;
}

ssize_t protocol_deserialize(const uint8_t *buf, size_t buflen, notification *n) {
    if (!buf || !n) return -1;

    // Need at least the fixed header
    if (buflen < PROTOCOL_HEADER_SIZE) return -1;

    memset(n, 0, sizeof(*n));

    size_t pos = 0;

    // Read total_len and validate
    uint32_t total_len = read_u32(buf + pos); pos += 4;
    if (total_len > buflen) return -1;
    if (total_len < PROTOCOL_HEADER_SIZE) return -1;

    // Read field_mask — separate data fields from transport flags
    uint16_t field_mask = read_u16(buf + pos); pos += 2;
    uint16_t known_mask = field_mask & FIELD_MASK_DATA;

    // Read fixed fields
    n->priority   = buf[pos];                pos += 1;
    n->timeout_ms = read_i32(buf + pos);     pos += 4;
    n->ts_sent    = read_u64(buf + pos);     pos += 8;

    // Read optional title
    if (known_mask & FIELD_TITLE) {
        n->title = read_string(buf, total_len, &pos);
        if (!n->title) goto fail;
    }

    // Read body (always present)
    n->body = read_string(buf, total_len, &pos);
    if (!n->body) goto fail;

    // Read optional app
    if (known_mask & FIELD_APP) {
        n->app = read_string(buf, total_len, &pos);
        if (!n->app) goto fail;
    }

    // Read optional group_id
    if (known_mask & FIELD_GROUP) {
        n->group_id = read_string(buf, total_len, &pos);
        if (!n->group_id) goto fail;
    }

    return (ssize_t)total_len;

fail:
    notification_free(n);
    return -1;
}
