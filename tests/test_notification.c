#include "lnotify.h"
#include "test_util.h"
#include <string.h>
#include <unistd.h>

void test_notification_suite(void) {
    // Test: create notification with defaults
    {
        notification n = {0};
        notification_init(&n, "Test Title", "Test Body");
        ASSERT_STR_EQ(n.title, "Test Title", "title set");
        ASSERT_STR_EQ(n.body, "Test Body", "body set");
        ASSERT_EQ(n.priority, 1, "default priority is normal");
        ASSERT_EQ(n.timeout_ms, -1, "default timeout is -1 (use config)");
        ASSERT_NULL(n.app, "app defaults to NULL");
        ASSERT_NULL(n.group_id, "group_id defaults to NULL");
        ASSERT_TRUE(n.ts_sent > 0, "ts_sent populated");
        notification_free(&n);
    }

    // Test: notification not expired immediately
    {
        notification n = {0};
        notification_init(&n, NULL, "body");
        n.timeout_ms = 5000;
        n.ts_mono = monotonic_ms();
        ASSERT_FALSE(notification_expired(&n, monotonic_ms()),
                     "not expired immediately");
        notification_free(&n);
    }

    // Test: notification expires after timeout
    {
        notification n = {0};
        notification_init(&n, NULL, "body");
        n.timeout_ms = 50;  // 50ms
        n.ts_mono = monotonic_ms();
        usleep(60 * 1000);  // sleep 60ms
        ASSERT_TRUE(notification_expired(&n, monotonic_ms()),
                    "expired after timeout");
        notification_free(&n);
    }

    // Test: remaining timeout calculation
    {
        notification n = {0};
        notification_init(&n, NULL, "body");
        n.timeout_ms = 5000;
        n.ts_mono = monotonic_ms();
        int32_t remaining = notification_remaining_ms(&n, monotonic_ms());
        ASSERT_TRUE(remaining > 4900 && remaining <= 5000,
                    "remaining timeout approximately correct");
        notification_free(&n);
    }

    // Test: group_id dedup match
    {
        notification a = {0};
        notification_init(&a, "A", "body a");
        a.group_id = strdup("backup");

        notification b = {0};
        notification_init(&b, "B", "body b");
        b.group_id = strdup("backup");

        ASSERT_TRUE(notification_group_matches(&a, &b),
                    "same group_id matches");

        notification c = {0};
        notification_init(&c, "C", "body c");
        c.group_id = strdup("other");

        ASSERT_FALSE(notification_group_matches(&a, &c),
                     "different group_id doesn't match");

        notification d = {0};
        notification_init(&d, "D", "body d");
        // d.group_id is NULL

        ASSERT_FALSE(notification_group_matches(&a, &d),
                     "NULL group_id doesn't match non-NULL");
        ASSERT_FALSE(notification_group_matches(&d, &d),
                     "two NULL group_ids don't match (no dedup on NULL)");

        notification_free(&a);
        notification_free(&b);
        notification_free(&c);
        notification_free(&d);
    }

    // Test: notification_copy with all fields set
    {
        notification src = {0};
        notification_init(&src, "Copy Title", "Copy Body");
        src.id          = 42;
        src.priority    = 2;
        src.timeout_ms  = 3000;
        src.origin_uid  = 1000;
        src.ts_sent     = 111111;
        src.ts_received = 222222;
        src.ts_mono     = 333333;
        src.app         = strdup("myapp");
        src.group_id    = strdup("grp1");

        notification dst = {0};
        int rc = notification_copy(&dst, &src);
        ASSERT_EQ(rc, 0, "copy succeeds");

        // Verify all fields
        ASSERT_STR_EQ(dst.title, "Copy Title", "copy title matches");
        ASSERT_STR_EQ(dst.body, "Copy Body", "copy body matches");
        ASSERT_STR_EQ(dst.app, "myapp", "copy app matches");
        ASSERT_STR_EQ(dst.group_id, "grp1", "copy group_id matches");
        ASSERT_EQ(dst.id, 42, "copy id matches");
        ASSERT_EQ(dst.priority, 2, "copy priority matches");
        ASSERT_EQ(dst.timeout_ms, 3000, "copy timeout_ms matches");
        ASSERT_EQ(dst.origin_uid, 1000, "copy origin_uid matches");
        ASSERT_EQ(dst.ts_sent, 111111, "copy ts_sent matches");
        ASSERT_EQ(dst.ts_received, 222222, "copy ts_received matches");
        ASSERT_EQ(dst.ts_mono, 333333, "copy ts_mono matches");

        // Verify deep copy (different pointers)
        ASSERT_TRUE(dst.title != src.title, "title is deep copy");
        ASSERT_TRUE(dst.body != src.body, "body is deep copy");
        ASSERT_TRUE(dst.app != src.app, "app is deep copy");
        ASSERT_TRUE(dst.group_id != src.group_id, "group_id is deep copy");

        notification_free(&dst);
        notification_free(&src);
    }

    // Test: notification_copy with NULL optional fields
    {
        notification src = {0};
        notification_init(&src, NULL, "body only");
        src.priority = 0;

        notification dst = {0};
        int rc = notification_copy(&dst, &src);
        ASSERT_EQ(rc, 0, "copy with NULLs succeeds");
        ASSERT_NULL(dst.title, "copy NULL title stays NULL");
        ASSERT_STR_EQ(dst.body, "body only", "copy body matches");
        ASSERT_NULL(dst.app, "copy NULL app stays NULL");
        ASSERT_NULL(dst.group_id, "copy NULL group_id stays NULL");
        ASSERT_EQ(dst.priority, 0, "copy priority 0 matches");

        notification_free(&dst);
        notification_free(&src);
    }

    // Test: original unchanged after copy
    {
        notification src = {0};
        notification_init(&src, "Orig", "OrigBody");
        src.app = strdup("origapp");
        src.group_id = strdup("origgrp");
        src.id = 99;
        src.priority = 1;
        src.timeout_ms = 500;

        notification dst = {0};
        notification_copy(&dst, &src);

        // Modify copy
        free(dst.title);
        dst.title = strdup("Changed");
        free(dst.app);
        dst.app = strdup("changed_app");
        dst.id = 0;
        dst.priority = 2;

        // Verify original is unchanged
        ASSERT_STR_EQ(src.title, "Orig", "original title unchanged");
        ASSERT_STR_EQ(src.body, "OrigBody", "original body unchanged");
        ASSERT_STR_EQ(src.app, "origapp", "original app unchanged");
        ASSERT_STR_EQ(src.group_id, "origgrp", "original group_id unchanged");
        ASSERT_EQ(src.id, 99, "original id unchanged");
        ASSERT_EQ(src.priority, 1, "original priority unchanged");

        notification_free(&dst);
        notification_free(&src);
    }

    // Test: free copy, verify original still valid
    {
        notification src = {0};
        notification_init(&src, "StillHere", "StillBody");
        src.app = strdup("stillapp");

        notification dst = {0};
        notification_copy(&dst, &src);
        notification_free(&dst);

        // Original must still be valid
        ASSERT_STR_EQ(src.title, "StillHere", "original title valid after copy freed");
        ASSERT_STR_EQ(src.body, "StillBody", "original body valid after copy freed");
        ASSERT_STR_EQ(src.app, "stillapp", "original app valid after copy freed");

        notification_free(&src);
    }
}
