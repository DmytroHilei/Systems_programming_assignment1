// SPDX-License-Identifier: MIT

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

#define INITIAL_CAP 64

static enum error_code process_error(int errnum) {
    return errnum == ENOENT || errnum == ESRCH ? PTOP_PROCESS_GONE : PTOP_SYS_CALL_ERROR;
}

static enum error_code read_one(pid_t pid, struct proc_info *out) {
    char path[32];   // Fits "/proc/" + any int + "/stat"
    char line[1024]; // Reaches past rss even if the rest of the line is cut
    struct stat st;

    snprintf(path, sizeof(path), "/proc/%d", (int)pid);
    if (stat(path, &st) != 0) {
        return process_error(errno);
    }

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return process_error(errno);
    }

    // fread, not fgets: a process name may contain '\n'
    size_t got = fread(line, 1, sizeof(line) - 1, f);
    fclose(f); // Opened for reading: a failure here cannot lose data
    if (got == 0) {
        return PTOP_PROCESS_GONE; // The process exited after the open
    }
    line[got] = '\0';

    enum error_code status = proc_parse_stat(line, out);
    if (status == PTOP_SUCCESS) {
        out->uid = st.st_uid;
    }
    return status;
}

static enum error_code proc_list_push(struct proc_list *l, const struct proc_info *p) {
    if (l->len == l->cap) {
        size_t new_cap = l->cap == 0 ? INITIAL_CAP : l->cap * 2;
        struct proc_info *grown = realloc(l->items, new_cap * sizeof(*grown));

        if (grown == NULL) {
            log_message(PTOP_LOG_ERR, "out of memory");
            return PTOP_OUT_OF_MEMORY;
        }
        l->items = grown;
        l->cap = new_cap;
    }

    l->items[l->len++] = *p;
    return PTOP_SUCCESS;
}

void proc_list_init(struct proc_list *l) {
    *l = (struct proc_list){0};
}

void proc_list_free(struct proc_list *l) {
    free(l->items);
    proc_list_init(l);
}

enum error_code proc_list_read(struct proc_list *out) {
    DIR *dir = opendir("/proc");
    if (dir == NULL) {
        log_errno(PTOP_LOG_ERR, "cannot open /proc");
        return PTOP_SYS_CALL_ERROR;
    }

    enum error_code result = PTOP_SUCCESS;
    struct dirent *entry;

    out->len = 0;

    // errno is reset every round: it is the only way to tell an error from the end
    while (errno = 0, entry = readdir(dir), entry != NULL) {
        long pid;
        struct proc_info info;

        if (parse_long(entry->d_name, 1, INT_MAX, &pid) != PTOP_SUCCESS) {
            continue; // Not a process: self, cpuinfo, ...
        }

        enum error_code status = read_one((pid_t)pid, &info);
        if (status != PTOP_SUCCESS) {
            log_message(PTOP_LOG_DBG, "skipping pid %ld: %s", pid, ptop_get_error_message(status));
            continue;
        }

        result = proc_list_push(out, &info);
        if (result != PTOP_SUCCESS) {
            break;
        }
    }

    if (entry == NULL && errno != 0) {
        log_errno(PTOP_LOG_ERR, "cannot read /proc");
        result = PTOP_SYS_CALL_ERROR;
    }

    closedir(dir);
    return result;
}

enum error_code proc_parse_stat(const char *line, struct proc_info *out) {
    // The name may contain spaces and ')', so it ends at the last ')'
    const char *open_paren = strchr(line, '(');
    const char *close_paren = strrchr(line, ')');
    int pid;
    unsigned long long utime, stime, rss_pages;

    if (open_paren == NULL || close_paren == NULL || open_paren > close_paren) {
        return PTOP_PARSE_ERROR;
    }
    if (sscanf(line, "%d", &pid) != 1) {
        return PTOP_PARSE_ERROR;
    }

    // man 5 proc fields 3 to 24: state, skip 10, utime, stime, skip 8, rss
    int fields = sscanf(close_paren + 1,
                        " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu"
                        " %*d %*d %*d %*d %*d %*d %*u %*u %llu",
                        &out->state, &utime, &stime, &rss_pages);
    if (fields != 4) {
        return PTOP_PARSE_ERROR;
    }

    size_t name_len = (size_t)(close_paren - open_paren - 1);
    if (name_len >= sizeof(out->name)) {
        name_len = sizeof(out->name) - 1;
    }
    memcpy(out->name, open_paren + 1, name_len);
    out->name[name_len] = '\0';

    out->pid = pid;
    out->ticks = utime + stime;
    out->rss_bytes = rss_pages * (unsigned long long)sysconf(_SC_PAGESIZE);
    out->cpu_pct = 0.0;
    return PTOP_SUCCESS;
}

void proc_compute_cpu(struct proc_list *cur, struct proc_list *prev, double elapsed_sec) {
    double ticks_per_sec = (double)sysconf(_SC_CLK_TCK);
    size_t j = 0;

    proc_list_sort(cur, SORT_PID);
    proc_list_sort(prev, SORT_PID);

    // Both lists are sorted, so one walk over each finds every match
    for (size_t i = 0; i < cur->len; i++) {
        struct proc_info *p = &cur->items[i];

        while (j < prev->len && prev->items[j].pid < p->pid) {
            j++;
        }

        p->cpu_pct = 0.0; // New process, or a reused pid whose ticks went backwards
        if (j < prev->len && prev->items[j].pid == p->pid && p->ticks >= prev->items[j].ticks &&
            elapsed_sec > 0.0) {
            double delta = (double)(p->ticks - prev->items[j].ticks);
            p->cpu_pct = 100.0 * delta / (ticks_per_sec * elapsed_sec);
        }
    }
}

static int cmp_pid(const void *a, const void *b) {
    const struct proc_info *x = a;
    const struct proc_info *y = b;

    return (x->pid > y->pid) - (x->pid < y->pid);
}

static int cmp_cpu(const void *a, const void *b) {
    const struct proc_info *x = a;
    const struct proc_info *y = b;

    if (x->cpu_pct != y->cpu_pct) {
        return x->cpu_pct < y->cpu_pct ? 1 : -1; // Biggest first
    }
    return cmp_pid(a, b);
}

static int cmp_mem(const void *a, const void *b) {
    const struct proc_info *x = a;
    const struct proc_info *y = b;

    if (x->rss_bytes != y->rss_bytes) {
        return x->rss_bytes < y->rss_bytes ? 1 : -1; // Biggest first
    }
    return cmp_pid(a, b);
}

void proc_list_sort(struct proc_list *l, enum proc_sort key) {
    int (*cmp)(const void *, const void *) = cmp_pid;

    if (l->len == 0) {
        return;
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
    qsort(l->items, l->len, sizeof(*l->items), cmp);
}

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

void core_list_init(struct core_list *l) {
    *l = (struct core_list){0};
}

void core_list_free(struct core_list *l) {
    free(l->items);
    core_list_init(l);
}

enum error_code core_parse_stat(const char *line, struct core_info *out) {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;

    // guest time is already counted in user, so the last two columns are not read
    int fields = sscanf(line, "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu", &out->id, &user,
                        &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    if (fields != 9) {
        return PTOP_PARSE_ERROR;
    }

    out->busy = user + nice + system + irq + softirq + steal;
    out->total = out->busy + idle + iowait;
    out->pct = 0.0;
    return PTOP_SUCCESS;
}

enum error_code core_list_read(struct core_list *out) {
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL) {
        log_errno(PTOP_LOG_ERR, "cannot open /proc/stat");
        return PTOP_SYS_CALL_ERROR;
    }

    enum error_code result = PTOP_SUCCESS;
    char line[256];

    out->len = 0;

    // The cpu lines come first. The "cpu " line is the sum of all cores, skip it.
    while (result == PTOP_SUCCESS && fgets(line, sizeof(line), f) != NULL &&
           strncmp(line, "cpu", 3) == 0) {
        struct core_info core;

        if (line[3] == ' ') {
            continue;
        }

        result = core_parse_stat(line, &core);
        if (result == PTOP_SUCCESS) {
            result = core_list_push(out, &core);
        } else {
            log_message(PTOP_LOG_ERR, "cannot parse /proc/stat line: %s", line);
        }
    }

    fclose(f); // Opened for reading: a failure here cannot lose data
    return result;
}

void core_compute_usage(struct core_list *cur, const struct core_list *prev) {
    for (size_t i = 0; i < cur->len; i++) {
        struct core_info *c = &cur->items[i];

        c->pct = 0.0;
        if (i >= prev->len || prev->items[i].id != c->id) {
            continue; // A core went online or offline between the samples
        }

        const struct core_info *old = &prev->items[i];
        if (c->total > old->total && c->busy >= old->busy) {
            c->pct = 100.0 * (double)(c->busy - old->busy) / (double)(c->total - old->total);
        }
    }
}
