// SPDX-License-Identifier: MIT

// Ties the modules together:
//   args_parse -> options
//   interval_s == 0: run_snapshot  read, wait 0.5 s, read again, compute, print once
//   interval_s > 0:  run_live      the same in a loop, drawn over the terminal (tty.c)
// Every function returns an error_code; only main turns it into an exit code.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "args.h"
#include "log.h"
#include "proc.h"
#include "render.h"
#include "status.h"
#include "tty.h"
#include "util.h"

#define FIRST_SAMPLE_MS 500 // CPU usage needs two samples, this far apart at the start
#define EXIT_USAGE 2        // Bad command line, like most tools (EXIT_FAILURE = 1 is the rest)

// One reading of the counters. CPU usage is the difference between two of these.
struct sample {
    struct proc_list procs; // Every process: pid, name, ticks so far, memory, ...
    struct core_list cores; // Every core: busy and total ticks so far
    double taken_at;        // now_sec() when it was read, for the elapsed time between two
};

// Set by the signal handler, read by the loop in live_loop.
//   volatile:     the compiler must read it from memory every time, it may change
//                 "behind its back" (in the handler)
//   sig_atomic_t: an integer type that is written in one step, so a signal can never see
//                 it half written
static volatile sig_atomic_t stop_requested;

// Signal handler: requests a stop.
// A handler can interrupt the program at any point, so it may only call
// async-signal-safe functions. printf, malloc, even tty_leave are not, so it only sets
// a flag and the loop does the cleanup.
static void on_signal(int sig) {
    (void)sig;          // Unused: every signal means the same thing, "stop"
    stop_requested = 1; // Only the flag: the main loop exits normally, so atexit runs
}

// Catches Ctrl-C and friends.
static enum error_code install_signals(void) {
    // sigaction instead of signal(): its behaviour is the same on every system.
    // sa_flags = 0: no SA_RESTART, so a signal makes poll() return with EINTR and the
    // loop notices the flag at once instead of waiting for the timeout.
    struct sigaction action = {.sa_handler = on_signal}; // No SA_RESTART: poll must be interrupted

    sigemptyset(&action.sa_mask); // Block no extra signals while the handler runs
    // SIGINT = Ctrl-C, SIGTERM = kill, SIGQUIT = Ctrl-\, SIGHUP = the terminal was closed.
    // SIGQUIT (Ctrl-\) and SIGHUP would otherwise kill ptop and leave the terminal broken
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0 ||
        sigaction(SIGQUIT, &action, NULL) != 0 || sigaction(SIGHUP, &action, NULL) != 0) {
        log_errno(PTOP_LOG_ERR, "cannot install signal handlers");
        return PTOP_SYS_CALL_ERROR;
    }
    return PTOP_SUCCESS;
}

// Empty sample.
// Empty lists (no memory yet), so sample_free is safe even if nothing was read
static void sample_init(struct sample *s) {
    proc_list_init(&s->procs);
    core_list_init(&s->cores);
    s->taken_at = 0.0;
}

// Frees sample memory.
static void sample_free(struct sample *s) {
    proc_list_free(&s->procs);
    core_list_free(&s->cores);
}

// Takes one reading.
// Read all processes, then all cores, and note the time. Reusing a sample keeps its
// memory: the read functions set len = 0 and fill the same arrays again.
static enum error_code sample_read(struct sample *s) {
    enum error_code status = proc_list_read(&s->procs);

    if (status == PTOP_SUCCESS) { // Each step runs only if the one before worked
        status = core_list_read(&s->cores);
    }
    s->taken_at = now_sec();
    return status;
}

// Computes percentages, sorts.
// Turn two samples into percentages, then put the processes in the order of -s.
// proc_compute_cpu sorts both lists by pid to match them, so the -s sort must come last.
static void sample_compute(struct sample *cur, struct sample *prev, enum proc_sort key) {
    proc_compute_cpu(&cur->procs, &prev->procs, cur->taken_at - prev->taken_at);
    core_compute_usage(&cur->cores, &prev->cores);
    proc_list_sort(&cur->procs, key);
}

// Prints one snapshot.
// So this function runs a single snapshot. Live mode does not call it: live_loop repeats
// the same steps (read, compute, draw) in a loop.
//
// A sample has these fields: procs (list of all processes), cores (list of all cores) and
// taken_at (time of reading, from now_sec(): a monotonic clock, on Linux counted from boot).
// The core list says how busy each core is (not what runs on it); the process list says
// how much CPU and memory each process uses, etc.
//
// So we create the samples for the previous and the current reading, initialise each,
// read the previous sample, sleep for a bit, read the current sample, compute the
// difference between the two samples, and then render the screen with the current data.
//
// This is the main use of struct o (the options from args_parse) here:
// o->count is the number of rows to display (0 means all) and o->sort is the sorting key
// (SORT_CPU, SORT_MEM or SORT_PID). o->interval_s is 0 here (that is why we are in
// snapshot mode), and the other fields were already handled by main.
static enum error_code run_snapshot(const struct options *o) {
    struct sample prev;
    struct sample cur;

    // Makes each of the lists empty (NULL, 0, 0) and sets taken_at to 0.0
    sample_init(&prev);
    sample_init(&cur);

    // Try to read the state of the system for the previous sample
    enum error_code status = sample_read(&prev);
    if (status == PTOP_SUCCESS) {
        // If the previous read was fine, we sleep for a bit and read the current one, and use
        // both for the compute. CPU% needs two readings: one alone only has totals since start.
        // (No swap is needed here: a snapshot computes only once. live_loop does swap.)
        sleep_ms(FIRST_SAMPLE_MS);
        status = sample_read(&cur);
    } // And status is updated and carried forward to the next step

    if (status == PTOP_SUCCESS) {
        // If the reads were fine, we create a new (local, on the stack) struct view that
        // copies only o->count; rows and cols become 0 (designated initializer: the fields
        // not named are zero). render_opts is defined in render.h.
        // render_screen needs a different set of values than options: count plus the screen
        // size. rows = cols = 0 means "no terminal": print everything, no cutting.
        // (In live mode the screen size can change every frame, so a fresh view is built in
        // each loop iteration and the terminal is measured again each time.)
        struct render_opts view = {.count = o->count};

        // Now we compute the difference between those. o->sort is the sorting key, which is
        // SORT_CPU, SORT_MEM or SORT_PID; it is used to sort the process list before rendering.
        sample_compute(&cur, &prev, o->sort);

        // The cores are shown separately at the beginning (top), just as a usage percentage,
        // then the process table
        status = render_screen(stdout, &cur.procs, &cur.cores, &view);
    }
    if (status == PTOP_SUCCESS) {
        status = render_flush(stdout); // Flush the output buffer (EPIPE may show up only here)
    }

    // Free on every path, then return the status for the error handling in main
    sample_free(&prev);
    sample_free(&cur);
    return status;
}

// Live redraw loop.
// One frame per pass:
//   wait (for a key or the time) -> read cur -> compute against prev -> draw -> swap
// The first frame comes FIRST_SAMPLE_MS after the start, the next ones every interval_s.
static enum error_code live_loop(const struct options *o, struct sample *prev, struct sample *cur) {
    double next_frame = now_sec() + FIRST_SAMPLE_MS / 1000.0; // When to draw the next frame
    enum error_code status = sample_read(prev);               // The first "old" sample

    // Stops on an error, on a signal (stop_requested), or by break: 'q' or end of input
    while (status == PTOP_SUCCESS && !stop_requested) {
        // Wait only for what is left, so pressing keys does not push the next frame back
        int wait_ms = (int)((next_frame - now_sec()) * 1000.0);
        if (wait_ms < 0) {
            wait_ms = 0; // Already late (drawing took long): no waiting
        }

        // Returns the key, 0 on timeout or a signal, -1 when the terminal is gone
        int key = tty_wait_key(wait_ms);

        if (key == 'q' || key == -1) {
            break;
        }
        if (key != 0 || stop_requested) {
            continue; // Any other key does nothing, or a signal woke us up
        }
        // Here key == 0 and no signal: the time is up, draw a frame

        status = sample_read(cur);
        if (status != PTOP_SUCCESS) {
            break;
        }
        sample_compute(cur, prev, o->sort);

        // A fresh view every frame: the window may have been resized since the last one
        struct render_opts view = {.count = o->count};
        tty_size(&view.rows, &view.cols);

        // Cursor home, draw over the old frame, erase what is left below, send it all at once
        tty_frame_begin();
        status = render_screen(stdout, &cur->procs, &cur->cores, &view);
        tty_frame_end();
        if (status == PTOP_SUCCESS) {
            status = render_flush(stdout);
        }

        // This frame becomes the "old" sample for the next one. Only the pointers swap:
        // nothing is copied, and the old prev's memory is reused by the next sample_read.
        struct sample *swap = prev;
        prev = cur;
        cur = swap;
        next_frame = now_sec() + (double)o->interval_s;
    }
    return status;
}

// Live mode: setup, loop, cleanup.
static enum error_code run_live(const struct options *o) {
    struct sample a; // The two samples live here, live_loop only swaps pointers to them
    struct sample b;

    // "ptop -w 2 > file" or in a pipe: no terminal to draw on, so do one snapshot instead
    if (tty_enter() != PTOP_SUCCESS) {
        log_message(PTOP_LOG_WARN, "not a terminal, printing one snapshot");
        return run_snapshot(o);
    }

    // Only in live mode: in snapshot mode the default Ctrl-C (kill) is fine, there is no
    // terminal state to restore
    enum error_code status = install_signals();
    if (status != PTOP_SUCCESS) {
        return status; // The terminal is still restored: tty_enter registered atexit
    }

    sample_init(&a);
    sample_init(&b);
    status = live_loop(o, &a, &b);
    sample_free(&a);
    sample_free(&b);

    // Restore first: a message printed on the alternate screen disappears with it
    tty_leave();
    if (status != PTOP_SUCCESS) {
        log_message(PTOP_LOG_ERR, "live mode stopped: %s", ptop_get_error_message(status));
    }
    return status;
}

// Parses options, picks the mode.
int main(int argc, char *argv[]) {
    struct options options; // Filled with defaults and the flags by args_parse

    if (args_parse(argc, argv, &options) != PTOP_SUCCESS) {
        args_usage(stderr); // Error: usage goes to stderr, exit code 2
        return EXIT_USAGE;
    }
    if (options.help) {
        args_usage(stdout); // Asked for: usage goes to stdout, exit code 0
        return EXIT_SUCCESS;
    }
    if (options.verbose) {
        log_set_level(PTOP_LOG_DBG); // -v: also show debug messages (like skipped pids)
    }

    // Writing to a pipe whose reader has closed sends SIGPIPE, and by default it kills
    // the process. Ignored, the write fails with errno EPIPE instead, and render.c turns
    // that into PTOP_OUTPUT_CLOSED.
    // Without this "ptop | head" dies on SIGPIPE instead of seeing EPIPE
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        log_errno(PTOP_LOG_ERR, "cannot ignore SIGPIPE");
        return EXIT_FAILURE;
    }

    // -w picks the mode: no interval = one snapshot, an interval = live
    enum error_code status = options.interval_s > 0 ? run_live(&options) : run_snapshot(&options);

    // Exit codes: 0 = worked (a closed pipe counts, "ptop | head" did its job),
    // 1 = something failed, 2 = bad command line (returned above)
    if (status == PTOP_SUCCESS || status == PTOP_OUTPUT_CLOSED) {
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
