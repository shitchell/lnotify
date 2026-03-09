#ifndef LNOTIFY_ENGINE_DBUS_H
#define LNOTIFY_ENGINE_DBUS_H

#include "engine.h"

#include <stdint.h>
#include <systemd/sd-bus.h>

extern engine engine_dbus;

// Callback type for D-Bus operations on a user's session bus.
typedef int (*dbus_user_fn)(sd_bus *bus, void *userdata);

// Run a D-Bus operation on a user's session bus.
// Same-user: opens user bus directly and calls fn.
// Cross-user: fork+setresuid, then opens user bus and calls fn in child.
// Returns 0 on success, non-zero on failure.
int dbus_call_as_user(uint32_t target_uid, dbus_user_fn fn, void *userdata);

#endif
