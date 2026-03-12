#ifndef LNOTIFY_SOCKET_H
#define LNOTIFY_SOCKET_H

#include "lnotify.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

// Create AF_UNIX socket, bind, listen. Returns socket fd, or -1 on error.
// Removes stale socket file if it exists.
int socket_listen(const char *path);

// Read message from client, deserialize, capture origin_uid via SO_PEERCRED,
// set ts_received and ts_mono. Logs the notification.
// On success, populates *out and returns 0 (caller must notification_free
// and close client_fd). If out_field_mask is non-NULL, the raw field_mask
// from the wire message is written there (for detecting transport flags
// like FIELD_DRY_RUN).
// On failure, closes client_fd, returns -1 and *out is zeroed.
int socket_handle_client(int client_fd, notification *out,
                          uint16_t *out_field_mask);

// Return default socket path. system_mode=true: "/run/lnotify.sock".
// Otherwise: "$XDG_RUNTIME_DIR/lnotify.sock" (fallback "/tmp/lnotify.sock").
// Returns pointer to static buffer — not thread-safe, do not free.
const char *socket_default_path(bool system_mode);

// Write all bytes to fd, retrying on EINTR. Returns len on success, -1 on error.
ssize_t write_all(int fd, const void *buf, size_t len);

#endif
