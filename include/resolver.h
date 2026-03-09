#ifndef LNOTIFY_RESOLVER_H
#define LNOTIFY_RESOLVER_H

#include "engine.h"

// Probe function signature — matches context_run_probe().
// Tests inject a mock; production code passes context_run_probe or NULL
// (NULL means use context_run_probe).
typedef void (*probe_fn)(session_context *ctx, probe_key key);

// Select the highest-priority engine that accepts the given session context.
// Engines are evaluated in array order (index 0 = highest priority).
//
// - ENGINE_ACCEPT: engine is selected, returned immediately.
// - ENGINE_REJECT: engine is marked rejected and skipped.
// - ENGINE_NEED_PROBE: the requested probe is run (via probe_fn), then the
//   engine is re-evaluated. If the probe was already completed, the engine
//   is treated as rejected (prevents infinite loops).
//
// Returns NULL if no engine accepts.
engine *resolver_select(engine *engines, int count, session_context *ctx,
                        probe_fn run_probe);

#endif
