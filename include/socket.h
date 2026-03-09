#ifndef LNOTIFY_SOCKET_H
#define LNOTIFY_SOCKET_H

#include <stdbool.h>

// Create AF_UNIX socket, bind, listen. Returns socket fd, or -1 on error.
// Removes stale socket file if it exists.
int socket_listen(const char *path);

// Read message from client, deserialize, capture origin_uid via SO_PEERCRED,
// set ts_received and ts_mono. Logs the notification, then frees and closes.
void socket_handle_client(int client_fd);

// Return default socket path. system_mode=true: "/run/lnotify.sock".
// Otherwise: "$XDG_RUNTIME_DIR/lnotify.sock" (fallback "/tmp/lnotify.sock").
// Returns pointer to static buffer — not thread-safe, do not free.
const char *socket_default_path(bool system_mode);

#endif
