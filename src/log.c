// SPDX-License-Identifier: MIT

// Logging to stderr: "ptop: <level>: <message>[: <strerror>]".
// stderr, not stdout, so messages never mix with the table (e.g. in "ptop > file").

#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// With the default level set to warning, debug messages are ignored, but errors and
// warnings are printed. -v raises it to PTOP_LOG_DBG (log_set_level).
static enum ptop_log_level current_log_level = PTOP_LOG_WARN; // Default: errors and warnings

// Level to word.
// This is just a switch over the enum values to the string representation of the log
// level, so we can print it in the log message. No default case: -Wall warns if a new
// level is added without a word here.
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

// Shared printing path.
// level is the log level of the message. If it is higher than the current level of the
// program's logs (which means it is less important), we ignore it.
// Lower is more severe, the same as syslog: ERR = 0, WARN = 1, DBG = 2.
// errnum == 0 means no "strerror" part.
static void log_write(enum ptop_log_level level, int errnum, const char *format, va_list ap) {
    if (level > current_log_level) {
        return; // Less severe than the current level: not shown (e.g. DBG without -v)
    }

    // Prints to the standard error stream the log level and the message (since it was not
    // ignored, it is important enough to be printed)
    fprintf(stderr, "ptop: %s: ", level_word(level));

    // ap is the variable argument list. vfprintf is the same as fprintf, but takes a va_list
    // instead of a variable number of arguments, so log_message and log_errno can share it.
    vfprintf(stderr, format, ap);

    if (errnum != 0) {
        // strerror returns the text for an error number, like "No such file or directory"
        fprintf(stderr, ": %s", strerror(errnum));
    }
    fputc('\n', stderr); // This line just puts the new line, so callers do not need '\n'
}

// Sets the filter level.
// Sets the current log level to the given level, so that messages less severe than it
// are ignored.
void log_set_level(enum ptop_log_level level) {
    current_log_level = level;
}

// Logs a plain message.
// There can be any number of arguments after the format, like printf:
//   log_message(PTOP_LOG_ERR, "unexpected argument: '%s'", argv[optind]);
// so we need va_list to handle them. The format string is the message that is used to
// format the output.
void log_message(enum ptop_log_level level, const char *format, ...) {
    // va_list is a cursor over the unnamed "..." arguments. The function cannot know how
    // many there are or their types: the format string tells vfprintf (%d = read an int).
    va_list ap;

    va_start(ap, format);            // Set ap to the first argument after format
    log_write(level, 0, format, ap); // Just the call; 0 = no strerror part
    va_end(ap);                      // Clean up the va_list, since it is no longer needed
}

// Logs a message plus errno.
// The same, but it saves errno and passes it to log_write as errnum, so the reason the
// system call failed is printed at the end.
// For my own functions I use log_message, but for system calls I use log_errno, since
// it is more convenient to use the errno variable.
//
// What is errno? It is a global integer that system calls and library functions set when
// they fail. It holds the code of why they failed, like ENOENT (no such file).
// Only read it after a call has failed: success does not reset it.
// Any later call can overwrite it, so we need to save it first, and then use it in the
// log_write function, so we can print the error message.
void log_errno(enum ptop_log_level level, const char *format, ...) {
    int saved_errno = errno; // Save first: any call below may change errno
    va_list ap;

    va_start(ap, format);
    log_write(level, saved_errno, format, ap);
    va_end(ap);
}
