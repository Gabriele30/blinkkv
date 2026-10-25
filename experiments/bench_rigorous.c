#define _POSIX_C_SOURCE 200809L

#include "blinkkv.h"
#include "blinkhash_a.h"
#include "blinkhash_b.h"
#include "blinkhash_c.h"
#include "blinkhash_hybrid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <sys/wait.h>

#define SAMPLES_MAX 50000

typedef struct {
    uint64_t *latencies_ns;
    size_t count;
    size_t cap;
} lat_tracker;

static void lat_init(lat_tracker *t, size_t cap) {
    t->latencies_ns = malloc(cap * sizeof(uint64_t));
    t->count = 0;
    t->cap = cap;
}

static void lat_free(lat_tracker *t) {
    free(t->latencies_ns);
    t->latencies_ns = NULL;
}

static inline void lat_record(lat_tracker *t, uint64_t ns) {
    if (t->count < t->cap) t->latencies_ns[t->count++] = ns;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t v1 = *(const uint64_t *)a;
    uint64_t v2 = *(const uint64_t *)b;
    return (v1 < v2) ? -1 : (v1 > v2) ? 1 : 0;
}

static void lat_calc(lat_tracker *t, double *p50, double *p95, double *p99) {
    if (t->count == 0) { *p50 = *p95 = *p99 = 0.0; return; }
    qsort(t->latencies_ns, t->count, sizeof(uint64_t), cmp_u64);
    *p50 = (double)t->latencies_ns[(size_t)(t->count * 0.50)] / 1000.0;
    *p95 = (double)t->latencies_ns[(size_t)(t->count * 0.95)] / 1000.0;
    *p99 = (double)t->latencies_ns[(size_t)(t->count * 0.99)] / 1000.0;
}

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static size_t get_proc_rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    if (fscanf(f, "%lu %lu", &size, &resident) == 2) {
        fclose(f);
        return (size_t)(resident * 4);
    }
    fclose(f);
    return 0;
}

static inline uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void format_key(char *buf, size_t buflen, uint64_t id, size_t target_len) {
    int w = snprintf(buf, buflen, "key:%016llx:", (unsigned long long)id);
    if (w < 0) w = 0;
    size_t cur = (size_t)w;
    while (cur < target_len && cur < buflen - 1) buf[cur++] = 'k';
    buf[cur] = '\0';
}

typedef struct {
    char name[48];
    double set_qps;
    double get_hit_qps;
    double get_miss_qps;
    double del_qps;
    double churn_qps;
    double set_p50;
    double set_p95;
    double set_p99;
    double hit_p50;
    double hit_p95;
    double hit_p99;
    size_t rss_kb;
    double bytes_per_entry;
} bench_metrics;

/* Benchmark Drivers */
static void bench_baseline(size_t n, size_t k_len, size_t v_len, bench_metrics *m) {
    strncpy(m->name, "Baseline (Chained Table)", sizeof(m->name) - 1);
    bk_store *store = bk_store_create(n);
    char *k_buf = malloc(k_len + 32);
    uint8_t *v_buf = malloc(v_len + 32);
    memset(v_buf, 0x41, v_len);

    lat_tracker l_set, l_hit;
    lat_init(&l_set, SAMPLES_MAX);
    lat_init(&l_hit, SAMPLES_MAX);

    size_t rss0 = get_proc_rss_kb();
    uint64_t prng = 0x123456789ULL;
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bk_set(store, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < SAMPLES_MAX) lat_record(&l_set, op_el);
    }
    m->set_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);
    size_t rss1 = get_proc_rss_kb();
    m->rss_kb = rss1 > rss0 ? rss1 - rss0 : 0;
    m->bytes_per_entry = (double)m->rss_kb * 1024.0 / (double)n;

    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bk_exists(store, (const uint8_t *)k_buf, k_len);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < SAMPLES_MAX) lat_record(&l_hit, op_el);
    }
    m->get_hit_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    prng = 0x999999999ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bk_exists(store, (const uint8_t *)k_buf, k_len);
    }
    m->get_miss_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bk_del(store, (const uint8_t *)k_buf, k_len);
    }
    m->del_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    size_t churn = 100000;
    t0 = get_time_ns();
    for (size_t i = 0; i < churn; i++) {
        uint64_t id = xorshift64(&prng) % (n / 2);
        format_key(k_buf, k_len + 32, id, k_len);
        if (i % 3 == 0) bk_set(store, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000);
        else if (i % 3 == 1) bk_exists(store, (const uint8_t *)k_buf, k_len);
        else bk_del(store, (const uint8_t *)k_buf, k_len);
    }
    m->churn_qps = (double)churn / ((double)(get_time_ns() - t0) / 1e9);

    lat_calc(&l_set, &m->set_p50, &m->set_p95, &m->set_p99);
    lat_calc(&l_hit, &m->hit_p50, &m->hit_p95, &m->hit_p99);

    lat_free(&l_set);
    lat_free(&l_hit);
    free(k_buf);
    free(v_buf);
    bk_store_free(store);
}

static void bench_candidate_a(size_t n, size_t k_len, size_t v_len, bench_metrics *m) {
    strncpy(m->name, "Candidate A (Flat SIMD)", sizeof(m->name) - 1);
    bha_table *tbl = bha_create(n);
    char *k_buf = malloc(k_len + 32);
    uint8_t *v_buf = malloc(v_len + 32);
    memset(v_buf, 0x42, v_len);

    lat_tracker l_set, l_hit;
    lat_init(&l_set, SAMPLES_MAX);
    lat_init(&l_hit, SAMPLES_MAX);

    size_t rss0 = get_proc_rss_kb();
    uint64_t prng = 0x123456789ULL;
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bha_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < SAMPLES_MAX) lat_record(&l_set, op_el);
    }
    m->set_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);
    size_t rss1 = get_proc_rss_kb();
    m->rss_kb = rss1 > rss0 ? rss1 - rss0 : 0;
    m->bytes_per_entry = (double)m->rss_kb * 1024.0 / (double)n;

    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bha_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < SAMPLES_MAX) lat_record(&l_hit, op_el);
    }
    m->get_hit_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    prng = 0x999999999ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bha_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
    }
    m->get_miss_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bha_del(tbl, (const uint8_t *)k_buf, k_len);
    }
    m->del_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    size_t churn = 100000;
    t0 = get_time_ns();
    for (size_t i = 0; i < churn; i++) {
        uint64_t id = xorshift64(&prng) % (n / 2);
        format_key(k_buf, k_len + 32, id, k_len);
        if (i % 3 == 0) bha_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000);
        else if (i % 3 == 1) bha_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
        else bha_del(tbl, (const uint8_t *)k_buf, k_len);
    }
    m->churn_qps = (double)churn / ((double)(get_time_ns() - t0) / 1e9);

    lat_calc(&l_set, &m->set_p50, &m->set_p95, &m->set_p99);
    lat_calc(&l_hit, &m->hit_p50, &m->hit_p95, &m->hit_p99);

    lat_free(&l_set);
    lat_free(&l_hit);
    free(k_buf);
    free(v_buf);
    bha_free(tbl);
}

static void bench_candidate_b(size_t n, size_t k_len, size_t v_len, bench_metrics *m) {
    strncpy(m->name, "Candidate B (Compact Index)", sizeof(m->name) - 1);
    bhb_table *tbl = bhb_create(n);
    char *k_buf = malloc(k_len + 32);
    uint8_t *v_buf = malloc(v_len + 32);
    memset(v_buf, 0x43, v_len);

    lat_tracker l_set, l_hit;
    lat_init(&l_set, SAMPLES_MAX);
    lat_init(&l_hit, SAMPLES_MAX);

    size_t rss0 = get_proc_rss_kb();
    uint64_t prng = 0x123456789ULL;
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bhb_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000, NULL);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < SAMPLES_MAX) lat_record(&l_set, op_el);
    }
    m->set_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);
    size_t rss1 = get_proc_rss_kb();
    m->rss_kb = rss1 > rss0 ? rss1 - rss0 : 0;
    m->bytes_per_entry = (double)m->rss_kb * 1024.0 / (double)n;

    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bhb_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < SAMPLES_MAX) lat_record(&l_hit, op_el);
    }
    m->get_hit_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    prng = 0x999999999ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bhb_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
    }
    m->get_miss_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bhb_del(tbl, (const uint8_t *)k_buf, k_len);
    }
    m->del_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    size_t churn = 100000;
    t0 = get_time_ns();
    for (size_t i = 0; i < churn; i++) {
        uint64_t id = xorshift64(&prng) % (n / 2);
        format_key(k_buf, k_len + 32, id, k_len);
        if (i % 3 == 0) bhb_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000, NULL);
        else if (i % 3 == 1) bhb_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
        else bhb_del(tbl, (const uint8_t *)k_buf, k_len);
    }
    m->churn_qps = (double)churn / ((double)(get_time_ns() - t0) / 1e9);

    lat_calc(&l_set, &m->set_p50, &m->set_p95, &m->set_p99);
    lat_calc(&l_hit, &m->hit_p50, &m->hit_p95, &m->hit_p99);

    lat_free(&l_set);
    lat_free(&l_hit);
    free(k_buf);
    free(v_buf);
    bhb_free(tbl);
}

static void bench_candidate_c(size_t n, size_t k_len, size_t v_len, bench_metrics *m) {
    strncpy(m->name, "Candidate C (Epoch Lists)", sizeof(m->name) - 1);
    bhc_table *tbl = bhc_create(n);
    char *k_buf = malloc(k_len + 32);
    uint8_t *v_buf = malloc(v_len + 32);
    memset(v_buf, 0x44, v_len);

    lat_tracker l_set, l_hit;
    lat_init(&l_set, SAMPLES_MAX);
    lat_init(&l_hit, SAMPLES_MAX);

    size_t rss0 = get_proc_rss_kb();
    uint64_t prng = 0x123456789ULL;
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bhc_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < SAMPLES_MAX) lat_record(&l_set, op_el);
    }
    m->set_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);
    size_t rss1 = get_proc_rss_kb();
    m->rss_kb = rss1 > rss0 ? rss1 - rss0 : 0;
    m->bytes_per_entry = (double)m->rss_kb * 1024.0 / (double)n;

    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bhc_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < SAMPLES_MAX) lat_record(&l_hit, op_el);
    }
    m->get_hit_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    prng = 0x999999999ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bhc_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
    }
    m->get_miss_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bhc_del(tbl, (const uint8_t *)k_buf, k_len);
    }
    m->del_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    size_t churn = 100000;
    t0 = get_time_ns();
    for (size_t i = 0; i < churn; i++) {
        uint64_t id = xorshift64(&prng) % (n / 2);
        format_key(k_buf, k_len + 32, id, k_len);
        if (i % 3 == 0) bhc_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000);
        else if (i % 3 == 1) bhc_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
        else bhc_del(tbl, (const uint8_t *)k_buf, k_len);
    }
    m->churn_qps = (double)churn / ((double)(get_time_ns() - t0) / 1e9);

    lat_calc(&l_set, &m->set_p50, &m->set_p95, &m->set_p99);
    lat_calc(&l_hit, &m->hit_p50, &m->hit_p95, &m->hit_p99);

    lat_free(&l_set);
    lat_free(&l_hit);
    free(k_buf);
    free(v_buf);
    bhc_free(tbl);
}

static void bench_candidate_hybrid(size_t n, size_t k_len, size_t v_len, bench_metrics *m) {
    strncpy(m->name, "BlinkHash Hybrid (SIMD+Arena+Wheel)", sizeof(m->name) - 1);
    bhh_table *tbl = bhh_create(n);
    char *k_buf = malloc(k_len + 32);
    uint8_t *v_buf = malloc(v_len + 32);
    memset(v_buf, 0x45, v_len);

    lat_tracker l_set, l_hit;
    lat_init(&l_set, SAMPLES_MAX);
    lat_init(&l_hit, SAMPLES_MAX);

    size_t rss0 = get_proc_rss_kb();
    uint64_t prng = 0x123456789ULL;
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bhh_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000, NULL);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < SAMPLES_MAX) lat_record(&l_set, op_el);
    }
    m->set_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);
    size_t rss1 = get_proc_rss_kb();
    m->rss_kb = rss1 > rss0 ? rss1 - rss0 : 0;
    m->bytes_per_entry = (double)m->rss_kb * 1024.0 / (double)n;

    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bhh_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < SAMPLES_MAX) lat_record(&l_hit, op_el);
    }
    m->get_hit_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    prng = 0x999999999ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bhh_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
    }
    m->get_miss_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bhh_del(tbl, (const uint8_t *)k_buf, k_len);
    }
    m->del_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    size_t churn = 100000;
    t0 = get_time_ns();
    for (size_t i = 0; i < churn; i++) {
        uint64_t id = xorshift64(&prng) % (n / 2);
        format_key(k_buf, k_len + 32, id, k_len);
        if (i % 3 == 0) bhh_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000, NULL);
        else if (i % 3 == 1) bhh_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
        else bhh_del(tbl, (const uint8_t *)k_buf, k_len);
    }
    m->churn_qps = (double)churn / ((double)(get_time_ns() - t0) / 1e9);

    lat_calc(&l_set, &m->set_p50, &m->set_p95, &m->set_p99);
    lat_calc(&l_hit, &m->hit_p50, &m->hit_p95, &m->hit_p99);

    lat_free(&l_set);
    lat_free(&l_hit);
    free(k_buf);
    free(v_buf);
    bhh_free(tbl);
}

static void run_forked(void (*fn)(size_t, size_t, size_t, bench_metrics *),
                       size_t n, size_t k_len, size_t v_len, bench_metrics *out)
{
    int pfd[2];
    if (pipe(pfd) != 0) { fn(n, k_len, v_len, out); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        bench_metrics m;
        memset(&m, 0, sizeof(m));
        fn(n, k_len, v_len, &m);
        ssize_t w = write(pfd[1], &m, sizeof(m));
        (void)w;
        close(pfd[1]);
        _exit(0);
    } else {
        close(pfd[1]);
        ssize_t r = read(pfd[0], out, sizeof(*out));
        (void)r;
        close(pfd[0]);
        waitpid(pid, NULL, 0);
    }
}

/* Expiration Storm Benchmark */
static void run_storm_bench(size_t storm_count)
{
    printf("\n--- EXPIRATION STORM BENCHMARK (N=%zu keys expiring simultaneously) ---\n", storm_count);

    /* Candidate C */
    {
        bhc_table *c = bhc_create(storm_count);
        char k_buf[32];
        uint8_t v_buf[32];
        memset(v_buf, 0x43, sizeof(v_buf));
        uint64_t prng = 0x8888ULL;
        for (size_t i = 0; i < storm_count; i++) {
            format_key(k_buf, sizeof(k_buf), xorshift64(&prng), 16);
            bhc_set(c, (const uint8_t *)k_buf, 16, v_buf, 32, 2000);
        }
        uint64_t t0 = get_time_ns();
        size_t purged = bhc_purge_expired(c, 2001);
        double sec = (double)(get_time_ns() - t0) / 1e9;
        printf("  [Candidate C] Purged %zu keys in %.4f sec -> %10.0f expired keys/sec\n",
               purged, sec, (double)purged / sec);
        bhc_free(c);
    }

    /* BlinkHash Hybrid */
    {
        bhh_table *h = bhh_create(storm_count);
        char k_buf[32];
        uint8_t v_buf[32];
        memset(v_buf, 0x45, sizeof(v_buf));
        uint64_t prng = 0x8888ULL;
        for (size_t i = 0; i < storm_count; i++) {
            format_key(k_buf, sizeof(k_buf), xorshift64(&prng), 16);
            bhh_set(h, (const uint8_t *)k_buf, 16, v_buf, 32, 2000, NULL);
        }
        uint64_t t0 = get_time_ns();
        size_t purged = bhh_purge_expired(h, 2001);
        double sec = (double)(get_time_ns() - t0) / 1e9;
        printf("  [BlinkHash Hybrid] Purged %zu keys in %.4f sec -> %10.0f expired keys/sec\n",
               purged, sec, (double)purged / sec);
        bhh_free(h);
    }
}

static void print_scale_table(size_t n, bench_metrics m[5])
{
    printf("\n================================================================================================================\n");
    printf(" RIGOROUS COMPARATIVE BENCHMARK: N = %zu (Payload: 16B key, 32B val = 48B)\n", n);
    printf(" AMD Ryzen 5 8400F, GCC 13.3.0 -O3 -DNDEBUG, Ubuntu 24.04 LTS (WSL2)\n");
    printf("================================================================================================================\n");
    printf("%-35s | %-12s | %-12s | %-12s | %-12s | %-12s\n",
           "Architecture", "SET QPS", "GET Hit QPS", "GET Miss QPS", "DEL QPS", "Churn QPS");
    printf("----------------------------------------------------------------------------------------------------------------\n");
    for (int i = 0; i < 5; i++) {
        printf("%-35s | %10.0f/s | %10.0f/s | %10.0f/s | %10.0f/s | %10.0f/s\n",
               m[i].name, m[i].set_qps, m[i].get_hit_qps, m[i].get_miss_qps, m[i].del_qps, m[i].churn_qps);
    }
    printf("================================================================================================================\n");
    printf("%-35s | %-8s | %-8s | %-8s | %-8s | %-14s | %-10s\n",
           "Architecture", "SET p50", "SET p99", "GET p50", "GET p99", "Memory Overhead", "Delta RSS");
    printf("----------------------------------------------------------------------------------------------------------------\n");
    for (int i = 0; i < 5; i++) {
        printf("%-35s | %6.2f us | %6.2f us | %6.2f us | %6.2f us | %6.1f B/entry | %6.1f MB\n",
               m[i].name, m[i].set_p50, m[i].set_p99, m[i].hit_p50, m[i].hit_p99,
               m[i].bytes_per_entry, (double)m[i].rss_kb / 1024.0);
    }
    printf("================================================================================================================\n");
}

int main(void)
{
    printf("\nInitializing Multi-Scale Rigorous Benchmarking Suite...\n");

    /* Scale 1: N = 100,000 */
    {
        size_t n = 100000;
        bench_metrics m[5];
        run_forked(bench_baseline, n, 16, 32, &m[0]);
        run_forked(bench_candidate_a, n, 16, 32, &m[1]);
        run_forked(bench_candidate_b, n, 16, 32, &m[2]);
        run_forked(bench_candidate_c, n, 16, 32, &m[3]);
        run_forked(bench_candidate_hybrid, n, 16, 32, &m[4]);
        print_scale_table(n, m);
    }

    /* Scale 2: N = 300,000 */
    {
        size_t n = 300000;
        bench_metrics m[5];
        run_forked(bench_baseline, n, 16, 32, &m[0]);
        run_forked(bench_candidate_a, n, 16, 32, &m[1]);
        run_forked(bench_candidate_b, n, 16, 32, &m[2]);
        run_forked(bench_candidate_c, n, 16, 32, &m[3]);
        run_forked(bench_candidate_hybrid, n, 16, 32, &m[4]);
        print_scale_table(n, m);
    }

    /* Scale 3: N = 1,000,000 */
    {
        size_t n = 1000000;
        bench_metrics m[5];
        printf("\nRunning 1,000,000 Elements Scalability Benchmark...\n");
        run_forked(bench_baseline, n, 16, 32, &m[0]);
        run_forked(bench_candidate_a, n, 16, 32, &m[1]);
        run_forked(bench_candidate_b, n, 16, 32, &m[2]);
        run_forked(bench_candidate_c, n, 16, 32, &m[3]);
        run_forked(bench_candidate_hybrid, n, 16, 32, &m[4]);
        print_scale_table(n, m);
    }

    /* Expiration Storms: 10k, 100k, 1M */
    run_storm_bench(10000);
    run_storm_bench(100000);
    run_storm_bench(1000000);

    return 0;
}
