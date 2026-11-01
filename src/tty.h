// SPDX-License-Identifier: MIT

// SPDX-License-Identifier: MIT

#ifndef TTY_H
#define TTY_H

#include "status.h"

// Switch to non-canonical mode and the alternate screen. Also registers tty_leave with
// atexit. Returns PTOP_TERMINAL_UNAVAILABLE if stdin or stdout is not a terminal.
enum error_code tty_enter(void);

// Restore the terminal. Safe to call more than once.
void tty_leave(void);

// Falls back to 24x80 if the size is unknown
void tty_size(int *rows, int *cols);

// Wait for a key. Returns the key, 0 on timeout or a signal, -1 on end of input or error.
int tty_wait_key(int timeout_ms);

// Start a frame at the top left, and clear what is left of the old frame after it
void tty_frame_begin(void);
void tty_frame_end(void);

#endif // TTY_H
