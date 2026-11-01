// SPDX-License-Identifier: MIT

#ifndef RENDER_H
#define RENDER_H

#include <stdio.h>

#include "proc.h"
#include "status.h"

// rows and cols are 0 in snapshot mode and the terminal size in live mode
struct render_opts {
    long count; // -n, 0 = all rows
    int rows;
    int cols;
};

// Print the core usage and the process table. PTOP_OUTPUT_CLOSED means the reader is gone.
enum error_code render_screen(FILE *out, const struct proc_list *procs,
                              const struct core_list *cores, const struct render_opts *o);

enum error_code render_flush(FILE *out);

#endif // RENDER_H
