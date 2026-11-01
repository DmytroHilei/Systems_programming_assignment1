// SPDX-License-Identifier: MIT

#include "args.h"

#include <getopt.h>
#include <limits.h>
#include <string.h>

#include "log.h"
#include "util.h"

#define MAX_INTERVAL_S 86400L // One day, so the wait in milliseconds fits an int

static enum error_code parse_sort(const char *s, enum proc_sort *out) {
    if (strcmp(s, "cpu") == 0) {
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

enum error_code args_parse(int argc, char *argv[], struct options *o) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"verbose", no_argument, NULL, 'v'},
        {NULL, 0, NULL, 0},
    };
    int r;

    *o = (struct options){.sort = SORT_CPU};

    while (r = getopt_long(argc, argv, "hn:s:vw:", long_options, NULL), r != -1) {
        switch (r) {
            case 'h':
                o->help = 1;
                break;
            case 'n':
                if (parse_long(optarg, 1, INT_MAX, &o->count) != PTOP_SUCCESS) {
                    log_message(PTOP_LOG_ERR, "row count must be a positive number: '%s'", optarg);
                    return PTOP_USAGE_ERROR;
                }
                break;
            case 's':
                if (parse_sort(optarg, &o->sort) != PTOP_SUCCESS) {
                    return PTOP_USAGE_ERROR;
                }
                break;
            case 'v':
                o->verbose = 1;
                break;
            case 'w':
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

    if (optind < argc) {
        log_message(PTOP_LOG_ERR, "unexpected argument: '%s'", argv[optind]);
        return PTOP_USAGE_ERROR;
    }
    return PTOP_SUCCESS;
}

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
