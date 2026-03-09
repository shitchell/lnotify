#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Minimal test framework -- no dependencies
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        tests_failed++; \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, msg); \
    } \
} while(0)

#define ASSERT_EQ(a, b, msg) ASSERT((a) == (b), msg)
#define ASSERT_STR_EQ(a, b, msg) ASSERT(strcmp((a), (b)) == 0, msg)
#define ASSERT_TRUE(a, msg) ASSERT((a), msg)
#define ASSERT_FALSE(a, msg) ASSERT(!(a), msg)
#define ASSERT_NULL(a, msg) ASSERT((a) == NULL, msg)
#define ASSERT_NOT_NULL(a, msg) ASSERT((a) != NULL, msg)

#define RUN_SUITE(name) do { \
    fprintf(stderr, "Suite: %s\n", #name); \
    name(); \
} while(0)

// Forward declarations -- each test file provides a suite function
// Uncomment as test files are added:
// extern void test_protocol_suite(void);
// extern void test_config_suite(void);
// extern void test_resolver_suite(void);
// extern void test_queue_suite(void);
// extern void test_notification_suite(void);
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

    fprintf(stderr, "\n===================\n");
    fprintf(stderr, "Results: %d passed, %d failed, %d total\n",
            tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
