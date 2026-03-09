#include <stdio.h>
#include <stdlib.h>

// Global test counters — shared with other test files via test_util.h
int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;

#include "test_util.h"

// Forward declarations -- each test file provides a suite function
extern void test_notification_suite(void);
extern void test_protocol_suite(void);
extern void test_config_suite(void);
// extern void test_resolver_suite(void);
// extern void test_queue_suite(void);
// extern void test_render_util_suite(void);

// Placeholder suite to verify harness works
static void test_harness_suite(void) {
    ASSERT_TRUE(1, "sanity check");
    ASSERT_EQ(2 + 2, 4, "basic math");
}

int main(void) {
    fprintf(stderr, "lnotify test runner\n");
    fprintf(stderr, "===================\n\n");

    RUN_SUITE(test_harness_suite);
    RUN_SUITE(test_notification_suite);
    RUN_SUITE(test_protocol_suite);
    RUN_SUITE(test_config_suite);

    fprintf(stderr, "\n===================\n");
    fprintf(stderr, "Results: %d passed, %d failed, %d total\n",
            tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
