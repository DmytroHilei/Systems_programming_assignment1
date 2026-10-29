// SPDX-License-Identifier: MIT

//This is the safe header file to help to work with error handling

#ifndef STATUS_H
#define STATUS_H

enum error_code{
    PTOP_SUCCESS = 0,
    PTOP_USAGE_ERROR,
    PTOP_OUT_OF_MEMORY,
    PTOP_SYS_CALL_ERROR, 
    PTOP_PARSE_ERROR,
    PTOP_PROCESS_GONE,
    PTOP_TERMINAL_UNAVAILABLE,
    PTOP_OUTPUT_CLOSED,
};

const char* ptop_get_error_message(enum error_code code);

#endif // STATUS_H