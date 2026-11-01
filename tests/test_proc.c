// SPDX-License-Identifier: MIT

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
#define EXIT_USAGE 2

// One reading of the counters. CPU usage is the difference between two of these.
struct sample {
    struct proc_list procs;
    struct core_list cores;
    double taken_at;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig) {
    (void)sig;
    stop_requested = 1; // Only the flag: the main loop exits normally, so atexit runs
}

static enum error_code install_signals(void) {
    struct sigaction action = {.sa_handler = on_signal}; // No SA_RESTART: poll must be interrupted

    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        log_errno(PTOP_LOG_ERR, "cannot install signal handlers");
        return PTOP_SYS_CALL_ERROR;
    }
    return PTOP_SUCCESS;
}

static void sample_init(struct sample *s) {
    proc_list_init(&s->procs);
    core_list_init(&s->cores);
    s->taken_at = 0.0;
}

static void sample_free(struct sample *s) {
    proc_list_free(&s->procs);
    core_list_free(&s->cores);
}

static enum error_code sample_read(struct sample *s) {
    enum error_code status = proc_list_read(&s->procs);

    if (status == PTOP_SUCCESS) {
        status = core_list_read(&s->cores);
    }
    s->taken_at = now_sec();
    return status;
}

static void sample_compute(struct sample *cur, struct sample *prev, enum proc_sort key) {
    proc_compute_cpu(&cur->procs, &prev->procs, cur->taken_at - prev->taken_at);
    core_compute_usage(&cur->cores, &prev->cores);
    proc_list_sort(&cur->procs, key);
}

static enum error_code run_snapshot(const struct options *o) {
    struct sample prev;
    struct sample cur;

    sample_init(&prev);
    sample_init(&cur);

    enum error_code status = sample_read(&prev);
    if (status == PTOP_SUCCESS) {
        sleep_ms(FIRST_SAMPLE_MS);
        status = sample_read(&cur);
    }
    if (status == PTOP_SUCCESS) {
        struct render_opts view = {.count = o->count};

        sample_compute(&cur, &prev, o->sort);
        status = render_screen(stdout, &cur.procs, &cur.cores, &view);
    }
    if (status == PTOP_SUCCESS) {
        status = render_flush(stdout);
    }

    sample_free(&prev);
    sample_free(&cur);
    return status;
}

static enum error_code live_loop(const struct options *o, struct sample *prev, struct sample *cur) {
    int wait_ms = FIRST_SAMPLE_MS;
    enum error_code status = sample_read(prev);

    while (status == PTOP_SUCCESS && !stop_requested) {
        int key = tty_wait_key(wait_ms);

        if (key == 'q' || key == -1) {
            break;
        }
        if (key != 0 || stop_requested) {
            continue; // Any other key does nothing, or a signal woke us up
        }

        status = sample_read(cur);
        if (status != PTOP_SUCCESS) {
            break;
        }
        sample_compute(cur, prev, o->sort);

        struct render_opts view = {.count = o->count};
        tty_size(&view.rows, &view.cols);

        tty_frame_begin();
        status = render_screen(stdout, &cur->procs, &cur->cores, &view);
        tty_frame_end();
        if (status == PTOP_SUCCESS) {
            status = render_flush(stdout);
        }

        struct sample *swap = prev;
        prev = cur;
        cur = swap;
        wait_ms = (int)(o->interval_s * 1000);
    }
    return status;
}

static enum error_code run_live(const struct options *o) {
    struct sample a;
    struct sample b;

    if (tty_enter() != PTOP_SUCCESS) {
        log_message(PTOP_LOG_WARN, "not a terminal, printing one snapshot");
        return run_snapshot(o);
    }

    enum error_code status = install_signals();
    if (status != PTOP_SUCCESS) {
        return status;
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

int main(int argc, char *argv[]) {
    struct options options;

    if (args_parse(argc, argv, &options) != PTOP_SUCCESS) {
        args_usage(stderr);
        return EXIT_USAGE;
    }
    if (options.help) {
        args_usage(stdout);
        return EXIT_SUCCESS;
    }
    if (options.verbose) {
        log_set_level(PTOP_LOG_DBG);
    }

    // Without this "ptop | head" dies on SIGPIPE instead of seeing EPIPE
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        log_errno(PTOP_LOG_ERR, "cannot ignore SIGPIPE");
        return EXIT_FAILURE;
    }

    enum error_code status = options.interval_s > 0 ? run_live(&options) : run_snapshot(&options);

    if (status == PTOP_SUCCESS || status == PTOP_OUTPUT_CLOSED) {
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
