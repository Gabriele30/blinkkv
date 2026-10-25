#define _POSIX_C_SOURCE 200809L

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
#define LATENCY_SAMPLES 50000

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
    double set_qps;
    double get_pure_hit_qps;
    double get_copyout_qps;
    double del_qps;

    double set_p50_us, set_p95_us, set_p99_us, set_max_us;
    double get_p50_us, get_p95_us, get_p99_us, get_max_us;
    double del_p50_us, del_p95_us, del_p99_us, del_max_us;

    size_t index_cap;
    size_t arena_cap;
    size_t active_slots;
    size_t free_slots;
    size_t internal_mem_kb;
    size_t delta_rss_kb;
    size_t peak_rss_kb;
} m21_run_metrics;

static void compute_percentiles(uint64_t *samples, size_t count, double *p50, double *p95, double *p99, double *max_us)
{
    if (count == 0) {
        *p50 = *p95 = *p99 = *max_us = 0.0;
        return;
    }
    qsort(samples, count, sizeof(uint64_t), compare_u64);
    *p50 = (double)samples[(size_t)(count * 0.50)] / 1000.0;
    *p95 = (double)samples[(size_t)(count * 0.95)] / 1000.0;
    *p99 = (double)samples[(size_t)(count * 0.99)] / 1000.0;
    *max_us = (double)samples[count - 1] / 1000.0;
}

static void run_hybrid_trial(size_t n, m21_run_metrics *m)
{
    size_t rss_start = get_proc_rss_kb();
    bhh_table *table = bhh_create(n * 2);

    uint8_t kbuf[32], vbuf[64], dest_buf[64];
    size_t klen, vlen;
    uint64_t now_ms = 1000000ULL;

    uint64_t *lat_samples = malloc(LATENCY_SAMPLES * sizeof(uint64_t));
    size_t sample_stride = (n > LATENCY_SAMPLES) ? (n / LATENCY_SAMPLES) : 1;
    size_t sample_idx = 0;
    uint64_t max_set_lat_ns = 0;

    /* --- SET --- */
    uint64_t t0 = now_ns();
    for (uint32_t i = 0; i < n; i++) {
        make_key(i, kbuf, &klen);
        make_val(i, vbuf, &vlen);

        uint64_t op_t0 = now_ns();
        bhh_set(table, kbuf, klen, vbuf, vlen, now_ms + 10000000ULL, NULL);
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        if (elapsed > max_set_lat_ns) max_set_lat_ns = elapsed;
        if (i % sample_stride == 0 && sample_idx < LATENCY_SAMPLES) {
            lat_samples[sample_idx++] = elapsed;
        }
    }
    uint64_t t1 = now_ns();
    m->set_qps = (double)n / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, sample_idx, &m->set_p50_us, &m->set_p95_us, &m->set_p99_us, &m->set_max_us);
    if ((double)max_set_lat_ns / 1000.0 > m->set_max_us) {
        m->set_max_us = (double)max_set_lat_ns / 1000.0;
    }

    size_t rss_populated = get_proc_rss_kb();
    m->delta_rss_kb = (rss_populated > rss_start) ? (rss_populated - rss_start) : 0;
    m->peak_rss_kb = get_proc_peak_rss_kb();

    /* --- PURE HIT LOOKUP --- */
    t0 = now_ns();
    for (uint32_t i = 0; i < n; i++) {
        make_key(i, kbuf, &klen);
        int rc = bhh_exists(table, kbuf, klen, now_ms);
        g_bench_sink += (rc == 0) ? 1 : 0;
    }
    t1 = now_ns();
    m->get_pure_hit_qps = (double)n / ((double)(t1 - t0) / 1e9);

    /* --- GET COPY-OUT (with latency) --- */
    sample_idx = 0;
    uint64_t max_get_lat_ns = 0;
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

        if (elapsed > max_get_lat_ns) max_get_lat_ns = elapsed;
        if (i % sample_stride == 0 && sample_idx < LATENCY_SAMPLES) {
            lat_samples[sample_idx++] = elapsed;
        }
    }
    t1 = now_ns();
    m->get_copyout_qps = (double)n / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, sample_idx, &m->get_p50_us, &m->get_p95_us, &m->get_p99_us, &m->get_max_us);
    if ((double)max_get_lat_ns / 1000.0 > m->get_max_us) {
        m->get_max_us = (double)max_get_lat_ns / 1000.0;
    }

    /* --- DEL (with latency) --- */
    sample_idx = 0;
    uint64_t max_del_lat_ns = 0;
    size_t del_count = n / 2;
    size_t del_stride = (del_count > LATENCY_SAMPLES) ? (del_count / LATENCY_SAMPLES) : 1;
    t0 = now_ns();
    for (uint32_t i = (uint32_t)n / 2; i < n; i++) {
        make_key(i, kbuf, &klen);

        uint64_t op_t0 = now_ns();
        bhh_del(table, kbuf, klen);
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        if (elapsed > max_del_lat_ns) max_del_lat_ns = elapsed;
        if ((i - n / 2) % del_stride == 0 && sample_idx < LATENCY_SAMPLES) {
            lat_samples[sample_idx++] = elapsed;
        }
    }
    t1 = now_ns();
    m->del_qps = (double)del_count / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, sample_idx, &m->del_p50_us, &m->del_p95_us, &m->del_p99_us, &m->del_max_us);
    if ((double)max_del_lat_ns / 1000.0 > m->del_max_us) {
        m->del_max_us = (double)max_del_lat_ns / 1000.0;
    }

    free(lat_samples);

    /* Capacities and memory stats */
    m->index_cap = table->capacity;
    m->arena_cap = table->arena_cap;
    m->active_slots = table->size;
    m->free_slots = (table->arena_cap > table->arena_count) ? (table->arena_cap - table->arena_count) : 0;
    /* Also count free list */
    uint32_t curr = table->first_free;
    while (curr != 0) {
        m->free_slots++;
        curr = table->arena[curr].next_expire;
    }

    m->internal_mem_kb = bhh_memory_bytes(table) / 1024;

    bhh_free(table);
}

static int run_isolated(size_t n, m21_run_metrics *out)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(pipefd[0]);
        m21_run_metrics res;
        memset(&res, 0, sizeof(res));
        run_hybrid_trial(n, &res);
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

static double median_of(double *arr, size_t count)
{
    qsort(arr, count, sizeof(double), compare_doubles);
    return (count % 2 == 1) ? arr[count / 2] : ((arr[count / 2 - 1] + arr[count / 2]) / 2.0);
}

void bench_size(const char *label, size_t n)
{
    printf("\n========================================================================\n");
    printf(" BENCHMARK: %s (N = %zu keys, 5 isolated rounds)\n", label, n);
    printf("========================================================================\n");

    m21_run_metrics runs[BENCH_ROUNDS];
    double set_qps[BENCH_ROUNDS], hit_qps[BENCH_ROUNDS], copy_qps[BENCH_ROUNDS], del_qps[BENCH_ROUNDS];
    double set_p50[BENCH_ROUNDS], set_p95[BENCH_ROUNDS], set_p99[BENCH_ROUNDS], set_max[BENCH_ROUNDS];
    double get_p50[BENCH_ROUNDS], get_p95[BENCH_ROUNDS], get_p99[BENCH_ROUNDS], get_max[BENCH_ROUNDS];
    double del_p50[BENCH_ROUNDS], del_p95[BENCH_ROUNDS], del_p99[BENCH_ROUNDS], del_max[BENCH_ROUNDS];
    double delta_rss[BENCH_ROUNDS], peak_rss[BENCH_ROUNDS], mem_kb[BENCH_ROUNDS];

    for (int r = 0; r < BENCH_ROUNDS; r++) {
        if (run_isolated(n, &runs[r]) != 0) {
            fprintf(stderr, "Trial %d failed!\n", r);
            return;
        }
        set_qps[r] = runs[r].set_qps;
        hit_qps[r] = runs[r].get_pure_hit_qps;
        copy_qps[r] = runs[r].get_copyout_qps;
        del_qps[r] = runs[r].del_qps;

        set_p50[r] = runs[r].set_p50_us;
        set_p95[r] = runs[r].set_p95_us;
        set_p99[r] = runs[r].set_p99_us;
        set_max[r] = runs[r].set_max_us;

        get_p50[r] = runs[r].get_p50_us;
        get_p95[r] = runs[r].get_p95_us;
        get_p99[r] = runs[r].get_p99_us;
        get_max[r] = runs[r].get_max_us;

        del_p50[r] = runs[r].del_p50_us;
        del_p95[r] = runs[r].del_p95_us;
        del_p99[r] = runs[r].del_p99_us;
        del_max[r] = runs[r].del_max_us;

        delta_rss[r] = (double)runs[r].delta_rss_kb;
        peak_rss[r] = (double)runs[r].peak_rss_kb;
        mem_kb[r] = (double)runs[r].internal_mem_kb;
    }

    printf("Capacities & Slots:\n");
    printf("  Index Capacity:      %zu slots\n", runs[0].index_cap);
    printf("  Arena Capacity:      %zu slots\n", runs[0].arena_cap);
    printf("  Active Slots:        %zu\n", runs[0].active_slots);
    printf("  Free/Unused Slots:   %zu (%.1f%% unused)\n",
           runs[0].free_slots, 100.0 * (double)runs[0].free_slots / (double)runs[0].arena_cap);

    printf("Memory Accounting (Median):\n");
    printf("  Internal Reserved:   %10.2f MB (%.1f B/key)\n", median_of(mem_kb, BENCH_ROUNDS) / 1024.0, (median_of(mem_kb, BENCH_ROUNDS) * 1024.0) / n);
    printf("  Populated Delta RSS: %10.2f MB (%.1f B/key)\n", median_of(delta_rss, BENCH_ROUNDS) / 1024.0, (median_of(delta_rss, BENCH_ROUNDS) * 1024.0) / n);
    printf("  Peak RSS (VmHWM):    %10.2f MB\n", median_of(peak_rss, BENCH_ROUNDS) / 1024.0);

    printf("Throughput (Median Mops/sec):\n");
    printf("  SET:                 %8.2f Mops/s\n", median_of(set_qps, BENCH_ROUNDS) / 1e6);
    printf("  GET Pure Hit:        %8.2f Mops/s\n", median_of(hit_qps, BENCH_ROUNDS) / 1e6);
    printf("  GET Copy-Out:        %8.2f Mops/s\n", median_of(copy_qps, BENCH_ROUNDS) / 1e6);
    printf("  DEL:                 %8.2f Mops/s\n", median_of(del_qps, BENCH_ROUNDS) / 1e6);

    printf("Latencies (Median percentiles in microseconds):\n");
    printf("  SET:     p50=%6.2f us, p95=%6.2f us, p99=%6.2f us, worst=%8.2f us\n",
           median_of(set_p50, BENCH_ROUNDS), median_of(set_p95, BENCH_ROUNDS), median_of(set_p99, BENCH_ROUNDS), median_of(set_max, BENCH_ROUNDS));
    printf("  GET cpy: p50=%6.2f us, p95=%6.2f us, p99=%6.2f us, worst=%8.2f us\n",
           median_of(get_p50, BENCH_ROUNDS), median_of(get_p95, BENCH_ROUNDS), median_of(get_p99, BENCH_ROUNDS), median_of(get_max, BENCH_ROUNDS));
    printf("  DEL:     p50=%6.2f us, p95=%6.2f us, p99=%6.2f us, worst=%8.2f us\n",
           median_of(del_p50, BENCH_ROUNDS), median_of(del_p95, BENCH_ROUNDS), median_of(del_p99, BENCH_ROUNDS), median_of(del_max, BENCH_ROUNDS));
}

int main(void)
{
    printf("========================================================================\n");
    printf(" BlinkHash Hybrid Milestone M2.1: Baseline Evaluation (Pre-Decoupling)\n");
    printf("========================================================================\n");

    bench_size("100K Keys", 100000);
    bench_size("300K Keys", 300000);
    bench_size("1M Keys",   1000000);

    return 0;
}
