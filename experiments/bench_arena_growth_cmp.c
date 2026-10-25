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

typedef struct {
    double factor; // 1.5 or 2.0
    size_t initial_arena_cap;
    size_t n;

    size_t realloc_count;
    size_t total_bytes_copied;
    uint64_t cumulative_realloc_ns;
    uint64_t worst_set_lat_ns;
    double set_qps;

    size_t final_index_cap;
    size_t final_arena_cap;
    size_t unused_slots;
    size_t internal_mem_kb;
    size_t delta_rss_kb;
    size_t peak_rss_kb;
} growth_metrics;

/* Simulated custom arena growth harness on top of bhh_table */
static void run_growth_sim(size_t n, double factor, size_t initial_arena, growth_metrics *m)
{
    size_t rss_start = get_proc_rss_kb();

    /* Index capacity sized for n * 2 */
    bhh_table *table = bhh_create(n * 2);

    /* Shrink the initial arena down to initial_arena to simulate decoupled arena start */
    if (table->arena_cap > initial_arena) {
        bhh_record *shrunk = realloc(table->arena, initial_arena * sizeof(bhh_record));
        if (shrunk) {
            table->arena = shrunk;
            table->arena_cap = initial_arena;
        }
    }

    m->factor = factor;
    m->initial_arena_cap = initial_arena;
    m->n = n;
    m->realloc_count = 0;
    m->total_bytes_copied = 0;
    m->cumulative_realloc_ns = 0;
    m->worst_set_lat_ns = 0;

    uint8_t kbuf[32], vbuf[64];
    size_t klen, vlen;
    uint64_t now_ms = 1000000ULL;

    uint64_t t_start = now_ns();

    for (uint32_t i = 0; i < n; i++) {
        make_key(i, kbuf, &klen);
        make_val(i, vbuf, &vlen);

        /* Intercept arena growth if needed */
        if (table->first_free == 0 && table->arena_count >= table->arena_cap) {
            size_t old_cap = table->arena_cap;
            size_t new_cap = (factor == 1.5) ? (old_cap + (old_cap >> 1)) : (old_cap * 2);
            if (new_cap <= old_cap) new_cap = old_cap + 64;

            uint64_t r0 = now_ns();
            bhh_record *new_arena = realloc(table->arena, new_cap * sizeof(bhh_record));
            uint64_t r1 = now_ns();

            if (new_arena) {
                memset(new_arena + old_cap, 0, (new_cap - old_cap) * sizeof(bhh_record));
                table->arena = new_arena;
                table->arena_cap = new_cap;

                m->realloc_count++;
                m->total_bytes_copied += (old_cap * sizeof(bhh_record));
                m->cumulative_realloc_ns += (r1 - r0);
            }
        }

        uint64_t op_t0 = now_ns();
        bhh_set(table, kbuf, klen, vbuf, vlen, now_ms + 10000000ULL, NULL);
        uint64_t op_t1 = now_ns();
        uint64_t op_lat = op_t1 - op_t0;
        if (op_lat > m->worst_set_lat_ns) {
            m->worst_set_lat_ns = op_lat;
        }
    }

    uint64_t t_end = now_ns();
    m->set_qps = (double)n / ((double)(t_end - t_start) / 1e9);

    size_t rss_end = get_proc_rss_kb();
    m->delta_rss_kb = (rss_end > rss_start) ? (rss_end - rss_start) : 0;
    m->peak_rss_kb = get_proc_peak_rss_kb();

    m->final_index_cap = table->capacity;
    m->final_arena_cap = table->arena_cap;
    m->unused_slots = (table->arena_cap > table->arena_count) ? (table->arena_cap - table->arena_count) : 0;
    m->internal_mem_kb = bhh_memory_bytes(table) / 1024;

    bhh_free(table);
}

static int run_isolated_sim(size_t n, double factor, size_t initial_arena, growth_metrics *out)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(pipefd[0]);
        growth_metrics res;
        memset(&res, 0, sizeof(res));
        run_growth_sim(n, factor, initial_arena, &res);
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

void compare_strategies(size_t n, size_t init_cap)
{
    printf("\n========================================================================\n");
    printf(" COMPARING GROWTH STRATEGIES: N = %zu (Initial Arena Cap = %zu)\n", n, init_cap);
    printf("========================================================================\n");

    growth_metrics m_15, m_20;
    run_isolated_sim(n, 1.5, init_cap, &m_15);
    run_isolated_sim(n, 2.0, init_cap, &m_20);

    printf("%-30s | %-18s | %-18s\n", "Metric", "Strategy 1.5x", "Strategy 2.0x");
    printf("-------------------------------+--------------------+-------------------\n");
    printf("%-30s | %18zu | %18zu\n", "Final Arena Capacity", m_15.final_arena_cap, m_20.final_arena_cap);
    printf("%-30s | %18zu | %18zu\n", "Unused Arena Slots", m_15.unused_slots, m_20.unused_slots);
    printf("%-30s | %17.1f%% | %17.1f%%\n", "Arena Waste Ratio",
           100.0 * (double)m_15.unused_slots / m_15.final_arena_cap,
           100.0 * (double)m_20.unused_slots / m_20.final_arena_cap);
    printf("%-30s | %18zu | %18zu\n", "Realloc Count", m_15.realloc_count, m_20.realloc_count);
    printf("%-30s | %15.2f MB | %15.2f MB\n", "Total Bytes Copied",
           (double)m_15.total_bytes_copied / (1024.0 * 1024.0),
           (double)m_20.total_bytes_copied / (1024.0 * 1024.0));
    printf("%-30s | %15.2f ms | %15.2f ms\n", "Cumulative Realloc Time",
           (double)m_15.cumulative_realloc_ns / 1e6,
           (double)m_20.cumulative_realloc_ns / 1e6);
    printf("%-30s | %15.2f us | %15.2f us\n", "Worst SET Latency (Spike)",
           (double)m_15.worst_set_lat_ns / 1e3,
           (double)m_20.worst_set_lat_ns / 1e3);
    printf("%-30s | %14.2f Mops/s | %14.2f Mops/s\n", "SET Average Throughput",
           m_15.set_qps / 1e6, m_20.set_qps / 1e6);
    printf("%-30s | %15.2f MB | %15.2f MB\n", "Internal Memory",
           (double)m_15.internal_mem_kb / 1024.0,
           (double)m_20.internal_mem_kb / 1024.0);
    printf("%-30s | %15.2f MB | %15.2f MB\n", "Populated Delta RSS",
           (double)m_15.delta_rss_kb / 1024.0,
           (double)m_20.delta_rss_kb / 1024.0);
    printf("%-30s | %15.2f MB | %15.2f MB\n", "Peak RSS (VmHWM)",
           (double)m_15.peak_rss_kb / 1024.0,
           (double)m_20.peak_rss_kb / 1024.0);
}

int main(void)
{
    printf("========================================================================\n");
    printf(" BlinkHash Hybrid: Experimental Arena Growth Strategy Comparison\n");
    printf(" Comparing 1.5x vs 2.0x Dynamic Growth under Identical Workloads\n");
    printf("========================================================================\n");

    /* Test 1: N = 100k, initial arena = 1024 */
    compare_strategies(100000, 1024);

    /* Test 2: N = 300k, initial arena = 1024 */
    compare_strategies(300000, 1024);

    /* Test 3: N = 1M, initial arena = 1024 */
    compare_strategies(1000000, 1024);

    return 0;
}
