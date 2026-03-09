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
        sd_bus_error prop_error = SD_BUS_ERROR_NULL;
        uint32_t vt_nr = 0;
        r = sd_bus_get_property_trivial(bus,
            "org.freedesktop.login1", obj,
            "org.freedesktop.login1.Session", "VTNr",
            &prop_error, 'u', &vt_nr);
        sd_bus_error_free(&prop_error);

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

        // Query Type
        sd_bus_error type_error = SD_BUS_ERROR_NULL;
        r = sd_bus_get_property_string(bus,
            "org.freedesktop.login1", obj,
            "org.freedesktop.login1.Session", "Type",
            &type_error, &out->type);
        sd_bus_error_free(&type_error);
        if (r < 0) out->type = strdup("unknown");

        // Query Class
        sd_bus_error class_error = SD_BUS_ERROR_NULL;
        r = sd_bus_get_property_string(bus,
            "org.freedesktop.login1", obj,
            "org.freedesktop.login1.Session", "Class",
            &class_error, &out->session_class);
        sd_bus_error_free(&class_error);
        if (r < 0) out->session_class = strdup("unknown");

        // Query Remote
        sd_bus_error remote_error = SD_BUS_ERROR_NULL;
        int remote_val = 0;
        r = sd_bus_get_property_trivial(bus,
            "org.freedesktop.login1", obj,
            "org.freedesktop.login1.Session", "Remote",
            &remote_error, 'b', &remote_val);
        sd_bus_error_free(&remote_error);
        out->remote = (r >= 0) ? (bool)remote_val : false;

        // Query Leader
        sd_bus_error leader_error = SD_BUS_ERROR_NULL;
        r = sd_bus_get_property_trivial(bus,
            "org.freedesktop.login1", obj,
            "org.freedesktop.login1.Session", "Leader",
            &leader_error, 'u', &out->leader_pid);
        sd_bus_error_free(&leader_error);

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
        sd_bus_error remote_error = SD_BUS_ERROR_NULL;
        int remote_val = 0;
        r = sd_bus_get_property_trivial(bus,
            "org.freedesktop.login1", obj,
            "org.freedesktop.login1.Session", "Remote",
            &remote_error, 'b', &remote_val);
        sd_bus_error_free(&remote_error);

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

        // Query Type
        sd_bus_error type_error = SD_BUS_ERROR_NULL;
        r = sd_bus_get_property_string(bus,
            "org.freedesktop.login1", obj,
            "org.freedesktop.login1.Session", "Type",
            &type_error, &s->type);
        sd_bus_error_free(&type_error);
        if (r < 0) s->type = strdup("unknown");

        // Query Class
        sd_bus_error class_error = SD_BUS_ERROR_NULL;
        r = sd_bus_get_property_string(bus,
            "org.freedesktop.login1", obj,
            "org.freedesktop.login1.Session", "Class",
            &class_error, &s->session_class);
        sd_bus_error_free(&class_error);
        if (r < 0) s->session_class = strdup("unknown");

        // Query Leader
        sd_bus_error leader_error = SD_BUS_ERROR_NULL;
        r = sd_bus_get_property_trivial(bus,
            "org.freedesktop.login1", obj,
            "org.freedesktop.login1.Session", "Leader",
            &leader_error, 'u', &s->leader_pid);
        sd_bus_error_free(&leader_error);

        // Query TTY
        sd_bus_error tty_error = SD_BUS_ERROR_NULL;
        r = sd_bus_get_property_string(bus,
            "org.freedesktop.login1", obj,
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
