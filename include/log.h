// SPDX-License-Identifier: MIT

#ifndef LOG_H
#define LOG_H

// Most severe first: a message is printed when its level <= the current level
enum ptop_log_level {
    PTOP_LOG_ERR = 0, // The program cannot continue, the error is fatal
    PTOP_LOG_WARN,    // The run continues
    PTOP_LOG_DBG,     // Debug information, only with -v
};

void log_set_level(enum ptop_log_level level);

// Print "ptop: <level>: <message>\n" to stderr. No '\n' needed in format.
void log_message(enum ptop_log_level level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

// Same, plus ": <strerror(errno)>" at the end
void log_errno(enum ptop_log_level level, const char *format, ...)
    __attribute__((format(printf, 2,
                          3))); //__attribute__ format, this if used to tell the compiler that this
                                //function takes a format string and variable arguments,
// so it can check the format string against the arguments and warn about mismatches.
#endif // LOG_H
