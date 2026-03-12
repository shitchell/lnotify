#include "strutil.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>

void test_strutil_suite(void) {
    // Test: replace non-NULL with non-NULL
    {
        char *s = strdup("old");
        int rc = replace_str(&s, "new");
        ASSERT_EQ(rc, 0, "non-NULL -> non-NULL: returns 0");
        ASSERT_NOT_NULL(s, "non-NULL -> non-NULL: result non-NULL");
        ASSERT_STR_EQ(s, "new", "non-NULL -> non-NULL: value updated");
        free(s);
    }

    // Test: replace non-NULL with NULL
    {
        char *s = strdup("old");
        int rc = replace_str(&s, NULL);
        ASSERT_EQ(rc, 0, "non-NULL -> NULL: returns 0");
        ASSERT_NULL(s, "non-NULL -> NULL: result is NULL");
    }

    // Test: replace NULL with non-NULL
    {
        char *s = NULL;
        int rc = replace_str(&s, "hello");
        ASSERT_EQ(rc, 0, "NULL -> non-NULL: returns 0");
        ASSERT_NOT_NULL(s, "NULL -> non-NULL: result non-NULL");
        ASSERT_STR_EQ(s, "hello", "NULL -> non-NULL: value set");
        free(s);
    }

    // Test: replace NULL with NULL (no-op)
    {
        char *s = NULL;
        int rc = replace_str(&s, NULL);
        ASSERT_EQ(rc, 0, "NULL -> NULL: returns 0");
        ASSERT_NULL(s, "NULL -> NULL: result still NULL");
    }

    // Test: empty string is a valid replacement
    {
        char *s = strdup("something");
        int rc = replace_str(&s, "");
        ASSERT_EQ(rc, 0, "non-NULL -> empty: returns 0");
        ASSERT_NOT_NULL(s, "non-NULL -> empty: result non-NULL");
        ASSERT_STR_EQ(s, "", "non-NULL -> empty: value is empty");
        free(s);
    }

    // Test: result is an independent copy (not aliased)
    {
        const char *src = "original";
        char *s = NULL;
        replace_str(&s, src);
        ASSERT_TRUE(s != src, "independent copy: pointers differ");
        ASSERT_STR_EQ(s, src, "independent copy: values match");
        free(s);
    }
}
