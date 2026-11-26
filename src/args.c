// SPDX-License-Identifier: MIT

// Command line parsing: turns argv into struct options. Only parses, does not act:
// main decides what to do with help, verbose and the mode.

#include "args.h"

#include <getopt.h>
#include <limits.h>
#include <string.h>

#include "log.h"
#include "util.h"

#define MAX_INTERVAL_S 86400L // One day, so the wait in milliseconds fits an int

// Text to sort key.
// Just turns the literal text to the enum value, and returns an error if it is not valid.
static enum error_code parse_sort(const char *s, enum proc_sort *out) {
    if (strcmp(s, "cpu") == 0) { // strcmp returns 0 when the strings are equal
        *out = SORT_CPU;
    } else if (strcmp(s, "mem") == 0) {
        *out = SORT_MEM;
    } else if (strcmp(s, "pid") == 0) {
        *out = SORT_PID;
    } else {
        log_message(PTOP_LOG_ERR, "sort key must be cpu, mem or pid: '%s'", s);
        return PTOP_USAGE_ERROR;
    }
    return PTOP_SUCCESS;
}

// Fills options from argv.
enum error_code args_parse(int argc, char *argv[], struct options *o) {
    // This is an array of structs option. static const means it is created only once
    // and never changes.
    // Each row is {name, has_arg, flag, val}:
    //   name     the long name (like --help)
    //   has_arg  no_argument, required_argument or optional_argument: the option does not
    //            take an argument, must take an argument, or may take an argument
    //   flag     NULL: getopt_long returns val; otherwise it sets *flag to val and returns 0
    //   val      the value getopt_long returns, here the matching short letter (like 'h'),
    //            so -h and --help are handled by the same case
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},    // h is the letter getopt_long will return
        {"verbose", no_argument, NULL, 'v'}, // v is the letter getopt_long will return
        {NULL, 0, NULL, 0}, // This is how getopt_long knows that the array has ended, since it
                            // is not given any length (like the '\0' in a string)
    };
    int r;

    // This is the compound literal: (type){initializer_list} creates an unnamed temporary
    // struct, and it is copied to the struct pointed to by o. This is a way to set default
    // values for the struct fields: sort is set to SORT_CPU and the rest are set to 0
    // (count = all rows, interval_s = snapshot mode, verbose and help off).
    // So we fill the empty struct (main's options holds garbage before this) with defaults.
    *o = (struct options){.sort = SORT_CPU};

    // getopt_long takes argc (number of arguments) and argv (array of arguments). Each call
    // breaks down the next option from the command line, until it gets to the end and
    // returns -1. The switch handles these options.
    //
    // "hn:s:vw:" is the string that specifies the short options h, n, s, v, w.
    // NOTE: ':' after a letter means the option takes a value (like -n 5): n, s and w take
    // one, h and v do not.
    //
    // It returns the letter of the short option if it was found, '?' if it was unknown or
    // its value is missing, or -1 if there are no options anymore.
    // The last argument (NULL) could receive the index of the matched long option in
    // long_options; we do not need it.
    //
    // The comma operator runs the call, stores it in r, and then only "r != -1" is the
    // condition (same as the usual while ((r = getopt_long(...)) != -1)).
    while (r = getopt_long(argc, argv, "hn:s:vw:", long_options, NULL), r != -1) {
        switch (r) {
            case 'h':
                o->help = 1;
                break;
            case 'n':
                // optarg is the global variable from this library that keeps the value as a
                // string (like -n 5 will keep "5", which is passed to parse_long to convert
                // it to a long int)
                if (parse_long(optarg, 1, INT_MAX, &o->count) != PTOP_SUCCESS) {
                    log_message(PTOP_LOG_ERR, "row count must be a positive number: '%s'", optarg);
                    return PTOP_USAGE_ERROR;
                }
                break;
            case 's':
                if (parse_sort(optarg, &o->sort) != PTOP_SUCCESS) {
                    return PTOP_USAGE_ERROR; // parse_sort already logged the reason
                }
                break;
            case 'v':
                o->verbose = 1;
                break;
            case 'w':
                // Same parse_long, range 1 s to one day (see MAX_INTERVAL_S)
                if (parse_long(optarg, 1, MAX_INTERVAL_S, &o->interval_s) != PTOP_SUCCESS) {
                    log_message(PTOP_LOG_ERR, "interval must be 1 to %ld seconds: '%s'",
                                MAX_INTERVAL_S, optarg);
                    return PTOP_USAGE_ERROR;
                }
                break;
            default: // '?': getopt_long already printed the reason
                return PTOP_USAGE_ERROR;
        }
    }

    // optind is another global variable from this library that keeps the index of the next
    // element to be processed in argv. So if it is less than argc, there are still
    // arguments left, which is an error in this case because we do not expect any
    // arguments other than options (like "ptop foo").
    // GNU getopt moves such arguments to the end of argv, so "ptop foo -v" is caught too.
    if (optind < argc) {
        log_message(PTOP_LOG_ERR, "unexpected argument: '%s'", argv[optind]);
        return PTOP_USAGE_ERROR;
    }
    return PTOP_SUCCESS;
}

// Prints the help text.
// Just prints the usage message to the given file stream (stdout for --help, stderr on an
// error). Adjacent string literals are joined into one string by the compiler.
void args_usage(FILE *out) {
    fputs("Usage: ptop [-n COUNT] [-s cpu|mem|pid] [-w SECS] [-v] [--help]\n"
          "\n"
          "List processes and CPU core usage from /proc (Linux only).\n"
          "\n"
          "  -n COUNT       show only the first COUNT processes\n"
          "  -s KEY         sort by cpu (default), mem or pid\n"
          "  -w SECS        refresh every SECS seconds until q or Ctrl-C\n"
          "  -v, --verbose  print debug messages to stderr\n"
          "      --help     print this help and exit\n",
          out);
}
