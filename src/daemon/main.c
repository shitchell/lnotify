#include "log.h"
#include "socket.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;
static const char *g_socket_path = NULL;

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

    // Determine socket path
    const char *path = socket_default_path(system_mode);
    // Copy to heap so cleanup can use it after static buffer is gone
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
    sa.sa_flags = 0;  // no SA_RESTART — we want accept() to be interrupted
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Start listening
    int server_fd = socket_listen(g_socket_path);
    if (server_fd < 0) {
        free((void *)g_socket_path);
        return 1;
    }

    log_info("lnotifyd ready (pid %d)", getpid());

    // Accept loop — Task 14 will replace this with poll()-based event loop
    while (g_running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (!g_running) break;  // interrupted by signal
            log_error("accept: %s", strerror(errno));
            continue;
        }
        socket_handle_client(client_fd);
    }

    // Cleanup
    close(server_fd);
    cleanup();
    free((void *)g_socket_path);
    g_socket_path = NULL;

    log_info("lnotifyd exiting");
    return 0;
}
