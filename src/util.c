// SPDX-License-Identifier: MIT

#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

enum error_code parse_long(const char *s, long min, long max, long *out) {
    char *end;

    errno = 0;
    long value = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || value < min || value > max) {
        return PTOP_PARSE_ERROR;
    }

    *out = value;
    return PTOP_SUCCESS;
}

void sleep_ms(long ms) {
    struct timespec left = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};

    while (nanosleep(&left, &left) == -1 && errno == EINTR) {
        // A signal cut the sleep short: left now holds the remaining time
    }
}

double now_sec(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t); // Cannot fail with a valid clock and pointer
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
