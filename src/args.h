// SPDX-License-Identifier: MIT

#ifndef ARGS_H
#define ARGS_H

#include <stdio.h>

#include "proc.h"
#include "status.h"

struct options {
    long count;          // -n, 0 = all rows
    enum proc_sort sort; // -s, default SORT_CPU
    long interval_s;     // -w, 0 = snapshot mode
    int verbose;         // -v
    int help;            // --help
};

// Returns PTOP_USAGE_ERROR on a bad option or argument. The cause is already logged.
enum error_code args_parse(int argc, char *argv[], struct options *o);

void args_usage(FILE *out);

#endif // ARGS_H
