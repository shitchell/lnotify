#include "engine_terminal.h"
#include "log.h"
#include "logind.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Maximum number of SSH ptys we'll deliver to per notification
#define MAX_SSH_PTYS 64

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Check if a word (whitespace or comma delimited) appears in a list string.
// Returns true if `word` is found as a whole token in `list`.
static bool word_in_list(const char *word, const char *list) {
    if (!word || !list || !*word || !*list) return false;

    size_t wlen = strlen(word);
    const char *p = list;

    while (*p) {
        // Skip delimiters
        while (*p && (*p == ' ' || *p == ',' || *p == '\t')) p++;
        if (!*p) break;

        // Find end of current token
        const char *start = p;
        while (*p && *p != ' ' && *p != ',' && *p != '\t') p++;
        size_t tlen = (size_t)(p - start);

        if (tlen == wlen && strncmp(start, word, wlen) == 0) {
            return true;
        }
    }

    return false;
}

// read_proc_env() is declared in engine_terminal.h and implemented in
// engine_terminal.c — shared between this file and the terminal renderer.

// Check if a user is in a given group (by group name).
static bool user_in_group(const char *username, const char *groupname) {
    struct group *grp = getgrnam(groupname);
    if (!grp) return false;

    for (char **member = grp->gr_mem; *member; member++) {
        if (strcmp(*member, username) == 0) return true;
    }

    return false;
}

// Check if a user qualifies based on ssh_users and ssh_groups config.
// For per-user daemon (running as same user), always qualifies.
// For system daemon, must match ssh_users or ssh_groups.
static bool user_qualifies(const char *username, uid_t uid,
                           const lnotify_config *cfg) {
    // Per-user daemon: only our own sessions are visible
    if ((uid_t)getuid() == uid) {
        return true;
    }

    // System daemon: check ssh_users and ssh_groups
    if (cfg->ssh_users && *cfg->ssh_users &&
        word_in_list(username, cfg->ssh_users)) {
        return true;
    }

    if (cfg->ssh_groups && *cfg->ssh_groups) {
        // Check each group in the list
        char *groups_copy = strdup(cfg->ssh_groups);
        if (groups_copy) {
            char *saveptr = NULL;
            char *grp = strtok_r(groups_copy, " ,\t", &saveptr);
            while (grp) {
                if (user_in_group(username, grp)) {
                    free(groups_copy);
                    return true;
                }
                grp = strtok_r(NULL, " ,\t", &saveptr);
            }
            free(groups_copy);
        }
    }

    return false;
}

// Parse the LNOTIFY_SSH environment variable from a session.
// Returns a heap-allocated string of allowed modes, or NULL for "all".
// Returns strdup("none") for opt-out.
static char *get_lnotify_ssh_modes(pid_t session_leader) {
    char *val = read_proc_env(session_leader, "LNOTIFY_SSH");
    if (!val) return NULL;  // not set = use config defaults

    if (strcmp(val, "all") == 0) {
        free(val);
        return NULL;  // all = use config defaults
    }

    return val;  // "none" or "overlay,text" etc.
}

// Check if a rendering mode is allowed given LNOTIFY_SSH and config.
static bool mode_allowed(const char *mode, const char *client_modes,
                         const lnotify_config *cfg) {
    // If client explicitly set modes, check those
    if (client_modes) {
        if (strcmp(client_modes, "none") == 0) return false;
        return word_in_list(mode, client_modes);
    }

    // Fall back to server config
    if (cfg->ssh_modes) {
        return word_in_list(mode, cfg->ssh_modes);
    }

    return true;  // no restrictions
}

// ---------------------------------------------------------------------------
// SSH session discovery
// ---------------------------------------------------------------------------

int ssh_find_qualifying_ptys(const lnotify_config *cfg,
                             ssh_pty_info *ptys, int max_ptys) {
    logind_session sessions[64];
    int session_count = logind_list_remote_sessions(sessions, 64);

    int count = 0;

    for (int i = 0; i < session_count; i++) {
        logind_session *s = &sessions[i];

        // Must have a TTY
        if (!s->tty || !s->tty[0]) {
            logind_session_free(s);
            continue;
        }

        pid_t leader = (pid_t)s->leader_pid;
        if (leader <= 0) {
            logind_session_free(s);
            continue;
        }

        // Check user qualification
        if (!user_qualifies(s->username, (uid_t)s->uid, cfg)) {
            log_debug("ssh: user %s does not qualify", s->username);
            logind_session_free(s);
            continue;
        }

        // Check LNOTIFY_SSH env var for opt-out
        char *client_modes = get_lnotify_ssh_modes(leader);
        if (client_modes && strcmp(client_modes, "none") == 0) {
            log_debug("ssh: session %s opted out (LNOTIFY_SSH=none)",
                      s->session_id);
            free(client_modes);
            logind_session_free(s);
            continue;
        }
        free(client_modes);

        // Build pty path
        char pty_path[64];
        if (strncmp(s->tty, "/dev/", 5) == 0) {
            snprintf(pty_path, sizeof(pty_path), "%s", s->tty);
        } else {
            snprintf(pty_path, sizeof(pty_path), "/dev/%s", s->tty);
        }

        // Verify pty exists and is writable
        if (access(pty_path, W_OK) != 0) {
            log_debug("ssh: %s not writable, skipping", pty_path);
            logind_session_free(s);
            continue;
        }

        // If we've hit max, free remaining and stop adding
        if (count >= max_ptys) {
            logind_session_free(s);
            continue;
        }

        // Add to results
        ssh_pty_info *info = &ptys[count];
        snprintf(info->pty_path, sizeof(info->pty_path), "%s", pty_path);
        info->session_leader = leader;
        info->uid = (uid_t)s->uid;
        snprintf(info->username, sizeof(info->username), "%s", s->username);

        log_debug("ssh: found qualifying pty %s (user=%s, leader=%d)",
                  pty_path, s->username, (int)leader);
        count++;

        logind_session_free(s);
    }

    return count;
}

// ---------------------------------------------------------------------------
// Full-screen app detection
// ---------------------------------------------------------------------------

bool ssh_check_fullscreen(const ssh_pty_info *pty,
                          const lnotify_config *cfg) {
    if (!cfg->ssh_fullscreen_apps || !*cfg->ssh_fullscreen_apps) {
        return false;
    }

    // Read /proc/{session_leader}/stat to get tpgid (field 8, 1-indexed)
    char stat_path[64];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat",
             (int)pty->session_leader);

    FILE *fp = fopen(stat_path, "r");
    if (!fp) return false;

    char buf[512];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    // Fields in /proc/pid/stat are space-separated, but field 2 (comm) is
    // enclosed in parens and may contain spaces. Skip past the closing paren.
    const char *p = strrchr(buf, ')');
    if (!p) return false;
    p++;  // skip ')'

    // After the closing paren, we need fields:
    // state(3) ppid(4) pgrp(5) session(6) tty_nr(7) tpgid(8)
    // So we need to skip 5 more fields to reach tpgid
    int field = 0;
    pid_t tpgid = 0;
    while (*p && field < 5) {
        while (*p == ' ') p++;
        if (field == 4) {
            // This is tpgid (field 8 in 1-indexed, 5th after comm close)
            tpgid = (pid_t)atoi(p);
            break;
        }
        while (*p && *p != ' ') p++;
        field++;
    }

    if (tpgid <= 0) return false;

    // Now look up the process name of the tpgid leader
    char comm_path[64];
    snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)tpgid);

    fp = fopen(comm_path, "r");
    if (!fp) return false;

    char comm[64] = {0};
    if (!fgets(comm, sizeof(comm), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    // Strip trailing newline
    char *nl = strchr(comm, '\n');
    if (nl) *nl = '\0';

    // Check against the fullscreen app list
    bool is_fullscreen = word_in_list(comm, cfg->ssh_fullscreen_apps);
    if (is_fullscreen) {
        log_debug("ssh: fullscreen app detected: %s (tpgid=%d on %s)",
                  comm, (int)tpgid, pty->pty_path);
    }

    return is_fullscreen;
}

// ---------------------------------------------------------------------------
// Main SSH delivery entry point
// ---------------------------------------------------------------------------

int ssh_deliver(const notification *notif, const lnotify_config *cfg) {
    if (!notif || !cfg) return 0;

    // Quick check: if ssh_modes is empty, SSH delivery is disabled
    if (!cfg->ssh_modes || !*cfg->ssh_modes) {
        log_debug("ssh: delivery disabled (ssh_modes empty)");
        return 0;
    }

    ssh_pty_info ptys[MAX_SSH_PTYS];
    int pty_count = ssh_find_qualifying_ptys(cfg, ptys, MAX_SSH_PTYS);

    if (pty_count == 0) {
        log_debug("ssh: no qualifying SSH sessions found");
        return 0;
    }

    log_info("ssh: found %d qualifying SSH session(s)", pty_count);

    int delivered = 0;
    int timeout_ms = notif->timeout_ms > 0 ? notif->timeout_ms : 5000;

    for (int i = 0; i < pty_count; i++) {
        ssh_pty_info *pty = &ptys[i];

        // Get client-side mode preferences
        char *client_modes = get_lnotify_ssh_modes(pty->session_leader);

        // Check fullscreen app detection
        bool fullscreen = ssh_check_fullscreen(pty, cfg);
        if (fullscreen && !cfg->ssh_notify_over_fullscreen) {
            log_info("ssh: skipping %s (fullscreen app, "
                     "ssh_notify_over_fullscreen=false)", pty->pty_path);
            // Design says: hold and deliver later. For v1, we skip.
            // Holding requires a per-pty queue + polling, deferred to v2.
            free(client_modes);
            continue;
        }

        // Open the pty
        int pty_fd = open(pty->pty_path, O_WRONLY | O_NOCTTY);
        if (pty_fd < 0) {
            log_error("ssh: could not open %s: %s",
                      pty->pty_path, strerror(errno));
            free(client_modes);
            continue;
        }

        bool sent = false;

        // Tier 1: OSC
        if (!sent && mode_allowed("osc", client_modes, cfg)) {
            sent = terminal_render_osc(pty_fd, pty, notif);
        }

        // Tier 2: tmux
        if (!sent && mode_allowed("tmux", client_modes, cfg)) {
            sent = terminal_render_tmux(pty, notif, timeout_ms);
        }

        // Tier 3: cursor overlay
        if (!sent && mode_allowed("overlay", client_modes, cfg)) {
            if (!fullscreen || cfg->ssh_notify_over_fullscreen) {
                sent = terminal_render_overlay(pty_fd, notif, timeout_ms);
            }
        }

        // Tier 4: plain text
        if (!sent && mode_allowed("text", client_modes, cfg)) {
            sent = terminal_render_text(pty_fd, notif);
        }

        close(pty_fd);
        free(client_modes);

        if (sent) {
            log_info("ssh: delivered to %s (user=%s)",
                     pty->pty_path, pty->username);
            delivered++;
        } else {
            log_debug("ssh: no tier succeeded for %s", pty->pty_path);
        }
    }

    return delivered;
}
