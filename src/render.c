// SPDX-License-Identifier: MIT

#include "render.h"

#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <string.h>

#include "log.h"

#define DEFAULT_COLS 80
#define CORE_CELL_WIDTH 13 // Width of "%3u:%6.1f%%  "

static enum error_code write_error(void) {
    if (errno == EPIPE) {
        return PTOP_OUTPUT_CLOSED; // "ptop | head": not an error
    }
    log_errno(PTOP_LOG_ERR, "cannot write output");
    return PTOP_SYS_CALL_ERROR;
}

// The returned pointer is valid until the next call: getpwuid has a static buffer
static const char *user_name(uid_t uid, char *buf, size_t size) {
    struct passwd *pw = getpwuid(uid);

    if (pw != NULL) {
        return pw->pw_name;
    }
    snprintf(buf, size, "%u", (unsigned)uid);
    return buf;
}

// Print the line, cut to the terminal width if there is one. Returns -1 on a write error.
static int print_line(FILE *out, const char *line, int cols) {
    const char *eol = cols > 0 ? "\x1b[K\n" : "\n"; // In live mode, erase the old frame's tail
    int width = cols > 0 ? cols : (int)strlen(line);

    return fprintf(out, "%.*s%s", width, line, eol) < 0 ? -1 : 0;
}

// Returns the number of lines printed, or -1 on a write error
static int print_cores(FILE *out, const struct core_list *cores, int cols) {
    int per_line = (cols > 0 ? cols : DEFAULT_COLS) / CORE_CELL_WIDTH;
    int lines = 0;

    if (per_line < 1) {
        per_line = 1;
    }

    for (size_t i = 0; i < cores->len; i++) {
        const struct core_info *c = &cores->items[i];

        if (fprintf(out, "%3u:%6.1f%%  ", c->id, c->pct) < 0) {
            return -1;
        }
        if ((i + 1) % (size_t)per_line == 0 || i + 1 == cores->len) {
            if (print_line(out, "", cols) != 0) {
                return -1;
            }
            lines++;
        }
    }
    return lines;
}

static int print_row(FILE *out, const struct proc_info *p, int cols) {
    char uid_text[16];
    char name[PROC_NAME_MAX];
    char line[128];

    // A process picks its own name, so it must not reach the terminal as control codes
    memcpy(name, p->name, sizeof(name));
    for (char *c = name; *c != '\0'; c++) {
        if (iscntrl((unsigned char)*c)) {
            *c = '?';
        }
    }

    snprintf(line, sizeof(line), "%7d %-8.8s %c %6.1f %9.1f  %s", (int)p->pid,
             user_name(p->uid, uid_text, sizeof(uid_text)), p->state, p->cpu_pct,
             (double)p->rss_bytes / (1024.0 * 1024.0), name);
    return print_line(out, line, cols);
}

enum error_code render_screen(FILE *out, const struct proc_list *procs,
                              const struct core_list *cores, const struct render_opts *o) {
    char header[64];
    long limit = (long)procs->len;

    int core_lines = print_cores(out, cores, o->cols);
    if (core_lines < 0) {
        return write_error();
    }

    if (o->count > 0 && o->count < limit) {
        limit = o->count;
    }
    if (o->rows > 0) {
        long room = (long)o->rows - core_lines - 2; // The header, and a free last line
        if (room < 1) {
            room = 1;
        }
        if (room < limit) {
            limit = room;
        }
    }

    snprintf(header, sizeof(header), "%7s %-8s %c %6s %9s  %s", "PID", "USER", 'S', "CPU%",
             "RSS(MiB)", "NAME");
    if (print_line(out, header, o->cols) != 0) {
        return write_error();
    }

    for (long i = 0; i < limit; i++) {
        if (print_row(out, &procs->items[i], o->cols) != 0) {
            return write_error();
        }
    }
    return PTOP_SUCCESS;
}

enum error_code render_flush(FILE *out) {
    if (fflush(out) != 0) {
        return write_error();
    }
    return PTOP_SUCCESS;
}
