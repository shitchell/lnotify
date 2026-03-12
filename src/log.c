#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

bool log_debug_enabled = false;

static void log_msg(const char *level, const char *fmt, va_list args) {
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);

    char buf[1024];
    int pos = snprintf(buf, sizeof(buf), "[%6ld.%03lds] %s: ",
                       (long)ts.tv_sec, (long)(ts.tv_nsec / 1000000), level);
    if (pos > 0 && pos < (int)sizeof(buf)) {
        pos += vsnprintf(buf + pos, sizeof(buf) - (size_t)pos, fmt, args);
    }
    if (pos >= (int)sizeof(buf)) pos = (int)sizeof(buf) - 1;
    if (pos > 0 && buf[pos - 1] != '\n') {
        if (pos < (int)sizeof(buf) - 1) buf[pos++] = '\n';
    }
    write(STDERR_FILENO, buf, (size_t)pos);
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
