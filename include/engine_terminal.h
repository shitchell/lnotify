#ifndef LNOTIFY_ENGINE_TERMINAL_H
#define LNOTIFY_ENGINE_TERMINAL_H

#include "config.h"
#include "engine.h"
#include "lnotify.h"

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// SSH session info for a qualifying pty
// ---------------------------------------------------------------------------

typedef struct {
    char    pty_path[64];       // e.g. "/dev/pts/3"
    pid_t   session_leader;     // PID of the session leader (sshd child)
    uid_t   uid;                // user who owns the session
    char    username[64];       // resolved username
} ssh_pty_info;

// ---------------------------------------------------------------------------
// SSH delivery (called by daemon after primary engine dispatch)
// ---------------------------------------------------------------------------

// Find qualifying SSH ptys and deliver a notification to each one.
// This is additive — runs alongside the primary engine, not instead of it.
// Returns the number of ptys notified (0 if none qualifying).
int ssh_deliver(const notification *notif, const lnotify_config *cfg);

// ---------------------------------------------------------------------------
// SSH session discovery
// ---------------------------------------------------------------------------

// Query loginctl for remote sessions, filter by ssh_users/ssh_groups config.
// Check LNOTIFY_SSH env var in /proc/{pid}/environ.
// Returns count of qualifying ptys. Fills ptys[] up to max_ptys.
int ssh_find_qualifying_ptys(const lnotify_config *cfg,
                             ssh_pty_info *ptys, int max_ptys);

// Check if a fullscreen app is running on the given pty.
// Reads /proc/{pid}/stat field 8 (tpgid) and matches against
// ssh_fullscreen_apps config list.
bool ssh_check_fullscreen(const ssh_pty_info *pty,
                          const lnotify_config *cfg);

// ---------------------------------------------------------------------------
// Terminal rendering tiers (called per qualifying pty)
// ---------------------------------------------------------------------------

// Tier 1: OSC escape sequences (OSC 9 / OSC 777)
// Returns true if the terminal supports OSC and the write succeeded.
bool terminal_render_osc(int pty_fd, const ssh_pty_info *pty,
                         const notification *notif);

// Tier 2: tmux display-popup / display-message
// Returns true if the session is inside tmux and the command succeeded.
bool terminal_render_tmux(const ssh_pty_info *pty,
                          const notification *notif,
                          int timeout_ms);

// Tier 3: Cursor overlay (ANSI-colored box at top-right)
// Returns true if written successfully.
// Skipped if fullscreen app detected and ssh_notify_over_fullscreen=false.
bool terminal_render_overlay(int pty_fd, const notification *notif,
                             int timeout_ms);

// Tier 4: Plain text (styled ANSI line)
// Always succeeds if the fd is writable.
bool terminal_render_text(int pty_fd, const notification *notif);

#endif
