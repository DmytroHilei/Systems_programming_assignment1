// SPDX-License-Identifier: MIT

// Everything that touches the terminal itself, only used in live mode (-w):
//   - termios: turn off line buffering and echo, so a key press arrives at once
//   - escape codes: alternate screen, hidden cursor, redraw in place
//   - window size, and waiting for a key with a timeout
// The most important rule: whatever happens, the terminal must be restored on exit
// (tty_leave), or the user is left with no cursor, no echo and our screen.
//
// Escape codes: \x1b is the ESC character (27). The terminal reads "ESC [ ..." as a
// command, not as text:
//   \x1b[?1049h  switch to the alternate screen (like vim, less, htop)
//   \x1b[?1049l  back to the normal screen, with the shell history as it was
//   \x1b[?25l    hide the cursor   (l = low = off)
//   \x1b[?25h    show the cursor   (h = high = on)
//   \x1b[H       move the cursor to the top left (Home), erases nothing
//   \x1b[J       erase from the cursor to the end of the screen

#include "tty.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "log.h"

// Used when the terminal does not report its size: the classic VT100 screen
#define DEFAULT_ROWS 24
#define DEFAULT_COLS 80

// Macros from the system headers used below:
//   STDIN_FILENO, STDOUT_FILENO  <unistd.h>   file descriptors 0 and 1 (keyboard, screen)
//   ECHO          <termios.h>  c_lflag bit: the terminal prints every typed key
//   ICANON        <termios.h>  c_lflag bit: canonical mode, input arrives a line at a time
//                              (after Enter), with editing like Backspace
//   ISIG          <termios.h>  c_lflag bit: Ctrl-C / Ctrl-\ / Ctrl-Z send signals. Kept on.
//   VSUSP         <termios.h>  index in c_cc[] of the "suspend" key, Ctrl-Z by default
//   _POSIX_VDISABLE <unistd.h> value for a c_cc[] slot that means "no key does this"
//   TCSANOW       <termios.h>  tcsetattr applies the change now (vs TCSADRAIN, TCSAFLUSH)
//   _IOFBF        <stdio.h>    setvbuf mode: fully buffered, written only when full or
//                              flushed (vs _IOLBF line buffered, _IONBF not buffered)
//   TIOCGWINSZ    <sys/ioctl.h> ioctl request "get window size", fills a struct winsize
//   POLLIN        <poll.h>     poll event: there is data to read
//   EINTR         <errno.h>    errno: a signal interrupted the call before it finished

static struct termios saved_termios; // The user's settings, put back by tty_leave
static int active;                   // 1 while the terminal is changed: tty_leave runs once

// Sets up the terminal.
enum error_code tty_enter(void) {
    // Must be static: stdout keeps using this buffer after the function returns,
    // a local array would be gone by then
    static char frame_buffer[1 << 16]; // Large enough to write a whole frame at once

    // Live mode makes no sense in a pipe or a file: main falls back to one snapshot
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return PTOP_TERMINAL_UNAVAILABLE;
    }

    // Save the current settings first, so they can be restored exactly
    if (tcgetattr(STDIN_FILENO, &saved_termios) != 0) {
        log_errno(PTOP_LOG_WARN, "cannot read terminal settings");
        return PTOP_TERMINAL_UNAVAILABLE;
    }

    // Change a copy: saved_termios stays untouched, so tty_leave can put it back.
    // c_lflag is a bit mask: "&= ~(A | B)" clears bits A and B, keeps the rest.
    //   ICANON off: input is not collected into lines, each key reaches read() at once,
    //               so 'q' works without Enter
    //   ECHO off:   typed keys are not printed over the frame
    //   ISIG kept:  Ctrl-C still raises SIGINT, so main's handler can exit and restore
    //               the terminal
    // The (tcflag_t) cast: ECHO | ICANON is an int, and ~ on an int gives a negative int.
    // Casting first makes the mask the same unsigned type as c_lflag (no sign warning).
    //
    // Ctrl-Z is switched off on its own. It would send SIGTSTP and stop ptop with no cleanup:
    // the shell would come back with no echo, no line mode and our frame still on screen.
    // With VSUSP set to _POSIX_VDISABLE, Ctrl-Z is just a byte (0x1A) for read(), not a signal.
    //
    // Nothing changes until tcsetattr below applies tio.
    struct termios tio = saved_termios;
    tio.c_lflag &= ~(tcflag_t)(ECHO | ICANON);
    tio.c_cc[VSUSP] = _POSIX_VDISABLE;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) != 0) { // TCSANOW: apply right away
        log_errno(PTOP_LOG_WARN, "cannot change terminal settings");
        return PTOP_TERMINAL_UNAVAILABLE;
    }
    active = 1;

    // atexit runs tty_leave on exit() or a return from main, whatever path got there.
    // It does not run when a signal kills the process: that is why main catches them.
    // If it cannot be registered, undo the change now instead of risking a broken terminal.
    if (atexit(tty_leave) != 0) {
        tty_leave();
        log_message(PTOP_LOG_WARN, "cannot register terminal cleanup");
        return PTOP_TERMINAL_UNAVAILABLE;
    }

    // stdout to a terminal is line buffered by default: each line would go out on its own
    // and the redraw would flicker. Fully buffered (_IOFBF) + render_flush = one write per frame.
    setvbuf(stdout, frame_buffer, _IOFBF, sizeof(frame_buffer));
    fputs("\x1b[?1049h\x1b[?25l", stdout); // Alternate screen, hide cursor
    fflush(stdout);
    return PTOP_SUCCESS;
}

// Restores the terminal.
// Called directly by run_live and again by atexit: the active flag makes the second call
// do nothing.
void tty_leave(void) {
    if (!active) {
        return;
    }
    active = 0;

    fputs("\x1b[?25h\x1b[?1049l", stdout); // Show cursor, normal screen
    fflush(stdout);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios) != 0) { // Echo and line mode back
        log_errno(PTOP_LOG_WARN, "cannot restore terminal settings, run 'reset'");
    }
}

// Gets the window size.
// Asked again every frame, so resizing the window works without a restart
void tty_size(int *rows, int *cols) {
    struct winsize w;

    *rows = DEFAULT_ROWS;
    *cols = DEFAULT_COLS;
    // TIOCGWINSZ asks the terminal driver for the window size. Some terminals report 0x0,
    // so the defaults stay unless both numbers are real.
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 && w.ws_col > 0) {
        *rows = w.ws_row;
        *cols = w.ws_col;
    }
}

// Waits for a key.
// Also the frame timer of live mode: it sleeps until a key arrives or the time is up,
// without using CPU in the meantime.
int tty_wait_key(int timeout_ms) {
    struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN}; // Wake up when stdin has data
    unsigned char key;

    // poll returns 1 = input ready, 0 = timeout, -1 = error.
    // EINTR: a signal (Ctrl-C) arrived during the wait. The handlers have no SA_RESTART,
    // so poll returns at once and the main loop sees stop_requested without waiting.
    int r = poll(&input, 1, timeout_ms);
    if (r == 0 || (r == -1 && errno == EINTR)) {
        return 0;
    }
    if (r == -1) {
        return -1;
    }

    // poll said there is data, so this read does not block. Thanks to ICANON off, it is
    // one key, not a whole line.
    ssize_t n = read(STDIN_FILENO, &key, 1);
    if (n == -1 && errno == EINTR) {
        return 0;
    }
    // n == 0 is end of input (the terminal is gone). key is unsigned char, so a byte like
    // 0xFF comes back as 255 and cannot be mistaken for -1.
    return n == 1 ? key : -1;
}

// Frame start / end.
// A frame is drawn over the old one instead of clearing the screen first: clearing would
// leave the screen empty for a moment and flicker.
//   \x1b[H   cursor to the top left
//   ... rows, each ending with \x1b[K (print_line) to erase the old line's tail ...
//   \x1b[J   erase whatever is below, in case the new frame is shorter
void tty_frame_begin(void) {
    fputs("\x1b[H", stdout);
}

void tty_frame_end(void) {
    fputs("\x1b[J", stdout);
}
