// Test: can root connect to a user's D-Bus session bus via sd-bus?
//
// Build: cc -o dbus-root-test prototype/dbus-root-test.c $(pkg-config --cflags --libs libsystemd)
// Run:   sudo ./dbus-root-test 1000
//
// Tests three approaches:
//   1. Connect to unix:path=/run/user/{uid}/bus as root directly
//   2. Connect to the default session bus (DBUS_SESSION_BUS_ADDRESS)
//   3. Fork+setresuid, then connect as the target user
//
// For each, tries to introspect org.freedesktop.Notifications

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>

static int try_introspect(sd_bus *bus, const char *label) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;

    int r = sd_bus_call_method(
        bus,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.DBus.Introspectable",
        "Introspect",
        &error,
        &reply,
        "");

    if (r < 0) {
        printf("  [%s] FAILED: %s (%s)\n", label, error.message, error.name);
        sd_bus_error_free(&error);
        return -1;
    }

    const char *xml = NULL;
    sd_bus_message_read(reply, "s", &xml);
    printf("  [%s] SUCCESS — got %zu bytes of introspection XML\n",
           label, xml ? strlen(xml) : 0);

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <target_uid>\n", argv[0]);
        return 1;
    }

    uint32_t target_uid = (uint32_t)atoi(argv[1]);
    printf("Running as uid=%u, target uid=%u\n\n", getuid(), target_uid);

    // Approach 1: Connect directly to the user's bus socket as root
    {
        printf("Approach 1: Direct connect to /run/user/%u/bus as root\n", target_uid);
        char addr[128];
        snprintf(addr, sizeof(addr), "unix:path=/run/user/%u/bus", target_uid);

        sd_bus *bus = NULL;
        int r = sd_bus_new(&bus);
        if (r < 0) { printf("  sd_bus_new failed: %d\n", r); goto skip1; }

        r = sd_bus_set_address(bus, addr);
        if (r < 0) { printf("  sd_bus_set_address failed: %d\n", r); sd_bus_unref(bus); goto skip1; }

        r = sd_bus_start(bus);
        if (r < 0) {
            printf("  sd_bus_start FAILED: %d (%s)\n", r, strerror(-r));
            sd_bus_unref(bus);
            goto skip1;
        }

        printf("  Connected! Trying introspect...\n");
        try_introspect(bus, "direct-as-root");
        sd_bus_unref(bus);
    }
skip1:
    printf("\n");

    // Approach 2: Default session bus (will likely fail as root)
    {
        printf("Approach 2: sd_bus_open_user() (default session bus)\n");
        sd_bus *bus = NULL;
        int r = sd_bus_open_user(&bus);
        if (r < 0) {
            printf("  sd_bus_open_user FAILED: %d (%s)\n", r, strerror(-r));
        } else {
            printf("  Connected! Trying introspect...\n");
            try_introspect(bus, "open-user");
            sd_bus_unref(bus);
        }
    }
    printf("\n");

    // Approach 3: Fork, setresuid, then connect
    {
        printf("Approach 3: fork + setresuid(%u) + sd_bus_open_user()\n", target_uid);

        pid_t pid = fork();
        if (pid == 0) {
            // Child: become target user
            char xdg[64];
            snprintf(xdg, sizeof(xdg), "/run/user/%u", target_uid);
            setenv("XDG_RUNTIME_DIR", xdg, 1);

            if (setresgid((gid_t)target_uid, (gid_t)target_uid, (gid_t)target_uid) < 0) {
                perror("  setresgid");
                _exit(1);
            }
            if (setresuid(target_uid, target_uid, target_uid) < 0) {
                perror("  setresuid");
                _exit(1);
            }

            sd_bus *bus = NULL;
            int r = sd_bus_open_user(&bus);
            if (r < 0) {
                printf("  [fork+setresuid] sd_bus_open_user FAILED: %d (%s)\n",
                       r, strerror(-r));
                _exit(1);
            }

            printf("  Connected as uid=%u! Trying introspect...\n", getuid());
            int rc = try_introspect(bus, "fork+setresuid");
            sd_bus_unref(bus);
            _exit(rc < 0 ? 1 : 0);
        }

        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("  Child succeeded\n");
        } else {
            printf("  Child failed (exit %d)\n",
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
    }

    return 0;
}
