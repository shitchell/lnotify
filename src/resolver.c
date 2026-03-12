#include "resolver.h"
#include "log.h"

#include <stdint.h>

_Static_assert(MAX_ENGINES <= 32,
               "resolver rejected bitfield requires MAX_ENGINES <= 32");

engine *resolver_select(engine *engines, int count, session_context *ctx,
                        probe_fn run_probe) {
    if (!engines || count <= 0)
        return NULL;

    // Default to context_run_probe if no override provided
    if (!run_probe)
        run_probe = context_run_probe;

    // Rejection bitfield — bit i set means engines[i] was rejected
    uint32_t rejected = 0;

    for (int i = 0; i < count; i++) {
        if (rejected & (1u << i))
            continue;

        engine_detect_result result = engines[i].detect(ctx);

        switch (result) {
        case ENGINE_ACCEPT:
            log_debug("resolver: engine '%s' accepted", engines[i].name);
            return &engines[i];

        case ENGINE_REJECT:
            log_debug("resolver: engine '%s' rejected", engines[i].name);
            rejected |= (1u << i);
            break;

        case ENGINE_NEED_PROBE: {
            probe_key key = ctx->requested_probe;

            if (context_probe_done(ctx, key)) {
                // Probe already run but engine still wants it — treat as reject
                // to prevent infinite loops
                log_debug("resolver: engine '%s' requested already-completed "
                          "probe %d, treating as reject", engines[i].name, key);
                rejected |= (1u << i);
                break;
            }

            log_debug("resolver: engine '%s' needs probe %d, running",
                      engines[i].name, key);
            run_probe(ctx, key);

            // Re-evaluate this engine (decrement i so the for loop re-visits)
            i--;
            break;
        }
        }
    }

    log_debug("resolver: no engine accepted");
    return NULL;
}
