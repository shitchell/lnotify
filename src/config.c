#include "config.h"
#include "log.h"
#include "strutil.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Trim leading and trailing whitespace in-place. Returns pointer into the
// original buffer (does not allocate).
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

// Parse an integer from a string, returning fallback on any error (overflow,
// non-numeric, empty). Uses strtol for defined behavior on all inputs.
static int safe_atoi(const char *str, int fallback) {
    char *endp;
    errno = 0;
    long val = strtol(str, &endp, 10);
    if (endp == str || *endp != '\0' || errno == ERANGE
        || val < INT_MIN || val > INT_MAX)
        return fallback;
    return (int)val;
}

// Clamp an integer to [lo, hi].
static int clamp_int(int val, int lo, int hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

// Parse a boolean value: "true"/"1"/"yes" -> true, everything else -> false.
static bool parse_bool(const char *value) {
    return (strcmp(value, "true") == 0 ||
            strcmp(value, "1") == 0 ||
            strcmp(value, "yes") == 0);
}

// ---------------------------------------------------------------------------
// Color parsing
// ---------------------------------------------------------------------------

// Convert a single hex character to its numeric value. Returns -1 on error.
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse two hex characters into a byte. Returns -1 on error.
static int hex_byte(const char *s) {
    int hi = hex_digit(s[0]);
    int lo = hex_digit(s[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

int config_parse_color(const char *str, lnotify_color *out) {
    if (!str || str[0] != '#') return -1;
    if (strlen(str) != 9) return -1;  // "#RRGGBBAA" = 9 chars

    int r = hex_byte(str + 1);
    int g = hex_byte(str + 3);
    int b = hex_byte(str + 5);
    int a = hex_byte(str + 7);

    if (r < 0 || g < 0 || b < 0 || a < 0) return -1;

    out->r = (uint8_t)r;
    out->g = (uint8_t)g;
    out->b = (uint8_t)b;
    out->a = (uint8_t)a;
    return 0;
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

int config_defaults(lnotify_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->default_timeout = 5000;
    cfg->position = strdup("top-right");
    cfg->font_name = strdup("monospace");
    cfg->font_path = strdup("");
    cfg->font_size = 18;

    config_parse_color("#282828E6", &cfg->bg_color);
    config_parse_color("#FFFFFFFF", &cfg->fg_color);
    config_parse_color("#64A0FFFF", &cfg->border_color);

    cfg->border_width = 2;
    cfg->border_radius = 12;
    cfg->padding = 20;
    cfg->margin = 30;

    cfg->ssh_modes = strdup("osc,overlay,text");
    cfg->ssh_fullscreen_apps = strdup("vim nvim less man htop top nano emacs");
    cfg->ssh_notify_over_fullscreen = false;
    cfg->ssh_groups = strdup("");
    cfg->ssh_users = strdup("");

    cfg->socket_path = NULL;  // auto-detect via socket_default_path()
    cfg->engine_priorities = strdup("dbus,framebuffer,terminal,queue");

    // Check all strdup'd fields for OOM
    if (!cfg->position || !cfg->font_name || !cfg->font_path ||
        !cfg->ssh_modes || !cfg->ssh_fullscreen_apps ||
        !cfg->ssh_groups || !cfg->ssh_users || !cfg->engine_priorities) {
        config_free(cfg);
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Config loading
// ---------------------------------------------------------------------------

// Apply a single key=value pair to cfg. Unknown keys are logged, not errors.
static void config_set(lnotify_config *cfg, const char *key, const char *value) {
    // Display
    if (strcmp(key, "default_timeout") == 0) {
        cfg->default_timeout = clamp_int(safe_atoi(value, 5000), 100, 300000);
    } else if (strcmp(key, "position") == 0) {
        (void)replace_str(&cfg->position, value);
    } else if (strcmp(key, "font_name") == 0) {
        (void)replace_str(&cfg->font_name, value);
    } else if (strcmp(key, "font_path") == 0) {
        (void)replace_str(&cfg->font_path, value);
    } else if (strcmp(key, "font_size") == 0) {
        cfg->font_size = clamp_int(safe_atoi(value, 16), 8, 200);
    }
    // Toast style
    else if (strcmp(key, "bg_color") == 0) {
        if (config_parse_color(value, &cfg->bg_color) != 0)
            log_debug("config: invalid color for bg_color: %s", value);
    } else if (strcmp(key, "fg_color") == 0) {
        if (config_parse_color(value, &cfg->fg_color) != 0)
            log_debug("config: invalid color for fg_color: %s", value);
    } else if (strcmp(key, "border_color") == 0) {
        if (config_parse_color(value, &cfg->border_color) != 0)
            log_debug("config: invalid color for border_color: %s", value);
    } else if (strcmp(key, "border_width") == 0) {
        cfg->border_width = clamp_int(safe_atoi(value, 0), 0, 50);
    } else if (strcmp(key, "border_radius") == 0) {
        cfg->border_radius = clamp_int(safe_atoi(value, 8), 0, 100);
    } else if (strcmp(key, "padding") == 0) {
        cfg->padding = clamp_int(safe_atoi(value, 20), 0, 500);
    } else if (strcmp(key, "margin") == 0) {
        cfg->margin = clamp_int(safe_atoi(value, 20), 0, 500);
    }
    // SSH terminal notifications
    else if (strcmp(key, "ssh_modes") == 0) {
        (void)replace_str(&cfg->ssh_modes, value);
    } else if (strcmp(key, "ssh_fullscreen_apps") == 0) {
        (void)replace_str(&cfg->ssh_fullscreen_apps, value);
    } else if (strcmp(key, "ssh_notify_over_fullscreen") == 0) {
        cfg->ssh_notify_over_fullscreen = parse_bool(value);
    } else if (strcmp(key, "ssh_groups") == 0) {
        (void)replace_str(&cfg->ssh_groups, value);
    } else if (strcmp(key, "ssh_users") == 0) {
        (void)replace_str(&cfg->ssh_users, value);
    }
    // Daemon
    else if (strcmp(key, "socket_path") == 0) {
        (void)replace_str(&cfg->socket_path, value);
    }
    // Internal
    else if (strcmp(key, "engine_priorities") == 0) {
        (void)replace_str(&cfg->engine_priorities, value);
    }
    // Unknown
    else {
        log_debug("config: unknown key '%s'", key);
    }
}

int config_load(lnotify_config *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        log_error("config: cannot open '%s'", path);
        return -1;
    }

    char line[1024];
    int lineno = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;

        // Strip newline
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *trimmed = trim(line);

        // Skip empty lines and comments
        if (*trimmed == '\0' || *trimmed == '#')
            continue;

        // Find first '='
        char *eq = strchr(trimmed, '=');
        if (!eq) {
            log_debug("config: line %d: no '=' found, skipping", lineno);
            continue;
        }

        // Split into key and value
        *eq = '\0';
        char *key = trim(trimmed);
        char *value = trim(eq + 1);

        // Skip lines with empty key
        if (*key == '\0') {
            log_debug("config: line %d: empty key, skipping", lineno);
            continue;
        }

        config_set(cfg, key, value);
    }

    fclose(f);
    return 0;
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void config_free(lnotify_config *cfg) {
    free(cfg->position);          cfg->position = NULL;
    free(cfg->font_name);         cfg->font_name = NULL;
    free(cfg->font_path);         cfg->font_path = NULL;
    free(cfg->ssh_modes);         cfg->ssh_modes = NULL;
    free(cfg->ssh_fullscreen_apps); cfg->ssh_fullscreen_apps = NULL;
    free(cfg->ssh_groups);        cfg->ssh_groups = NULL;
    free(cfg->ssh_users);         cfg->ssh_users = NULL;
    free(cfg->socket_path);       cfg->socket_path = NULL;
    free(cfg->engine_priorities); cfg->engine_priorities = NULL;
}
