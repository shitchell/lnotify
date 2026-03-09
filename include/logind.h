#ifndef LNOTIFY_LOGIND_H
#define LNOTIFY_LOGIND_H

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

// Session info from logind (heap-allocated strings, caller must free via logind_session_free)
typedef struct {
    char    *session_id;    // e.g. "36625"
    char    *type;          // "wayland", "x11", "tty"
    char    *session_class; // "user", "greeter"
    char    *username;
    char    *seat;          // "seat0"
    char    *tty;           // "pts/3" (remote sessions)
    uint32_t uid;
    uint32_t leader_pid;
    uint32_t vt;
    bool     remote;
} logind_session;

// Find the session on a given VT. Returns 0 on success, -1 if none found.
// Populates *out with heap-allocated strings.
int logind_get_session_by_vt(uint32_t vt, logind_session *out);

// List remote sessions. Fills out[] up to max entries.
// Returns count of sessions found (0 if none). Each entry has heap-allocated strings.
int logind_list_remote_sessions(logind_session *out, int max);

// Free heap-allocated strings in a logind_session. Safe to call multiple times.
void logind_session_free(logind_session *s);

// Close the cached system bus connection. Call at daemon shutdown.
void logind_close(void);

#endif
