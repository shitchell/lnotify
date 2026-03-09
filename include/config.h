#ifndef LNOTIFY_CONFIG_H
#define LNOTIFY_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

// RGBA color (parsed from #RRGGBBAA hex strings)
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} lnotify_color;

// All v1 configuration keys with their defaults.
// String fields are heap-allocated by config_load / config_defaults
// and freed by config_free.
typedef struct {
    // Display
    int      default_timeout;       // ms, default 5000
    char    *position;              // e.g. "top-right"
    char    *font_path;             // path to TTF
    int      font_size;             // px, default 14

    // Toast style
    lnotify_color bg_color;         // default #282828E6
    lnotify_color fg_color;         // default #FFFFFFFF
    lnotify_color border_color;     // default #64A0FFFF
    int      border_width;          // px, default 2
    int      border_radius;         // px, default 12
    int      padding;               // px, default 20
    int      margin;                // px, default 30

    // SSH terminal notifications
    char    *ssh_modes;             // comma-separated, default "osc,overlay,text"
    char    *ssh_fullscreen_apps;   // space-separated
    bool     ssh_notify_over_fullscreen; // default false
    char    *ssh_groups;            // space-separated
    char    *ssh_users;             // space-separated

    // Daemon
    char    *socket_path;           // override socket path (default: auto-detect)

    // Internal (not in config file for v1)
    char    *engine_priorities;     // comma-separated engine names
} lnotify_config;

// Populate cfg with hardcoded defaults. All string fields are strdup'd.
void config_defaults(lnotify_config *cfg);

// Load config from file, overriding defaults already set in cfg.
// Returns 0 on success, -1 if the file cannot be opened.
// Malformed or unknown keys are silently skipped (logged at debug).
int config_load(lnotify_config *cfg, const char *path);

// Free heap-allocated strings in cfg. Safe to call multiple times.
void config_free(lnotify_config *cfg);

// Parse a "#RRGGBBAA" hex color string into an lnotify_color.
// Returns 0 on success, -1 on invalid format.
int config_parse_color(const char *str, lnotify_color *out);

#endif
