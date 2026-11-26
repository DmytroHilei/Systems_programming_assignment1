// SPDX-License-Identifier: MIT

// Reading processes and cores from /proc, and turning two readings into percentages.
// Nothing here prints to stdout: that is render.c.

#include "proc.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "util.h"

#define INITIAL_CAP 64 // Initial capacity for proc_list and core_list

// errno to status.
// ENOENT / ESRCH: the process is gone, which is normal and the caller just skips it.
// Anything else (e.g. EACCES, permission denied) is a real system call error.
static enum error_code process_error(int errnum) {
    return errnum == ENOENT || errnum == ESRCH ? PTOP_PROCESS_GONE : PTOP_SYS_CALL_ERROR;
}

// /proc is a virtual file system: nothing is stored on disk, the kernel generates
// the contents on every read. It is a "window" into kernel state that user mode
// can read as ordinary text files. Every running process has a /proc/<PID>/ folder.
//
// What ptop uses from it:
//   /proc/<PID>/        - the folder itself; its owner (st_uid from stat()) is the process uid
//   /proc/<PID>/stat    - one line: "pid (name) state ppid ... utime stime ... rss ..."
//                         we take name, state, utime + stime (CPU ticks) and rss (pages)
//   /proc/stat          - "cpu" / "cpuN" lines with busy and idle ticks per core
//
// Other useful files there: /proc/<PID>/status (same data, human readable),
// /proc/<PID>/cmdline (full command), /proc/meminfo, /proc/uptime, /proc/loadavg.
//
// A process can exit between readdir() and the stat() / fopen() / fread() below,
// so ENOENT / ESRCH is normal and means "process gone" (see process_error above).

// Reads one process.
// uid from the /proc/<PID>/ folder, everything else from /proc/<PID>/stat.
// The folder is stat()ed first because the stat file has no uid in it.
static enum error_code read_one(pid_t pid, struct proc_info *out) {
    char path[32];   // Fits "/proc/" + any int + "/stat"
    char line[1024]; // Reaches past rss even if the rest of the line is cut
    struct stat
        st; // File metadata from stat(): owner (st_uid), mode, size, times; we only need st_uid

    // snprintf writes at most sizeof(path) bytes, so the path can never overflow the buffer.
    // The folder belongs to the user who runs the process, so st.st_uid gives the USER column.
    snprintf(path, sizeof(path), "/proc/%d", (int)pid);
    if (stat(path, &st) != 0) {
        return process_error(errno);
    }

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return process_error(errno);
    }

    // The file is generated on open, so its size is unknown: read into a fixed buffer.
    // fread, not fgets: fgets stops at '\n', and a process name may contain '\n'.
    size_t got = fread(line, 1, sizeof(line) - 1, f); // - 1 leaves room for the '\0'
    fclose(f); // Opened for reading: a failure here cannot lose data, so no check
    if (got == 0) {
        return PTOP_PROCESS_GONE; // Exited after the open: not fatal, the caller skips it
    }
    line[got] = '\0'; // fread does not add the terminator itself

    enum error_code status = proc_parse_stat(line, out); // Fills pid, name, state, ticks, rss
    if (status == PTOP_SUCCESS) {
        out->uid = st.st_uid; // Known from stat() above; set only if the line parsed
    }
    return status;
}

// Appends, growing if full.
// len = slots in use, cap = slots allocated. When full, the capacity doubles
// (0 -> 64 -> 128 -> ...), so realloc runs only O(log N) times for N processes and a push
// is amortized O(1).
static enum error_code proc_list_push(struct proc_list *l, const struct proc_info *p) {
    if (l->len == l->cap) {
        size_t new_cap = l->cap == 0 ? INITIAL_CAP : l->cap * 2;
        // realloc(NULL, n) works like malloc(n), so the first push needs no special case.
        // The result goes into a temporary: on failure realloc returns NULL but the old
        // block is still allocated, and "l->items = realloc(...)" would lose (leak) it.
        struct proc_info *grown = realloc(l->items, new_cap * sizeof(*grown));

        if (grown == NULL) {
            log_message(PTOP_LOG_ERR, "out of memory");
            return PTOP_OUT_OF_MEMORY;
        }
        l->items = grown;
        l->cap = new_cap;
    }

    l->items[l->len++] = *p; // Copy the struct into slot len, then len + 1
    return PTOP_SUCCESS;
}

// Empty list.
// The compound literal (struct proc_list){0} zeroes every field: items = NULL, len = 0,
// cap = 0. So it is just the initialisation before any read and expansion.
void proc_list_init(struct proc_list *l) {
    *l = (struct proc_list){0};
}

// Frees and zeroes it.
// Back to the empty state, so a second free is safe (free(NULL) does nothing).
void proc_list_free(struct proc_list *l) {
    free(l->items);
    proc_list_init(l);
}

// Reads every process.
// Basically read_one in a loop over the numeric folders of /proc.
enum error_code proc_list_read(struct proc_list *out) {
    // Set up the directory
    DIR *dir = opendir("/proc");
    if (dir == NULL) {
        log_errno(PTOP_LOG_ERR, "cannot open /proc");
        return PTOP_SYS_CALL_ERROR;
    }

    enum error_code result = PTOP_SUCCESS; // Changed below only if something goes wrong
    struct dirent *entry; // One /proc entry from readdir(); d_name is a PID or a file name

    out->len = 0; // Empty the list but keep items and cap: the buffer is reused every refresh

    // IMPORTANT: readdir() returns NULL both at the end of /proc and on an error.
    // Only errno tells them apart, so it is set to 0 before every call and checked
    // after the loop: NULL + errno == 0 means the end, NULL + errno != 0 means an error.
    // (The comma operator runs the three parts in order; only the last one is the condition.)
    while (errno = 0, entry = readdir(dir), entry != NULL) {
        long pid;
        struct proc_info info;

        // Only folders with a numeric name are processes; skip everything else
        if (parse_long(entry->d_name, 1, INT_MAX, &pid) != PTOP_SUCCESS) {
            continue; // Not a process: self, cpuinfo, ...
        }

        // Any failure for one process (it exited, permission denied, odd stat line)
        // only skips that process: the rest of the list is still useful
        enum error_code status = read_one((pid_t)pid, &info);
        if (status != PTOP_SUCCESS) {
            log_message(PTOP_LOG_DBG, "skipping pid %ld: %s", pid, ptop_get_error_message(status));
            continue;
        }

        // Out of memory is fatal for the whole read, so stop the loop
        result = proc_list_push(out, &info);
        if (result != PTOP_SUCCESS) {
            break;
        }
    }

    // NULL with errno set means readdir() failed, not that /proc ended.
    // After a break entry is not NULL, so this does not hide the push error.
    if (entry == NULL && errno != 0) {
        log_errno(PTOP_LOG_ERR, "cannot read /proc");
        result = PTOP_SYS_CALL_ERROR;
    }

    closedir(dir); // Closed on every path after a successful opendir()
    return result;
}

// Parses one stat line.
enum error_code proc_parse_stat(const char *line, struct proc_info *out) {
    // The name may contain spaces and ')', so it ends at the last ')'.
    // This is done for the sake of safety: the process picks its own name, so it may have
    // ')' in it, and we can never be sure. strchr finds the first match, strrchr the last.
    // Example: a process named "a) R 1" gives "123 (a) R 1) S ...": the first ')' would
    // end the name too early and read 'R' as the state.
    const char *open_paren = strchr(line, '(');
    const char *close_paren = strrchr(line, ')');
    int pid;
    unsigned long long utime, stime, rss_pages;

    // The kernel always puts the process name in parentheses
    if (open_paren == NULL || close_paren == NULL || open_paren > close_paren) {
        return PTOP_PARSE_ERROR;
    }

    // Read the int value for the pid at the start of the line
    if (sscanf(line, "%d", &pid) != 1) {
        return PTOP_PARSE_ERROR;
    }

    // Fields 3 to 24 of man 5 proc, read from just after the name's ')'.
    // %*d / %*u read a field and throw it away: not stored, not counted in the result.
    //
    //   Format        Fields  Meaning
    //   " %c"         3       state (R S D Z T); the leading space skips the space after ')'
    //   5 x %*d       4-8     skip: ppid, pgrp, session, tty_nr, tpgid
    //   5 x %*u       9-13    skip: flags, minflt, cminflt, majflt, cmajflt
    //   %llu %llu     14-15   utime, stime: CPU time in user / kernel mode, in clock ticks
    //   6 x %*d       16-21   skip: cutime, cstime, priority, nice, num_threads, itrealvalue
    //   2 x %*u       22-23   skip: starttime, vsize
    //   %llu          24      rss: resident memory in pages (times the page size = bytes)
    //
    // sscanf returns how many values it stored, so anything but 4 means a cut or odd line.
    // (The two format strings are joined into one by the compiler.)
    int fields = sscanf(close_paren + 1,
                        " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu"
                        " %*d %*d %*d %*d %*d %*d %*u %*u %llu",
                        &out->state, &utime, &stime, &rss_pages);
    if (fields != 4) {
        return PTOP_PARSE_ERROR; // We need to read exactly 4 fields
    }

    // Size of the name: the characters between the parentheses (pointer difference)
    size_t name_len = (size_t)(close_paren - open_paren - 1);
    if (name_len >= sizeof(out->name)) {
        name_len = sizeof(out->name) - 1; // Max 15, so name[16] can never overflow
    }
    memcpy(out->name, open_paren + 1, name_len);
    out->name[name_len] = '\0'; // memcpy does not add the terminator

    // All of the above is copied, and the results are written to the out struct
    out->pid = pid;
    out->ticks = utime + stime; // Total CPU time since the process started (user + kernel)
    out->rss_bytes = rss_pages * (unsigned long long)sysconf(_SC_PAGESIZE); // Pages to bytes
    out->cpu_pct = 0.0; // Needs two samples: filled in by proc_compute_cpu
    return PTOP_SUCCESS;
}

// CPU% from two samples.
void proc_compute_cpu(struct proc_list *cur, struct proc_list *prev, double elapsed_sec) {
    double ticks_per_sec = (double)sysconf(_SC_CLK_TCK); // How many ticks per second, usually 100
    size_t j = 0;                                        // Position in prev, only moves forward

    // Sort by pid. We need to find the same process (from prev) in cur
    proc_list_sort(cur, SORT_PID);
    proc_list_sort(prev, SORT_PID);

    // Two-pointer walk: i goes over cur, j over prev. For each process in cur, j skips
    // the pids in prev that are smaller (those processes have exited). After that,
    // prev[j] is either the same pid or a bigger one (then p is a new process).
    //
    //   prev: 1  5  7  9
    //   cur:  1  7  8  9
    //   p=1: j stays on 1           -> match
    //   p=7: j skips 5 (exited)     -> match on 7
    //   p=8: j stops on 9 (9 > 8)   -> no match, 8 is new: 0%
    //   p=9: j is on 9              -> match
    //
    // j never goes back, so the walk is O(n + m); with the sorts, O(n log n) in total.
    for (size_t i = 0; i < cur->len; i++) {
        struct proc_info *p = &cur->items[i];

        // Until the end of prev, or until the pid of item j in prev is no longer smaller
        // than the pid of item i in cur
        while (j < prev->len && prev->items[j].pid < p->pid) {
            j++;
        }

        p->cpu_pct = 0.0;                       // Default when there is nothing to compare with
        if (j < prev->len &&                    // prev is not used up
            prev->items[j].pid == p->pid &&     // Same pid: the process was there last time
            p->ticks >= prev->items[j].ticks && // Fewer ticks = a reused pid; the unsigned
                                                // subtraction below would wrap around
            elapsed_sec > 0.0) {                // No division by zero
            // CPU% = 100 * ticks used / ticks one core could use in that time.
            // Example: 2 s apart, 100 ticks/s, 150 ticks used -> 100 * 150 / 200 = 75%.
            // A process with several threads can go over 100% (like top).
            double delta = (double)(p->ticks - prev->items[j].ticks);
            p->cpu_pct = 100.0 * delta / (ticks_per_sec * elapsed_sec);
        }
    }
}

// qsort comparators: return < 0 if a goes first, > 0 if b goes first, 0 if equal.
// They take const void * because qsort works with any type; we cast back to proc_info.

// Lowest pid first.
// (x > y) - (x < y) gives 1, -1 or 0 without subtracting: x - y could overflow.
static int cmp_pid(const void *a, const void *b) {
    const struct proc_info *x = a;
    const struct proc_info *y = b;

    return (x->pid > y->pid) - (x->pid < y->pid);
}

// Highest CPU first.
// Doubles cannot be subtracted into an int, so compare. Ties fall back to pid, so the
// order is stable between frames instead of jumping around.
static int cmp_cpu(const void *a, const void *b) {
    const struct proc_info *x = a;
    const struct proc_info *y = b;

    if (x->cpu_pct != y->cpu_pct) {
        return x->cpu_pct < y->cpu_pct ? 1 : -1; // Biggest first
    }
    return cmp_pid(a, b);
}

// Most memory first.
// Same idea as cmp_cpu, by rss_bytes.
static int cmp_mem(const void *a, const void *b) {
    const struct proc_info *x = a;
    const struct proc_info *y = b;

    if (x->rss_bytes != y->rss_bytes) {
        return x->rss_bytes < y->rss_bytes ? 1 : -1; // Biggest first
    }
    return cmp_pid(a, b);
}

// Sorts by the key.
void proc_list_sort(struct proc_list *l, enum proc_sort key) {
    // A function pointer: cmp holds the address of one of the comparators above
    int (*cmp)(const void *, const void *) = cmp_pid;

    if (l->len == 0) {
        return; // Nothing to sort (and items may be NULL)
    }

    switch (key) {
        case SORT_PID:
            cmp = cmp_pid;
            break;
        case SORT_CPU:
            cmp = cmp_cpu;
            break;
        case SORT_MEM:
            cmp = cmp_mem;
            break;
    }
    qsort(l->items, l->len, sizeof(*l->items), cmp); // O(n log n) on average
}

// Appends, growing if full.
// Same as proc_list_push, for cores.
static enum error_code core_list_push(struct core_list *l, const struct core_info *c) {
    if (l->len == l->cap) {
        size_t new_cap = l->cap == 0 ? INITIAL_CAP : l->cap * 2;
        struct core_info *grown = realloc(l->items, new_cap * sizeof(*grown));

        if (grown == NULL) {
            log_message(PTOP_LOG_ERR, "out of memory");
            return PTOP_OUT_OF_MEMORY;
        }
        l->items = grown;
        l->cap = new_cap;
    }

    l->items[l->len++] = *c;
    return PTOP_SUCCESS;
}

// Empty list.
void core_list_init(struct core_list *l) {
    *l = (struct core_list){0};
}

// Frees and zeroes it.
void core_list_free(struct core_list *l) {
    free(l->items);
    core_list_init(l);
}

// Parses one cpuN line.
enum error_code core_parse_stat(const char *line, struct core_info *out) {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;

    // One "cpuN" line of /proc/stat (man 5 proc), for example:
    //   cpu0 89614 180 56096 3586718 1198 0 20645 0 0 0
    // Every number is time since boot, in clock ticks, that this core spent in each state.
    //
    //   Format   Column   Meaning                                            Counted as
    //   cpu%u    1        "cpu" then the core number -> id                   -
    //   %llu     2        user: normal processes in user mode                busy
    //   %llu     3        nice: low priority (niced) processes in user mode  busy
    //   %llu     4        system: the kernel                                 busy
    //   %llu     5        idle: doing nothing                                idle
    //   %llu     6        iowait: idle, waiting for disk I/O                 idle
    //   %llu     7        irq: hardware interrupts                           busy
    //   %llu     8        softirq: software interrupts                       busy
    //   %llu     9        steal: taken by the hypervisor (in a VM)           busy
    //   (not read) 10-11  guest, guest_nice: already included in user/nice
    //
    // The "cpu " line (sum of all cores) has no number, so "cpu%u" fails on it; core_list_read
    // skips it before calling this. sscanf must store all 9 values (id + 8 columns).
    int fields = sscanf(line, "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu", &out->id, &user,
                        &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    if (fields != 9) {
        return PTOP_PARSE_ERROR;
    }

    // Like ticks for a process, these are totals since boot. core_compute_usage turns the
    // difference between two samples into pct = 100 * delta busy / delta total.
    out->busy = user + nice + system + irq + softirq + steal; // Time the core was working
    out->total = out->busy + idle + iowait;                   // All time: busy + idle
    out->pct = 0.0;                                           // Filled in by core_compute_usage
    return PTOP_SUCCESS;
}

// Reads every core.
// /proc/stat starts with the "cpu" lines, then other counters (intr, ctxt, btime, ...):
//   cpu  1978385 4633 662128 ...    <- sum of all cores, skipped
//   cpu0 89614 180 56096 ...        <- one line per core, parsed by core_parse_stat
//   cpu1 ...
//   intr ...                        <- first line not starting with "cpu": stop
enum error_code core_list_read(struct core_list *out) {
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL) {
        log_errno(PTOP_LOG_ERR, "cannot open /proc/stat");
        return PTOP_SYS_CALL_ERROR;
    }

    enum error_code result = PTOP_SUCCESS;
    char line[256]; // A cpu line is about 60-100 characters

    out->len = 0; // Same as for processes: keep the buffer, refill it

    // The cpu lines come first. The "cpu " line is the sum of all cores, skip it.
    // fgets is fine here (unlike in read_one): these lines have no user-chosen text.
    // Stops on an error, at the end of the file, or at the first non-cpu line.
    while (result == PTOP_SUCCESS && fgets(line, sizeof(line), f) != NULL &&
           strncmp(line, "cpu", 3) == 0) {
        struct core_info core;

        if (line[3] == ' ') {
            continue; // "cpu " (space after it) = the total line, not a core
        }

        result = core_parse_stat(line, &core);
        if (result == PTOP_SUCCESS) {
            result = core_list_push(out, &core);
        } else {
            // Unlike a vanished process, a bad /proc/stat line is unexpected: a real error
            log_message(PTOP_LOG_ERR, "cannot parse /proc/stat line: %s", line);
        }
    }

    fclose(f); // Opened for reading: a failure here cannot lose data
    return result;
}

// Core % from two samples.
// Cores are in the same order every time, so they are matched by index; the id check
// catches a core that went online or offline in between.
void core_compute_usage(struct core_list *cur, const struct core_list *prev) {
    for (size_t i = 0; i < cur->len; i++) {
        struct core_info *c = &cur->items[i];

        c->pct = 0.0;
        if (i >= prev->len || prev->items[i].id != c->id) {
            continue; // A core went online or offline between the samples
        }

        // pct = busy time / all time, between the two samples. Always 0-100%.
        // total > old total: no division by zero; busy >= old busy: no unsigned wrap-around.
        const struct core_info *old = &prev->items[i];
        if (c->total > old->total && c->busy >= old->busy) {
            c->pct = 100.0 * (double)(c->busy - old->busy) / (double)(c->total - old->total);
        }
    }
}
