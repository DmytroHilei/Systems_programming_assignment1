// SPDX-License-Identifier: MIT

#include "status.h"

// Error code to text.
// This function returns a string that could be written to stderr or a log file from an
// error code. Similar concept to level_word in log.c: just a switch across all of the error
// codes. No default case, so -Wall warns if a new code is added without a message; the
// return after the switch covers a value that is not in the enum at all.
const char *ptop_get_error_message(enum error_code code) {
    switch (code) {
        case PTOP_SUCCESS:
            return "success";
        case PTOP_USAGE_ERROR:
            return "usage error";
        case PTOP_OUT_OF_MEMORY:
            return "out of memory";
        case PTOP_SYS_CALL_ERROR:
            return "system call error";
        case PTOP_PARSE_ERROR:
            return "parse error";
        case PTOP_PROCESS_GONE:
            return "process gone";
        case PTOP_TERMINAL_UNAVAILABLE:
            return "terminal unavailable";
        case PTOP_OUTPUT_CLOSED:
            return "output closed";
    }
    return "unknown error code";
}
