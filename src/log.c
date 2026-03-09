#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

bool log_debug_enabled = false;

static void log_msg(const char *level, const char *fmt, va_list args) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%6ld.%03lds] %s: ",
            ts.tv_sec, ts.tv_nsec / 1000000, level);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

void log_debug(const char *fmt, ...) {
    if (!log_debug_enabled) return;
    va_list args;
    va_start(args, fmt);
    log_msg("DEBUG", fmt, args);
    va_end(args);
}

void log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_msg("INFO", fmt, args);
    va_end(args);
}

void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_msg("ERROR", fmt, args);
    va_end(args);
}
