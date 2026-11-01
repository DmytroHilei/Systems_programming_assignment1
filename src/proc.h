// SPDX-License-Identifier: MIT

// SPDX-License-Identifier: MIT

#ifndef PROC_H
#define PROC_H

#include <stddef.h>
#include <sys/types.h>

#include "status.h"

#define PROC_NAME_MAX 16 // comm is at most 15 chars + '\0', anything longer is cut

enum proc_sort { SORT_PID, SORT_CPU, SORT_MEM };

struct proc_info {
    pid_t pid;
    uid_t uid;
    char state; // R S D Z T ...
    char name[PROC_NAME_MAX];
    unsigned long long ticks; // utime + stime
    unsigned long long rss_bytes;
    double cpu_pct; // Filled by proc_compute_cpu
};

// items is allocated by the read function and freed by the matching free function
struct proc_list {
    struct proc_info *items;
    size_t len;
    size_t cap;
};

struct core_info {
    unsigned id;
    unsigned long long busy;  // Ticks spent working
    unsigned long long total; // busy + idle ticks
    double pct;               // Filled by core_compute_usage
};

struct core_list {
    struct core_info *items;
    size_t len;
    size_t cap;
};

void proc_list_init(struct proc_list *l);
void proc_list_free(struct proc_list *l);

// Replace the contents of out with every process in /proc. Processes that vanish
// while reading are skipped.
enum error_code proc_list_read(struct proc_list *out);

// Parse one line of /proc/<pid>/stat. Does not set uid or touch the file system.
enum error_code proc_parse_stat(const char *line, struct proc_info *out);

// Fill cpu_pct from the tick difference. Sorts both lists by pid.
void proc_compute_cpu(struct proc_list *cur, struct proc_list *prev, double elapsed_sec);

void proc_list_sort(struct proc_list *l, enum proc_sort key);

void core_list_init(struct core_list *l);
void core_list_free(struct core_list *l);

// Replace the contents of out with one entry per online core from /proc/stat
enum error_code core_list_read(struct core_list *out);

// Parse one "cpuN ..." line of /proc/stat
enum error_code core_parse_stat(const char *line, struct core_info *out);

void core_compute_usage(struct core_list *cur, const struct core_list *prev);

#endif // PROC_H
