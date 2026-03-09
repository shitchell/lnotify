#include "engine.h"
#include "engine_dbus.h"
#include "engine_fb.h"
#include "engine_queue.h"
#include "log.h"
#include "queue.h"
#include "resolver.h"
#include "socket.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_running = 1;
static const char *g_socket_path = NULL;

// Engine registry — built at startup from extern engine structs.
// Priority order (lowest index = highest priority).
// Terminal engine (Task 15) will be inserted between framebuffer and queue.
// Note: this must be initialized at runtime because the engine structs are
// extern globals and C does not allow non-constant initializers for file-scope
// arrays. See init_engines().
static engine engines[MAX_ENGINES];
static int engine_count = 0;

// Active engine (currently rendering a notification)
static engine *g_active_engine = NULL;

// VT sysfs file path
#define VT_SYSFS_PATH "/sys/class/tty/tty0/active"

// Poll timeout in ms — allows periodic housekeeping even with no events
#define POLL_TIMEOUT_MS 5000

// ---------------------------------------------------------------------------
// Engine initialization
// ---------------------------------------------------------------------------

static void init_engines(void) {
    engines[0] = engine_dbus;        // priority 10: GUI sessions
    engines[1] = engine_framebuffer; // priority 50: raw TTY framebuffer
    engines[2] = engine_queue;       // priority 100: universal fallback
    engine_count = 3;
}

// ---------------------------------------------------------------------------
// Signal handler
// ---------------------------------------------------------------------------

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void cleanup(void) {
    if (g_socket_path) {
        unlink(g_socket_path);
        log_info("removed socket %s", g_socket_path);
    }
}

// ---------------------------------------------------------------------------
// VT reading
// ---------------------------------------------------------------------------

// Read the current VT number from the sysfs file.
// Returns 0 on failure.
static uint32_t read_vt_number(int sysfs_fd) {
    char buf[32];

    if (lseek(sysfs_fd, 0, SEEK_SET) < 0) {
        log_error("lseek on sysfs: %s", strerror(errno));
        return 0;
    }

    ssize_t n = read(sysfs_fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        log_error("read sysfs: %s", n < 0 ? strerror(errno) : "empty");
        return 0;
    }
    buf[n] = '\0';

    // File contains e.g. "tty3\n" — extract the number after "tty"
    const char *p = buf;
    if (strncmp(p, "tty", 3) == 0)
        p += 3;

    uint32_t vt = (uint32_t)atoi(p);
    return vt;
}

// ---------------------------------------------------------------------------
// Notification dispatch
// ---------------------------------------------------------------------------

// Dispatch a notification through the engine resolver. If an engine accepts,
// render the notification. Otherwise push to queue.
static void dispatch_notification(notification *notif, session_context *ctx) {
    engine *eng = resolver_select(engines, engine_count, ctx, NULL);

    if (eng) {
        log_info("dispatching via engine '%s'", eng->name);
        bool ok = eng->render(notif, ctx);
        if (ok) {
            g_active_engine = eng;
        } else {
            log_error("engine '%s' render failed, queueing", eng->name);
            queue_push(&g_queue, notif);
        }
    } else {
        log_info("no engine available, notification queued");
        queue_push(&g_queue, notif);
    }
}

// Drain the queue: pop live notifications and dispatch through the resolver.
static void drain_queue(session_context *ctx) {
    notification *n;
    int count = 0;

    while ((n = queue_pop_live(&g_queue)) != NULL) {
        log_info("draining queued notification: \"%s\"",
                 n->body ? n->body : "(none)");
        dispatch_notification(n, ctx);
        notification_free(n);
        free(n);
        count++;
    }

    if (count > 0) {
        log_info("drained %d notification(s) from queue", count);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    bool system_mode = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            log_debug_enabled = true;
        } else if (strcmp(argv[i], "--system") == 0) {
            system_mode = true;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            fprintf(stderr, "usage: lnotifyd [--debug] [--system]\n");
            return 1;
        }
    }

    log_info("lnotifyd starting");
    log_debug("debug logging enabled");

    if (system_mode) {
        log_info("system mode enabled");
    }

    // Initialize engine registry
    init_engines();

    // Log engine registry
    log_info("registered %d engines:", engine_count);
    for (int i = 0; i < engine_count; i++) {
        log_info("  [%d] %s (priority %d)", i, engines[i].name,
                 engines[i].priority);
    }

    // Initialize notification queue
    queue_init(&g_queue);

    // Determine socket path
    const char *path = socket_default_path(system_mode);
    g_socket_path = strdup(path);
    if (!g_socket_path) {
        log_error("strdup failed");
        return 1;
    }

    // Set up signal handlers for clean shutdown
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART — we want poll() to be interrupted
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Start listening on Unix socket
    int server_fd = socket_listen(g_socket_path);
    if (server_fd < 0) {
        free((void *)g_socket_path);
        queue_destroy(&g_queue);
        return 1;
    }

    // Open sysfs VT file for poll()-based VT switch detection
    int vt_fd = open(VT_SYSFS_PATH, O_RDONLY);
    bool have_vt_monitor = (vt_fd >= 0);

    if (!have_vt_monitor) {
        log_info("could not open %s: %s (VT monitoring disabled)",
                 VT_SYSFS_PATH, strerror(errno));
    }

    // Read initial VT number and build initial session context
    uint32_t current_vt = 0;
    session_context ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (have_vt_monitor) {
        current_vt = read_vt_number(vt_fd);
        log_info("initial VT: %u", current_vt);
        context_init_from_logind(&ctx, current_vt);
    } else {
        // No VT monitoring — initialize empty context
        context_init_from_logind(&ctx, 0);
    }

    log_info("lnotifyd ready (pid %d)", getpid());

    // Set up poll array:
    //   fds[0] = server socket (POLLIN for new client connections)
    //   fds[1] = sysfs VT file (POLLPRI|POLLERR for VT switch events)
    enum { POLL_SOCKET = 0, POLL_SYSFS = 1, POLL_COUNT = 2 };
    struct pollfd fds[POLL_COUNT];

    fds[POLL_SOCKET].fd = server_fd;
    fds[POLL_SOCKET].events = POLLIN;

    fds[POLL_SYSFS].fd = have_vt_monitor ? vt_fd : -1;
    fds[POLL_SYSFS].events = POLLPRI | POLLERR;

    int nfds = have_vt_monitor ? POLL_COUNT : 1;

    // Do an initial read to consume the sysfs file content (required before
    // poll() will report changes — sysfs needs the first read consumed).
    if (have_vt_monitor) {
        char discard[32];
        lseek(vt_fd, 0, SEEK_SET);
        (void)read(vt_fd, discard, sizeof(discard));
    }

    // -----------------------------------------------------------------------
    // Event loop
    // -----------------------------------------------------------------------
    while (g_running) {
        int ret = poll(fds, (nfds_t)nfds, POLL_TIMEOUT_MS);

        if (ret < 0) {
            if (errno == EINTR) {
                // Signal received — check g_running
                continue;
            }
            log_error("poll: %s", strerror(errno));
            break;
        }

        if (ret == 0) {
            // Timeout — opportunity for periodic housekeeping
            log_debug("poll timeout (housekeeping)");
            continue;
        }

        // Check for VT switch (sysfs activity)
        if (have_vt_monitor &&
            (fds[POLL_SYSFS].revents & (POLLPRI | POLLERR))) {

            uint32_t new_vt = read_vt_number(vt_fd);

            if (new_vt != 0 && new_vt != current_vt) {
                log_info("VT switch: %u -> %u", current_vt, new_vt);
                current_vt = new_vt;

                // Dismiss current notification (if any active engine)
                if (g_active_engine) {
                    log_debug("dismissing active engine '%s'",
                              g_active_engine->name);
                    g_active_engine->dismiss();
                    g_active_engine = NULL;
                }

                // Rebuild session context for new VT
                context_free(&ctx);
                context_init_from_logind(&ctx, current_vt);

                // Drain the queue through the engine resolver
                drain_queue(&ctx);
            } else if (new_vt == 0) {
                log_error("failed to read VT number after sysfs event");
            }
            // else: same VT, spurious wakeup — ignore
        }

        // Check for incoming client connections
        if (fds[POLL_SOCKET].revents & POLLIN) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (!g_running) break;
                log_error("accept: %s", strerror(errno));
                continue;
            }

            notification notif;
            if (socket_handle_client(client_fd, &notif) == 0) {
                dispatch_notification(&notif, &ctx);
                notification_free(&notif);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Cleanup
    // -----------------------------------------------------------------------

    // Dismiss any active notification
    if (g_active_engine) {
        g_active_engine->dismiss();
        g_active_engine = NULL;
    }

    // Free session context
    context_free(&ctx);

    // Close fds
    close(server_fd);
    if (have_vt_monitor) {
        close(vt_fd);
    }

    // Destroy queue (frees any remaining notifications)
    queue_destroy(&g_queue);

    // Remove socket file
    cleanup();
    free((void *)g_socket_path);
    g_socket_path = NULL;

    log_info("lnotifyd exiting");
    return 0;
}
