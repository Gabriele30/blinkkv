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

#define KEY_BYTES 16
#define VAL_BYTES 32
#define BENCH_ROUNDS 5

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

typedef struct {
    bool is_m2_static;
    size_t n;

    /* Memoria */
    size_t arena_cap;
    size_t internal_mem_bytes;
    size_t rss_kb;
    size_t peak_rss_kb;
    size_t realloc_count;

    /* Fase 1: Populate */
    double populate_qps;
    double pop_p50_us, pop_p95_us, pop_p99_us, pop_p99_9_us, pop_max_us;

    /* Fase 2: Steady-state (GET e Overwrite Update) */
    double steady_get_qps;
    double steady_get_p50_us, steady_get_p99_us, steady_get_max_us;
    double steady_set_qps;
    double steady_set_p50_us, steady_set_p99_us, steady_set_max_us;

    /* Fase 3: Continuous Churn (100k delete + new insert) */
    double churn_qps;
    double churn_p50_us, churn_p99_us, churn_max_us;
    size_t churn_reallocs;
    size_t post_churn_mem_bytes;
} tradeoff_metrics;

static void run_tradeoff_worker(bool is_m2_static, size_t n, tradeoff_metrics *m)
{
    m->is_m2_static = is_m2_static;
    m->n = n;

    bhh_table *table = bhh_create(n * 2);

    if (is_m2_static) {
        /* Simula M2: arena preallocata alla dimensione della capacita' indice */
        if (table->arena_cap < table->capacity) {
            bhh_record *big_arena = realloc(table->arena, table->capacity * sizeof(bhh_record));
            if (big_arena) {
                memset(big_arena + table->arena_cap, 0, (table->capacity - table->arena_cap) * sizeof(bhh_record));
                table->arena = big_arena;
                table->arena_cap = table->capacity;
            }
        }
    } else {
        /* M2.1: arena dinamica 1.5x (inizia a 64 slot) */
        bhh_set_growth_policy(table, BHH_GROWTH_1_5X);
    }

    uint8_t kbuf[32], vbuf[64], dest_buf[64];
    size_t klen, vlen;
    uint64_t now_ms = 1000000ULL;

    uint64_t *lat_samples = malloc(n * sizeof(uint64_t));
    uint64_t worst_lat = 0;

    /* --- FASE 1: POPULATE N CHIAVI --- */
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
    m->populate_qps = (double)n / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, n,
                        &m->pop_p50_us, &m->pop_p95_us,
                        &m->pop_p99_us, &m->pop_p99_9_us,
                        &m->pop_max_us);
    if ((double)worst_lat / 1000.0 > m->pop_max_us) {
        m->pop_max_us = (double)worst_lat / 1000.0;
    }

    bhh_stats st;
    bhh_get_stats(table, &st);
    m->arena_cap = st.arena_cap;
    m->realloc_count = st.arena_realloc_count;
    m->internal_mem_bytes = bhh_memory_bytes(table);
    m->rss_kb = get_proc_rss_kb();
    m->peak_rss_kb = get_proc_peak_rss_kb();

    /* --- FASE 2: STEADY-STATE POST-POPULAMENTO (100k GET e 100k Overwrite) --- */
    size_t ops = (n >= 100000) ? 100000 : n;

    /* Steady GET */
    worst_lat = 0;
    t0 = now_ns();
    for (uint32_t i = 0; i < ops; i++) {
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
    m->steady_get_qps = (double)ops / ((double)(t1 - t0) / 1e9);
    double dummy95, dummy999;
    compute_percentiles(lat_samples, ops, &m->steady_get_p50_us, &dummy95, &m->steady_get_p99_us, &dummy999, &m->steady_get_max_us);
    if ((double)worst_lat / 1000.0 > m->steady_get_max_us) m->steady_get_max_us = (double)worst_lat / 1000.0;

    /* Steady Overwrite SET (stesse chiavi, nuovi valori) */
    worst_lat = 0;
    t0 = now_ns();
    for (uint32_t i = 0; i < ops; i++) {
        make_key(i, kbuf, &klen);
        make_val(i + 1000000, vbuf, &vlen);

        uint64_t op_t0 = now_ns();
        bhh_set(table, kbuf, klen, vbuf, vlen, now_ms + 10000000ULL, NULL);
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        lat_samples[i] = elapsed;
        if (elapsed > worst_lat) worst_lat = elapsed;
    }
    t1 = now_ns();
    m->steady_set_qps = (double)ops / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, ops, &m->steady_set_p50_us, &dummy95, &m->steady_set_p99_us, &dummy999, &m->steady_set_max_us);
    if ((double)worst_lat / 1000.0 > m->steady_set_max_us) m->steady_set_max_us = (double)worst_lat / 1000.0;

    /* --- FASE 3: CONTINUOUS CHURN (100k cancellazioni seguite da 100k nuovi inserimenti) --- */
    size_t realloc_before = table->arena_realloc_count;
    worst_lat = 0;
    t0 = now_ns();
    for (uint32_t i = 0; i < ops; i++) {
        /* Cancella chiave vecchia */
        make_key(i, kbuf, &klen);
        bhh_del(table, kbuf, klen);

        /* Inserisci nuova chiave (usa free-list) */
        make_key(i + (uint32_t)n * 4, kbuf, &klen);
        make_val(i, vbuf, &vlen);

        uint64_t op_t0 = now_ns();
        bhh_set(table, kbuf, klen, vbuf, vlen, now_ms + 10000000ULL, NULL);
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        lat_samples[i] = elapsed;
        if (elapsed > worst_lat) worst_lat = elapsed;
    }
    t1 = now_ns();
    m->churn_qps = (double)ops / ((double)(t1 - t0) / 1e9);
    compute_percentiles(lat_samples, ops, &m->churn_p50_us, &dummy95, &m->churn_p99_us, &dummy999, &m->churn_max_us);
    if ((double)worst_lat / 1000.0 > m->churn_max_us) m->churn_max_us = (double)worst_lat / 1000.0;

    m->churn_reallocs = table->arena_realloc_count - realloc_before;
    m->post_churn_mem_bytes = bhh_memory_bytes(table);

    free(lat_samples);
    bhh_free(table);
}

static int run_isolated_tradeoff(bool is_m2_static, size_t n, tradeoff_metrics *out)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(pipefd[0]);
        tradeoff_metrics res;
        memset(&res, 0, sizeof(res));
        run_tradeoff_worker(is_m2_static, n, &res);
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

static void aggregate_tradeoff(tradeoff_metrics *rounds, size_t r_count, tradeoff_metrics *med)
{
    memset(med, 0, sizeof(*med));
    med->is_m2_static = rounds[0].is_m2_static;
    med->n = rounds[0].n;

    #define MED_D(field) do { \
        double vals[32]; \
        for (size_t i = 0; i < r_count; i++) vals[i] = rounds[i].field; \
        med->field = median_d(vals, r_count); \
    } while (0)

    #define MED_SZ(field) do { \
        size_t vals[32]; \
        for (size_t i = 0; i < r_count; i++) vals[i] = rounds[i].field; \
        med->field = median_sz(vals, r_count); \
    } while (0)

    MED_SZ(arena_cap);
    MED_SZ(internal_mem_bytes);
    MED_SZ(rss_kb);
    MED_SZ(peak_rss_kb);
    MED_SZ(realloc_count);

    MED_D(populate_qps);
    MED_D(pop_p50_us); MED_D(pop_p95_us); MED_D(pop_p99_us); MED_D(pop_p99_9_us); MED_D(pop_max_us);

    MED_D(steady_get_qps);
    MED_D(steady_get_p50_us); MED_D(steady_get_p99_us); MED_D(steady_get_max_us);
    MED_D(steady_set_qps);
    MED_D(steady_set_p50_us); MED_D(steady_set_p99_us); MED_D(steady_set_max_us);

    MED_D(churn_qps);
    MED_D(churn_p50_us); MED_D(churn_p99_us); MED_D(churn_max_us);
    MED_SZ(churn_reallocs);
    MED_SZ(post_churn_mem_bytes);

    #undef MED_D
    #undef MED_SZ
}

static void bench_tradeoff_size(const char *label, size_t n)
{
    printf("\n========================================================================================================================================\n");
    printf(" CONFRONTO COMPROMESSO M2 (Accoppiata) vs M2.1 (Arena Dinamica 1.5x): %s (N = %zu)\n", label, n);
    printf("========================================================================================================================================\n");

    tradeoff_metrics m2_r[BENCH_ROUNDS];
    tradeoff_metrics m21_r[BENCH_ROUNDS];

    for (int r = 0; r < BENCH_ROUNDS; r++) {
        if (r % 2 == 0) {
            run_isolated_tradeoff(true, n, &m2_r[r]);
            run_isolated_tradeoff(false, n, &m21_r[r]);
        } else {
            run_isolated_tradeoff(false, n, &m21_r[r]);
            run_isolated_tradeoff(true, n, &m2_r[r]);
        }
    }

    tradeoff_metrics m2, m21;
    aggregate_tradeoff(m2_r, BENCH_ROUNDS, &m2);
    aggregate_tradeoff(m21_r, BENCH_ROUNDS, &m21);

    printf("+---------------------------------------+------------------------+------------------------+--------------------+\n");
    printf("| Metrica / Dimensione Compromesso      | M2 Precedente (Static) | M2.1 Attuale (Dynamic) | Variazione         |\n");
    printf("+---------------------------------------+------------------------+------------------------+--------------------+\n");
    printf("| Capacita' Arena Allocata              | %22zu | %22zu | %+18zd |\n",
           m2.arena_cap, m21.arena_cap, (ssize_t)m21.arena_cap - (ssize_t)m2.arena_cap);
    printf("| Memoria Interna Riservata             | %19.2f MB | %19.2f MB | %17.1f%% |\n",
           (double)m2.internal_mem_bytes / (1024.0 * 1024.0),
           (double)m21.internal_mem_bytes / (1024.0 * 1024.0),
           100.0 * ((double)m21.internal_mem_bytes / (double)m2.internal_mem_bytes - 1.0));
    printf("| Peak RSS Misurato (VmHWM)             | %19.2f MB | %19.2f MB | %17.1f%% |\n",
           (double)m2.peak_rss_kb / 1024.0,
           (double)m21.peak_rss_kb / 1024.0,
           100.0 * ((double)m21.peak_rss_kb / (double)m2.peak_rss_kb - 1.0));
    printf("| Espansioni Arena durante Populate     | %22zu | %22zu | %+18zd |\n",
           m2.realloc_count, m21.realloc_count, (ssize_t)m21.realloc_count - (ssize_t)m2.realloc_count);
    printf("+---------------------------------------+------------------------+------------------------+--------------------+\n");
    printf("| FASE 1: POPULATE N CHIAVI             |                        |                        |                    |\n");
    printf("|   Throughput SET Populate             | %17.0f ops/s | %17.0f ops/s | %17.1f%% |\n",
           m2.populate_qps, m21.populate_qps, 100.0 * ((m21.populate_qps / m2.populate_qps) - 1.0));
    printf("|   Latenza SET p50                     | %20.2f us | %20.2f us | %+15.2f us |\n",
           m2.pop_p50_us, m21.pop_p50_us, m21.pop_p50_us - m2.pop_p50_us);
    printf("|   Latenza SET p95                     | %20.2f us | %20.2f us | %+15.2f us |\n",
           m2.pop_p95_us, m21.pop_p95_us, m21.pop_p95_us - m2.pop_p95_us);
    printf("|   Latenza SET p99                     | %20.2f us | %20.2f us | %+15.2f us |\n",
           m2.pop_p99_us, m21.pop_p99_us, m21.pop_p99_us - m2.pop_p99_us);
    printf("|   Latenza SET p99.9                   | %20.2f us | %20.2f us | %+15.2f us |\n",
           m2.pop_p99_9_us, m21.pop_p99_9_us, m21.pop_p99_9_us - m2.pop_p99_9_us);
    printf("|   Latenza SET Massima (Spike Realloc) | %20.2f us | %20.2f us | %+15.2f us |\n",
           m2.pop_max_us, m21.pop_max_us, m21.pop_max_us - m2.pop_max_us);
    printf("+---------------------------------------+------------------------+------------------------+--------------------+\n");
    printf("| FASE 2: STEADY-STATE POST-POPULATE    |                        |                        |                    |\n");
    printf("|   Throughput GET Copy-out             | %17.0f ops/s | %17.0f ops/s | %17.1f%% |\n",
           m2.steady_get_qps, m21.steady_get_qps, 100.0 * ((m21.steady_get_qps / m2.steady_get_qps) - 1.0));
    printf("|   GET p99                             | %20.2f us | %20.2f us | %+15.2f us |\n",
           m2.steady_get_p99_us, m21.steady_get_p99_us, m21.steady_get_p99_us - m2.steady_get_p99_us);
    printf("|   GET Max                             | %20.2f us | %20.2f us | %+15.2f us |\n",
           m2.steady_get_max_us, m21.steady_get_max_us, m21.steady_get_max_us - m2.steady_get_max_us);
    printf("|   Throughput In-Place SET             | %17.0f ops/s | %17.0f ops/s | %17.1f%% |\n",
           m2.steady_set_qps, m21.steady_set_qps, 100.0 * ((m21.steady_set_qps / m2.steady_set_qps) - 1.0));
    printf("|   In-Place SET p99                    | %20.2f us | %20.2f us | %+15.2f us |\n",
           m2.steady_set_p99_us, m21.steady_set_p99_us, m21.steady_set_p99_us - m2.steady_set_p99_us);
    printf("+---------------------------------------+------------------------+------------------------+--------------------+\n");
    printf("| FASE 3: CONTINUOUS CHURN (100k Cicli) |                        |                        |                    |\n");
    printf("|   Throughput Churn SET (su Free-List) | %17.0f ops/s | %17.0f ops/s | %17.1f%% |\n",
           m2.churn_qps, m21.churn_qps, 100.0 * ((m21.churn_qps / m2.churn_qps) - 1.0));
    printf("|   Churn SET p99                       | %20.2f us | %20.2f us | %+15.2f us |\n",
           m2.churn_p99_us, m21.churn_p99_us, m21.churn_p99_us - m2.churn_p99_us);
    printf("|   Churn SET Max                       | %20.2f us | %20.2f us | %+15.2f us |\n",
           m2.churn_max_us, m21.churn_max_us, m21.churn_max_us - m2.churn_max_us);
    printf("|   Realloc Arena Aggiuntive in Churn   | %22zu | %22zu | %+18zd |\n",
           m2.churn_reallocs, m21.churn_reallocs, (ssize_t)m21.churn_reallocs - (ssize_t)m2.churn_reallocs);
    printf("|   Crescita Memoria durante Churn      | %19.2f MB | %19.2f MB | %+15.2f MB |\n",
           (double)m2.post_churn_mem_bytes / (1024.0 * 1024.0),
           (double)m21.post_churn_mem_bytes / (1024.0 * 1024.0),
           (double)((ssize_t)m21.post_churn_mem_bytes - (ssize_t)m2.post_churn_mem_bytes) / (1024.0 * 1024.0));
    printf("+---------------------------------------+------------------------+------------------------+--------------------+\n");
}

int main(void)
{
    printf("========================================================================================================================================\n");
    printf(" BLINKHASH HYBRID: VALIDAZIONE COMPLETA DEL COMPROMESSO M2 (Accoppiata) vs M2.1 (Arena Dinamica)\n");
    printf(" Analisi empirica: Risparmio RAM, Costo espansioni (Spike di latenza p99..max), Steady-state e Churn continuo\n");
    printf("========================================================================================================================================\n");

    bench_tradeoff_size("300k Keys", 300000);
    bench_tradeoff_size("1M Keys",   1000000);

    return 0;
}
