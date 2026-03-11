#include "protocol.h"
#include "lnotify.h"
#include "test_util.h"
#include <string.h>

void test_protocol_suite(void) {
    // Test: minimal notification (body only)
    {
        notification orig = {0};
        notification_init(&orig, NULL, "hello world");

        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        ASSERT_TRUE(len > 0, "serialize minimal succeeds");

        notification decoded = {0};
        ssize_t consumed = protocol_deserialize(buf, len, &decoded);
        ASSERT_EQ(consumed, len, "deserialize consumes all bytes");
        ASSERT_NULL(decoded.title, "title is NULL");
        ASSERT_STR_EQ(decoded.body, "hello world", "body round-trips");
        ASSERT_EQ(decoded.priority, 1, "priority round-trips");
        ASSERT_EQ(decoded.timeout_ms, -1, "timeout round-trips");

        notification_free(&orig);
        notification_free(&decoded);
    }

    // Test: full notification (all fields)
    {
        notification orig = {0};
        notification_init(&orig, "Alert", "server on fire");
        orig.priority = 2;
        orig.timeout_ms = 10000;
        orig.app = strdup("monitoring");
        orig.group_id = strdup("server-health");

        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        ASSERT_TRUE(len > 0, "serialize full succeeds");

        notification decoded = {0};
        protocol_deserialize(buf, len, &decoded);
        ASSERT_STR_EQ(decoded.title, "Alert", "title round-trips");
        ASSERT_STR_EQ(decoded.body, "server on fire", "body round-trips");
        ASSERT_EQ(decoded.priority, 2, "priority round-trips");
        ASSERT_EQ(decoded.timeout_ms, 10000, "timeout round-trips");
        ASSERT_STR_EQ(decoded.app, "monitoring", "app round-trips");
        ASSERT_STR_EQ(decoded.group_id, "server-health", "group_id round-trips");
        ASSERT_TRUE(decoded.ts_sent > 0, "ts_sent round-trips");

        notification_free(&orig);
        notification_free(&decoded);
    }

    // Test: buffer too small
    {
        notification orig = {0};
        notification_init(&orig, NULL, "body");
        uint8_t buf[2];  // way too small
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        ASSERT_TRUE(len < 0, "serialize fails on tiny buffer");
        notification_free(&orig);
    }

    // Test: truncated input
    {
        notification orig = {0};
        notification_init(&orig, NULL, "body");
        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));

        notification decoded = {0};
        ssize_t consumed = protocol_deserialize(buf, len / 2, &decoded);
        ASSERT_TRUE(consumed < 0, "deserialize fails on truncated input");
        notification_free(&orig);
    }

    // Test: unknown field_mask bits are ignored
    {
        notification orig = {0};
        notification_init(&orig, NULL, "body");
        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        ASSERT_TRUE(len > 0, "serialized ok");

        // Flip a high bit in field_mask (offset 4, uint16)
        buf[4] |= 0x80;

        notification decoded = {0};
        ssize_t consumed = protocol_deserialize(buf, len, &decoded);
        ASSERT_TRUE(consumed > 0, "deserialize succeeds with unknown field_mask bits");
        ASSERT_STR_EQ(decoded.body, "body", "body still decoded correctly");
        notification_free(&decoded);
        notification_free(&orig);
    }

    // Test: empty body
    {
        notification orig = {0};
        notification_init(&orig, NULL, "");

        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        ASSERT_TRUE(len > 0, "serialize empty body succeeds");

        notification decoded = {0};
        ssize_t consumed = protocol_deserialize(buf, len, &decoded);
        ASSERT_TRUE(consumed > 0, "deserialize empty body succeeds");
        ASSERT_STR_EQ(decoded.body, "", "empty body round-trips");

        notification_free(&orig);
        notification_free(&decoded);
    }

    // Test: title only (no app, no group)
    {
        notification orig = {0};
        notification_init(&orig, "Just a Title", "some body");

        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        ASSERT_TRUE(len > 0, "serialize title-only succeeds");

        notification decoded = {0};
        protocol_deserialize(buf, len, &decoded);
        ASSERT_STR_EQ(decoded.title, "Just a Title", "title round-trips");
        ASSERT_STR_EQ(decoded.body, "some body", "body round-trips");
        ASSERT_NULL(decoded.app, "app is NULL");
        ASSERT_NULL(decoded.group_id, "group_id is NULL");

        notification_free(&orig);
        notification_free(&decoded);
    }

    // Test: app only (no title, no group)
    {
        notification orig = {0};
        notification_init(&orig, NULL, "app notification");
        orig.app = strdup("myapp");

        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        ASSERT_TRUE(len > 0, "serialize app-only succeeds");

        notification decoded = {0};
        protocol_deserialize(buf, len, &decoded);
        ASSERT_NULL(decoded.title, "title is NULL");
        ASSERT_STR_EQ(decoded.body, "app notification", "body round-trips");
        ASSERT_STR_EQ(decoded.app, "myapp", "app round-trips");
        ASSERT_NULL(decoded.group_id, "group_id is NULL");

        notification_free(&orig);
        notification_free(&decoded);
    }

    // Test: exact header size calculation
    {
        notification orig = {0};
        notification_init(&orig, NULL, "x");  // 1-byte body

        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        // Fixed header: 4(total_len) + 2(field_mask) + 1(priority) + 4(timeout) + 8(ts_sent) = 19
        // Body: 2(body_len) + 1(body bytes) = 3
        // Total: 22
        ASSERT_EQ(len, 22, "minimal message is 22 bytes");

        notification_free(&orig);
    }

    // Test: NULL body should fail
    {
        notification orig = {0};
        memset(&orig, 0, sizeof(orig));
        // body is NULL - serialize should fail
        uint8_t buf[4096];
        ssize_t len = protocol_serialize(&orig, buf, sizeof(buf));
        ASSERT_TRUE(len < 0, "serialize fails with NULL body");
    }

    // Test: string exceeding UINT16_MAX is rejected
    {
        size_t oversized_len = (size_t)UINT16_MAX + 1;
        char *big = malloc(oversized_len + 1);
        ASSERT_TRUE(big != NULL, "malloc oversized string");
        memset(big, 'A', oversized_len);
        big[oversized_len] = '\0';

        notification orig = {0};
        notification_init(&orig, NULL, big);

        // Buffer large enough for the data if it were allowed
        size_t buflen = PROTOCOL_HEADER_SIZE + 2 + oversized_len + 64;
        uint8_t *buf = malloc(buflen);
        ASSERT_TRUE(buf != NULL, "malloc oversized buffer");

        ssize_t len = protocol_serialize(&orig, buf, buflen);
        ASSERT_TRUE(len < 0, "serialize rejects body exceeding UINT16_MAX");

        // Also test with oversized title
        notification orig2 = {0};
        notification_init(&orig2, big, "ok body");
        ssize_t len2 = protocol_serialize(&orig2, buf, buflen);
        ASSERT_TRUE(len2 < 0, "serialize rejects title exceeding UINT16_MAX");

        free(buf);
        free(big);
        notification_free(&orig);
        notification_free(&orig2);
    }
}
