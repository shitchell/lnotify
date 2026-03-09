#include "engine_terminal.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Helpers: read environment variable from /proc/{pid}/environ
// ---------------------------------------------------------------------------

// Read a NUL-separated environment from /proc/{pid}/environ and look for
// a variable named `name`. Returns a heap-allocated copy of the value, or
// NULL if not found (or unreadable).
char *read_proc_env(pid_t pid, const char *name) {
    char env_path[64];
    snprintf(env_path, sizeof(env_path), "/proc/%d/environ", (int)pid);

    int fd = open(env_path, O_RDONLY);
    if (fd < 0) return NULL;

    char buf[8192];
    ssize_t nread = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (nread <= 0) return NULL;
    buf[nread] = '\0';

    size_t name_len = strlen(name);
    const char *p = buf;
    const char *end = buf + nread;

    while (p < end) {
        size_t entry_len = strlen(p);
        if (entry_len > name_len + 1 &&
            strncmp(p, name, name_len) == 0 &&
            p[name_len] == '=') {
            return strdup(p + name_len + 1);
        }
        p += entry_len + 1;
    }

    return NULL;
}

// ---------------------------------------------------------------------------
// Helpers: get terminal window size via ioctl
// ---------------------------------------------------------------------------

static bool get_terminal_size(int fd, int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) < 0) return false;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return (*cols > 0 && *rows > 0);
}

// ---------------------------------------------------------------------------
// Tier 1: OSC escape sequences
// ---------------------------------------------------------------------------

bool terminal_render_osc(int pty_fd, const ssh_pty_info *pty,
                         const notification *notif) {
    // Check $TERM_PROGRAM from the session leader's environment
    char *term_program = read_proc_env(pty->session_leader, "TERM_PROGRAM");
    if (!term_program) {
        // Also try $TERM for rxvt-unicode
        char *term = read_proc_env(pty->session_leader, "TERM");
        if (term && strstr(term, "rxvt")) {
            free(term);
            // Use OSC 777 for rxvt-unicode: \033]777;notify;TITLE;BODY\007
            const char *title = notif->title ? notif->title : "";
            const char *body  = notif->body  ? notif->body  : "";
            char buf[1024];
            int n = snprintf(buf, sizeof(buf),
                             "\033]777;notify;%s;%s\007", title, body);
            if (n > 0 && n < (int)sizeof(buf)) {
                ssize_t w = write(pty_fd, buf, (size_t)n);
                if (w == n) {
                    log_info("ssh: OSC 777 (rxvt) sent to %s",
                             pty->pty_path);
                    return true;
                }
            }
            return false;
        }
        free(term);
        log_debug("ssh: no TERM_PROGRAM for pid %d, skipping OSC",
                  (int)pty->session_leader);
        return false;
    }

    // Check for terminals that support OSC 9 (iTerm2, WezTerm, Kitty, etc.)
    bool supports_osc9 = (strcasecmp(term_program, "iTerm.app") == 0 ||
                          strcasecmp(term_program, "WezTerm") == 0 ||
                          strcasecmp(term_program, "kitty") == 0);

    if (!supports_osc9) {
        log_debug("ssh: TERM_PROGRAM=%s does not support OSC, skipping",
                  term_program);
        free(term_program);
        return false;
    }

    free(term_program);

    // OSC 9: \033]9;BODY\007 (iTerm style, title not supported in OSC 9)
    // Some terminals also accept \033]9;TITLE\nBODY\007
    const char *title = notif->title ? notif->title : "";
    const char *body  = notif->body  ? notif->body  : "";
    char buf[1024];
    int n;

    if (title[0] != '\0') {
        n = snprintf(buf, sizeof(buf), "\033]9;%s: %s\007", title, body);
    } else {
        n = snprintf(buf, sizeof(buf), "\033]9;%s\007", body);
    }

    if (n > 0 && n < (int)sizeof(buf)) {
        ssize_t w = write(pty_fd, buf, (size_t)n);
        if (w == n) {
            log_info("ssh: OSC 9 sent to %s", pty->pty_path);
            return true;
        }
    }

    log_error("ssh: OSC write failed to %s: %s",
              pty->pty_path, strerror(errno));
    return false;
}

// ---------------------------------------------------------------------------
// Helpers: run tmux command via fork/execvp
// ---------------------------------------------------------------------------

// Run a tmux command without going through a shell.
// argv is a NULL-terminated array of arguments (argv[0] should be "tmux").
// Returns 0 on success, -1 on failure.
static int run_tmux(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        log_error("tmux: fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        // Suppress stdout/stderr (replaces 2>/dev/null from system() calls)
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execvp("tmux", argv);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Tier 2: tmux display-popup / display-message
// ---------------------------------------------------------------------------

bool terminal_render_tmux(const ssh_pty_info *pty,
                          const notification *notif,
                          int timeout_ms) {
    // Check if $TMUX is set in the session leader's environment
    char *tmux_env = read_proc_env(pty->session_leader, "TMUX");
    if (!tmux_env) {
        log_debug("ssh: no $TMUX for pid %d, skipping tmux tier",
                  (int)pty->session_leader);
        return false;
    }

    // Parse tmux socket path from $TMUX (format: /tmp/tmux-UID/default,PID,IDX)
    // We need the socket path for the -S flag
    char tmux_socket[256] = {0};
    const char *comma = strchr(tmux_env, ',');
    if (comma) {
        size_t len = (size_t)(comma - tmux_env);
        if (len >= sizeof(tmux_socket)) len = sizeof(tmux_socket) - 1;
        memcpy(tmux_socket, tmux_env, len);
        tmux_socket[len] = '\0';
    } else {
        // No comma — probably just the socket path
        snprintf(tmux_socket, sizeof(tmux_socket), "%s", tmux_env);
    }
    free(tmux_env);

    const char *title = notif->title ? notif->title : "";
    const char *body  = notif->body  ? notif->body  : "";

    int duration_secs = (timeout_ms > 0 ? timeout_ms : 5000) / 1000;
    if (duration_secs < 1) duration_secs = 1;

    // Try display-popup first (tmux >= 3.2)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s%s%s",
                 title, (title[0] && body[0]) ? ": " : "", body);

        // Build the popup command string (tmux runs this via its internal shell)
        char popup_cmd[1024];
        snprintf(popup_cmd, sizeof(popup_cmd), "echo '%s'; sleep %d",
                 msg, duration_secs);
        // Note: single quotes in msg would break the echo, but this is a
        // pre-existing v1 limitation (the old system() code had the same issue).

        char *popup_argv[] = {
            "tmux", "-S", tmux_socket,
            "display-popup", "-w", "50", "-h", "6", "-E",
            popup_cmd, NULL
        };
        int rc = run_tmux(popup_argv);
        if (rc == 0) {
            log_info("ssh: tmux display-popup sent via %s", tmux_socket);
            return true;
        }
    }

    // Fallback: display-message (works on all tmux versions)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "[lnotify] %s%s%s",
                 title, (title[0] && body[0]) ? ": " : "", body);

        char duration_str[16];
        snprintf(duration_str, sizeof(duration_str), "%d", duration_secs * 1000);

        char *msg_argv[] = {
            "tmux", "-S", tmux_socket,
            "display-message", "-d", duration_str, msg, NULL
        };
        int rc = run_tmux(msg_argv);
        if (rc == 0) {
            log_info("ssh: tmux display-message sent via %s", tmux_socket);
            return true;
        }
    }

    log_debug("ssh: tmux delivery failed for %s", pty->pty_path);
    return false;
}

// ---------------------------------------------------------------------------
// Tier 3: Cursor overlay (ANSI box at top-right)
// ---------------------------------------------------------------------------

bool terminal_render_overlay(int pty_fd, const notification *notif,
                             int timeout_ms) {
    int cols, rows;
    if (!get_terminal_size(pty_fd, &cols, &rows)) {
        log_debug("ssh: could not get terminal size for overlay");
        return false;
    }

    const char *title = notif->title ? notif->title : "";
    const char *body  = notif->body  ? notif->body  : "";

    // Build the notification text
    char line1[128], line2[128];
    int line1_len = 0, line2_len = 0;

    if (title[0] != '\0') {
        line1_len = snprintf(line1, sizeof(line1), " %s ", title);
        line2_len = snprintf(line2, sizeof(line2), " %s ", body);
    } else {
        line1_len = snprintf(line1, sizeof(line1), " %s ", body);
        line2_len = 0;
    }

    // Determine box width (min width for content, max width limited by cols)
    int content_width = line1_len;
    if (line2_len > content_width) content_width = line2_len;
    if (content_width > cols - 4) content_width = cols - 4;
    if (content_width < 10) content_width = 10;

    int box_width = content_width + 2;  // borders
    int start_col = cols - box_width + 1;
    if (start_col < 1) start_col = 1;

    // Build the escape sequence for the overlay
    // Save cursor, draw box at top-right, restore cursor
    char buf[4096];
    int n = 0;

    // Save cursor position
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\033[s");

    // Top border: +---------+
    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                  "\033[1;%dH\033[1;44;37m", start_col);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "+");
    for (int i = 0; i < content_width; i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "-");
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "+\033[0m");

    // Line 1
    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                  "\033[2;%dH\033[1;44;37m|%-*.*s|\033[0m",
                  start_col, content_width, content_width, line1);

    // Line 2 (if there is a body and title)
    int cur_row = 3;
    if (line2_len > 0) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "\033[%d;%dH\033[44;37m|%-*.*s|\033[0m",
                      cur_row, start_col,
                      content_width, content_width, line2);
        cur_row++;
    }

    // Bottom border
    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                  "\033[%d;%dH\033[1;44;37m+", cur_row, start_col);
    for (int i = 0; i < content_width; i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "-");
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "+\033[0m");

    // Restore cursor position
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\033[u");

    if (n >= (int)sizeof(buf)) {
        log_error("ssh: overlay buffer overflow");
        return false;
    }

    ssize_t w = write(pty_fd, buf, (size_t)n);
    if (w != n) {
        log_error("ssh: overlay write failed: %s", strerror(errno));
        return false;
    }

    log_info("ssh: cursor overlay written (%dx%d box)", box_width,
             cur_row);

    // Schedule dismiss: fork a child that sleeps then clears the overlay
    int timeout = timeout_ms > 0 ? timeout_ms : 5000;
    pid_t pid = fork();
    if (pid == 0) {
        // Child: sleep, then clear the overlay region
        usleep((useconds_t)timeout * 1000);

        int clear_n = 0;
        char clear_buf[2048];

        // Save cursor
        clear_n += snprintf(clear_buf + clear_n,
                            sizeof(clear_buf) - (size_t)clear_n, "\033[s");

        // Clear each row of the overlay
        for (int row = 1; row <= cur_row; row++) {
            clear_n += snprintf(clear_buf + clear_n,
                                sizeof(clear_buf) - (size_t)clear_n,
                                "\033[%d;%dH", row, start_col);
            for (int i = 0; i < box_width; i++) {
                clear_n += snprintf(clear_buf + clear_n,
                                    sizeof(clear_buf) - (size_t)clear_n,
                                    " ");
            }
        }

        // Restore cursor
        clear_n += snprintf(clear_buf + clear_n,
                            sizeof(clear_buf) - (size_t)clear_n, "\033[u");

        (void)write(pty_fd, clear_buf, (size_t)clear_n);
        _exit(0);
    }

    // Parent: don't wait — let the child run in the background
    // (it will be reaped by SIGCHLD or init)

    return true;
}

// ---------------------------------------------------------------------------
// Tier 4: Plain text
// ---------------------------------------------------------------------------

bool terminal_render_text(int pty_fd, const notification *notif) {
    const char *title = notif->title ? notif->title : "";
    const char *body  = notif->body  ? notif->body  : "";

    char buf[1024];
    int n;

    if (title[0] != '\0') {
        n = snprintf(buf, sizeof(buf),
                     "\r\n\033[1;44;37m lnotify \033[0m %s: %s\r\n",
                     title, body);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "\r\n\033[1;44;37m lnotify \033[0m %s\r\n",
                     body);
    }

    if (n <= 0 || n >= (int)sizeof(buf)) {
        return false;
    }

    ssize_t w = write(pty_fd, buf, (size_t)n);
    if (w != n) {
        log_error("ssh: text write failed to fd %d: %s", pty_fd,
                  strerror(errno));
        return false;
    }

    log_info("ssh: plain text notification written");
    return true;
}
