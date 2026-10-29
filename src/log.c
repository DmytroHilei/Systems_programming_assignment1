// SPDX-License-Identifier: MIT

#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static enum ptop_log_level current_log_level = PTOP_LOG_WARN; // Default: errors and warnings

static const char *level_word(enum ptop_log_level level) {
    switch (level) {
        case PTOP_LOG_ERR:
            return "error";
        case PTOP_LOG_WARN:
            return "warning";
        case PTOP_LOG_DBG:
            return "debug";
    }
    return "log";
}

// errnum == 0 means no "strerror" part

static void log_write(enum ptop_log_level level, int errnum,
                      const char *format, va_list ap) {
    if (level > current_log_level) {
        return; // Less important than the current level
    }

    fprintf(stderr, "ptop: %s: ", level_word(level));
    vfprintf(stderr, format, ap);
    if (errnum != 0) {
        fprintf(stderr, ": %s", strerror(errnum));
    }
    fputc('\n', stderr);
}

void log_set_level(enum ptop_log_level level) {
    current_log_level = level;
}

void log_message(enum ptop_log_level level, const char *format, ...) {
    va_list ap;

    va_start(ap, format);
    log_write(level, 0, format, ap);
    va_end(ap);
}

void log_errno(enum ptop_log_level level, const char *format, ...) {
    int saved_errno = errno; // Save first: any call below may change errno
    va_list ap;

    va_start(ap, format);
    log_write(level, saved_errno, format, ap);
    va_end(ap);
}
