// SPDX-License-Identifier: MIT

// small helpers that do not belong anywhere else

#ifndef UTIL_H
#define UTIL_H

#include "status.h"

// Parse a whole string as a base 10 number in [min, max].
// Returns PTOP_PARSE_ERROR on an empty string, trailing junk or a number out of range.
enum error_code parse_long(const char *s, long min, long max, long *out);

void sleep_ms(long ms);

// CLOCK_MONOTONIC in seconds: only differences between two calls are meaningful
double now_sec(void);

#endif // UTIL_H
