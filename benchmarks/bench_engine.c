#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "lsm.h"

#define NUM_OPS 1000000
#define WARMUP_OPS 10000
#define KEY_SIZE 16
#define VAL_SIZE 100

static inline uint64_t get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int compare_uint64(const void *a, const void *b) {
    uint64_t fa = *(const uint64_t *)a;
    uint64_t fb = *(const uint64_t *)b;
    return (fa > fb) - (fa < fb);
}

int main(void) {
    lsm_db *db = NULL;
    lsm_config config;
    lsm_config_init(&config);
    config.max_file_size = 64 * 1024 * 1024;
    config.block_size = 4096;

    if (lsm_open("bench_db", &config, &db) != LSM_OK) {
        fprintf(stderr, "Failed to open LSM database\n");
        return 1;
    }

    uint64_t *latencies = malloc(NUM_OPS * sizeof(uint64_t));
    if (!latencies) {
        fprintf(stderr, "Memory allocation failed\n");
        lsm_close(db);
        return 1;
    }

    char key[KEY_SIZE];
    char val[VAL_SIZE];
    memset(val, 'x', VAL_SIZE);

    for (int i = 0; i < WARMUP_OPS; i++) {
        snprintf(key, KEY_SIZE, "key%012d", i);
        lsm_put(db, key, strlen(key), val, VAL_SIZE);
    }

    uint64_t start_time = get_ns();
    for (int i = 0; i < NUM_OPS; i++) {
        snprintf(key, KEY_SIZE, "key%012d", WARMUP_OPS + i);
        uint64_t op_start = get_ns();
        lsm_put(db, key, strlen(key), val, VAL_SIZE);
        uint64_t op_end = get_ns();
        latencies[i] = op_end - op_start;
    }
    uint64_t end_time = get_ns();

    uint64_t total_ns = end_time - start_time;
    double total_sec = (double)total_ns / 1e9;
    double ops_sec = (double)NUM_OPS / total_sec;

    qsort(latencies, NUM_OPS, sizeof(uint64_t), compare_uint64);

    uint64_t p50 = latencies[(int)(NUM_OPS * 0.50)];
    uint64_t p90 = latencies[(int)(NUM_OPS * 0.90)];
    uint64_t p99 = latencies[(int)(NUM_OPS * 0.99)];
    uint64_t p999 = latencies[(int)(NUM_OPS * 0.999)];

    printf("+------------------------+----------------------+\n");
    printf("| Metric                 | Value                |\n");
    printf("+------------------------+----------------------+\n");
    printf("| Total Operations       | %-20d |\n", NUM_OPS);
    printf("| Throughput (ops/sec)   | %-20.2f |\n", ops_sec);
    printf("| P50 Latency (ns)       | %-20llu |\n", (unsigned long long)p50);
    printf("| P90 Latency (ns)       | %-20llu |\n", (unsigned long long)p90);
    printf("| P99 Latency (ns)       | %-20llu |\n", (unsigned long long)p99);
    printf("| P99.9 Latency (ns)     | %-20llu |\n", (unsigned long long)p999);
    printf("+------------------------+----------------------+\n");

    free(latencies);
    lsm_close(db);
    return 0;
}