#ifndef LNOTIFY_LOG_H
#define LNOTIFY_LOG_H

#include <stdbool.h>

// Set by --debug flag at startup
extern bool log_debug_enabled;

void log_debug(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);

#endif
