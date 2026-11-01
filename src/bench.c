#include "riden_core.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: riden-bench DEVICE [SAMPLES=200] [hot|status|full]\n");
        return 2;
    }
    unsigned n = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 200;
    if (n < 2 || n > 100000)
        return 2;
    rc_config cfg = rc_default_config();
    if (argc > 3) {
        if (!strcmp(argv[3], "status")) {
            cfg.poll_start = 4;
            cfg.poll_count = 16;
        } else if (!strcmp(argv[3], "full")) {
            cfg.poll_start = 0;
            cfg.poll_count = 121;
        } else if (strcmp(argv[3], "hot"))
            return 2;
    }
    rc_transport t;
    int e = rc_serial_open(argv[1], 115200, &t);
    if (e)
        return 1;
    rc_core *c = rc_create(&t, &cfg);
    if (!c) {
        rc_serial_close(&t);
        return 1;
    }
    double *intervals = calloc(n, sizeof(double));
    if (!intervals) {
        rc_destroy(c);
        rc_serial_close(&t);
        return 1;
    }
    rc_snapshot s;
    uint64_t seq = 0, prev = 0, begin = 0, failures = 0;
    for (unsigned i = 0; i < 3; i++) {
        if (rc_snapshot_wait(c, seq, 5000, &s))
            goto fail;
        seq = s.sequence;
    }
    prev = s.completed_ns;
    begin = prev;
    failures = s.failures;
    printf("index,sequence,completed_ns,rtt_ms,interval_ms,voltage_raw,current_raw,power_hi,power_"
           "lo\n");
    for (unsigned i = 0; i < n; i++) {
        if (rc_snapshot_wait(c, seq, 5000, &s))
            goto fail;
        seq = s.sequence;
        intervals[i] = (s.completed_ns - prev) / 1e6;
        printf("%u,%" PRIu64 ",%" PRIu64 ",%.6f,%.6f,%u,%u,%u,%u\n", i, seq, s.completed_ns,
               (s.completed_ns - s.request_ns) / 1e6, intervals[i], s.registers[10],
               s.registers[11], s.registers[12], s.registers[13]);
        prev = s.completed_ns;
    }
    qsort(intervals, n, sizeof(double), cmp);
    fprintf(stderr,
            "{\"samples\":%u,\"hz\":%.6f,\"interval_p50_ms\":%.6f,\"interval_p95_ms\":%.6f,"
            "\"interval_p99_ms\":%.6f,\"interval_max_ms\":%.6f,\"failures\":%" PRIu64 "}\n",
            n, n * 1e9 / (prev - begin), intervals[n / 2], intervals[(n - 1) * 95 / 100],
            intervals[(n - 1) * 99 / 100], intervals[n - 1], s.failures - failures);
    free(intervals);
    rc_destroy(c);
    rc_serial_close(&t);
    return 0;
fail:
    free(intervals);
    rc_destroy(c);
    rc_serial_close(&t);
    return 1;
}
