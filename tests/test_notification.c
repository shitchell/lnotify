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
}
