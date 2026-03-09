// _GNU_SOURCE needed for struct ucred (SO_PEERCRED)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "socket.h"
#include "lnotify.h"
#include "log.h"
#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Maximum wire message size (64 KiB — generous for text notifications)
#define MAX_MSG_SIZE 65536

int socket_listen(const char *path) {
    if (!path) {
        log_error("socket_listen: NULL path");
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_error("socket(): %s", strerror(errno));
        return -1;
    }

    // Remove stale socket file (ignore ENOENT — file doesn't exist is fine)
    if (unlink(path) < 0 && errno != ENOENT) {
        log_error("unlink(%s): %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        log_error("socket path too long: %s", path);
        close(fd);
        return -1;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("bind(%s): %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        log_error("listen(%s): %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    log_info("listening on %s", path);
    return fd;
}

int socket_handle_client(int client_fd, notification *out) {
    memset(out, 0, sizeof(*out));

    uint8_t buf[MAX_MSG_SIZE];
    ssize_t total = 0;

    // Read all data — client writes then closes (fire-and-forget)
    for (;;) {
        ssize_t n = read(client_fd, buf + total, (size_t)(MAX_MSG_SIZE - total));
        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("read from client: %s", strerror(errno));
            close(client_fd);
            return -1;
        }
        if (n == 0) break;  // client closed connection
        total += n;
        if (total >= MAX_MSG_SIZE) {
            log_error("client message too large (>%d bytes)", MAX_MSG_SIZE);
            close(client_fd);
            return -1;
        }
    }

    if (total == 0) {
        log_debug("client sent empty message");
        close(client_fd);
        return -1;
    }

    // Deserialize
    ssize_t consumed = protocol_deserialize(buf, (size_t)total, out);
    if (consumed < 0) {
        log_error("protocol_deserialize failed");
        close(client_fd);
        return -1;
    }

    // Capture origin_uid via SO_PEERCRED
    struct ucred cred;
    socklen_t cred_len = sizeof(cred);
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) == 0) {
        out->origin_uid = (uint32_t)cred.uid;
        log_debug("client uid=%u pid=%d", cred.uid, cred.pid);
    } else {
        log_error("getsockopt(SO_PEERCRED): %s", strerror(errno));
        out->origin_uid = 0;
    }

    // Set daemon-side timestamps
    out->ts_received = wallclock_ms();
    out->ts_mono = monotonic_ms();

    // Log the notification
    log_info("received notification: title=\"%s\" body=\"%s\" priority=%u timeout=%d app=\"%s\" group=\"%s\" uid=%u",
             out->title ? out->title : "(none)",
             out->body ? out->body : "(none)",
             out->priority,
             out->timeout_ms,
             out->app ? out->app : "(none)",
             out->group_id ? out->group_id : "(none)",
             out->origin_uid);

    close(client_fd);
    return 0;
}

const char *socket_default_path(bool system_mode) {
    static char path_buf[256];

    if (system_mode) {
        return "/run/lnotify.sock";
    }

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] != '\0') {
        snprintf(path_buf, sizeof(path_buf), "%s/lnotify.sock", xdg);
    } else {
        log_debug("XDG_RUNTIME_DIR not set, falling back to /tmp");
        snprintf(path_buf, sizeof(path_buf), "/tmp/lnotify.sock");
    }
    return path_buf;
}
