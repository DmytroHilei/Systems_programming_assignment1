// SPDX-License-Identifier: MIT

#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

// Strict string to number.
// So this function is used to convert a string into a long int.
// s is the string from the command line (optarg), and min, max are the allowed range
// (args.c passes 1 to INT_MAX for -n, 1 to one day for -w).
// out is where the result is stored. The function returns PTOP_PARSE_ERROR if the parsing
// fails, or PTOP_SUCCESS if it succeeds; *out is only written on success.
enum error_code parse_long(const char *s, long min, long max, long *out) {
    char *end; // strtol points it at the first character that is not part of the number

    errno = 0; // strtol sets errno only when it fails and never clears it, so clear it first
    long value = strtol(s, &end, 10); // Converts the string s to a long int, with base 10
    if (end == s || *end != '\0' || errno == ERANGE || value < min || value > max) {
        // This checks if:
        //   end == s         no characters were converted ("", "abc")
        //   *end != '\0'     there are extra characters after the number ("5x")
        //   errno == ERANGE  the number does not fit in a long
        //   value < min, value > max  the number is not in the allowed range
        // If any of these conditions is true, it returns PTOP_PARSE_ERROR.
        // atoi could not detect any of these: atoi("abc") is just 0.
        return PTOP_PARSE_ERROR;
    }

    *out = value;
    return PTOP_SUCCESS;
}

// Sleeps, even through signals.
void sleep_ms(long ms) {
    // This creates a struct timespec variable called left, which represents the amount of
    // time to sleep in seconds and nanoseconds (1 ms = 1 000 000 ns)
    struct timespec left = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};

    // nanosleep suspends the calling thread for the given amount of time. It returns 0 if
    // the sleep was completed, or -1 with errno EINTR if a signal interrupted it; then it
    // writes the time still left into its second argument (here the same variable).
    // So we keep sleeping for what is left until the whole time has passed.
    while (nanosleep(&left, &left) == -1 && errno == EINTR) {
        // A signal cut the sleep short: left now holds the remaining time
    }
}

// Seconds on a steady clock.
double now_sec(void) {
    struct timespec t;

    // clock_gettime retrieves the current time of the given clock and fills t with seconds
    // and nanoseconds. CLOCK_MONOTONIC only counts forward: unlike the wall clock it does
    // not jump when the system time is changed (NTP, the user), which would make the
    // elapsed time, and so CPU%, wrong or even negative. Only differences are meaningful.
    clock_gettime(CLOCK_MONOTONIC, &t); // Cannot fail with a valid clock and pointer

    // This returns the time in seconds as a double, by adding the seconds and the
    // nanoseconds converted to seconds
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
