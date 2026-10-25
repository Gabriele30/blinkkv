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

#define CHURN_LIVE_KEYS     300000U
#define CHURN_TOTAL_OPS     2000000U
#define CHURN_SAMPLE_STEP   400000U
#define LATENCY_SAMPLES     20000U

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static size_t get_rss_kb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages = 0;
    if (fscanf(f, "%*s%ld", &pages) == 1) {
        fclose(f);
        return (size_t)pages * 4; /* 4KB page size */
    }
    fclose(f);
    return 0;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

/* Deterministic PRNG */
static uint64_t g_rng = 0x123456789ABCDEF0ULL;
static inline uint64_t fast_rand(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1DULL;
}

static inline void make_key(uint32_t id, uint8_t *buf, size_t *len)
{
    buf[0] = 'k';
    buf[1] = '_';
    memcpy(buf + 2, &id, sizeof(id));
    *len = 2 + sizeof(id);
}

typedef struct {
    double qps_hit;
    double qps_miss;
    double p50_us;
    double p95_us;
    double p99_us;
    double avg_probe;
    uint32_t max_probe;
    size_t tombstones;
    size_t capacity;
    size_t mem_kb;
    size_t rss_kb;
    uint64_t rehashes;
    uint64_t inplace_rehashes;
} churn_metrics;

static void evaluate_table(bhh_table *table, churn_metrics *m)
{
    bhh_stats st;
    bhh_get_stats(table, &st);
    m->tombstones = st.tombstones;
    m->capacity = st.capacity;
    m->mem_kb = bhh_memory_bytes(table) / 1024;
    m->rss_kb = get_rss_kb();
    m->rehashes = st.rehash_count;
    m->inplace_rehashes = st.inplace_rehash_count;
    m->avg_probe = st.avg_probe_groups;
    m->max_probe = st.max_probe_groups;

    /* Measure GET hit throughput and latencies */
    uint64_t *latencies = malloc(LATENCY_SAMPLES * sizeof(uint64_t));
    uint8_t kbuf[32];
    size_t klen;
    const uint8_t *val_out;
    size_t val_len;

    /* Reset lookup counters to measure fresh lookup behavior */
    table->total_lookups = 0;
    table->total_probe_steps = 0;
    table->max_probe_steps = 0;

    uint64_t t0 = now_ns();
    for (size_t i = 0; i < LATENCY_SAMPLES; i++) {
        uint32_t id = (uint32_t)(fast_rand() % CHURN_LIVE_KEYS);
        make_key(id, kbuf, &klen);

        uint64_t s0 = now_ns();
        int rc = bhh_get(table, kbuf, klen, 1000ULL, &val_out, &val_len, NULL);
        uint64_t s1 = now_ns();
        latencies[i] = (s1 - s0);
        (void)rc;
    }
    uint64_t t1 = now_ns();
    m->qps_hit = (double)LATENCY_SAMPLES / ((double)(t1 - t0) / 1e9);

    /* Latency percentiles */
    qsort(latencies, LATENCY_SAMPLES, sizeof(uint64_t), compare_u64);
    m->p50_us = (double)latencies[(size_t)(LATENCY_SAMPLES * 0.50)] / 1000.0;
    m->p95_us = (double)latencies[(size_t)(LATENCY_SAMPLES * 0.95)] / 1000.0;
    m->p99_us = (double)latencies[(size_t)(LATENCY_SAMPLES * 0.99)] / 1000.0;

    /* Measure GET miss throughput */
    t0 = now_ns();
    for (size_t i = 0; i < LATENCY_SAMPLES; i++) {
        uint32_t id = 0x80000000U + (uint32_t)i; /* Guaranteed absent */
        make_key(id, kbuf, &klen);
        int rc = bhh_get(table, kbuf, klen, 1000ULL, &val_out, &val_len, NULL);
        (void)rc;
    }
    t1 = now_ns();
    m->qps_miss = (double)LATENCY_SAMPLES / ((double)(t1 - t0) / 1e9);

    /* Update probe metrics from fresh lookups */
    bhh_get_stats(table, &st);
    m->avg_probe = st.avg_probe_groups;
    m->max_probe = st.max_probe_groups;

    free(latencies);
}

static void run_churn_experiment(const char *name, bhh_tombstone_policy policy)
{
    printf("\n========================================================================\n");
    printf(" Experiment: %s\n", name);
    printf(" Maintaining %u Constant Live Keys over %u Churn Operations\n",
           CHURN_LIVE_KEYS, CHURN_TOTAL_OPS);
    printf("========================================================================\n");

    bhh_table *table = bhh_create(524288);
    bhh_set_tombstone_policy(table, policy);

    /* Track active keys in circular churn window */
    uint32_t *active_keys = malloc(CHURN_LIVE_KEYS * sizeof(uint32_t));
    uint8_t kbuf[32], vbuf[32];
    size_t klen, vlen;

    /* Initial population: CHURN_LIVE_KEYS */
    for (uint32_t i = 0; i < CHURN_LIVE_KEYS; i++) {
        active_keys[i] = i;
        make_key(i, kbuf, &klen);
        vbuf[0] = 'v';
        memcpy(vbuf + 1, &i, sizeof(i));
        vlen = 1 + sizeof(i);
        bhh_set(table, kbuf, klen, vbuf, vlen, UINT64_MAX, NULL);
    }

    churn_metrics m0;
    evaluate_table(table, &m0);
    printf(" [Initial Baseline] Cap: %zu | Tombstones: %zu | Probe Avg: %.2f Max: %u | Hit QPS: %.2fM | Miss QPS: %.2fM | p99: %.2f us | Mem: %zu KB | RSS: %zu KB\n",
           m0.capacity, m0.tombstones, m0.avg_probe, m0.max_probe,
           m0.qps_hit / 1e6, m0.qps_miss / 1e6, m0.p99_us, m0.mem_kb, m0.rss_kb);

    printf("---------------------------------------------------------------------------------------------------------------------------------\n");
    printf("%-10s | %-9s | %-10s | %-12s | %-12s | %-9s | %-9s | %-8s | %-8s | %-8s\n",
           "Ops", "Cap", "Tombstones", "Avg/Max Prob", "Hit QPS", "Miss QPS", "p50 (us)", "p99 (us)", "Mem(KB)", "Rehashes");
    printf("---------------------------------------------------------------------------------------------------------------------------------\n");

    uint32_t next_new_id = CHURN_LIVE_KEYS;
    size_t churn_cursor = 0;

    for (uint32_t op = 1; op <= CHURN_TOTAL_OPS; op++) {
        /* Churn cycle: Delete one active key, insert one new key */
        uint32_t old_id = active_keys[churn_cursor];
        make_key(old_id, kbuf, &klen);
        bhh_del(table, kbuf, klen);

        uint32_t new_id = next_new_id++;
        active_keys[churn_cursor] = new_id;
        make_key(new_id, kbuf, &klen);
        vbuf[0] = 'v';
        memcpy(vbuf + 1, &new_id, sizeof(new_id));
        vlen = 1 + sizeof(new_id);
        bhh_set(table, kbuf, klen, vbuf, vlen, UINT64_MAX, NULL);

        churn_cursor = (churn_cursor + 1) % CHURN_LIVE_KEYS;

        if (op % CHURN_SAMPLE_STEP == 0) {
            churn_metrics m;
            evaluate_table(table, &m);
            char probe_buf[32];
            snprintf(probe_buf, sizeof(probe_buf), "%.2f / %u", m.avg_probe, m.max_probe);
            char rehash_buf[32];
            snprintf(rehash_buf, sizeof(rehash_buf), "%lu (+%lu)",
                     (unsigned long)m.rehashes, (unsigned long)m.inplace_rehashes);

            printf("%-10u | %-9zu | %-10zu | %-12s | %-9.2fM    | %-9.2fM  | %-9.2f | %-8.2f | %-8zu | %-8s\n",
                   op, m.capacity, m.tombstones, probe_buf,
                   m.qps_hit / 1e6, m.qps_miss / 1e6, m.p50_us, m.p99_us, m.mem_kb, rehash_buf);
        }
    }

    free(active_keys);
    bhh_free(table);
}

int main(void)
{
    printf("========================================================================\n");
    printf(" BlinkKV / BlinkHash Tombstone Churn & Degradation Study\n");
    printf(" Evaluating Tombstone Growth, Probing Chains, and Memory Inflation\n");
    printf("========================================================================\n");

    run_churn_experiment("POLICY 1: Naive Capacity Doubling (No Scrubbing)", BHH_POLICY_DOUBLING);
    run_churn_experiment("POLICY 2: In-Place Rehash & Tombstone Scrubbing", BHH_POLICY_INPLACE_REHASH);

    return 0;
}
