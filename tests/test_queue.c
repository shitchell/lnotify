#include "queue.h"
#include "lnotify.h"
#include "test_util.h"
#include <string.h>
#include <unistd.h>

void test_queue_suite(void) {
    // Test: enqueue and dequeue
    {
        notif_queue q;
        queue_init(&q);

        notification n = {0};
        notification_init(&n, "Title", "Body");
        n.timeout_ms = 5000;
        n.ts_mono = monotonic_ms();
        queue_push(&q, &n);

        ASSERT_EQ(queue_size(&q), 1, "queue has 1 item");

        notification *out = queue_pop(&q);
        ASSERT_NOT_NULL(out, "popped notification");
        ASSERT_STR_EQ(out->body, "Body", "body matches");

        ASSERT_EQ(queue_size(&q), 0, "queue empty after pop");

        notification_free(out);
        free(out);
        notification_free(&n);
        queue_destroy(&q);
    }

    // Test: dedup via group_id (replaces existing)
    {
        notif_queue q;
        queue_init(&q);

        notification a = {0};
        notification_init(&a, "Backup", "50%");
        a.group_id = strdup("backup");
        a.timeout_ms = 5000;
        a.ts_mono = monotonic_ms();
        queue_push(&q, &a);

        notification b = {0};
        notification_init(&b, "Backup", "75%");
        b.group_id = strdup("backup");
        b.timeout_ms = 5000;
        b.ts_mono = monotonic_ms();
        queue_push(&q, &b);

        ASSERT_EQ(queue_size(&q), 1, "dedup kept size at 1");

        notification *out = queue_pop(&q);
        ASSERT_STR_EQ(out->body, "75%", "dedup replaced with newer");

        notification_free(out);
        free(out);
        notification_free(&a);
        notification_free(&b);
        queue_destroy(&q);
    }

    // Test: expired notifications skipped on drain
    {
        notif_queue q;
        queue_init(&q);

        notification n = {0};
        notification_init(&n, NULL, "ephemeral");
        n.timeout_ms = 50;  // 50ms
        n.ts_mono = monotonic_ms();
        queue_push(&q, &n);

        usleep(60 * 1000);  // sleep 60ms

        notification *out = queue_pop_live(&q);
        ASSERT_NULL(out, "expired notification skipped");
        ASSERT_EQ(queue_size(&q), 0, "expired notification removed");

        notification_free(&n);
        queue_destroy(&q);
    }

    // Test: queue size capped at QUEUE_MAX_SIZE (oldest dropped)
    {
        notif_queue q;
        queue_init(&q);

        // Push QUEUE_MAX_SIZE + 1 notifications
        for (int i = 0; i <= (int)QUEUE_MAX_SIZE; i++) {
            notification n = {0};
            char buf[32];
            snprintf(buf, sizeof(buf), "notif-%d", i);
            notification_init(&n, NULL, buf);
            n.timeout_ms = 60000;
            n.ts_mono = monotonic_ms();
            queue_push(&q, &n);
            notification_free(&n);
        }

        ASSERT_EQ(queue_size(&q), QUEUE_MAX_SIZE,
                  "queue capped at QUEUE_MAX_SIZE");

        // The oldest (notif-0) should have been dropped; head should be notif-1
        notification *front = queue_pop(&q);
        ASSERT_NOT_NULL(front, "front not null after cap");
        ASSERT_STR_EQ(front->body, "notif-1",
                      "oldest notification dropped when queue full");
        notification_free(front);
        free(front);

        queue_destroy(&q);
    }

    // Test: FIFO order
    {
        notif_queue q;
        queue_init(&q);

        notification a = {0};
        notification_init(&a, NULL, "first");
        a.timeout_ms = 5000;
        a.ts_mono = monotonic_ms();
        queue_push(&q, &a);

        notification b = {0};
        notification_init(&b, NULL, "second");
        b.timeout_ms = 5000;
        b.ts_mono = monotonic_ms();
        queue_push(&q, &b);

        notification *out1 = queue_pop(&q);
        notification *out2 = queue_pop(&q);
        ASSERT_STR_EQ(out1->body, "first", "FIFO: first out");
        ASSERT_STR_EQ(out2->body, "second", "FIFO: second out");

        notification_free(out1); free(out1);
        notification_free(out2); free(out2);
        notification_free(&a);
        notification_free(&b);
        queue_destroy(&q);
    }
}
