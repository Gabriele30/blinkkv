#define _POSIX_C_SOURCE 200809L

#include "blinkkv.h"
#include "blinkhash_a.h"
#include "blinkhash_b.h"
#include "blinkhash_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

#define BENCH_SAMPLES 50000

typedef struct {
    uint64_t *latencies_ns;
    size_t count;
    size_t cap;
} bench_lat;

static void lat_init(bench_lat *l, size_t cap)
{
    l->latencies_ns = malloc(cap * sizeof(uint64_t));
    l->count = 0;
    l->cap = cap;
}

static void lat_free(bench_lat *l)
{
    free(l->latencies_ns);
    l->latencies_ns = NULL;
}

static inline void lat_record(bench_lat *l, uint64_t ns)
{
    if (l->count < l->cap) {
        l->latencies_ns[l->count++] = ns;
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

static void lat_p(bench_lat *l, double *p50, double *p95, double *p99)
{
    if (l->count == 0) {
        *p50 = *p95 = *p99 = 0.0;
        return;
    }
    qsort(l->latencies_ns, l->count, sizeof(uint64_t), compare_u64);
    *p50 = (double)l->latencies_ns[(size_t)(l->count * 0.50)] / 1000.0;
    *p95 = (double)l->latencies_ns[(size_t)(l->count * 0.95)] / 1000.0;
    *p99 = (double)l->latencies_ns[(size_t)(l->count * 0.99)] / 1000.0;
}

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static size_t get_rss_kb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    if (fscanf(f, "%lu %lu", &size, &resident) == 2) {
        fclose(f);
        return (size_t)(resident * 4); /* pages to KB */
    }
    fclose(f);
    return 0;
}

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
    int w = snprintf(buf, buflen, "k:%016llx:", (unsigned long long)id);
    if (w < 0) w = 0;
    size_t cur = (size_t)w;
    while (cur < target_len && cur < buflen - 1) buf[cur++] = 'z';
    buf[cur] = '\0';
}

typedef struct {
    const char *name;
    double set_qps;
    double get_hit_qps;
    double get_miss_qps;
    double del_qps;
    double churn_qps;
    double storm_purge_sec;
    double set_p50;
    double set_p99;
    double hit_p50;
    double hit_p99;
    size_t rss_kb;
    double bytes_per_entry;
} model_results;

static void benchmark_candidate_a(size_t n, size_t k_len, size_t v_len, model_results *r)
{
    r->name = "Candidate A (Flat SIMD Table)";
    bha_table *tbl = bha_create(n);

    char *k_buf = malloc(k_len + 32);
    uint8_t *v_buf = malloc(v_len + 32);
    memset(v_buf, 0x41, v_len);

    bench_lat lat_set, lat_hit;
    lat_init(&lat_set, BENCH_SAMPLES);
    lat_init(&lat_hit, BENCH_SAMPLES);

    size_t rss0 = get_rss_kb();

    /* SET */
    uint64_t prng = 0x123456789ULL;
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bha_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < BENCH_SAMPLES) lat_record(&lat_set, op_el);
    }
    r->set_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    size_t rss1 = get_rss_kb();
    r->rss_kb = rss1 > rss0 ? rss1 - rss0 : 0;
    r->bytes_per_entry = (double)r->rss_kb * 1024.0 / (double)n;

    /* GET Hit */
    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        const uint8_t *val = NULL;
        size_t val_len = 0;
        uint64_t op_t0 = get_time_ns();
        bha_get(tbl, (const uint8_t *)k_buf, k_len, 500, &val, &val_len);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < BENCH_SAMPLES) lat_record(&lat_hit, op_el);
    }
    r->get_hit_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    /* GET Miss */
    prng = 0x999999999ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bha_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
    }
    r->get_miss_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    /* DEL */
    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bha_del(tbl, (const uint8_t *)k_buf, k_len);
    }
    r->del_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    /* Churn: 100k random mixed ops */
    size_t churn_ops = 100000;
    t0 = get_time_ns();
    for (size_t i = 0; i < churn_ops; i++) {
        uint64_t id = xorshift64(&prng) % (n / 2);
        format_key(k_buf, k_len + 32, id, k_len);
        if (i % 3 == 0) {
            bha_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000);
        } else if (i % 3 == 1) {
            bha_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
        } else {
            bha_del(tbl, (const uint8_t *)k_buf, k_len);
        }
    }
    r->churn_qps = (double)churn_ops / ((double)(get_time_ns() - t0) / 1e9);

    double p95;
    lat_p(&lat_set, &r->set_p50, &p95, &r->set_p99);
    lat_p(&lat_hit, &r->hit_p50, &p95, &r->hit_p99);

    lat_free(&lat_set);
    lat_free(&lat_hit);
    free(k_buf);
    free(v_buf);
    bha_free(tbl);
}

static void benchmark_candidate_b(size_t n, size_t k_len, size_t v_len, model_results *r)
{
    r->name = "Candidate B (Compact Index + Arena Handles)";
    bhb_table *tbl = bhb_create(n);

    char *k_buf = malloc(k_len + 32);
    uint8_t *v_buf = malloc(v_len + 32);
    memset(v_buf, 0x42, v_len);

    bench_lat lat_set, lat_hit;
    lat_init(&lat_set, BENCH_SAMPLES);
    lat_init(&lat_hit, BENCH_SAMPLES);

    size_t rss0 = get_rss_kb();

    /* SET */
    uint64_t prng = 0x123456789ULL;
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bhb_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000, NULL);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < BENCH_SAMPLES) lat_record(&lat_set, op_el);
    }
    r->set_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    size_t rss1 = get_rss_kb();
    r->rss_kb = rss1 > rss0 ? rss1 - rss0 : 0;
    r->bytes_per_entry = (double)r->rss_kb * 1024.0 / (double)n;

    /* GET Hit */
    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        const uint8_t *val = NULL;
        size_t val_len = 0;
        uint64_t op_t0 = get_time_ns();
        bhb_get(tbl, (const uint8_t *)k_buf, k_len, 500, &val, &val_len, NULL);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < BENCH_SAMPLES) lat_record(&lat_hit, op_el);
    }
    r->get_hit_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    /* GET Miss */
    prng = 0x999999999ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bhb_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
    }
    r->get_miss_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    /* DEL */
    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bhb_del(tbl, (const uint8_t *)k_buf, k_len);
    }
    r->del_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    /* Churn */
    size_t churn_ops = 100000;
    t0 = get_time_ns();
    for (size_t i = 0; i < churn_ops; i++) {
        uint64_t id = xorshift64(&prng) % (n / 2);
        format_key(k_buf, k_len + 32, id, k_len);
        if (i % 3 == 0) {
            bhb_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000, NULL);
        } else if (i % 3 == 1) {
            bhb_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
        } else {
            bhb_del(tbl, (const uint8_t *)k_buf, k_len);
        }
    }
    r->churn_qps = (double)churn_ops / ((double)(get_time_ns() - t0) / 1e9);

    double p95;
    lat_p(&lat_set, &r->set_p50, &p95, &r->set_p99);
    lat_p(&lat_hit, &r->hit_p50, &p95, &r->hit_p99);

    lat_free(&lat_set);
    lat_free(&lat_hit);
    free(k_buf);
    free(v_buf);
    bhb_free(tbl);
}

static void benchmark_candidate_c(size_t n, size_t k_len, size_t v_len, model_results *r)
{
    r->name = "Candidate C (BlinkHash-TTL Epoch Lists)";
    bhc_table *tbl = bhc_create(n);

    char *k_buf = malloc(k_len + 32);
    uint8_t *v_buf = malloc(v_len + 32);
    memset(v_buf, 0x43, v_len);

    bench_lat lat_set, lat_hit;
    lat_init(&lat_set, BENCH_SAMPLES);
    lat_init(&lat_hit, BENCH_SAMPLES);

    size_t rss0 = get_rss_kb();

    /* SET */
    uint64_t prng = 0x123456789ULL;
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bhc_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < BENCH_SAMPLES) lat_record(&lat_set, op_el);
    }
    r->set_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    size_t rss1 = get_rss_kb();
    r->rss_kb = rss1 > rss0 ? rss1 - rss0 : 0;
    r->bytes_per_entry = (double)r->rss_kb * 1024.0 / (double)n;

    /* GET Hit */
    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        const uint8_t *val = NULL;
        size_t val_len = 0;
        uint64_t op_t0 = get_time_ns();
        bhc_get(tbl, (const uint8_t *)k_buf, k_len, 500, &val, &val_len);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < BENCH_SAMPLES) lat_record(&lat_hit, op_el);
    }
    r->get_hit_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    /* GET Miss */
    prng = 0x999999999ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bhc_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
    }
    r->get_miss_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    /* DEL */
    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bhc_del(tbl, (const uint8_t *)k_buf, k_len);
    }
    r->del_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    /* Expiration Storm Benchmark: insert 100k keys that expire at T=2000, then purge them! */
    size_t storm_n = 100000;
    for (size_t i = 0; i < storm_n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bhc_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 2000);
    }
    t0 = get_time_ns();
    size_t purged = bhc_purge_expired(tbl, 2001);
    r->storm_purge_sec = (double)(get_time_ns() - t0) / 1e9;
    (void)purged;

    /* Churn */
    size_t churn_ops = 100000;
    t0 = get_time_ns();
    for (size_t i = 0; i < churn_ops; i++) {
        uint64_t id = xorshift64(&prng) % (n / 2);
        format_key(k_buf, k_len + 32, id, k_len);
        if (i % 3 == 0) {
            bhc_set(tbl, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000);
        } else if (i % 3 == 1) {
            bhc_exists(tbl, (const uint8_t *)k_buf, k_len, 500);
        } else {
            bhc_del(tbl, (const uint8_t *)k_buf, k_len);
        }
    }
    r->churn_qps = (double)churn_ops / ((double)(get_time_ns() - t0) / 1e9);

    double p95;
    lat_p(&lat_set, &r->set_p50, &p95, &r->set_p99);
    lat_p(&lat_hit, &r->hit_p50, &p95, &r->hit_p99);

    lat_free(&lat_set);
    lat_free(&lat_hit);
    free(k_buf);
    free(v_buf);
    bhc_free(tbl);
}

static void benchmark_baseline(size_t n, size_t k_len, size_t v_len, model_results *r)
{
    r->name = "Baseline (Chained Hash Table)";
    bk_store *store = bk_store_create(n);

    char *k_buf = malloc(k_len + 32);
    uint8_t *v_buf = malloc(v_len + 32);
    memset(v_buf, 0x44, v_len);

    bench_lat lat_set, lat_hit;
    lat_init(&lat_set, BENCH_SAMPLES);
    lat_init(&lat_hit, BENCH_SAMPLES);

    size_t rss0 = get_rss_kb();

    /* SET */
    uint64_t prng = 0x123456789ULL;
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bk_set(store, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < BENCH_SAMPLES) lat_record(&lat_set, op_el);
    }
    r->set_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    size_t rss1 = get_rss_kb();
    r->rss_kb = rss1 > rss0 ? rss1 - rss0 : 0;
    r->bytes_per_entry = (double)r->rss_kb * 1024.0 / (double)n;

    /* GET Hit (Pure exists to compare lookup core without malloc) */
    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        uint64_t op_t0 = get_time_ns();
        bk_exists(store, (const uint8_t *)k_buf, k_len);
        uint64_t op_el = get_time_ns() - op_t0;
        if (i < BENCH_SAMPLES) lat_record(&lat_hit, op_el);
    }
    r->get_hit_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    /* GET Miss */
    prng = 0x999999999ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bk_exists(store, (const uint8_t *)k_buf, k_len);
    }
    r->get_miss_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    /* DEL */
    prng = 0x123456789ULL;
    t0 = get_time_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t id = xorshift64(&prng);
        format_key(k_buf, k_len + 32, id, k_len);
        bk_del(store, (const uint8_t *)k_buf, k_len);
    }
    r->del_qps = (double)n / ((double)(get_time_ns() - t0) / 1e9);

    /* Churn */
    size_t churn_ops = 100000;
    t0 = get_time_ns();
    for (size_t i = 0; i < churn_ops; i++) {
        uint64_t id = xorshift64(&prng) % (n / 2);
        format_key(k_buf, k_len + 32, id, k_len);
        if (i % 3 == 0) {
            bk_set(store, (const uint8_t *)k_buf, k_len, v_buf, v_len, 1000000);
        } else if (i % 3 == 1) {
            bk_exists(store, (const uint8_t *)k_buf, k_len);
        } else {
            bk_del(store, (const uint8_t *)k_buf, k_len);
        }
    }
    r->churn_qps = (double)churn_ops / ((double)(get_time_ns() - t0) / 1e9);

    double p95;
    lat_p(&lat_set, &r->set_p50, &p95, &r->set_p99);
    lat_p(&lat_hit, &r->hit_p50, &p95, &r->hit_p99);

    lat_free(&lat_set);
    lat_free(&lat_hit);
    free(k_buf);
    free(v_buf);
    bk_store_free(store);
}

static void print_comparative_table(const model_results res[4])
{
    printf("\n========================================================================================================\n");
    printf(" COMPARATIVE BENCHMARK MATRIX (AMD Ryzen 5 8400F, GCC 13.3.0 -O3 -DNDEBUG, Ubuntu 24.04 on WSL2)\n");
    printf("========================================================================================================\n");
    printf("%-38s | %-12s | %-12s | %-12s | %-12s | %-12s\n",
           "Architecture Model", "SET QPS", "GET Hit QPS", "GET Miss QPS", "DEL QPS", "Churn QPS");
    printf("--------------------------------------------------------------------------------------------------------\n");
    for (int i = 0; i < 4; i++) {
        printf("%-38s | %10.0f/s | %10.0f/s | %10.0f/s | %10.0f/s | %10.0f/s\n",
               res[i].name, res[i].set_qps, res[i].get_hit_qps, res[i].get_miss_qps, res[i].del_qps, res[i].churn_qps);
    }
    printf("========================================================================================================\n");
    printf("%-38s | %-10s | %-10s | %-10s | %-10s | %-14s\n",
           "Architecture Model", "SET p50", "SET p99", "GET p50", "GET p99", "Memory Overhead");
    printf("--------------------------------------------------------------------------------------------------------\n");
    for (int i = 0; i < 4; i++) {
        printf("%-38s | %7.2f us | %7.2f us | %7.2f us | %7.2f us | %6.1f B/entry\n",
               res[i].name, res[i].set_p50, res[i].set_p99, res[i].hit_p50, res[i].hit_p99, res[i].bytes_per_entry);
    }
    printf("========================================================================================================\n");
    if (res[3].storm_purge_sec > 0.0) {
        printf("  [Expiration Storm Test: 100,000 keys expiring simultaneously]\n");
        printf("    Candidate C Purge Time: %.4f seconds (%.0f expired keys/sec active purge!)\n",
               res[3].storm_purge_sec, 100000.0 / res[3].storm_purge_sec);
        printf("========================================================================================================\n");
    }
}

#include <unistd.h>
#include <sys/wait.h>

static void run_isolated(void (*fn)(size_t, size_t, size_t, model_results *),
                         size_t n, size_t k_len, size_t v_len, model_results *out)
{
    int pfd[2];
    if (pipe(pfd) != 0) {
        fn(n, k_len, v_len, out);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        model_results r;
        memset(&r, 0, sizeof(r));
        fn(n, k_len, v_len, &r);
        ssize_t w = write(pfd[1], &r, sizeof(r));
        (void)w;
        close(pfd[1]);
        _exit(0);
    } else {
        close(pfd[1]);
        ssize_t rd = read(pfd[0], out, sizeof(*out));
        (void)rd;
        close(pfd[0]);
        waitpid(pid, NULL, 0);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    size_t n = 300000;
    size_t k_len = 16;
    size_t v_len = 32;

    printf("\nStarting Comparative Benchmark across Baseline, Candidate A, Candidate B, and Candidate C...\n");
    printf("Parameters: N=%zu items, Key=%zu bytes, Value=%zu bytes (Payload = %zu bytes)\n",
           n, k_len, v_len, k_len + v_len);

    model_results results[4];
    memset(results, 0, sizeof(results));

    printf("  Benchmarking Baseline (in isolated process)...\n");
    run_isolated(benchmark_baseline, n, k_len, v_len, &results[0]);

    printf("  Benchmarking Candidate A (Flat SIMD Table)...\n");
    run_isolated(benchmark_candidate_a, n, k_len, v_len, &results[1]);

    printf("  Benchmarking Candidate B (Compact Index + Arena Handles)...\n");
    run_isolated(benchmark_candidate_b, n, k_len, v_len, &results[2]);

    printf("  Benchmarking Candidate C (BlinkHash-TTL Epoch Lists)...\n");
    run_isolated(benchmark_candidate_c, n, k_len, v_len, &results[3]);

    print_comparative_table(results);
    return 0;
}
