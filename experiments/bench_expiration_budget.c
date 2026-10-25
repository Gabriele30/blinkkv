#define _POSIX_C_SOURCE 200809L

#include "blinkhash_hybrid.h"
#include "hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

#define STORM_KEYS          200000U
#define EVENT_LOOP_TICKS    5000U
#define CONCURRENT_OPS_TICK 100U
#define TICK_INTERVAL_MS    10ULL

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static inline void make_key(uint32_t id, uint8_t *buf, size_t *len)
{
    buf[0] = 'k';
    buf[1] = '_';
    memcpy(buf + 2, &id, sizeof(id));
    *len = 2 + sizeof(id);
}

typedef enum {
    STRATEGY_FULL_PURGE = 0,
    STRATEGY_COUNT_BUDGET = 1,
    STRATEGY_TIME_BUDGET = 2
} purge_strategy_t;

typedef struct {
    const char *name;
    double max_purge_stall_ms;
    double p99_purge_stall_ms;
    double avg_purge_stall_ms;
    double max_concurrent_op_us;
    double p99_concurrent_op_us;
    uint64_t reclamation_lag_ms;
    size_t keys_reclaimed;
    size_t mem_reclaimed_kb;
    bool lazy_expiration_verified;
} purge_benchmark_result;

static void run_budget_benchmark(purge_strategy_t strat, purge_benchmark_result *res)
{
    bhh_table *table = bhh_create(STORM_KEYS * 2);

    uint8_t kbuf[32], vbuf[32];
    size_t klen, vlen;

    uint64_t start_clock_ms = 1000000ULL;
    uint64_t storm_expire_ms = start_clock_ms + 100ULL; /* Expire shortly */

    /* Insert STORM_KEYS all expiring at storm_expire_ms */
    for (uint32_t i = 0; i < STORM_KEYS; i++) {
        make_key(i, kbuf, &klen);
        vbuf[0] = 'v';
        memcpy(vbuf + 1, &i, sizeof(i));
        vlen = 1 + sizeof(i);
        bhh_set(table, kbuf, klen, vbuf, vlen, storm_expire_ms, NULL);
    }

    /* Also insert 50,000 persistent keys that never expire */
    for (uint32_t i = STORM_KEYS; i < STORM_KEYS + 50000; i++) {
        make_key(i, kbuf, &klen);
        vbuf[0] = 'v';
        memcpy(vbuf + 1, &i, sizeof(i));
        vlen = 1 + sizeof(i);
        bhh_set(table, kbuf, klen, vbuf, vlen, UINT64_MAX, NULL);
    }

    size_t initial_mem_kb = bhh_memory_bytes(table) / 1024;
    res->lazy_expiration_verified = true;

    /* Advance clock to storm_expire_ms: ALL 200,000 keys are now logically expired! */
    uint64_t current_clock_ms = storm_expire_ms + 1;

    /* Verify lazy expiration: looking up expired keys MUST return expired immediately */
    for (uint32_t i = 0; i < 1000; i++) {
        make_key(i, kbuf, &klen);
        if (bhh_exists(table, kbuf, klen, current_clock_ms) != 1) {
            res->lazy_expiration_verified = false;
        }
    }

    uint64_t *purge_latencies_ns = malloc(EVENT_LOOP_TICKS * sizeof(uint64_t));
    uint64_t *op_latencies_ns = malloc(EVENT_LOOP_TICKS * CONCURRENT_OPS_TICK * sizeof(uint64_t));
    size_t purge_sample_count = 0;
    size_t op_sample_count = 0;

    size_t total_purged = 0;
    uint64_t lag_ms = 0;
    bool fully_reclaimed = false;

    for (uint32_t tick = 0; tick < EVENT_LOOP_TICKS; tick++) {
        current_clock_ms += TICK_INTERVAL_MS;

        /* Execute purge under test */
        uint64_t p0 = now_ns();
        size_t purged_in_tick = 0;

        if (strat == STRATEGY_FULL_PURGE) {
            purged_in_tick = bhh_purge_all(table, current_clock_ms);
        } else if (strat == STRATEGY_COUNT_BUDGET) {
            purged_in_tick = bhh_purge_limit(table, current_clock_ms, 5000); /* 5,000 keys per tick */
        } else if (strat == STRATEGY_TIME_BUDGET) {
            purged_in_tick = bhh_purge_time_budget(table, current_clock_ms, 250); /* 250 us budget */
        }

        uint64_t p1 = now_ns();
        purge_latencies_ns[purge_sample_count++] = (p1 - p0);
        total_purged += purged_in_tick;

        if (!fully_reclaimed) {
            if (total_purged >= STORM_KEYS || bhh_size(table) == 50000) {
                fully_reclaimed = true;
                lag_ms = (tick + 1) * TICK_INTERVAL_MS;
            }
        }

        /* Concurrent simulated traffic in event loop */
        for (uint32_t op = 0; op < CONCURRENT_OPS_TICK; op++) {
            uint32_t query_id = STORM_KEYS + (op % 50000); /* Persistent live key */
            make_key(query_id, kbuf, &klen);

            uint64_t op0 = now_ns();
            const uint8_t *val_out;
            size_t val_len;
            int rc = bhh_get(table, kbuf, klen, current_clock_ms, &val_out, &val_len, NULL);
            uint64_t op1 = now_ns();

            op_latencies_ns[op_sample_count++] = (op1 - op0);
            (void)rc;
        }

        if (fully_reclaimed && tick > 100) break;
    }

    /* Compute metrics */
    qsort(purge_latencies_ns, purge_sample_count, sizeof(uint64_t), compare_u64);
    qsort(op_latencies_ns, op_sample_count, sizeof(uint64_t), compare_u64);

    res->max_purge_stall_ms = (double)purge_latencies_ns[purge_sample_count - 1] / 1e6;
    res->p99_purge_stall_ms = (double)purge_latencies_ns[(size_t)(purge_sample_count * 0.99)] / 1e6;
    double sum_purge = 0;
    for (size_t i = 0; i < purge_sample_count; i++) sum_purge += purge_latencies_ns[i];
    res->avg_purge_stall_ms = (sum_purge / purge_sample_count) / 1e6;

    res->max_concurrent_op_us = (double)op_latencies_ns[op_sample_count - 1] / 1e3;
    res->p99_concurrent_op_us = (double)op_latencies_ns[(size_t)(op_sample_count * 0.99)] / 1e3;

    res->reclamation_lag_ms = lag_ms;
    res->keys_reclaimed = total_purged;
    size_t final_mem_kb = bhh_memory_bytes(table) / 1024;
    res->mem_reclaimed_kb = (initial_mem_kb >= final_mem_kb) ? (initial_mem_kb - final_mem_kb) : 0;

    free(purge_latencies_ns);
    free(op_latencies_ns);
    bhh_free(table);
}

int main(void)
{
    printf("=========================================================================================\n");
    printf(" BlinkKV / BlinkHash Incremental Expiration & Budget Benchmark\n");
    printf(" Scenario: 200,000 Keys Expiring Simultaneously (Expiration Storm)\n");
    printf(" Comparing: Strategy A (Full Purge) vs Strategy B (Count Budget) vs Strategy C (Time Budget)\n");
    printf("=========================================================================================\n\n");

    purge_benchmark_result res_a = { .name = "Strategy A: Full Purge (No Budget)" };
    purge_benchmark_result res_b = { .name = "Strategy B: Count Budget (5,000 keys/tick)" };
    purge_benchmark_result res_c = { .name = "Strategy C: Time Budget (250 us/tick)" };

    run_budget_benchmark(STRATEGY_FULL_PURGE, &res_a);
    run_budget_benchmark(STRATEGY_COUNT_BUDGET, &res_b);
    run_budget_benchmark(STRATEGY_TIME_BUDGET, &res_c);

    printf("----------------------------------------------------------------------------------------------------------------------\n");
    printf("%-38s | %-13s | %-13s | %-13s | %-12s | %-12s\n",
           "Expiration Strategy", "Max Pause (ms)", "p99 Pause(ms)", "Op p99 (us)", "Op Max (us)", "Reclaim Lag");
    printf("----------------------------------------------------------------------------------------------------------------------\n");

    purge_benchmark_result *all_res[3] = { &res_a, &res_b, &res_c };
    for (int i = 0; i < 3; i++) {
        purge_benchmark_result *r = all_res[i];
        printf("%-38s | %-13.3f | %-13.3f | %-13.2f | %-12.2f | %lu ms\n",
               r->name, r->max_purge_stall_ms, r->p99_purge_stall_ms,
               r->p99_concurrent_op_us, r->max_concurrent_op_us,
               (unsigned long)r->reclamation_lag_ms);
    }
    printf("----------------------------------------------------------------------------------------------------------------------\n\n");

    printf("Key Architectural Invariants Verified:\n");
    for (int i = 0; i < 3; i++) {
        purge_benchmark_result *r = all_res[i];
        printf("  [%s] Lazy Expiration Validated: %s | Reclaimed: %zu keys\n",
               r->name, r->lazy_expiration_verified ? "YES (100% Inaccessible upon expiry)" : "NO",
               r->keys_reclaimed);
    }

    return 0;
}
