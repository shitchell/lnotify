#include "sanitize.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>

void test_sanitize_suite(void) {
    // Test: normal ASCII passes through unchanged
    {
        char *out = sanitize_for_terminal("Hello, world!");
        ASSERT_NOT_NULL(out, "normal ASCII: non-NULL");
        ASSERT_STR_EQ(out, "Hello, world!", "normal ASCII: unchanged");
        free(out);
    }

    // Test: newlines are preserved
    {
        char *out = sanitize_for_terminal("line1\nline2\nline3");
        ASSERT_NOT_NULL(out, "newlines: non-NULL");
        ASSERT_STR_EQ(out, "line1\nline2\nline3", "newlines: preserved");
        free(out);
    }

    // Test: ESC (0x1B) is stripped
    {
        char *out = sanitize_for_terminal("before\033after");
        ASSERT_NOT_NULL(out, "ESC byte: non-NULL");
        ASSERT_STR_EQ(out, "beforeafter", "ESC byte: stripped");
        free(out);
    }

    // Test: ANSI color sequence stripped
    {
        char *out = sanitize_for_terminal("red\033[31mtext\033[0m");
        ASSERT_NOT_NULL(out, "ANSI color: non-NULL");
        ASSERT_STR_EQ(out, "red[31mtext[0m", "ANSI color: ESC stripped");
        free(out);
    }

    // Test: OSC sequence stripped (title injection)
    {
        char *out = sanitize_for_terminal("\033]0;evil title\007rest");
        ASSERT_NOT_NULL(out, "OSC: non-NULL");
        ASSERT_STR_EQ(out, "]0;evil titlerest", "OSC: ESC and BEL stripped");
        free(out);
    }

    // Test: DEL (0x7F) is stripped
    {
        char *out = sanitize_for_terminal("abc\x7F" "def");
        ASSERT_NOT_NULL(out, "DEL: non-NULL");
        ASSERT_STR_EQ(out, "abcdef", "DEL: stripped");
        free(out);
    }

    // Test: all control chars 0x01-0x1F (except 0x0A) are stripped
    {
        // Build a string with all control chars 0x01-0x1F
        char input[33];
        for (int i = 1; i <= 0x1F; i++) {
            input[i - 1] = (char)i;
        }
        input[0x1F] = 'X';
        input[0x20] = '\0';

        char *out = sanitize_for_terminal(input);
        ASSERT_NOT_NULL(out, "control chars: non-NULL");
        // Only 0x0A (\n) and 'X' should remain
        ASSERT_EQ(strlen(out), 2, "control chars: only newline + X remain");
        ASSERT_EQ(out[0], '\n', "control chars: newline preserved");
        ASSERT_EQ(out[1], 'X', "control chars: X preserved");
        free(out);
    }

    // Test: NULL input returns NULL
    {
        char *out = sanitize_for_terminal(NULL);
        ASSERT_NULL(out, "NULL input: returns NULL");
    }

    // Test: empty string returns empty string
    {
        char *out = sanitize_for_terminal("");
        ASSERT_NOT_NULL(out, "empty input: non-NULL");
        ASSERT_STR_EQ(out, "", "empty input: empty result");
        free(out);
    }

    // Test: string with only control chars becomes empty
    {
        char *out = sanitize_for_terminal("\x01\x02\x03\x1B\x7F");
        ASSERT_NOT_NULL(out, "all-control: non-NULL");
        ASSERT_STR_EQ(out, "", "all-control: empty result");
        free(out);
    }

    // Test: high bytes (0x80+) pass through (UTF-8 safe)
    {
        char *out = sanitize_for_terminal("caf\xC3\xA9");  // "café" in UTF-8
        ASSERT_NOT_NULL(out, "UTF-8: non-NULL");
        ASSERT_STR_EQ(out, "caf\xC3\xA9", "UTF-8: high bytes preserved");
        free(out);
    }

    // Test: mixed content
    {
        char *out = sanitize_for_terminal("Hello\033[1mWorld\n\x07" "Bye\x7F");
        ASSERT_NOT_NULL(out, "mixed: non-NULL");
        ASSERT_STR_EQ(out, "Hello[1mWorld\nBye", "mixed: correct sanitization");
        free(out);
    }

    // Test: carriage return (0x0D) is stripped
    {
        char *out = sanitize_for_terminal("line1\r\nline2");
        ASSERT_NOT_NULL(out, "CR: non-NULL");
        ASSERT_STR_EQ(out, "line1\nline2", "CR: stripped, LF preserved");
        free(out);
    }

    // Test: tab (0x09) is stripped
    {
        char *out = sanitize_for_terminal("col1\tcol2");
        ASSERT_NOT_NULL(out, "tab: non-NULL");
        ASSERT_STR_EQ(out, "col1col2", "tab: stripped");
        free(out);
    }
}
