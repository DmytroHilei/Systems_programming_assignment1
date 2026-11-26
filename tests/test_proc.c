// SPDX-License-Identifier: MIT

// Unit tests for the pure parts of proc.c: parsing and the math. They take made-up input
// (a string or a small array), not the real /proc, so the result is always known and the
// same on every machine. That is why proc_parse_stat only takes a string and does not
// read files itself: it can be tested without real processes.
//
// Built by "make test" with every object except main.o (this file has its own main).
// Exit code 0 = all passed, 1 = something failed, so make (or CI) sees the failure.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "proc.h"

static int failures; // How many checks failed; static, so it starts at 0

// Checks one condition. On failure prints "file:line: failed: <the condition as text>"
// and counts it, but keeps going, so one run shows every failed check, not only the first.
//   #condition         turns the condition into a string ("p.pid == 1234")
//   __FILE__/__LINE__  where the CHECK is written
//   do { } while (0)   makes the macro one statement, so "if (x) CHECK(a); else ..." works
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition);                \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

// Name containing ") ".
// The most important parser case: the name "we ) ird" has a space and a ')'. Only taking
// the LAST ')' (strrchr) gives the right name, and the fields after it still line up.
// utime = 7 and stime = 3 (fields 14, 15) -> ticks 10; rss = 250 pages (field 24).
static void test_name_with_paren(void) {
    const char *line = "1234 (we ) ird) S 1 1234 1234 0 -1 4194304 100 0 0 0 7 3 0 0 20 0 1 0 "
                       "5000 12345678 250 18446744073709551615\n";
    struct proc_info p;

    CHECK(proc_parse_stat(line, &p) == PTOP_SUCCESS);
    CHECK(p.pid == 1234);
    CHECK(strcmp(p.name, "we ) ird") == 0);
    CHECK(p.state == 'S');
    CHECK(p.ticks == 10);
    CHECK(p.rss_bytes == 250ULL * (unsigned long long)sysconf(_SC_PAGESIZE)); // Pages to bytes
}

// Ordinary line, rss 0.
// A normal short line, ending exactly at field 24. utime 100 + stime 50 = 150 ticks.
// rss = 0 (like a kernel thread) must be read as 0, not as an error.
static void test_zero_rss(void) {
    const char *line = "5 (sleep) R 1 5 5 0 -1 0 0 0 0 0 100 50 0 0 20 0 1 0 1 1 0 0\n";
    struct proc_info p;

    CHECK(proc_parse_stat(line, &p) == PTOP_SUCCESS);
    CHECK(p.pid == 5);
    CHECK(strcmp(p.name, "sleep") == 0);
    CHECK(p.state == 'R');
    CHECK(p.ticks == 150);
    CHECK(p.rss_bytes == 0);
}

// Too long name is cut.
// The kernel limits names to 15 characters, but the parser must not trust that: a longer
// name is cut to PROC_NAME_MAX - 1 = 15 characters + '\0', so name[16] never overflows.
static void test_long_name_is_cut(void) {
    const char *line = "7 (a_name_much_longer_than_fifteen) S 1 7 7 0 -1 0 0 0 0 0 1 2 0 0 20 0 "
                       "1 0 1 1 3 0\n";
    struct proc_info p;

    CHECK(proc_parse_stat(line, &p) == PTOP_SUCCESS);
    CHECK(strlen(p.name) == PROC_NAME_MAX - 1);
    CHECK(strcmp(p.name, "a_name_much_lon") == 0);
}

// Bad lines are rejected.
// Every return PTOP_PARSE_ERROR path in proc_parse_stat, one line each:
static void test_malformed(void) {
    struct proc_info p;

    CHECK(proc_parse_stat("", &p) == PTOP_PARSE_ERROR);               // Empty: no parentheses
    CHECK(proc_parse_stat("12", &p) == PTOP_PARSE_ERROR);             // Only a pid
    CHECK(proc_parse_stat("12 (abc", &p) == PTOP_PARSE_ERROR);        // No ')'
    CHECK(proc_parse_stat("12 abc) S", &p) == PTOP_PARSE_ERROR);      // No '('
    CHECK(proc_parse_stat("12 )abc( S", &p) == PTOP_PARSE_ERROR);     // ')' before '('
    CHECK(proc_parse_stat("12 (abc) S 1 2", &p) == PTOP_PARSE_ERROR); // Cut line: not 4 fields
}

// CPU% and pid matching.
// Two small lists with every case of proc_compute_cpu, elapsed time 1 second:
//   pid 1: in both, used tck ticks = exactly one core for 1 s -> 100%
//   pid 2: only in prev (exited)                               -> skipped, not in cur
//   pid 3: only in cur (new)                                   -> 0%
//   pid 4: in both, but ticks 900 -> 10 (pid reused)           -> 0%, no unsigned wrap-around
// cur is deliberately not sorted (4, 3, 1): the function sorts both lists by pid itself,
// so the checks also prove that side effect (cur ends up as 1, 3, 4).
// The arrays are on the stack: proc_list just points at them, no malloc needed.
static void test_cpu_percent(void) {
    long tck = sysconf(_SC_CLK_TCK); // Ticks per second, so the test works on any system
    struct proc_info prev_items[] = {
        {.pid = 1, .ticks = 100}, {.pid = 2, .ticks = 50}, {.pid = 4, .ticks = 900}};
    struct proc_info cur_items[] = {{.pid = 4, .ticks = 10},
                                    {.pid = 3, .ticks = 500},
                                    {.pid = 1, .ticks = 100 + (unsigned long long)tck}};
    struct proc_list prev = {.items = prev_items, .len = 3, .cap = 3};
    struct proc_list cur = {.items = cur_items, .len = 3, .cap = 3};

    proc_compute_cpu(&cur, &prev, 1.0);

    // Doubles are compared with a small range, not ==, because of rounding
    CHECK(cur.items[0].pid == 1);
    CHECK(cur.items[0].cpu_pct > 99.9 && cur.items[0].cpu_pct < 100.1); // One core for 1 s
    CHECK(cur.items[1].pid == 3);
    CHECK(cur.items[1].cpu_pct == 0.0); // Started between the samples
    CHECK(cur.items[2].pid == 4);
    CHECK(cur.items[2].cpu_pct == 0.0); // Reused pid: ticks went backwards
}

// All three sort keys.
// pids 2 and 1 have the same CPU (5.0), so SORT_CPU also checks the tie-break by pid:
//   SORT_CPU: 9.0 (pid 3), then 5.0 tie -> pid 1 before pid 2   -> 3, 1, 2
//   SORT_MEM: 30 (pid 1), 20 (pid 3), 10 (pid 2)                -> 1, 3, 2
//   SORT_PID: lowest first                                      -> 1, 2, 3
static void test_sort(void) {
    struct proc_info items[] = {{.pid = 2, .cpu_pct = 5.0, .rss_bytes = 10},
                                {.pid = 1, .cpu_pct = 5.0, .rss_bytes = 30},
                                {.pid = 3, .cpu_pct = 9.0, .rss_bytes = 20}};
    struct proc_list list = {.items = items, .len = 3, .cap = 3};

    proc_list_sort(&list, SORT_CPU);
    CHECK(list.items[0].pid == 3 && list.items[1].pid == 1 && list.items[2].pid == 2);

    proc_list_sort(&list, SORT_MEM);
    CHECK(list.items[0].pid == 1 && list.items[1].pid == 3 && list.items[2].pid == 2);

    proc_list_sort(&list, SORT_PID);
    CHECK(list.items[0].pid == 1 && list.items[1].pid == 2 && list.items[2].pid == 3);
}

// Parses a cpuN line.
// "cpu3 user=10 nice=1 system=5 idle=80 iowait=2 irq=1 softirq=1 steal=0 guest guest_nice"
//   busy  = 10 + 1 + 5 + 1 + 1 + 0 = 18
//   total = 18 + 80 + 2             = 100
// And a cut line (only 2 numbers) must be an error.
static void test_core_parse(void) {
    const char *line = "cpu3 10 1 5 80 2 1 1 0 0 0\n";
    struct core_info c;

    CHECK(core_parse_stat(line, &c) == PTOP_SUCCESS);
    CHECK(c.id == 3);
    CHECK(c.busy == 18);
    CHECK(c.total == 100);
    CHECK(core_parse_stat("cpu3 10 1", &c) == PTOP_PARSE_ERROR);
}

// Core % and the id check.
//   index 0: core 0 in both: busy +50, total +100     -> 50%
//   index 1: core 1 before, core 2 now (ids differ)   -> 0%, not compared with the wrong core
static void test_core_usage(void) {
    struct core_info prev_items[] = {{.id = 0, .busy = 100, .total = 1000},
                                     {.id = 1, .busy = 100, .total = 1000}};
    struct core_info cur_items[] = {{.id = 0, .busy = 150, .total = 1100},
                                    {.id = 2, .busy = 500, .total = 5000}};
    struct core_list prev = {.items = prev_items, .len = 2, .cap = 2};
    struct core_list cur = {.items = cur_items, .len = 2, .cap = 2};

    core_compute_usage(&cur, &prev);

    CHECK(cur.items[0].pct > 49.9 && cur.items[0].pct < 50.1);
    CHECK(cur.items[1].pct == 0.0); // Core 1 went offline and core 2 came online
}

// Runs every test, reports.
int main(void) {
    test_name_with_paren();
    test_zero_rss();
    test_long_name_is_cut();
    test_malformed();
    test_cpu_percent();
    test_sort();
    test_core_parse();
    test_core_usage();

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all tests passed");
    return 0;
}
