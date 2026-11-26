// SPDX-License-Identifier: MIT

// Turns the already computed lists into text. Nothing here reads /proc or does math.
// Two modes, chosen by render_opts:
//   - snapshot (rows = cols = 0): print everything as plain text, fine for a file or a pipe
//   - live (rows, cols = terminal size): cut the frame to fit the window and add escape
//     codes so the old frame is overwritten in place
//
// Screen layout:
//     0:  15.7%    1:   9.8% ...     <- print_cores (cells per line = width / 13)
//       PID USER     S   CPU% ...    <- header
//      6155 dmytro-h S   94.7 ...    <- print_row, as many as -n and the height allow

#include "render.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <string.h>

#include "log.h"

#define DEFAULT_COLS 80
#define CORE_CELL_WIDTH 13 // Width of "%3u:%6.1f%%  ": 3 + ':' + 6 + '%' + 2 spaces

// Write error to status.
// Called after a failed write. errno says why it failed.
static enum error_code write_error(void) {
    if (errno == EPIPE) {
        return PTOP_OUTPUT_CLOSED; // "ptop | head": not an error
    }
    log_errno(PTOP_LOG_ERR, "cannot write output");
    return PTOP_SYS_CALL_ERROR;
}

// uid to user name.
// The returned pointer is valid until the next call: getpwuid has a static buffer
// getpwuid looks the uid up in /etc/passwd (or LDAP etc.) and returns a struct passwd:
// pw_name, pw_uid, pw_gid, pw_dir, pw_shell, ... Only pw_name is used here.
// It is NULL when the uid has no user (deleted user, a process from a container).
static const char *user_name(uid_t uid, char *buf, size_t size) {
    struct passwd *pw = getpwuid(uid); // valid until the next call

    if (pw != NULL) {
        return pw->pw_name;
    }
    snprintf(buf, size, "%u", (unsigned)uid); // No user: show the number, in the caller's buffer
    return buf; // will print just the number of the user id instead of the user
}

// Prints one line.
// Print the line, cut to the terminal width if there is one. Returns -1 on a write error.
//
// Escape codes: \x1b is the ESC character (27). The terminal reads "ESC [ ..." as a
// command, not as text. \x1b[K = erase from the cursor to the end of the line.
// The new frame is written over the old one, so a shorter new line would leave the
// tail of the old line on screen without it. Not used in snapshot mode: in a file or
// a pipe it would only be junk characters.
static int print_line(FILE *out, const char *line, int cols) {
    const char *eol = cols > 0 ? "\x1b[K\n" : "\n"; // In live mode, erase the old frame's tail
    int width = cols > 0 ? cols : (int)strlen(line);

    // %.*s prints at most width characters of line: the width comes from the argument list
    return fprintf(out, "%.*s%s", width, line, eol) < 0 ? -1 : 0;
}

// Prints the core block.
// Prints at most max_lines lines. Returns the number printed, or -1 on a write error.
// The return value matters: render_screen uses it to know how many process rows still fit.
static int print_cores(FILE *out, const struct core_list *cores, int cols, int max_lines) {
    // How many 13-character cells fit in one line (80 columns when there is no terminal)
    int per_line = (cols > 0 ? cols : DEFAULT_COLS) / CORE_CELL_WIDTH;
    int lines = 0;

    if (per_line < 1) {
        per_line = 1; // A terminal narrower than one cell still gets one core per line
    }

    for (size_t i = 0; i < cores->len && lines < max_lines; i++) {
        const struct core_info *c = &cores->items[i];

        // One cell, always 13 characters, so the columns line up:
        //   %3u    core id, right-aligned in 3 characters   "  0"
        //   :      literal
        //   %6.1f  usage, 6 characters with 1 decimal       "  15.7"
        //   %%     a literal '%' sign
        //   "  "   two spaces before the next cell
        if (fprintf(out, "%3u:%6.1f%%  ", c->id, c->pct) < 0) {
            return -1;
        }
        // End the line after every per_line cells, and after the last core
        if ((i + 1) % (size_t)per_line == 0 || i + 1 == cores->len) {
            if (print_line(out, "", cols) != 0) { // Empty line = just the line ending (+ erase)
                return -1;
            }
            lines++;
        }
    }
    return lines;
}

// Prints one process.
static int print_row(FILE *out, const struct proc_info *p, int cols) {
    char uid_text[16];        // Fallback text for user_name: any uid fits
    char name[PROC_NAME_MAX]; // A copy we can change: p is const
    char line[128];           // One full row, cut later to the terminal width

    // A process picks its own name, so it must not reach the terminal as control codes
    // (a name with \x1b[2J would clear the screen). Every control character becomes '?'.
    // The cast to unsigned char matters: iscntrl with a negative char is undefined behaviour.
    memcpy(name, p->name, sizeof(name));
    for (char *c = name; *c != '\0'; c++) {
        if (iscntrl((unsigned char)*c)) {
            *c = '?';
        }
    }

    // Row format, matching the header in render_screen:
    //   %7d     pid, right-aligned in 7
    //   %-8.8s  user, left-aligned, padded to 8 and cut at 8 ("dmytro-hilei" -> "dmytro-h")
    //   %c      state letter
    //   %6.1f   CPU% with 1 decimal
    //   %9.1f   RSS in MiB (bytes / 1024 / 1024)
    //   %s      name, last because its length varies
    // snprintf never writes past line, a longer row is cut safely.
    snprintf(line, sizeof(line), "%7d %-8.8s %c %6.1f %9.1f  %s", (int)p->pid,
             user_name(p->uid, uid_text, sizeof(uid_text)), p->state, p->cpu_pct,
             (double)p->rss_bytes / (1024.0 * 1024.0), name);
    return print_line(out, line, cols);
}

// Draws the whole screen.
enum error_code render_screen(FILE *out, const struct proc_list *procs,
                              const struct core_list *cores, const struct render_opts *o) {
    char header[64];
    long limit = (long)procs->len; // How many process rows to print, start with all of them

    // In live mode the frame must fit in rows - 1 lines, or the terminal scrolls on every
    // redraw: keep room for the header, one process and the free last line
    // (rows = 0 is snapshot mode: no limit)
    int max_core_lines = o->rows > 0 ? o->rows - 3 : INT_MAX;

    int core_lines = print_cores(out, cores, o->cols, max_core_lines);
    if (core_lines < 0) {
        return write_error();
    }

    // Limit 1: -n, when it is given and smaller than the list
    if (o->count > 0 && o->count < limit) {
        limit = o->count;
    }
    // Limit 2 (live mode only): what is left of the height after the cores
    if (o->rows > 0) {
        long room = (long)o->rows - core_lines - 2; // The header, and a free last line
        if (room < 0) {
            room = 0;
        }
        if (room < limit) {
            limit = room;
        }
    }
    // The list is already sorted (-s), so the first limit rows are the ones to show

    snprintf(header, sizeof(header), "%7s %-8s %c %6s %9s  %s", "PID", "USER", 'S', "CPU%",
             "RSS(MiB)", "NAME"); // Same widths as the row format in print_row
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

// Sends the frame out.
// stdout is buffered, so the text above may still be in memory. fflush sends it out,
// and a write error (like EPIPE from "ptop | head") may only show up here.
enum error_code render_flush(FILE *out) {
    if (fflush(out) != 0) {
        return write_error();
    }
    return PTOP_SUCCESS;
}
