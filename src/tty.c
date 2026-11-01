// SPDX-License-Identifier: MIT

#include "tty.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "log.h"

#define DEFAULT_ROWS 24
#define DEFAULT_COLS 80

static struct termios saved_termios;
static int active;

enum error_code tty_enter(void) {
    static char frame_buffer[1 << 16]; // Large enough to write a whole frame at once

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return PTOP_TERMINAL_UNAVAILABLE;
    }

    if (tcgetattr(STDIN_FILENO, &saved_termios) != 0) {
        log_errno(PTOP_LOG_WARN, "cannot read terminal settings");
        return PTOP_TERMINAL_UNAVAILABLE;
    }

    struct termios tio = saved_termios;
    tio.c_lflag &= ~(tcflag_t)(ECHO | ICANON); // ISIG stays on so Ctrl-C raises SIGINT
    tio.c_cc[VSUSP] = _POSIX_VDISABLE;         // Ctrl-Z would leave the shell in our screen

    if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) != 0) {
        log_errno(PTOP_LOG_WARN, "cannot change terminal settings");
        return PTOP_TERMINAL_UNAVAILABLE;
    }
    active = 1;

    if (atexit(tty_leave) != 0) {
        tty_leave();
        log_message(PTOP_LOG_WARN, "cannot register terminal cleanup");
        return PTOP_TERMINAL_UNAVAILABLE;
    }

    setvbuf(stdout, frame_buffer, _IOFBF, sizeof(frame_buffer));
    fputs("\x1b[?1049h\x1b[?25l", stdout); // Alternate screen, hide cursor
    fflush(stdout);
    return PTOP_SUCCESS;
}

void tty_leave(void) {
    if (!active) {
        return;
    }
    active = 0;

    fputs("\x1b[?25h\x1b[?1049l", stdout); // Show cursor, normal screen
    fflush(stdout);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios) != 0) {
        log_errno(PTOP_LOG_WARN, "cannot restore terminal settings, run 'reset'");
    }
}

void tty_size(int *rows, int *cols) {
    struct winsize w;

    *rows = DEFAULT_ROWS;
    *cols = DEFAULT_COLS;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 && w.ws_col > 0) {
        *rows = w.ws_row;
        *cols = w.ws_col;
    }
}

int tty_wait_key(int timeout_ms) {
    struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
    unsigned char key;

    int r = poll(&input, 1, timeout_ms);
    if (r == 0 || (r == -1 && errno == EINTR)) {
        return 0;
    }
    if (r == -1) {
        return -1;
    }

    ssize_t n = read(STDIN_FILENO, &key, 1);
    if (n == -1 && errno == EINTR) {
        return 0;
    }
    return n == 1 ? key : -1;
}

void tty_frame_begin(void) {
    fputs("\x1b[H", stdout);
}

void tty_frame_end(void) {
    fputs("\x1b[J", stdout);
}
