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

typedef struct {
    bhh_growth_policy policy;
    size_t n;

    /* Telemetria Arena */
    size_t realloc_count;
    size_t theoretical_bytes_subject_to_realloc;
    size_t final_arena_cap;
    size_t never_allocated_slots;
    size_t internal_mem_bytes;
    size_t rss_kb;
    size_t peak_rss_kb;

    /* Throughput e Latenze */
    double set_qps;
    double p50_us;
    double p95_us;
    double p99_us;
    double p99_9_us;
    double max_us;
} growth_run_metrics;

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

/* Worker che esegue esclusivamente tramite API pubblica di BlinkHash Hybrid */
static void run_growth_worker(size_t n, bhh_growth_policy policy, growth_run_metrics *m)
{
    bhh_table *table = bhh_create(n * 2);
    bhh_set_growth_policy(table, policy);

    m->policy = policy;
    m->n = n;

    uint8_t kbuf[32], vbuf[64];
    size_t klen, vlen;
    uint64_t now_ms = 1000000ULL;

    uint64_t *latencies = malloc(n * sizeof(uint64_t));
    uint64_t worst_lat = 0;

    /* Misura l'intera operazione SET, INCLUSA l'eventuale riallocazione interna */
    uint64_t t_start = now_ns();
    for (uint32_t i = 0; i < n; i++) {
        make_key(i, kbuf, &klen);
        make_val(i, vbuf, &vlen);

        uint64_t op_t0 = now_ns();
        bhh_set(table, kbuf, klen, vbuf, vlen, now_ms + 10000000ULL, NULL);
        uint64_t op_t1 = now_ns();
        uint64_t elapsed = op_t1 - op_t0;

        latencies[i] = elapsed;
        if (elapsed > worst_lat) worst_lat = elapsed;
    }
    uint64_t t_end = now_ns();

    m->set_qps = (double)n / ((double)(t_end - t_start) / 1e9);
    compute_percentiles(latencies, n, &m->p50_us, &m->p95_us, &m->p99_us, &m->p99_9_us, &m->max_us);
    if ((double)worst_lat / 1000.0 > m->max_us) {
        m->max_us = (double)worst_lat / 1000.0;
    }

    bhh_stats st;
    bhh_get_stats(table, &st);
    m->realloc_count = st.arena_realloc_count;
    m->theoretical_bytes_subject_to_realloc = st.arena_bytes_copied;
    m->final_arena_cap = st.arena_cap;
    m->never_allocated_slots = st.never_allocated_slots;
    m->internal_mem_bytes = bhh_memory_bytes(table);
    m->rss_kb = get_proc_rss_kb();
    m->peak_rss_kb = get_proc_peak_rss_kb();

    free(latencies);
    bhh_free(table);
}

static int run_isolated_trial(size_t n, bhh_growth_policy policy, growth_run_metrics *out)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(pipefd[0]);
        growth_run_metrics res;
        memset(&res, 0, sizeof(res));
        run_growth_worker(n, policy, &res);
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

static void aggregate_growth_metrics(growth_run_metrics *rounds, size_t r_count, growth_run_metrics *med)
{
    memset(med, 0, sizeof(*med));
    med->policy = rounds[0].policy;
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

    MED_SZ(realloc_count);
    MED_SZ(theoretical_bytes_subject_to_realloc);
    MED_SZ(final_arena_cap);
    MED_SZ(never_allocated_slots);
    MED_SZ(internal_mem_bytes);
    MED_SZ(rss_kb);
    MED_SZ(peak_rss_kb);

    MED_D(set_qps);
    MED_D(p50_us);
    MED_D(p95_us);
    MED_D(p99_us);
    MED_D(p99_9_us);
    MED_D(max_us);

    #undef MED_D
    #undef MED_SZ
}

static void compare_growth_size(const char *label, size_t n)
{
    printf("\n========================================================================================================================================\n");
    printf(" CONFRONTO CRESCITA ARENA: %s (N = %zu, 5 round alternati in processi isolati)\n", label, n);
    printf("========================================================================================================================================\n");

    growth_run_metrics r15[BENCH_ROUNDS];
    growth_run_metrics r20[BENCH_ROUNDS];

    for (int r = 0; r < BENCH_ROUNDS; r++) {
        /* Alternanza rigorosa dell'ordine di esecuzione per eliminare bias di allocatore/cache */
        if (r % 2 == 0) {
            /* Round dispari (0, 2, 4): 1.5x prima, poi 2.0x */
            if (run_isolated_trial(n, BHH_GROWTH_1_5X, &r15[r]) != 0) {
                fprintf(stderr, "Round %d: 1.5x trial failed\n", r + 1);
            }
            if (run_isolated_trial(n, BHH_GROWTH_2X, &r20[r]) != 0) {
                fprintf(stderr, "Round %d: 2.0x trial failed\n", r + 1);
            }
        } else {
            /* Round pari (1, 3): 2.0x prima, poi 1.5x */
            if (run_isolated_trial(n, BHH_GROWTH_2X, &r20[r]) != 0) {
                fprintf(stderr, "Round %d: 2.0x trial failed\n", r + 1);
            }
            if (run_isolated_trial(n, BHH_GROWTH_1_5X, &r15[r]) != 0) {
                fprintf(stderr, "Round %d: 1.5x trial failed\n", r + 1);
            }
        }
    }

    growth_run_metrics m15, m20;
    aggregate_growth_metrics(r15, BENCH_ROUNDS, &m15);
    aggregate_growth_metrics(r20, BENCH_ROUNDS, &m20);

    printf("+---------------------------------------+------------------------+------------------------+--------------------+\n");
    printf("| Metrica Misurata                      | Politica 1.5x (M2.1)   | Politica 2.0x          | Differenza         |\n");
    printf("+---------------------------------------+------------------------+------------------------+--------------------+\n");
    printf("| Numero Realloc Arena Eseguite         | %22zu | %22zu | %+18zd |\n",
           m15.realloc_count, m20.realloc_count, (ssize_t)m15.realloc_count - (ssize_t)m20.realloc_count);
    printf("| Byte Teorici Soggetti a Realloc       | %19.2f MB | %19.2f MB | %+15.2f MB |\n",
           (double)m15.theoretical_bytes_subject_to_realloc / (1024.0 * 1024.0),
           (double)m20.theoretical_bytes_subject_to_realloc / (1024.0 * 1024.0),
           (double)((ssize_t)m15.theoretical_bytes_subject_to_realloc - (ssize_t)m20.theoretical_bytes_subject_to_realloc) / (1024.0 * 1024.0));
    printf("| Capacita' Arena Finale (slot)         | %22zu | %22zu | %+18zd |\n",
           m15.final_arena_cap, m20.final_arena_cap, (ssize_t)m15.final_arena_cap - (ssize_t)m20.final_arena_cap);
    printf("| Slot Mai Allocati Rimasti (headroom)  | %22zu | %22zu | %+18zd |\n",
           m15.never_allocated_slots, m20.never_allocated_slots, (ssize_t)m15.never_allocated_slots - (ssize_t)m20.never_allocated_slots);
    printf("| Memoria Interna Riservata             | %19.2f MB | %19.2f MB | %+15.2f MB |\n",
           (double)m15.internal_mem_bytes / (1024.0 * 1024.0),
           (double)m20.internal_mem_bytes / (1024.0 * 1024.0),
           (double)((ssize_t)m15.internal_mem_bytes - (ssize_t)m20.internal_mem_bytes) / (1024.0 * 1024.0));
    printf("| Peak RSS (VmHWM da Kernel)            | %19.2f MB | %19.2f MB | %+15.2f MB |\n",
           (double)m15.peak_rss_kb / 1024.0,
           (double)m20.peak_rss_kb / 1024.0,
           (double)((ssize_t)m15.peak_rss_kb - (ssize_t)m20.peak_rss_kb) / 1024.0);
    printf("| Throughput SET Globale                | %17.0f ops/s | %17.0f ops/s | %+15.1f%% |\n",
           m15.set_qps, m20.set_qps, 100.0 * ((m15.set_qps / m20.set_qps) - 1.0));
    printf("+---------------------------------------+------------------------+------------------------+--------------------+\n");
    printf("| Latenza SET p50                       | %20.2f us | %20.2f us | %+15.2f us |\n",
           m15.p50_us, m20.p50_us, m15.p50_us - m20.p50_us);
    printf("| Latenza SET p95                       | %20.2f us | %20.2f us | %+15.2f us |\n",
           m15.p95_us, m20.p95_us, m15.p95_us - m20.p95_us);
    printf("| Latenza SET p99                       | %20.2f us | %20.2f us | %+15.2f us |\n",
           m15.p99_us, m20.p99_us, m15.p99_us - m20.p99_us);
    printf("| Latenza SET p99.9                     | %20.2f us | %20.2f us | %+15.2f us |\n",
           m15.p99_9_us, m20.p99_9_us, m15.p99_9_us - m20.p99_9_us);
    printf("| Latenza SET Massima (Spike Realloc)   | %20.2f us | %20.2f us | %+15.2f us |\n",
           m15.max_us, m20.max_us, m15.max_us - m20.max_us);
    printf("+---------------------------------------+------------------------+------------------------+--------------------+\n");

    printf("\n  NOTA ARCHITETTURALE SU REALLOC / MREMAP:\n");
    printf("  1. 'Byte teorici soggetti a realloc' rappresenta la somma delle dimensioni delle arene preesistenti\n");
    printf("     passate a realloc(). Tuttavia, sotto Linux/glibc, per buffer di grandi dimensioni (>128 KB),\n");
    printf("     realloc() si appoggia a mremap(MREMAP_MAYMOVE). Se lo spazio di indirizzamento virtuale adiacente\n");
    printf("     e' libero o viene rimappato a livello di page-table, il kernel sposta i mapping virtuali SENZA\n");
    printf("     copiare fisicamente le pagine in RAM sulla CPU.\n");
    printf("  2. Le prestazioni di realloc()/mremap() NON sono deterministiche: la presenza o assenza di copy fisica\n");
    printf("     e la durata dello spike dipendono dallo stato della memoria virtuale, dalla frammentazione dell'heap\n");
    printf("     e dalla topologia dei thread/processi del kernel Linux.\n");
}

int main(void)
{
    printf("========================================================================================================================================\n");
    printf(" BLINKHASH HYBRID — BENCHMARK REALE DELLA CRESCITA ARENA (1.5x vs 2.0x)\n");
    printf(" Esecuzione al 100%% tramite API pubblica, misura reale di bhh_set(), 5 round alternati, cattura code di latenza p99.9 e max\n");
    printf("========================================================================================================================================\n");

    compare_growth_size("100k Keys", 100000);
    compare_growth_size("300k Keys", 300000);
    compare_growth_size("1M Keys",   1000000);

    return 0;
}
