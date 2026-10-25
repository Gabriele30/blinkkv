#define _POSIX_C_SOURCE 200809L

#include "blinkkv.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <unistd.h>

/* Number of latency samples to record for percentile calculations */
#define LATENCY_SAMPLES 50000

typedef struct {
    uint64_t *latencies_ns;
    size_t sample_count;
    size_t sample_capacity;
} latency_tracker;

static void latency_init(latency_tracker *t, size_t cap)
{
    t->latencies_ns = malloc(cap * sizeof(uint64_t));
    t->sample_count = 0;
    t->sample_capacity = cap;
}

static void latency_free(latency_tracker *t)
{
    free(t->latencies_ns);
    t->latencies_ns = NULL;
}

static inline void latency_record(latency_tracker *t, uint64_t elapsed_ns)
{
    if (t->sample_count < t->sample_capacity) {
        t->latencies_ns[t->sample_count++] = elapsed_ns;
    }
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t val_a = *(const uint64_t *)a;
    uint64_t val_b = *(const uint64_t *)b;
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

static void latency_percentiles(latency_tracker *t, double *p50, double *p95, double *p99)
{
    if (t->sample_count == 0) {
        *p50 = *p95 = *p99 = 0.0;
        return;
    }
    qsort(t->latencies_ns, t->sample_count, sizeof(uint64_t), compare_u64);
    *p50 = (double)t->latencies_ns[(size_t)(t->sample_count * 0.50)] / 1000.0; /* to microseconds */
    *p95 = (double)t->latencies_ns[(size_t)(t->sample_count * 0.95)] / 1000.0;
    *p99 = (double)t->latencies_ns[(size_t)(t->sample_count * 0.99)] / 1000.0;
}

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static size_t get_peak_rss_kb(void)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return (size_t)usage.ru_maxrss; /* KB on Linux */
    }
    return 0;
}

/* Fast 64-bit XorShift PRNG for reproducible synthetic key generation */
static inline uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void format_key(char *buf, size_t buflen, uint64_t id, size_t target_len)
{
    int written = snprintf(buf, buflen, "k:%016llx:", (unsigned long long)id);
    if (written < 0) written = 0;
    size_t cur = (size_t)written;
    while (cur < target_len && cur < buflen - 1) {
        buf[cur++] = 'x';
    }
    buf[cur] = '\0';
}

static void fill_value(uint8_t *buf, size_t len, uint64_t seed)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)((seed + i) & 0xFF);
    }
}

typedef struct {
    size_t count;
    size_t bucket_count;
    size_t key_len;
    size_t val_len;
    uint64_t ttl_ms;
} bench_config;

static void run_bench_suite(const bench_config *cfg)
{
    printf("\n>>> Running Baseline Benchmark: N=%zu, Buckets=%zu (Load Factor=%.2f), Key=%zuB, Val=%zuB <<<\n",
           cfg->count, cfg->bucket_count, (double)cfg->count / (double)cfg->bucket_count,
           cfg->key_len, cfg->val_len);

    bk_store *store = bk_store_create(cfg->bucket_count);
    if (!store) {
        fprintf(stderr, "Failed to create store\n");
        return;
    }

    size_t rss_before = get_peak_rss_kb();

    latency_tracker lat_set, lat_hit, lat_miss, lat_exists, lat_del;
    latency_init(&lat_set, LATENCY_SAMPLES);
    latency_init(&lat_hit, LATENCY_SAMPLES);
    latency_init(&lat_miss, LATENCY_SAMPLES);
    latency_init(&lat_exists, LATENCY_SAMPLES);
    latency_init(&lat_del, LATENCY_SAMPLES);

    char *key_buf = malloc(cfg->key_len + 32);
    uint8_t *val_buf = malloc(cfg->val_len + 32);
    fill_value(val_buf, cfg->val_len, 0x12345678);

    /* 1. Benchmark SET (Insertions) */
    uint64_t prng = 0x853c49e6748fea9bULL;
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < cfg->count; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(key_buf, cfg->key_len + 32, id, cfg->key_len);

        uint64_t op_t0 = get_time_ns();
        bk_status s = bk_set(store, (const uint8_t *)key_buf, cfg->key_len, val_buf, cfg->val_len, cfg->ttl_ms);
        uint64_t op_el = get_time_ns() - op_t0;
        if (s != BK_OK) {
            fprintf(stderr, "SET failed at %zu\n", i);
            break;
        }
        if (i < LATENCY_SAMPLES) latency_record(&lat_set, op_el);
    }
    uint64_t t_set_total = get_time_ns() - t0;
    double set_qps = (double)cfg->count / ((double)t_set_total / 1e9);

    size_t rss_after_set = get_peak_rss_kb();
    size_t memory_used_kb = rss_after_set > rss_before ? rss_after_set - rss_before : 0;
    double bytes_per_item = cfg->count > 0 ? ((double)memory_used_kb * 1024.0 / (double)cfg->count) : 0;

    /* 2. Benchmark EXISTS (Pure Core Index Lookup without malloc/memcpy) */
    prng = 0x853c49e6748fea9bULL; /* Rewind PRNG to look up existing keys */
    t0 = get_time_ns();
    for (size_t i = 0; i < cfg->count; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(key_buf, cfg->key_len + 32, id, cfg->key_len);

        uint64_t op_t0 = get_time_ns();
        bk_status s = bk_exists(store, (const uint8_t *)key_buf, cfg->key_len);
        uint64_t op_el = get_time_ns() - op_t0;
        if (s != BK_OK) {
            fprintf(stderr, "EXISTS failed at %zu\n", i);
            break;
        }
        if (i < LATENCY_SAMPLES) latency_record(&lat_exists, op_el);
    }
    uint64_t t_exists_total = get_time_ns() - t0;
    double exists_qps = (double)cfg->count / ((double)t_exists_total / 1e9);

    /* 3. Benchmark GET Hit (With malloc/memcpy value copy) */
    prng = 0x853c49e6748fea9bULL; /* Rewind PRNG */
    t0 = get_time_ns();
    for (size_t i = 0; i < cfg->count; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(key_buf, cfg->key_len + 32, id, cfg->key_len);

        uint8_t *got = NULL;
        size_t got_len = 0;
        uint64_t op_t0 = get_time_ns();
        bk_status s = bk_get(store, (const uint8_t *)key_buf, cfg->key_len, &got, &got_len);
        uint64_t op_el = get_time_ns() - op_t0;
        if (s != BK_OK || got == NULL) {
            fprintf(stderr, "GET hit failed at %zu\n", i);
            break;
        }
        free(got);
        if (i < LATENCY_SAMPLES) latency_record(&lat_hit, op_el);
    }
    uint64_t t_hit_total = get_time_ns() - t0;
    double hit_qps = (double)cfg->count / ((double)t_hit_total / 1e9);

    /* 4. Benchmark GET Miss (Nonexistent keys) */
    prng = 0x1122334455667788ULL; /* Distinct PRNG seed to ensure misses */
    t0 = get_time_ns();
    for (size_t i = 0; i < cfg->count; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(key_buf, cfg->key_len + 32, id, cfg->key_len);

        uint8_t *got = NULL;
        size_t got_len = 0;
        uint64_t op_t0 = get_time_ns();
        bk_status s = bk_get(store, (const uint8_t *)key_buf, cfg->key_len, &got, &got_len);
        uint64_t op_el = get_time_ns() - op_t0;
        if (s != BK_ERR_NOT_FOUND) {
            fprintf(stderr, "GET miss unexpected hit at %zu\n", i);
            free(got);
            break;
        }
        if (i < LATENCY_SAMPLES) latency_record(&lat_miss, op_el);
    }
    uint64_t t_miss_total = get_time_ns() - t0;
    double miss_qps = (double)cfg->count / ((double)t_miss_total / 1e9);

    /* 5. Benchmark DEL */
    prng = 0x853c49e6748fea9bULL; /* Rewind to delete inserted keys */
    t0 = get_time_ns();
    for (size_t i = 0; i < cfg->count; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(key_buf, cfg->key_len + 32, id, cfg->key_len);

        uint64_t op_t0 = get_time_ns();
        bk_status s = bk_del(store, (const uint8_t *)key_buf, cfg->key_len);
        uint64_t op_el = get_time_ns() - op_t0;
        if (s != BK_OK) {
            fprintf(stderr, "DEL failed at %zu\n", i);
            break;
        }
        if (i < LATENCY_SAMPLES) latency_record(&lat_del, op_el);
    }
    uint64_t t_del_total = get_time_ns() - t0;
    double del_qps = (double)cfg->count / ((double)t_del_total / 1e9);

    /* Print Results */
    double p50, p95, p99;
    printf("  [Throughput]\n");
    printf("    SET:          %10.0f ops/sec\n", set_qps);
    printf("    EXISTS (hit): %10.0f ops/sec (Pure index lookup)\n", exists_qps);
    printf("    GET (hit):    %10.0f ops/sec (With value copy)\n", hit_qps);
    printf("    GET (miss):   %10.0f ops/sec\n", miss_qps);
    printf("    DEL:          %10.0f ops/sec\n", del_qps);

    printf("  [Latency (microseconds)]\n");
    latency_percentiles(&lat_set, &p50, &p95, &p99);
    printf("    SET:          p50=%6.2f us, p95=%6.2f us, p99=%6.2f us\n", p50, p95, p99);
    latency_percentiles(&lat_exists, &p50, &p95, &p99);
    printf("    EXISTS (hit): p50=%6.2f us, p95=%6.2f us, p99=%6.2f us\n", p50, p95, p99);
    latency_percentiles(&lat_hit, &p50, &p95, &p99);
    printf("    GET (hit):    p50=%6.2f us, p95=%6.2f us, p99=%6.2f us\n", p50, p95, p99);
    latency_percentiles(&lat_miss, &p50, &p95, &p99);
    printf("    GET (miss):   p50=%6.2f us, p95=%6.2f us, p99=%6.2f us\n", p50, p95, p99);
    latency_percentiles(&lat_del, &p50, &p95, &p99);
    printf("    DEL:          p50=%6.2f us, p95=%6.2f us, p99=%6.2f us\n", p50, p95, p99);

    printf("  [Memory]\n");
    printf("    Delta RSS:    %zu KB (%6.1f MB)\n", memory_used_kb, (double)memory_used_kb / 1024.0);
    printf("    Per Entry:    %6.1f bytes/entry (Net payload = %zu bytes)\n",
           bytes_per_item, cfg->key_len + cfg->val_len);

    latency_free(&lat_set);
    latency_free(&lat_hit);
    latency_free(&lat_miss);
    latency_free(&lat_exists);
    latency_free(&lat_del);
    free(key_buf);
    free(val_buf);
    bk_store_free(store);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("========================================================================\n");
    printf(" BlinkKV Baseline Store Benchmark (Chained Hash Table + FNV-1a)\n");
    printf("========================================================================\n");

    /* Test 1: Varying Load Factors (N=200,000, Key=16B, Val=32B) */
    printf("\n--- PART 1: IMPACT OF LOAD FACTOR (CHAIN LENGTH) ---\n");
    size_t n = 200000;
    bench_config cfg_lf[] = {
        { .count = n, .bucket_count = n * 4, .key_len = 16, .val_len = 32, .ttl_ms = 60000 }, /* LF = 0.25 */
        { .count = n, .bucket_count = n,     .key_len = 16, .val_len = 32, .ttl_ms = 60000 }, /* LF = 1.00 */
        { .count = n, .bucket_count = n / 2, .key_len = 16, .val_len = 32, .ttl_ms = 60000 }, /* LF = 2.00 */
        { .count = n, .bucket_count = n / 8, .key_len = 16, .val_len = 32, .ttl_ms = 60000 }, /* LF = 8.00 */
    };
    for (size_t i = 0; i < sizeof(cfg_lf) / sizeof(cfg_lf[0]); i++) {
        run_bench_suite(&cfg_lf[i]);
    }

    /* Test 2: Scalability to 1,000,000 items (LF = 1.0, Key=16B, Val=32B) */
    printf("\n--- PART 2: SCALABILITY TO 1,000,000 ELEMENTS ---\n");
    bench_config cfg_1m = {
        .count = 1000000,
        .bucket_count = 1000000,
        .key_len = 16,
        .val_len = 32,
        .ttl_ms = 120000
    };
    run_bench_suite(&cfg_1m);

    /* Test 3: Key/Value Payload Sizes (N=200,000, LF=1.0) */
    printf("\n--- PART 3: KEY / VALUE SIZES (N=200,000, LF=1.0) ---\n");
    bench_config cfg_sizes[] = {
        { .count = n, .bucket_count = n, .key_len = 16, .val_len = 32,   .ttl_ms = 60000 }, /* Small: 48B payload */
        { .count = n, .bucket_count = n, .key_len = 32, .val_len = 128,  .ttl_ms = 60000 }, /* Medium: 160B payload */
        { .count = n, .bucket_count = n, .key_len = 64, .val_len = 1024, .ttl_ms = 60000 }  /* Large: 1088B payload */
    };
    for (size_t i = 0; i < sizeof(cfg_sizes) / sizeof(cfg_sizes[0]); i++) {
        run_bench_suite(&cfg_sizes[i]);
    }

    return 0;
}
