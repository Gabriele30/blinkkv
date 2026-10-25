#define _POSIX_C_SOURCE 200809L

#include "store.h"
#include "blinkhash_hybrid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define BENCH_ROUNDS 5
#define KEY_BYTES    16
#define VAL_BYTES    32
#define TOTAL_ENTRY_PAYLOAD (KEY_BYTES + VAL_BYTES) /* 48 bytes */

volatile uint64_t g_bench_sink = 0;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static size_t get_proc_rss_kb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages = 0;
    if (fscanf(f, "%*s%ld", &pages) == 1) {
        fclose(f);
        return (size_t)pages * 4;
    }
    fclose(f);
    return 0;
}

static size_t get_proc_peak_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[128];
    size_t peak_kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            sscanf(line + 6, "%zu", &peak_kb);
            break;
        }
    }
    fclose(f);
    return peak_kb;
}

static inline void make_key(uint32_t id, uint8_t *buf, size_t *len)
{
    snprintf((char *)buf, KEY_BYTES + 1, "k_%014u", id);
    *len = KEY_BYTES;
}

static inline void make_val(uint32_t id, uint8_t *buf, size_t *len)
{
    snprintf((char *)buf, VAL_BYTES + 1, "val_%028u", id);
    *len = VAL_BYTES;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static int compare_doubles(const void *a, const void *b)
{
    double va = *(const double *)a;
    double vb = *(const double *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

typedef struct {
    size_t live_keys;
    size_t simd_cap;
    size_t arena_cap;
    size_t never_allocated_slots;
    size_t freelist_count;
    size_t tombstones;
    size_t payload_bytes;
    size_t internal_mem_bytes;
    size_t rss_kb;
    size_t peak_rss_kb;
    double internal_overhead_per_key;
    double rss_per_key;
} memory_snapshot;

typedef struct {
    /* 4 Lifecycle Snapshots */
    memory_snapshot snap_empty;
    memory_snapshot snap_populated;
    memory_snapshot snap_deleted;
    memory_snapshot snap_reinserted;

    /* Throughput (QPS) */
    double populate_set_qps;
    double get_pure_hit_qps;
    double get_copyout_qps;
    double del_qps;
    double reinsert_set_qps;

    /* Latencies (in microseconds) */
    double populate_set_p50_us, populate_set_p95_us, populate_set_p99_us, populate_set_p99_9_us, populate_set_max_us;
    double get_p50_us, get_p95_us, get_p99_us, get_p99_9_us, get_max_us;
    double del_p50_us, del_p95_us, del_p99_us, del_p99_9_us, del_max_us;
    double reinsert_set_p50_us, reinsert_set_p95_us, reinsert_set_p99_us, reinsert_set_p99_9_us, reinsert_set_max_us;
} lifecycle_metrics;

static void compute_percentiles(uint64_t *samples, size_t count,
                                double *p50, double *p95, double *p99, double *p99_9, double *max_us)
{
    if (count == 0) {
        *p50 = *p95 = *p99 = *p99_9 = *max_us = 0.0;
        return;
    }
    qsort(samples, count, sizeof(uint64_t), compare_u64);
    *p50   = (double)samples[(size_t)(count * 0.50)] / 1000.0;
    *p95   = (double)samples[(size_t)(count * 0.95)] / 1000.0;
    *p99   = (double)samples[(size_t)(count * 0.99)] / 1000.0;
    size_t idx999 = (size_t)(count * 0.999);
    if (idx999 >= count) idx999 = count - 1;
    *p99_9 = (double)samples[idx999] / 1000.0;
    *max_us = (double)samples[count - 1] / 1000.0;
}

/* -------------------------------------------------------------------------
 * Target: BlinkHash Hybrid (Milestone M2.1 Decoupled Dynamic Arena)
 * ------------------------------------------------------------------------- */
static void run_hybrid_trial(size_t n, lifecycle_metrics *m)
{
    bhh_table *table = bhh_create(n * 2);
    bhh_set_growth_policy(table, BHH_GROWTH_1_5X);

    /* Snapshot 1: Store vuoto */
    bhh_stats st;
    bhh_get_stats(table, &st);
    m->snap_empty.live_keys = st.size;
    m->snap_empty.simd_cap = st.capacity;
    m->snap_empty.arena_cap = st.arena_cap;
    m->snap_empty.never_allocated_slots = st.never_allocated_slots;
    m->snap_empty.freelist_count = st.freelist_count;
    m->snap_empty.tombstones = st.tombstones;
    m->snap_empty.payload_bytes = st.payload_bytes;
    m->snap_empty.internal_mem_bytes = bhh_memory_bytes(table);
    m->snap_empty.rss_kb = get_proc_rss_kb();
    m->snap_empty.peak_rss_kb = get_proc_peak_rss_kb();
    m->snap_empty.internal_overhead_per_key = 0.0;
    m->snap_empty.rss_per_key = 0.0;

    uint8_t kbuf[32], vbuf[64], dest_buf[64];
    size_t klen, vlen;
    uint64_t now_ms = 1000000ULL;

    uint64_t *lat_samples = malloc(n * sizeof(uint64_t));

    /* --- FASE 1 -> 2: POPULATE N CHIAVI --- */
    uint64_t worst_lat = 0;
    uint64_t t0 = now_ns();
    for (uint32_t i = 0; i < n; i++) {
        make_key(i, kbuf, &klen);
        make_val(i, vbuf, &vlen);

        uint64_t op_t0 = now_ns();
        bhh_set(table, kbuf, klen, vbuf, vlen, now_ms + 10000000ULL, NULL);
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        lat_samples[i] = elapsed;
        if (elapsed > worst_lat) worst_lat = elapsed;
    }
    uint64_t t1 = now_ns();
    m->populate_set_qps = (double)n / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, n,
                        &m->populate_set_p50_us, &m->populate_set_p95_us,
                        &m->populate_set_p99_us, &m->populate_set_p99_9_us,
                        &m->populate_set_max_us);
    if ((double)worst_lat / 1000.0 > m->populate_set_max_us) {
        m->populate_set_max_us = (double)worst_lat / 1000.0;
    }

    /* Snapshot 2: N chiavi vive */
    bhh_get_stats(table, &st);
    m->snap_populated.live_keys = st.size;
    m->snap_populated.simd_cap = st.capacity;
    m->snap_populated.arena_cap = st.arena_cap;
    m->snap_populated.never_allocated_slots = st.never_allocated_slots;
    m->snap_populated.freelist_count = st.freelist_count;
    m->snap_populated.tombstones = st.tombstones;
    m->snap_populated.payload_bytes = st.payload_bytes;
    m->snap_populated.internal_mem_bytes = bhh_memory_bytes(table);
    m->snap_populated.rss_kb = get_proc_rss_kb();
    m->snap_populated.peak_rss_kb = get_proc_peak_rss_kb();
    m->snap_populated.internal_overhead_per_key = (double)(m->snap_populated.internal_mem_bytes - m->snap_populated.payload_bytes) / (double)st.size;
    m->snap_populated.rss_per_key = (double)(m->snap_populated.rss_kb * 1024) / (double)st.size;

    /* --- PURE HIT LOOKUP --- */
    t0 = now_ns();
    for (uint32_t i = 0; i < n; i++) {
        make_key(i, kbuf, &klen);
        int rc = bhh_exists(table, kbuf, klen, now_ms);
        g_bench_sink += (rc == 0) ? 1 : 0;
    }
    t1 = now_ns();
    m->get_pure_hit_qps = (double)n / ((double)(t1 - t0) / 1e9);

    /* --- GET COPY-OUT (with full latency array) --- */
    worst_lat = 0;
    t0 = now_ns();
    for (uint32_t i = 0; i < n; i++) {
        make_key(i, kbuf, &klen);
        const uint8_t *val_out;
        size_t val_len;

        uint64_t op_t0 = now_ns();
        int rc = bhh_get(table, kbuf, klen, now_ms, &val_out, &val_len, NULL);
        if (rc == 0 && val_out) {
            memcpy(dest_buf, val_out, val_len);
            g_bench_sink += dest_buf[0];
        }
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        lat_samples[i] = elapsed;
        if (elapsed > worst_lat) worst_lat = elapsed;
    }
    t1 = now_ns();
    m->get_copyout_qps = (double)n / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, n,
                        &m->get_p50_us, &m->get_p95_us,
                        &m->get_p99_us, &m->get_p99_9_us,
                        &m->get_max_us);
    if ((double)worst_lat / 1000.0 > m->get_max_us) {
        m->get_max_us = (double)worst_lat / 1000.0;
    }

    /* --- FASE 2 -> 3: DELETE N/2 CHIAVI --- */
    size_t del_count = n / 2;
    worst_lat = 0;
    t0 = now_ns();
    for (uint32_t i = (uint32_t)del_count; i < n; i++) {
        make_key(i, kbuf, &klen);

        uint64_t op_t0 = now_ns();
        bhh_del(table, kbuf, klen);
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        lat_samples[i - del_count] = elapsed;
        if (elapsed > worst_lat) worst_lat = elapsed;
    }
    t1 = now_ns();
    m->del_qps = (double)del_count / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, del_count,
                        &m->del_p50_us, &m->del_p95_us,
                        &m->del_p99_us, &m->del_p99_9_us,
                        &m->del_max_us);
    if ((double)worst_lat / 1000.0 > m->del_max_us) {
        m->del_max_us = (double)worst_lat / 1000.0;
    }

    /* Snapshot 3: N/2 chiavi vive dopo DELETE */
    bhh_get_stats(table, &st);
    m->snap_deleted.live_keys = st.size;
    m->snap_deleted.simd_cap = st.capacity;
    m->snap_deleted.arena_cap = st.arena_cap;
    m->snap_deleted.never_allocated_slots = st.never_allocated_slots;
    m->snap_deleted.freelist_count = st.freelist_count;
    m->snap_deleted.tombstones = st.tombstones;
    m->snap_deleted.payload_bytes = st.payload_bytes;
    m->snap_deleted.internal_mem_bytes = bhh_memory_bytes(table);
    m->snap_deleted.rss_kb = get_proc_rss_kb();
    m->snap_deleted.peak_rss_kb = get_proc_peak_rss_kb();
    m->snap_deleted.internal_overhead_per_key = (st.size > 0)
        ? (double)(m->snap_deleted.internal_mem_bytes - m->snap_deleted.payload_bytes) / (double)st.size
        : 0.0;
    m->snap_deleted.rss_per_key = (st.size > 0)
        ? (double)(m->snap_deleted.rss_kb * 1024) / (double)st.size
        : 0.0;

    /* --- FASE 3 -> 4: REINSERIMENTO N/2 CHIAVI --- */
    worst_lat = 0;
    t0 = now_ns();
    for (uint32_t i = (uint32_t)del_count; i < n; i++) {
        make_key(i, kbuf, &klen);
        make_val(i, vbuf, &vlen);

        uint64_t op_t0 = now_ns();
        bhh_set(table, kbuf, klen, vbuf, vlen, now_ms + 10000000ULL, NULL);
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        lat_samples[i - del_count] = elapsed;
        if (elapsed > worst_lat) worst_lat = elapsed;
    }
    t1 = now_ns();
    m->reinsert_set_qps = (double)del_count / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, del_count,
                        &m->reinsert_set_p50_us, &m->reinsert_set_p95_us,
                        &m->reinsert_set_p99_us, &m->reinsert_set_p99_9_us,
                        &m->reinsert_set_max_us);
    if ((double)worst_lat / 1000.0 > m->reinsert_set_max_us) {
        m->reinsert_set_max_us = (double)worst_lat / 1000.0;
    }

    /* Snapshot 4: N chiavi vive dopo reinserimento */
    bhh_get_stats(table, &st);
    m->snap_reinserted.live_keys = st.size;
    m->snap_reinserted.simd_cap = st.capacity;
    m->snap_reinserted.arena_cap = st.arena_cap;
    m->snap_reinserted.never_allocated_slots = st.never_allocated_slots;
    m->snap_reinserted.freelist_count = st.freelist_count;
    m->snap_reinserted.tombstones = st.tombstones;
    m->snap_reinserted.payload_bytes = st.payload_bytes;
    m->snap_reinserted.internal_mem_bytes = bhh_memory_bytes(table);
    m->snap_reinserted.rss_kb = get_proc_rss_kb();
    m->snap_reinserted.peak_rss_kb = get_proc_peak_rss_kb();
    m->snap_reinserted.internal_overhead_per_key = (st.size > 0)
        ? (double)(m->snap_reinserted.internal_mem_bytes - m->snap_reinserted.payload_bytes) / (double)st.size
        : 0.0;
    m->snap_reinserted.rss_per_key = (st.size > 0)
        ? (double)(m->snap_reinserted.rss_kb * 1024) / (double)st.size
        : 0.0;

    free(lat_samples);
    bhh_free(table);
}

/* -------------------------------------------------------------------------
 * Target: Baseline (bk_store with chained linked-list buckets)
 * ------------------------------------------------------------------------- */
static void run_baseline_trial(size_t n, lifecycle_metrics *m)
{
    size_t bucket_count = n * 2;
    bk_store *store = bk_store_create(bucket_count);

    /* Baseline memory per entry: struct (56B) + key (16B) + val (32B) = 104B */
    size_t base_struct = sizeof(void *) * 8;
    size_t buckets_mem = bucket_count * sizeof(void *);
    size_t entry_struct_mem = 56;

    /* Snapshot 1: Store vuoto */
    m->snap_empty.live_keys = 0;
    m->snap_empty.simd_cap = bucket_count;
    m->snap_empty.arena_cap = 0;
    m->snap_empty.never_allocated_slots = 0;
    m->snap_empty.freelist_count = 0;
    m->snap_empty.tombstones = 0;
    m->snap_empty.payload_bytes = 0;
    m->snap_empty.internal_mem_bytes = base_struct + buckets_mem;
    m->snap_empty.rss_kb = get_proc_rss_kb();
    m->snap_empty.peak_rss_kb = get_proc_peak_rss_kb();
    m->snap_empty.internal_overhead_per_key = 0.0;
    m->snap_empty.rss_per_key = 0.0;

    uint8_t kbuf[32], vbuf[64], dest_buf[64];
    size_t klen, vlen;

    uint64_t *lat_samples = malloc(n * sizeof(uint64_t));

    /* --- FASE 1 -> 2: POPULATE N CHIAVI --- */
    uint64_t worst_lat = 0;
    uint64_t t0 = now_ns();
    for (uint32_t i = 0; i < n; i++) {
        make_key(i, kbuf, &klen);
        make_val(i, vbuf, &vlen);

        uint64_t op_t0 = now_ns();
        bk_set(store, kbuf, klen, vbuf, vlen, 10000000ULL);
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        lat_samples[i] = elapsed;
        if (elapsed > worst_lat) worst_lat = elapsed;
    }
    uint64_t t1 = now_ns();
    m->populate_set_qps = (double)n / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, n,
                        &m->populate_set_p50_us, &m->populate_set_p95_us,
                        &m->populate_set_p99_us, &m->populate_set_p99_9_us,
                        &m->populate_set_max_us);
    if ((double)worst_lat / 1000.0 > m->populate_set_max_us) {
        m->populate_set_max_us = (double)worst_lat / 1000.0;
    }

    /* Snapshot 2: N chiavi vive */
    m->snap_populated.live_keys = n;
    m->snap_populated.simd_cap = bucket_count;
    m->snap_populated.arena_cap = 0;
    m->snap_populated.never_allocated_slots = 0;
    m->snap_populated.freelist_count = 0;
    m->snap_populated.tombstones = 0;
    m->snap_populated.payload_bytes = n * TOTAL_ENTRY_PAYLOAD;
    m->snap_populated.internal_mem_bytes = base_struct + buckets_mem + n * (entry_struct_mem + TOTAL_ENTRY_PAYLOAD);
    m->snap_populated.rss_kb = get_proc_rss_kb();
    m->snap_populated.peak_rss_kb = get_proc_peak_rss_kb();
    m->snap_populated.internal_overhead_per_key = (double)(m->snap_populated.internal_mem_bytes - m->snap_populated.payload_bytes) / (double)n;
    m->snap_populated.rss_per_key = (double)(m->snap_populated.rss_kb * 1024) / (double)n;

    /* --- PURE HIT LOOKUP (bk_exists) --- */
    t0 = now_ns();
    for (uint32_t i = 0; i < n; i++) {
        make_key(i, kbuf, &klen);
        bk_status st = bk_exists(store, kbuf, klen);
        g_bench_sink += (st == BK_OK) ? 1 : 0;
    }
    t1 = now_ns();
    m->get_pure_hit_qps = (double)n / ((double)(t1 - t0) / 1e9);

    /* --- GET COPY-OUT (bk_get + memcpy + free) --- */
    worst_lat = 0;
    t0 = now_ns();
    for (uint32_t i = 0; i < n; i++) {
        make_key(i, kbuf, &klen);
        uint8_t *out_val = NULL;
        size_t out_len = 0;

        uint64_t op_t0 = now_ns();
        bk_status st = bk_get(store, kbuf, klen, &out_val, &out_len);
        if (st == BK_OK && out_val) {
            memcpy(dest_buf, out_val, out_len);
            g_bench_sink += dest_buf[0];
            free(out_val);
        }
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        lat_samples[i] = elapsed;
        if (elapsed > worst_lat) worst_lat = elapsed;
    }
    t1 = now_ns();
    m->get_copyout_qps = (double)n / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, n,
                        &m->get_p50_us, &m->get_p95_us,
                        &m->get_p99_us, &m->get_p99_9_us,
                        &m->get_max_us);
    if ((double)worst_lat / 1000.0 > m->get_max_us) {
        m->get_max_us = (double)worst_lat / 1000.0;
    }

    /* --- FASE 2 -> 3: DELETE N/2 CHIAVI --- */
    size_t del_count = n / 2;
    worst_lat = 0;
    t0 = now_ns();
    for (uint32_t i = (uint32_t)del_count; i < n; i++) {
        make_key(i, kbuf, &klen);

        uint64_t op_t0 = now_ns();
        bk_del(store, kbuf, klen);
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        lat_samples[i - del_count] = elapsed;
        if (elapsed > worst_lat) worst_lat = elapsed;
    }
    t1 = now_ns();
    m->del_qps = (double)del_count / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, del_count,
                        &m->del_p50_us, &m->del_p95_us,
                        &m->del_p99_us, &m->del_p99_9_us,
                        &m->del_max_us);
    if ((double)worst_lat / 1000.0 > m->del_max_us) {
        m->del_max_us = (double)worst_lat / 1000.0;
    }

    /* Snapshot 3: N/2 chiavi vive dopo DELETE */
    size_t rem = n - del_count;
    m->snap_deleted.live_keys = rem;
    m->snap_deleted.simd_cap = bucket_count;
    m->snap_deleted.arena_cap = 0;
    m->snap_deleted.never_allocated_slots = 0;
    m->snap_deleted.freelist_count = 0;
    m->snap_deleted.tombstones = 0;
    m->snap_deleted.payload_bytes = rem * TOTAL_ENTRY_PAYLOAD;
    m->snap_deleted.internal_mem_bytes = base_struct + buckets_mem + rem * (entry_struct_mem + TOTAL_ENTRY_PAYLOAD);
    m->snap_deleted.rss_kb = get_proc_rss_kb();
    m->snap_deleted.peak_rss_kb = get_proc_peak_rss_kb();
    m->snap_deleted.internal_overhead_per_key = (rem > 0)
        ? (double)(m->snap_deleted.internal_mem_bytes - m->snap_deleted.payload_bytes) / (double)rem
        : 0.0;
    m->snap_deleted.rss_per_key = (rem > 0)
        ? (double)(m->snap_deleted.rss_kb * 1024) / (double)rem
        : 0.0;

    /* --- FASE 3 -> 4: REINSERIMENTO N/2 CHIAVI --- */
    worst_lat = 0;
    t0 = now_ns();
    for (uint32_t i = (uint32_t)del_count; i < n; i++) {
        make_key(i, kbuf, &klen);
        make_val(i, vbuf, &vlen);

        uint64_t op_t0 = now_ns();
        bk_set(store, kbuf, klen, vbuf, vlen, 10000000ULL);
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        lat_samples[i - del_count] = elapsed;
        if (elapsed > worst_lat) worst_lat = elapsed;
    }
    t1 = now_ns();
    m->reinsert_set_qps = (double)del_count / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, del_count,
                        &m->reinsert_set_p50_us, &m->reinsert_set_p95_us,
                        &m->reinsert_set_p99_us, &m->reinsert_set_p99_9_us,
                        &m->reinsert_set_max_us);
    if ((double)worst_lat / 1000.0 > m->reinsert_set_max_us) {
        m->reinsert_set_max_us = (double)worst_lat / 1000.0;
    }

    /* Snapshot 4: N chiavi vive dopo reinserimento */
    m->snap_reinserted.live_keys = n;
    m->snap_reinserted.simd_cap = bucket_count;
    m->snap_reinserted.arena_cap = 0;
    m->snap_reinserted.never_allocated_slots = 0;
    m->snap_reinserted.freelist_count = 0;
    m->snap_reinserted.tombstones = 0;
    m->snap_reinserted.payload_bytes = n * TOTAL_ENTRY_PAYLOAD;
    m->snap_reinserted.internal_mem_bytes = base_struct + buckets_mem + n * (entry_struct_mem + TOTAL_ENTRY_PAYLOAD);
    m->snap_reinserted.rss_kb = get_proc_rss_kb();
    m->snap_reinserted.peak_rss_kb = get_proc_peak_rss_kb();
    m->snap_reinserted.internal_overhead_per_key = (double)(m->snap_reinserted.internal_mem_bytes - m->snap_reinserted.payload_bytes) / (double)n;
    m->snap_reinserted.rss_per_key = (double)(m->snap_reinserted.rss_kb * 1024) / (double)n;

    free(lat_samples);
    bk_store_free(store);
}

static int run_isolated_target(bool is_hybrid, size_t n, lifecycle_metrics *out)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(pipefd[0]);
        lifecycle_metrics res;
        memset(&res, 0, sizeof(res));
        if (is_hybrid) {
            run_hybrid_trial(n, &res);
        } else {
            run_baseline_trial(n, &res);
        }
        if (write(pipefd[1], &res, sizeof(res)) != sizeof(res)) {
            _exit(EXIT_FAILURE);
        }
        close(pipefd[1]);
        _exit(EXIT_SUCCESS);
    }
    close(pipefd[1]);
    ssize_t rd = read(pipefd[0], out, sizeof(*out));
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return (rd == sizeof(*out) && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static double median_d(double *arr, size_t count)
{
    qsort(arr, count, sizeof(double), compare_doubles);
    return (count % 2 == 1) ? arr[count / 2] : ((arr[count / 2 - 1] + arr[count / 2]) / 2.0);
}

static size_t median_sz(size_t *arr, size_t count)
{
    double d[32];
    for (size_t i = 0; i < count; i++) d[i] = (double)arr[i];
    return (size_t)median_d(d, count);
}

static void aggregate_snapshots(lifecycle_metrics *rounds, size_t r_count, lifecycle_metrics *med)
{
    memset(med, 0, sizeof(*med));

    /* Helper macro to median double fields */
    #define MED_D(field) do { \
        double vals[32]; \
        for (size_t i = 0; i < r_count; i++) vals[i] = rounds[i].field; \
        med->field = median_d(vals, r_count); \
    } while (0)

    /* Helper macro to median size_t fields */
    #define MED_SZ(field) do { \
        size_t vals[32]; \
        for (size_t i = 0; i < r_count; i++) vals[i] = rounds[i].field; \
        med->field = median_sz(vals, r_count); \
    } while (0)

    #define AGG_SNAP(snap) do { \
        MED_SZ(snap.live_keys); \
        MED_SZ(snap.simd_cap); \
        MED_SZ(snap.arena_cap); \
        MED_SZ(snap.never_allocated_slots); \
        MED_SZ(snap.freelist_count); \
        MED_SZ(snap.tombstones); \
        MED_SZ(snap.payload_bytes); \
        MED_SZ(snap.internal_mem_bytes); \
        MED_SZ(snap.rss_kb); \
        MED_SZ(snap.peak_rss_kb); \
        MED_D(snap.internal_overhead_per_key); \
        MED_D(snap.rss_per_key); \
    } while (0)

    AGG_SNAP(snap_empty);
    AGG_SNAP(snap_populated);
    AGG_SNAP(snap_deleted);
    AGG_SNAP(snap_reinserted);

    MED_D(populate_set_qps);
    MED_D(get_pure_hit_qps);
    MED_D(get_copyout_qps);
    MED_D(del_qps);
    MED_D(reinsert_set_qps);

    MED_D(populate_set_p50_us); MED_D(populate_set_p95_us); MED_D(populate_set_p99_us); MED_D(populate_set_p99_9_us); MED_D(populate_set_max_us);
    MED_D(get_p50_us); MED_D(get_p95_us); MED_D(get_p99_us); MED_D(get_p99_9_us); MED_D(get_max_us);
    MED_D(del_p50_us); MED_D(del_p95_us); MED_D(del_p99_us); MED_D(del_p99_9_us); MED_D(del_max_us);
    MED_D(reinsert_set_p50_us); MED_D(reinsert_set_p95_us); MED_D(reinsert_set_p99_us); MED_D(reinsert_set_p99_9_us); MED_D(reinsert_set_max_us);

    #undef MED_D
    #undef MED_SZ
    #undef AGG_SNAP
}

static void print_snapshot_row(const char *phase_label, const memory_snapshot *s)
{
    printf("| %-26s | %10zu | %10zu | %10zu | %11zu | %10zu | %9zu | %11.2f MB | %10.2f MB | %10.2f MB | %13.1f B | %11.1f B |\n",
           phase_label,
           s->live_keys,
           s->simd_cap,
           s->arena_cap,
           s->never_allocated_slots,
           s->freelist_count,
           s->tombstones,
           (double)s->payload_bytes / (1024.0 * 1024.0),
           (double)s->internal_mem_bytes / (1024.0 * 1024.0),
           (double)s->rss_kb / 1024.0,
           s->internal_overhead_per_key,
           s->rss_per_key);
}

static void bench_size(const char *label, size_t n)
{
    printf("\n========================================================================================================================================\n");
    printf(" BENCHMARK COMPLETO: %s (N = %zu chiavi, 5 round isolati per target)\n", label, n);
    printf("========================================================================================================================================\n");

    lifecycle_metrics hy_rounds[BENCH_ROUNDS];
    lifecycle_metrics bs_rounds[BENCH_ROUNDS];

    for (int r = 0; r < BENCH_ROUNDS; r++) {
        if (run_isolated_target(true, n, &hy_rounds[r]) != 0) {
            fprintf(stderr, "Hybrid trial failed at round %d\n", r);
        }
        if (run_isolated_target(false, n, &bs_rounds[r]) != 0) {
            fprintf(stderr, "Baseline trial failed at round %d\n", r);
        }
    }

    lifecycle_metrics hy_med;
    lifecycle_metrics bs_med;
    aggregate_snapshots(hy_rounds, BENCH_ROUNDS, &hy_med);
    aggregate_snapshots(bs_rounds, BENCH_ROUNDS, &bs_med);

    printf("\n--- SNAPSHOT MEMORIA: BLINKHASH HYBRID (Milestone M2.1 Arena Dinamica 1.5x) ---\n");
    printf("+----------------------------+------------+------------+------------+-------------+------------+-----------+--------------+--------------+--------------+---------------+-------------+\n");
    printf("| Fase Lifecycle             | Live Keys  | SIMD Cap   | Arena Cap  | Mai Alloc.  | Free-List  | Tombstone | Payload Act. | Mem Interna  | RSS Reale    | Overhead/Key  | RSS/Key     |\n");
    printf("+----------------------------+------------+------------+------------+-------------+------------+-----------+--------------+--------------+--------------+---------------+-------------+\n");
    print_snapshot_row("1. Store Vuoto", &hy_med.snap_empty);
    print_snapshot_row("2. N Chiavi Vive", &hy_med.snap_populated);
    print_snapshot_row("3. N/2 Vive (dopo DELETE)", &hy_med.snap_deleted);
    print_snapshot_row("4. N Vive (Reinserimento)", &hy_med.snap_reinserted);
    printf("+----------------------------+------------+------------+------------+-------------+------------+-----------+--------------+--------------+--------------+---------------+-------------+\n");

    printf("\n--- SNAPSHOT MEMORIA: BASELINE (Chained Linked-List) ---\n");
    printf("+----------------------------+------------+------------+------------+-------------+------------+-----------+--------------+--------------+--------------+---------------+-------------+\n");
    printf("| Fase Lifecycle             | Live Keys  | SIMD Cap   | Arena Cap  | Mai Alloc.  | Free-List  | Tombstone | Payload Act. | Mem Interna  | RSS Reale    | Overhead/Key  | RSS/Key     |\n");
    printf("+----------------------------+------------+------------+------------+-------------+------------+-----------+--------------+--------------+--------------+---------------+-------------+\n");
    print_snapshot_row("1. Store Vuoto", &bs_med.snap_empty);
    print_snapshot_row("2. N Chiavi Vive", &bs_med.snap_populated);
    print_snapshot_row("3. N/2 Vive (dopo DELETE)", &bs_med.snap_deleted);
    print_snapshot_row("4. N Vive (Reinserimento)", &bs_med.snap_reinserted);
    printf("+----------------------------+------------+------------+------------+-------------+------------+-----------+--------------+--------------+--------------+---------------+-------------+\n");

    printf("\n--- CONFRONTO DIRETTO A PARITA' DI STATO (Snapshot 2: N = %zu chiavi vive) ---\n", n);
    double ram_saving_internal = 100.0 * (1.0 - (double)hy_med.snap_populated.internal_mem_bytes / (double)bs_med.snap_populated.internal_mem_bytes);
    double ram_saving_rss = 100.0 * (1.0 - (double)hy_med.snap_populated.rss_kb / (double)bs_med.snap_populated.rss_kb);
    printf("  - Memoria Interna: Baseline = %6.2f MB | Hybrid = %6.2f MB (Risparmio: %+.1f%%)\n",
           (double)bs_med.snap_populated.internal_mem_bytes / (1024.0 * 1024.0),
           (double)hy_med.snap_populated.internal_mem_bytes / (1024.0 * 1024.0),
           ram_saving_internal);
    printf("  - RSS Misurato:    Baseline = %6.2f MB | Hybrid = %6.2f MB (Risparmio: %+.1f%%)\n",
           (double)bs_med.snap_populated.rss_kb / 1024.0,
           (double)hy_med.snap_populated.rss_kb / 1024.0,
           ram_saving_rss);
    printf("  - Overhead/Key:    Baseline = %6.1f B  | Hybrid = %6.1f B  (Overhead Netto: %+.1f%%)\n",
           bs_med.snap_populated.internal_overhead_per_key,
           hy_med.snap_populated.internal_overhead_per_key,
           100.0 * ((hy_med.snap_populated.internal_overhead_per_key / bs_med.snap_populated.internal_overhead_per_key) - 1.0));

    printf("\n--- PRESTAZIONI OPERATIVE & LATENZE (Mediana 5 Round) ---\n");
    printf("+--------------------------------+--------------------+--------------------+------------+\n");
    printf("| Operazione                     | Baseline (QPS)     | Hybrid (QPS)       | Speedup    |\n");
    printf("+--------------------------------+--------------------+--------------------+------------+\n");
    printf("| SET Populate (con crescita)    | %15.0f ops/s | %15.0f ops/s | %9.2fx  |\n",
           bs_med.populate_set_qps, hy_med.populate_set_qps, hy_med.populate_set_qps / bs_med.populate_set_qps);
    printf("| GET Pure Hit (Index Lookup)    | %15.0f ops/s | %15.0f ops/s | %9.2fx  |\n",
           bs_med.get_pure_hit_qps, hy_med.get_pure_hit_qps, hy_med.get_pure_hit_qps / bs_med.get_pure_hit_qps);
    printf("| GET Copy-Out (Payload Copy)    | %15.0f ops/s | %15.0f ops/s | %9.2fx  |\n",
           bs_med.get_copyout_qps, hy_med.get_copyout_qps, hy_med.get_copyout_qps / bs_med.get_copyout_qps);
    printf("| DELETE (N/2 chiavi)            | %15.0f ops/s | %15.0f ops/s | %9.2fx  |\n",
           bs_med.del_qps, hy_med.del_qps, hy_med.del_qps / bs_med.del_qps);
    printf("| SET Reinsert (su free-list)    | %15.0f ops/s | %15.0f ops/s | %9.2fx  |\n",
           bs_med.reinsert_set_qps, hy_med.reinsert_set_qps, hy_med.reinsert_set_qps / bs_med.reinsert_set_qps);
    printf("+--------------------------------+--------------------+--------------------+------------+\n");

    printf("\n--- DETTAGLIO PERCENTILI DI LATENZA HYBRID M2.1 (microsecondi) ---\n");
    printf("+--------------------------------+----------+----------+----------+------------+------------+\n");
    printf("| Fase / Operazione              | p50 (us) | p95 (us) | p99 (us) | p99.9 (us) | worst (us) |\n");
    printf("+--------------------------------+----------+----------+----------+------------+------------+\n");
    printf("| SET Populate (espansione arena)| %8.2f | %8.2f | %8.2f | %10.2f | %10.2f |\n",
           hy_med.populate_set_p50_us, hy_med.populate_set_p95_us, hy_med.populate_set_p99_us, hy_med.populate_set_p99_9_us, hy_med.populate_set_max_us);
    printf("| GET Copy-Out (steady-state)    | %8.2f | %8.2f | %8.2f | %10.2f | %10.2f |\n",
           hy_med.get_p50_us, hy_med.get_p95_us, hy_med.get_p99_us, hy_med.get_p99_9_us, hy_med.get_max_us);
    printf("| DELETE (unlinking & free-list) | %8.2f | %8.2f | %8.2f | %10.2f | %10.2f |\n",
           hy_med.del_p50_us, hy_med.del_p95_us, hy_med.del_p99_us, hy_med.del_p99_9_us, hy_med.del_max_us);
    printf("| SET Reinsert (riciclo slot)    | %8.2f | %8.2f | %8.2f | %10.2f | %10.2f |\n",
           hy_med.reinsert_set_p50_us, hy_med.reinsert_set_p95_us, hy_med.reinsert_set_p99_us, hy_med.reinsert_set_p99_9_us, hy_med.reinsert_set_max_us);
    printf("+--------------------------------+----------+----------+----------+------------+------------+\n");
}

int main(void)
{
    printf("========================================================================================================================================\n");
    printf(" BLINKKV BENCHMARK SUITE — MILESTONE M2.1 AUDITED METHODOLOGY\n");
    printf(" Metodologia corretta: 4 Snapshot completi, parita' rigorosa di chiavi vive/payload, isolamento fork, 5 round\n");
    printf("========================================================================================================================================\n");

    bench_size("100k Keys", 100000);
    bench_size("300k Keys", 300000);
    bench_size("1M Keys",   1000000);

    return 0;
}
