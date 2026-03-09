#include "log.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            log_debug_enabled = true;
        }
    }

    log_info("lnotifyd starting");
    log_debug("debug logging enabled");
    log_info("lnotifyd exiting (stub)");
    return 0;
}
