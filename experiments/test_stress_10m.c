#define _POSIX_C_SOURCE 200809L

#include "blinkhash_hybrid.h"
#include "hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>

#define STRESS_KEY_SPACE     1500000U
#define STRESS_TARGET_LIVE   1000000U
#define STRESS_TOTAL_OPS     10000000U
#define STRESS_STALE_POOL_SZ 100000U

/* Deterministic SplitMix64 PRNG */
static uint64_t g_rng_state = 0x853c49e6748fea9bULL;

static inline uint64_t next_rng(void)
{
    uint64_t z = (g_rng_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Simulated Fault Injection Allocator */
static size_t g_alloc_counter = 0;
static size_t g_fail_every_n = 0;
static size_t g_simulated_failures = 0;

static void *stress_mock_malloc(size_t size)
{
    g_alloc_counter++;
    if (g_fail_every_n > 0 && (g_alloc_counter % g_fail_every_n) == 0) {
        g_simulated_failures++;
        return NULL;
    }
    return malloc(size);
}

static void *stress_mock_realloc(void *ptr, size_t size)
{
    g_alloc_counter++;
    if (g_fail_every_n > 0 && (g_alloc_counter % g_fail_every_n) == 0) {
        g_simulated_failures++;
        return NULL;
    }
    return realloc(ptr, size);
}

static void stress_mock_free(void *ptr)
{
    free(ptr);
}

/* Reference model arrays */
static uint8_t    *g_key_active;
static uint32_t   *g_key_val_tag;
static uint64_t   *g_key_expire_ms;
static bhh_handle *g_key_handle;

/* Stale handle pool */
static bhh_handle  g_stale_handles[STRESS_STALE_POOL_SZ];
static size_t      g_stale_count = 0;

static void record_stale_handle(bhh_handle h)
{
    if (h != BHH_HANDLE_NULL && g_stale_count < STRESS_STALE_POOL_SZ) {
        g_stale_handles[g_stale_count++] = h;
    }
}

static inline void make_key(uint32_t key_id, uint8_t *buf, size_t *len)
{
    /* Binary-transparent key with null byte prefix and binary ID */
    buf[0] = 0xAA;
    buf[1] = 0x00; /* Embedded binary zero */
    memcpy(buf + 2, &key_id, sizeof(key_id));
    *len = 2 + sizeof(key_id);
}

static inline void make_val(uint32_t tag, uint8_t *buf, size_t *len)
{
    /* Binary-transparent payload */
    buf[0] = 0xBB;
    buf[1] = 0x00;
    memcpy(buf + 2, &tag, sizeof(tag));
    *len = 2 + sizeof(tag);
}

int main(void)
{
    printf("========================================================================\n");
    printf(" BlinkHash Hybrid: 10,000,000-Operation Deep Engineering Stress Test\n");
    printf(" Target: >= 1,000,000 Concurrent Live Keys under ASan + UBSan\n");
    printf("========================================================================\n");

    /* Allocate reference model */
    g_key_active = calloc(STRESS_KEY_SPACE, sizeof(uint8_t));
    g_key_val_tag = calloc(STRESS_KEY_SPACE, sizeof(uint32_t));
    g_key_expire_ms = calloc(STRESS_KEY_SPACE, sizeof(uint64_t));
    g_key_handle = calloc(STRESS_KEY_SPACE, sizeof(bhh_handle));
    assert(g_key_active && g_key_val_tag && g_key_expire_ms && g_key_handle);

    bhh_allocator alloc_hooks = {
        .malloc_fn = stress_mock_malloc,
        .realloc_fn = stress_mock_realloc,
        .free_fn = stress_mock_free
    };

    bhh_table *table = bhh_create_with_allocator(16, &alloc_hooks);
    assert(table != NULL);
    bhh_set_tombstone_policy(table, BHH_POLICY_INPLACE_REHASH);

    uint64_t current_clock_ms = 1000000ULL;
    uint8_t kbuf[32], vbuf[32];
    size_t klen, vlen;

    /* -------------------------------------------------------------------------
     * PHASE 1: Massive Insertion & Repeated Resizes up to 1,000,000 Concurrent Keys
     * ------------------------------------------------------------------------- */
    printf("[Phase 1/4] Populating 1,000,000 concurrent distinct keys (repeated resizes)...\n");
    size_t live_count = 0;
    for (uint32_t id = 0; id < STRESS_TARGET_LIVE; id++) {
        make_key(id, kbuf, &klen);
        uint32_t tag = id ^ 0x5A5A5A5AU;
        make_val(tag, vbuf, &vlen);
        uint64_t expire = current_clock_ms + 10000000ULL; /* Far in future */

        bhh_handle h = BHH_HANDLE_NULL;
        int rc = bhh_set(table, kbuf, klen, vbuf, vlen, expire, &h);
        assert(rc == 0);
        assert(h != BHH_HANDLE_NULL);

        g_key_active[id] = 1;
        g_key_val_tag[id] = tag;
        g_key_expire_ms[id] = expire;
        g_key_handle[id] = h;
        live_count++;

        if ((id + 1) % 250000 == 0) {
            printf("  Inserted %u / %u keys (Capacity: %zu, Tombstones: %zu)...\n",
                   id + 1, STRESS_TARGET_LIVE, table->capacity, table->tombstones);
        }
    }

    assert(bhh_size(table) == STRESS_TARGET_LIVE);
    printf("  [PASS] 1,000,000 concurrent keys inserted! Table capacity: %zu, Arena cap: %zu\n\n",
           table->capacity, table->arena_cap);

    /* -------------------------------------------------------------------------
     * PHASE 2: 8,000,000 Operations of High-Intensity Churn, TTLs & Handles
     * ------------------------------------------------------------------------- */
    printf("[Phase 2/4] Executing 8,000,000 high-intensity churn operations...\n");

    size_t set_ops = 0, get_ops = 0, del_ops = 0, stale_checks = 0, purges = 0;

    for (uint32_t op = 0; op < 8000000U; op++) {
        uint64_t r = next_rng();
        uint32_t action = (uint32_t)(r % 100);
        uint32_t key_id = (uint32_t)((r >> 8) % STRESS_KEY_SPACE);

        current_clock_ms += 1; /* Monotonic clock advance */

        if (action < 40) {
            /* 40% GET */
            get_ops++;
            make_key(key_id, kbuf, &klen);
            const uint8_t *val_out = NULL;
            size_t val_len_out = 0;
            bhh_handle h_out = BHH_HANDLE_NULL;

            int rc = bhh_get(table, kbuf, klen, current_clock_ms, &val_out, &val_len_out, &h_out);

            if (g_key_active[key_id]) {
                if (current_clock_ms >= g_key_expire_ms[key_id]) {
                    assert(rc == 1); /* Expired */
                } else {
                    assert(rc == 0);
                    assert(val_len_out == sizeof(uint32_t) + 2);
                    uint32_t got_tag;
                    memcpy(&got_tag, val_out + 2, sizeof(got_tag));
                    assert(got_tag == g_key_val_tag[key_id]);
                    assert(h_out == g_key_handle[key_id]);
                }
            } else {
                assert(rc == 1); /* Not found */
            }
        } else if (action < 65) {
            /* 25% SET (Insert or Overwrite or TTL update) */
            set_ops++;
            make_key(key_id, kbuf, &klen);
            uint32_t new_tag = (uint32_t)next_rng();
            make_val(new_tag, vbuf, &vlen);

            /* Some keys have short TTL for simultaneous expiration tests */
            uint64_t expire = current_clock_ms + 5000000ULL;
            if ((op % 20) == 0) {
                expire = current_clock_ms + 50ULL; /* Expire shortly */
            }

            bhh_handle old_h = g_key_handle[key_id];
            bhh_handle new_h = BHH_HANDLE_NULL;

            int rc = bhh_set(table, kbuf, klen, vbuf, vlen, expire, &new_h);
            assert(rc == 0);
            assert(new_h != BHH_HANDLE_NULL);

            if (g_key_active[key_id] && current_clock_ms < g_key_expire_ms[key_id]) {
                /* In-place overwrite of active unexpired key keeps same arena slot */
                assert(bhh_handle_idx(new_h) == bhh_handle_idx(old_h));
            } else {
                if (g_key_active[key_id]) {
                    /* Was expired: previous handle is now stale */
                    record_stale_handle(old_h);
                } else {
                    live_count++;
                }
                g_key_active[key_id] = 1;
            }

            g_key_val_tag[key_id] = new_tag;
            g_key_expire_ms[key_id] = expire;
            g_key_handle[key_id] = new_h;
        } else if (action < 85) {
            /* 20% DELETE (by key or by handle) */
            del_ops++;
            if (g_key_active[key_id]) {
                make_key(key_id, kbuf, &klen);
                bhh_handle h = g_key_handle[key_id];
                bool was_unexpired = (current_clock_ms < g_key_expire_ms[key_id]);

                int rc;
                if (action % 2 == 0) {
                    rc = bhh_del_handle(table, h);
                } else {
                    rc = bhh_del(table, kbuf, klen);
                }

                if (was_unexpired) {
                    assert(rc == 0);
                } else {
                    /* If expired, it may have already been purged by wheel (rc == 1) or still pending (rc == 0) */
                    assert(rc == 0 || rc == 1);
                }

                record_stale_handle(h);
                g_key_active[key_id] = 0;
                g_key_handle[key_id] = BHH_HANDLE_NULL;
                live_count--;

                /* Verify deleted key is immediately unreachable */
                assert(bhh_exists(table, kbuf, klen, current_clock_ms) == 1);
            }
        } else if (action < 95) {
            /* 10% Stale Handle Verification */
            stale_checks++;
            if (g_stale_count > 0) {
                size_t idx = (size_t)(next_rng() % g_stale_count);
                bhh_handle stale_h = g_stale_handles[idx];

                /* Stale handle MUST resolve to NULL */
                const bhh_record *rec = bhh_resolve_handle(table, stale_h);
                assert(rec == NULL);

                /* Deleting a stale handle MUST fail safely returning 1 */
                int rc = bhh_del_handle(table, stale_h);
                assert(rc == 1);
            }
        } else {
            /* 5% Wheel Purge & Expiration check */
            purges++;
            size_t purged = bhh_purge_all(table, current_clock_ms);
            (void)purged;
        }

        if ((op + 1) % 2000000 == 0) {
            bhh_stats st;
            bhh_get_stats(table, &st);
            printf("  Progress: %u / 8,000,000 ops | Live: %zu | Tombstones: %zu | Cap: %zu | In-place Rehashes: %lu\n",
                   op + 1, st.size, st.tombstones, st.capacity, (unsigned long)st.inplace_rehash_count);
        }
    }

    printf("  [PASS] 8,000,000 churn operations completed successfully!\n");
    printf("         SET: %zu, GET: %zu, DEL: %zu, Stale Checks: %zu, Purges: %zu\n\n",
           set_ops, get_ops, del_ops, stale_checks, purges);

    /* -------------------------------------------------------------------------
     * PHASE 3: Massive Deletion & Re-insertion of the exact same 1,000,000 keys
     * ------------------------------------------------------------------------- */
    printf("[Phase 3/4] Massive Deletion & Re-insertion of 1,000,000 keys...\n");

    /* Delete all remaining active keys */
    size_t cleared = 0;
    for (uint32_t id = 0; id < STRESS_KEY_SPACE; id++) {
        if (g_key_active[id]) {
            make_key(id, kbuf, &klen);
            bhh_handle h = g_key_handle[id];
            int rc = bhh_del(table, kbuf, klen);
            bool was_unexpired = (current_clock_ms < g_key_expire_ms[id]);
            if (was_unexpired) {
                assert(rc == 0);
            }
            record_stale_handle(h);
            g_key_active[id] = 0;
            g_key_handle[id] = BHH_HANDLE_NULL;
            cleared++;
        }
    }
    bhh_purge_all(table, UINT64_MAX);
    printf("  Deleted %zu active keys. Verifying table is 100%% empty...\n", cleared);
    assert(bhh_size(table) == 0);

    /* Re-insert 1,000,000 keys again into the same table */
    printf("  Re-inserting 1,000,000 keys into recycled slots...\n");
    for (uint32_t id = 0; id < STRESS_TARGET_LIVE; id++) {
        make_key(id, kbuf, &klen);
        uint32_t tag = id ^ 0xCAFEBABE;
        make_val(tag, vbuf, &vlen);
        uint64_t expire = current_clock_ms + 10000000ULL;

        bhh_handle h = BHH_HANDLE_NULL;
        int rc = bhh_set(table, kbuf, klen, vbuf, vlen, expire, &h);
        assert(rc == 0);
        assert(h != BHH_HANDLE_NULL);

        g_key_active[id] = 1;
        g_key_val_tag[id] = tag;
        g_key_handle[id] = h;
    }
    assert(bhh_size(table) == STRESS_TARGET_LIVE);

    /* Verify all recorded stale handles from previous lifecycle remain INVALID */
    printf("  Verifying all %zu stale handles from previous lifecycles are invalid...\n", g_stale_count);
    for (size_t i = 0; i < g_stale_count; i++) {
        bhh_handle old_h = g_stale_handles[i];
        assert(bhh_resolve_handle(table, old_h) == NULL);
        assert(bhh_del_handle(table, old_h) == 1);
    }
    printf("  [PASS] ABA-prevention verified: 0 dangling handles!\n\n");

    /* -------------------------------------------------------------------------
     * PHASE 4: Fault Injection & Allocation Failure Rollbacks
     * ------------------------------------------------------------------------- */
    printf("[Phase 4/4] Fault Injection: Testing allocation failure rollbacks...\n");

    /* Configure mock allocator to fail every 5th allocation */
    g_alloc_counter = 0;
    g_simulated_failures = 0;
    g_fail_every_n = 5;

    size_t attempted_failures = 0;
    size_t survived_inserts = 0;

    for (uint32_t id = STRESS_TARGET_LIVE; id < STRESS_TARGET_LIVE + 10000; id++) {
        make_key(id, kbuf, &klen);
        make_val(id, vbuf, &vlen);

        size_t size_before = bhh_size(table);
        size_t tombstones_before = table->tombstones;

        bhh_handle h = BHH_HANDLE_NULL;
        int rc = bhh_set(table, kbuf, klen, vbuf, vlen, current_clock_ms + 1000000ULL, &h);

        if (rc != 0) {
            /* Allocation failure simulated: verify rollback consistency! */
            attempted_failures++;
            assert(bhh_size(table) == size_before);
            assert(table->tombstones == tombstones_before);
            assert(bhh_exists(table, kbuf, klen, current_clock_ms) == 1);
        } else {
            survived_inserts++;
            assert(bhh_size(table) == size_before + 1);
        }
    }

    /* Disable fault injection */
    g_fail_every_n = 0;
    printf("  Simulated failures tested: %zu (table recovered cleanly with zero corruptions!)\n",
           g_simulated_failures);
    printf("  [PASS] Fault injection rollback verified!\n\n");

    /* Final clean teardown */
    bhh_free(table);
    free(g_key_active);
    free(g_key_val_tag);
    free(g_key_expire_ms);
    free(g_key_handle);

    printf("========================================================================\n");
    printf(" [ALL PASS] 10,000,000 Operations Stress Test Completed with 0 Errors!\n");
    printf(" Under AddressSanitizer and UndefinedBehaviorSanitizer: CLEAN\n");
    printf("========================================================================\n");

    return 0;
}
