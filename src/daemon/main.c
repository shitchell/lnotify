#include "config.h"
#include "engine.h"
#include "engine_dbus.h"
#include "engine_fb.h"
#include "engine_queue.h"
#include "engine_terminal.h"
#include "font.h"
#include "log.h"
#include "logind.h"
#include "protocol.h"
#include "queue.h"
#include "resolver.h"
#include "socket.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
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
static lnotify_config g_config;

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
// Probe name lookup
// ---------------------------------------------------------------------------

static const char *probe_name(probe_key key) {
    switch (key) {
    case PROBE_HAS_DBUS_NOTIFICATIONS: return "HAS_DBUS_NOTIFICATIONS";
    case PROBE_COMPOSITOR_NAME:        return "COMPOSITOR_NAME";
    case PROBE_HAS_FRAMEBUFFER:        return "HAS_FRAMEBUFFER";
    case PROBE_TERMINAL_CAPABILITIES:  return "TERMINAL_CAPABILITIES";
    case PROBE_FOREGROUND_PROCESS:     return "FOREGROUND_PROCESS";
    default:                           return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// Dry-run handler
// ---------------------------------------------------------------------------

// Handle a dry-run request: run the engine resolver without rendering,
// format diagnostic output, and write it back to the client socket.
static void handle_dry_run(int client_fd, notification *notif,
                            session_context *ctx) {
    (void)notif;  // notification content not needed for dry-run output

    char response[4096];
    int pos = 0;
    int remaining = (int)sizeof(response);

    // VT info
    if (ctx->vt > 0) {
        pos += snprintf(response + pos, (size_t)remaining,
                        "VT: tty%u\n", ctx->vt);
    } else {
        pos += snprintf(response + pos, (size_t)remaining,
                        "VT: (unknown)\n");
    }
    remaining = (int)sizeof(response) - pos;

    // Session info
    {
        const char *stype = ctx->session_type ? ctx->session_type : "(unknown)";
        const char *sclass = ctx->session_class ? ctx->session_class : "(unknown)";
        const char *username = ctx->username ? ctx->username : "(unknown)";
        pos += snprintf(response + pos, (size_t)remaining,
                        "Session: %s, %s, uid=%u, user=%s\n",
                        stype, sclass, ctx->uid, username);
        remaining = (int)sizeof(response) - pos;
    }

    // Probes run
    {
        pos += snprintf(response + pos, (size_t)remaining, "Probes run:");
        remaining = (int)sizeof(response) - pos;

        // Save original probe state to detect what resolver adds
        uint32_t probes_before = ctx->probes_completed;

        // Run resolver to select an engine (dry-run — we won't call render)
        engine *eng = resolver_select(engines, engine_count, ctx, NULL);

        // Report all completed probes (including newly run ones)
        bool any_probe = false;
        for (int k = 0; k < PROBE_COUNT; k++) {
            if (ctx->probes_completed & (1u << k)) {
                const char *pname = probe_name((probe_key)k);
                // Show probe result
                const char *val = "?";
                switch ((probe_key)k) {
                case PROBE_HAS_DBUS_NOTIFICATIONS:
                    val = ctx->has_dbus_notifications ? "true" : "false";
                    break;
                case PROBE_HAS_FRAMEBUFFER:
                    val = ctx->has_framebuffer ? "true" : "false";
                    break;
                case PROBE_COMPOSITOR_NAME:
                    val = ctx->compositor_name ? ctx->compositor_name : "(null)";
                    break;
                case PROBE_TERMINAL_CAPABILITIES:
                    val = ctx->terminal_type ? ctx->terminal_type : "(null)";
                    break;
                case PROBE_FOREGROUND_PROCESS:
                    val = ctx->foreground_process ? ctx->foreground_process : "(null)";
                    break;
                default:
                    break;
                }
                pos += snprintf(response + pos, (size_t)(remaining),
                                " %s=%s", pname, val);
                remaining = (int)sizeof(response) - pos;
                any_probe = true;
            }
        }
        if (!any_probe) {
            pos += snprintf(response + pos, (size_t)remaining, " (none)");
            remaining = (int)sizeof(response) - pos;
        }
        pos += snprintf(response + pos, (size_t)remaining, "\n");
        remaining = (int)sizeof(response) - pos;

        // Restore original probe state (dry-run should be side-effect-free
        // for subsequent notifications — probes will be re-run as needed)
        ctx->probes_completed = probes_before;

        // Engine selected
        pos += snprintf(response + pos, (size_t)remaining,
                        "Engine selected: %s\n",
                        eng ? eng->name : "(none — would queue)");
        remaining = (int)sizeof(response) - pos;
    }

    // SSH targets
    {
        ssh_pty_info ptys[64];
        int ssh_count = ssh_find_qualifying_ptys(&g_config, ptys, 64);
        if (ssh_count > 0) {
            for (int i = 0; i < ssh_count; i++) {
                // Determine which modes would be used
                const char *modes = g_config.ssh_modes ? g_config.ssh_modes : "osc,overlay,text";
                pos += snprintf(response + pos, (size_t)remaining,
                                "SSH target: %s@%s (modes: %s)\n",
                                ptys[i].username, ptys[i].pty_path, modes);
                remaining = (int)sizeof(response) - pos;
            }
        } else {
            pos += snprintf(response + pos, (size_t)remaining,
                            "SSH targets: (none)\n");
            remaining = (int)sizeof(response) - pos;
        }
    }

    // Write response back to client
    ssize_t total_written = 0;
    while (total_written < pos) {
        ssize_t n = write(client_fd, response + total_written,
                          (size_t)(pos - total_written));
        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("dry-run write: %s", strerror(errno));
            break;
        }
        total_written += n;
    }

    log_info("dry-run response sent (%d bytes)", pos);
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
    const char *config_path_override = NULL;
    const char *socket_path_override = NULL;

    // First pass: extract --config, --system, --debug (needed before config load)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            log_debug_enabled = true;
        } else if (strcmp(argv[i], "--system") == 0) {
            system_mode = true;
        } else if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --config requires an argument\n");
                return 1;
            }
            config_path_override = argv[++i];
        } else if (strcmp(argv[i], "--socket") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --socket requires an argument\n");
                return 1;
            }
            socket_path_override = argv[++i];
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("lnotifyd %s\n", LNOTIFY_VERSION);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            fprintf(stderr, "usage: lnotifyd [--debug] [--system] [--config PATH] [--socket PATH] [--version]\n");
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

    // Load configuration: --config overrides --system default
    if (config_defaults(&g_config) < 0) {
        fprintf(stderr, "error: config allocation failed\n");
        return 1;
    }
    if (config_path_override) {
        if (config_load(&g_config, config_path_override) != 0) {
            fprintf(stderr, "error: cannot load config '%s'\n",
                    config_path_override);
            return 1;
        }
    } else if (system_mode) {
        // Non-fatal: system config is optional
        if (access("/etc/lnotify.conf", R_OK) == 0) {
            config_load(&g_config, "/etc/lnotify.conf");
        } else {
            log_debug("no config file at /etc/lnotify.conf, using defaults");
        }
    } else {
        // Try $XDG_CONFIG_HOME/lnotify/config, then ~/.config/lnotify/config
        const char *xdg_config = getenv("XDG_CONFIG_HOME");
        char config_path[512];
        if (xdg_config && *xdg_config) {
            snprintf(config_path, sizeof(config_path),
                     "%s/lnotify/config", xdg_config);
        } else {
            const char *home = getenv("HOME");
            snprintf(config_path, sizeof(config_path),
                     "%s/.config/lnotify/config", home ? home : "/tmp");
        }
        // Non-fatal: config file is optional
        if (access(config_path, R_OK) == 0) {
            config_load(&g_config, config_path);
        } else {
            log_debug("no config file at %s, using defaults", config_path);
        }
    }

    // Initialize font backend (after config so font settings are available)
    font_init(g_config.font_name, g_config.font_path);

    // Log engine registry
    log_info("registered %d engines:", engine_count);
    for (int i = 0; i < engine_count; i++) {
        log_info("  [%d] %s (priority %d)", i, engines[i].name,
                 engines[i].priority);
    }

    // Initialize notification queue
    queue_init(&g_queue);

    // Determine socket path: CLI --socket > config socket_path > auto-detect
    const char *resolved_socket;
    if (socket_path_override) {
        resolved_socket = socket_path_override;
    } else if (g_config.socket_path) {
        resolved_socket = g_config.socket_path;
    } else {
        resolved_socket = socket_default_path(system_mode);
    }
    g_socket_path = strdup(resolved_socket);
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
            uint16_t field_mask = 0;
            if (socket_handle_client(client_fd, &notif, &field_mask) == 0) {

                if (field_mask & FIELD_DRY_RUN) {
                    // Dry-run: run resolver but don't render — send diagnostic
                    // text back to client
                    log_info("dry-run request received");
                    handle_dry_run(client_fd, &notif, &ctx);
                } else {
                    // Normal dispatch
                    dispatch_notification(&notif, &ctx);

                    // SSH delivery is additive — runs alongside primary engine
                    int ssh_count = ssh_deliver(&notif, &g_config);
                    if (ssh_count > 0) {
                        log_info("ssh: delivered to %d session(s)", ssh_count);
                    }
                }

                close(client_fd);
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

    // Clean up font backend
    font_cleanup();

    // Free config
    config_free(&g_config);

    // Close shared sd-bus connection
    logind_close();

    // Remove socket file
    cleanup();
    free((void *)g_socket_path);
    g_socket_path = NULL;

    log_info("lnotifyd exiting");
    return 0;
}
