#ifndef LNOTIFY_ENGINE_DBUS_H
#define LNOTIFY_ENGINE_DBUS_H

#include "engine.h"

#include <stdint.h>

extern engine engine_dbus;

// Run a shell command on the session D-Bus of the given user.
// If the caller is already that user, runs directly. Otherwise, discovers
// DBUS_SESSION_BUS_ADDRESS via /proc, then fork+setresuid to execute as
// the target user. Returns 0 on success, non-zero on failure.
int dbus_run_as_user(const char *cmd, uint32_t target_uid);

#endif
