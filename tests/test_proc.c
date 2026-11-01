// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "proc.h"

static int failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition);                \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void test_name_with_paren(void) {
    const char *line = "1234 (we ) ird) S 1 1234 1234 0 -1 4194304 100 0 0 0 7 3 0 0 20 0 1 0 "
                       "5000 12345678 250 18446744073709551615\n";
    struct proc_info p;

    CHECK(proc_parse_stat(line, &p) == PTOP_SUCCESS);
    CHECK(p.pid == 1234);
    CHECK(strcmp(p.name, "we ) ird") == 0);
    CHECK(p.state == 'S');
    CHECK(p.ticks == 10);
    CHECK(p.rss_bytes == 250ULL * (unsigned long long)sysconf(_SC_PAGESIZE));
}

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

static void test_long_name_is_cut(void) {
    const char *line = "7 (a_name_much_longer_than_fifteen) S 1 7 7 0 -1 0 0 0 0 0 1 2 0 0 20 0 "
                       "1 0 1 1 3 0\n";
    struct proc_info p;

    CHECK(proc_parse_stat(line, &p) == PTOP_SUCCESS);
    CHECK(strlen(p.name) == PROC_NAME_MAX - 1);
    CHECK(strcmp(p.name, "a_name_much_lon") == 0);
}

static void test_malformed(void) {
    struct proc_info p;

    CHECK(proc_parse_stat("", &p) == PTOP_PARSE_ERROR);
    CHECK(proc_parse_stat("12", &p) == PTOP_PARSE_ERROR);
    CHECK(proc_parse_stat("12 (abc", &p) == PTOP_PARSE_ERROR);
    CHECK(proc_parse_stat("12 abc) S", &p) == PTOP_PARSE_ERROR);
    CHECK(proc_parse_stat("12 )abc( S", &p) == PTOP_PARSE_ERROR);
    CHECK(proc_parse_stat("12 (abc) S 1 2", &p) == PTOP_PARSE_ERROR);
}

static void test_cpu_percent(void) {
    long tck = sysconf(_SC_CLK_TCK);
    struct proc_info prev_items[] = {
        {.pid = 1, .ticks = 100}, {.pid = 2, .ticks = 50}, {.pid = 4, .ticks = 900}};
    struct proc_info cur_items[] = {{.pid = 4, .ticks = 10},
                                    {.pid = 3, .ticks = 500},
                                    {.pid = 1, .ticks = 100 + (unsigned long long)tck}};
    struct proc_list prev = {.items = prev_items, .len = 3, .cap = 3};
    struct proc_list cur = {.items = cur_items, .len = 3, .cap = 3};

    proc_compute_cpu(&cur, &prev, 1.0);

    CHECK(cur.items[0].pid == 1);
    CHECK(cur.items[0].cpu_pct > 99.9 && cur.items[0].cpu_pct < 100.1); // One core for 1 s
    CHECK(cur.items[1].pid == 3);
    CHECK(cur.items[1].cpu_pct == 0.0); // Started between the samples
    CHECK(cur.items[2].pid == 4);
    CHECK(cur.items[2].cpu_pct == 0.0); // Reused pid: ticks went backwards
}

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

static void test_core_parse(void) {
    const char *line = "cpu3 10 1 5 80 2 1 1 0 0 0\n";
    struct core_info c;

    CHECK(core_parse_stat(line, &c) == PTOP_SUCCESS);
    CHECK(c.id == 3);
    CHECK(c.busy == 18);
    CHECK(c.total == 100);
    CHECK(core_parse_stat("cpu3 10 1", &c) == PTOP_PARSE_ERROR);
}

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
