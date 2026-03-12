#include "logind.h"
#include "log.h"

#include <systemd/sd-bus.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Cached system bus connection
static sd_bus *g_system_bus = NULL;

static sd_bus *get_system_bus(void) {
    if (!g_system_bus) {
        int r = sd_bus_open_system(&g_system_bus);
        if (r < 0) {
            log_error("logind: failed to open system bus: %s", strerror(-r));
            return NULL;
        }
    }
    return g_system_bus;
}

void logind_close(void) {
    if (g_system_bus) {
        sd_bus_unref(g_system_bus);
        g_system_bus = NULL;
    }
}

/**
 * Query a string property from a logind session object.
 * Returns a heap-allocated string on success, or strdup("unknown") on failure.
 * May return NULL on OOM; callers handle NULL.
 */
static char *logind_get_session_str(sd_bus *bus, const char *obj,
                                     const char *prop) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char *val = NULL;
    int r = sd_bus_get_property_string(bus, "org.freedesktop.login1", obj,
                "org.freedesktop.login1.Session", prop, &err, &val);
    sd_bus_error_free(&err);
    if (r < 0 || !val) {
        char *fallback = strdup("unknown");
        return fallback;  // may be NULL on OOM, callers handle NULL
    }
    return val;
}

/**
 * Query a uint32 property from a logind session object.
 * Returns 0 on success, negative errno on failure.
 */
static int logind_get_session_uint(sd_bus *bus, const char *obj,
                                    const char *prop, uint32_t *out) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_get_property_trivial(bus, "org.freedesktop.login1", obj,
                "org.freedesktop.login1.Session", prop, &err, 'u', out);
    sd_bus_error_free(&err);
    return r;
}

/**
 * Query a boolean property from a logind session object.
 * Returns 0 on success, negative errno on failure.
 */
static int logind_get_session_bool(sd_bus *bus, const char *obj,
                                    const char *prop, int *out) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_get_property_trivial(bus, "org.freedesktop.login1", obj,
                "org.freedesktop.login1.Session", prop, &err, 'b', out);
    sd_bus_error_free(&err);
    return r;
}

void logind_session_free(logind_session *s) {
    if (!s) return;
    free(s->session_id);    s->session_id = NULL;
    free(s->type);          s->type = NULL;
    free(s->session_class);  s->session_class = NULL;
    free(s->username);       s->username = NULL;
    free(s->seat);           s->seat = NULL;
    free(s->tty);            s->tty = NULL;
}

int logind_get_session_by_vt(uint32_t vt, logind_session *out) {
    sd_bus *bus = get_system_bus();
    if (!bus) return -1;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int ret = -1;

    // Call ListSessions -> a(susso)
    int r = sd_bus_call_method(bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "ListSessions",
        &error, &reply, "");
    if (r < 0) {
        log_error("logind: ListSessions failed: %s", error.message);
        goto finish;
    }

    r = sd_bus_message_enter_container(reply, 'a', "(susso)");
    if (r < 0) {
        log_error("logind: failed to parse ListSessions reply: %s", strerror(-r));
        goto finish;
    }

    while (sd_bus_message_enter_container(reply, 'r', "susso") > 0) {
        const char *sid = NULL, *user = NULL, *seat = NULL, *obj = NULL;
        uint32_t uid = 0;

        r = sd_bus_message_read(reply, "susso", &sid, &uid, &user, &seat, &obj);
        if (r < 0) {
            sd_bus_message_exit_container(reply);
            continue;
        }

        // Query VTNr for this session
        uint32_t vt_nr = 0;
        r = logind_get_session_uint(bus, obj, "VTNr", &vt_nr);

        if (r < 0 || vt_nr != vt) {
            sd_bus_message_exit_container(reply);
            continue;
        }

        // VT matches — populate the output struct
        memset(out, 0, sizeof(*out));
        out->session_id = strdup(sid);
        out->uid = uid;
        out->username = strdup(user);
        out->seat = strdup(seat);
        out->vt = vt_nr;

        if (!out->session_id || !out->username || !out->seat) {
            log_error("logind: strdup failed populating session");
            logind_session_free(out);
            sd_bus_message_exit_container(reply);
            ret = -1;
            goto finish;
        }

        // Query Type, Class, Remote, Leader
        out->type = logind_get_session_str(bus, obj, "Type");
        out->session_class = logind_get_session_str(bus, obj, "Class");

        int remote_val = 0;
        r = logind_get_session_bool(bus, obj, "Remote", &remote_val);
        out->remote = (r >= 0) ? (bool)remote_val : false;

        logind_get_session_uint(bus, obj, "Leader", &out->leader_pid);

        ret = 0;
        sd_bus_message_exit_container(reply);
        break;
    }

    sd_bus_message_exit_container(reply);

finish:
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    return ret;
}

int logind_list_remote_sessions(logind_session *out, int max) {
    if (!out || max <= 0) return 0;

    sd_bus *bus = get_system_bus();
    if (!bus) return 0;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int count = 0;

    // Call ListSessions -> a(susso)
    int r = sd_bus_call_method(bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "ListSessions",
        &error, &reply, "");
    if (r < 0) {
        log_error("logind: ListSessions failed: %s", error.message);
        goto finish;
    }

    r = sd_bus_message_enter_container(reply, 'a', "(susso)");
    if (r < 0) {
        log_error("logind: failed to parse ListSessions reply: %s", strerror(-r));
        goto finish;
    }

    while (count < max && sd_bus_message_enter_container(reply, 'r', "susso") > 0) {
        const char *sid = NULL, *user = NULL, *seat = NULL, *obj = NULL;
        uint32_t uid = 0;

        r = sd_bus_message_read(reply, "susso", &sid, &uid, &user, &seat, &obj);
        if (r < 0) {
            sd_bus_message_exit_container(reply);
            continue;
        }

        // Query Remote — skip non-remote sessions
        int remote_val = 0;
        r = logind_get_session_bool(bus, obj, "Remote", &remote_val);

        if (r < 0 || !remote_val) {
            sd_bus_message_exit_container(reply);
            continue;
        }

        // Remote session found — populate the output struct
        logind_session *s = &out[count];
        memset(s, 0, sizeof(*s));
        s->session_id = strdup(sid);
        s->uid = uid;
        s->username = strdup(user);
        s->seat = strdup(seat);
        s->remote = true;

        if (!s->session_id || !s->username || !s->seat) {
            log_error("logind: strdup failed populating remote session");
            logind_session_free(s);
            sd_bus_message_exit_container(reply);
            continue;
        }

        // Query Type, Class, Leader
        s->type = logind_get_session_str(bus, obj, "Type");
        s->session_class = logind_get_session_str(bus, obj, "Class");
        logind_get_session_uint(bus, obj, "Leader", &s->leader_pid);

        // Query TTY (NULL on failure, not "unknown" — ssh_delivery needs this)
        sd_bus_error tty_error = SD_BUS_ERROR_NULL;
        r = sd_bus_get_property_string(bus, "org.freedesktop.login1", obj,
                "org.freedesktop.login1.Session", "TTY",
                &tty_error, &s->tty);
        sd_bus_error_free(&tty_error);
        if (r < 0) s->tty = NULL;

        count++;
        sd_bus_message_exit_container(reply);
    }

    sd_bus_message_exit_container(reply);

finish:
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    return count;
}
