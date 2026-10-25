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
#include <assert.h>
#include <time.h>

#define TOTAL_OPS 1000000
#define KEY_SPACE 20000

typedef struct {
    uint8_t key[64];
    size_t key_len;
    uint8_t val[128];
    size_t val_len;
    uint64_t expires_at_ms;
    bhh_handle hybrid_handle;
    int active;
} ref_item;

static ref_item g_ref[KEY_SPACE];

static int diff_mock_clock(uint64_t *now_ms_out, void *user_data)
{
    *now_ms_out = *(uint64_t *)user_data;
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

int main(void)
{
    printf("\n========================================================================\n");
    printf(" BlinkHash 1,000,000-Operation Differential Verification Suite\n");
    printf(" Testing: Baseline vs Candidate A vs Candidate B vs Candidate C vs Hybrid\n");
    printf("========================================================================\n");

    uint64_t simulated_time = 1000000ULL;
    bk_store *base = bk_store_create_with_clock(512, diff_mock_clock, &simulated_time);
    bha_table *a = bha_create(64);
    bhb_table *b = bhb_create(64);
    bhc_table *c = bhc_create(64);
    bhh_table *hybrid = bhh_create(64);

    assert(base && a && b && c && hybrid);
    memset(g_ref, 0, sizeof(g_ref));

    uint64_t prng = 0x5bf97827992f31e7ULL;

    size_t sets = 0, gets = 0, dels = 0, purges = 0, collisions = 0;

    for (int op_i = 0; op_i < TOTAL_OPS; op_i++) {
        uint64_t r = xorshift64(&prng);
        int op = (int)(r % 10);
        int key_id;

        /* Induce deliberate collision clusters every 50 ops */
        if (op_i % 50 == 0) {
            key_id = (int)(r % 10); /* Force onto 10 dense collision keys */
            collisions++;
        } else {
            key_id = (int)(r % KEY_SPACE);
        }

        uint8_t key[32];
        size_t key_len = snprintf((char *)key, sizeof(key), "key_%d", key_id);
        int ref_idx = key_id;

        /* Advance time periodically to trigger expirations and wheel rotation */
        if (op_i % 500 == 0) {
            simulated_time += 150;
            bhc_purge_expired(c, simulated_time);
            size_t h_purged = bhh_purge_expired(hybrid, simulated_time);
            if (h_purged > 0) purges += h_purged;
        }

        /* Test SWAR fallback toggle every 50,000 ops */
        if (op_i % 50000 == 0) {
            bhh_set_force_swar(hybrid, (op_i / 50000) % 2 == 1);
        }

        if (op < 5) {
            /* SET or OVERWRITE with variable payload length */
            sets++;
            size_t val_len = 10 + (size_t)((r >> 8) % 80);
            uint8_t val[128];
            for (size_t j = 0; j < val_len; j++) val[j] = (uint8_t)((r + j) & 0xFF);

            uint64_t ttl_ms = 2000 + ((r >> 16) % 20000);
            uint64_t expires_at = simulated_time + ttl_ms;

            /* Update Reference */
            memcpy(g_ref[ref_idx].key, key, key_len);
            g_ref[ref_idx].key_len = key_len;
            memcpy(g_ref[ref_idx].val, val, val_len);
            g_ref[ref_idx].val_len = val_len;
            g_ref[ref_idx].expires_at_ms = expires_at;
            g_ref[ref_idx].active = 1;

            /* Baseline */
            assert(bk_set(base, key, key_len, val, val_len, ttl_ms) == BK_OK);

            /* Candidate A */
            assert(bha_set(a, key, key_len, val, val_len, expires_at) == 0);

            /* Candidate B */
            assert(bhb_set(b, key, key_len, val, val_len, expires_at, NULL) == 0);

            /* Candidate C */
            assert(bhc_set(c, key, key_len, val, val_len, expires_at) == 0);

            /* Hybrid */
            bhh_handle new_h = BHH_HANDLE_NULL;
            assert(bhh_set(hybrid, key, key_len, val, val_len, expires_at, &new_h) == 0);
            assert(new_h != BHH_HANDLE_NULL);
            g_ref[ref_idx].hybrid_handle = new_h;

            /* Validate handle resolution immediately */
            const bhh_record *rec = bhh_resolve_handle(hybrid, new_h);
            assert(rec != NULL);
            assert(rec->val_len == val_len);
            assert(memcmp(rec->payload + rec->key_len, val, val_len) == 0);
        } else if (op < 8) {
            /* GET and EXISTS */
            gets++;
            int ref_found = (g_ref[ref_idx].active && simulated_time < g_ref[ref_idx].expires_at_ms);

            const uint8_t *va = NULL, *vb = NULL, *vc = NULL, *vh = NULL;
            size_t la = 0, lb = 0, lc = 0, lh = 0;
            bhh_handle h_got = BHH_HANDLE_NULL;

            int ra = bha_get(a, key, key_len, simulated_time, &va, &la);
            int rb = bhb_get(b, key, key_len, simulated_time, &vb, &lb, NULL);
            int rc = bhc_get(c, key, key_len, simulated_time, &vc, &lc);
            int rh = bhh_get(hybrid, key, key_len, simulated_time, &vh, &lh, &h_got);

            uint8_t *v_base = NULL;
            size_t l_base = 0;
            bk_status s_base_get = bk_get(base, key, key_len, &v_base, &l_base);
            bk_status s_base_ex  = bk_exists(base, key, key_len);

            if (ref_found) {
                assert(s_base_ex == BK_OK);
                assert(s_base_get == BK_OK);
                assert(l_base == g_ref[ref_idx].val_len);
                assert(memcmp(v_base, g_ref[ref_idx].val, l_base) == 0);
                free(v_base);

                assert(ra == 0 && rb == 0 && rc == 0 && rh == 0);
                assert(la == g_ref[ref_idx].val_len);
                assert(lb == g_ref[ref_idx].val_len);
                assert(lc == g_ref[ref_idx].val_len);
                assert(lh == g_ref[ref_idx].val_len);
                assert(memcmp(va, g_ref[ref_idx].val, la) == 0);
                assert(memcmp(vb, g_ref[ref_idx].val, lb) == 0);
                assert(memcmp(vc, g_ref[ref_idx].val, lc) == 0);
                assert(memcmp(vh, g_ref[ref_idx].val, lh) == 0);

                /* Handle check */
                assert(h_got == g_ref[ref_idx].hybrid_handle);
                assert(bhh_resolve_handle(hybrid, h_got) != NULL);
            } else {
                assert(s_base_ex == BK_ERR_NOT_FOUND);
                assert(s_base_get == BK_ERR_NOT_FOUND);
                assert(ra == 1 && rb == 1 && rc == 1 && rh == 1);
            }
        } else {
            /* DEL */
            dels++;
            int ref_had_active = (g_ref[ref_idx].active && simulated_time < g_ref[ref_idx].expires_at_ms);
            bhh_handle prev_handle = g_ref[ref_idx].hybrid_handle;
            g_ref[ref_idx].active = 0;

            bk_status s_base_del = bk_del(base, key, key_len);
            int ra = bha_del(a, key, key_len);
            int rb = bhb_del(b, key, key_len);
            int rc = bhc_del(c, key, key_len);
            int rh = bhh_del(hybrid, key, key_len);

            if (ref_had_active) {
                assert(s_base_del == BK_OK);
                assert(ra == 0 && rb == 0 && rc == 0 && rh == 0);
                /* Crucial: previously held handle must now be STALE! */
                assert(bhh_resolve_handle(hybrid, prev_handle) == NULL);
            } else {
                assert(s_base_del == BK_ERR_NOT_FOUND);
                assert(ra == 1 && rb == 1 && rc == 1 && rh == 1);
            }
        }

        if ((op_i + 1) % 250000 == 0) {
            printf("  Progress: %d / %d operations verified...\n", op_i + 1, TOTAL_OPS);
        }
    }

    printf("\n========================================================================\n");
    printf(" RESULTS:\n");
    printf("   Total Operations: %d\n", TOTAL_OPS);
    printf("   SET Operations:   %zu\n", sets);
    printf("   GET Operations:   %zu\n", gets);
    printf("   DEL Operations:   %zu\n", dels);
    printf("   Wheel Purges:     %zu\n", purges);
    printf("   Dense Collisions: %zu\n", collisions);
    printf(" [SUCCESS] 1,000,000 differential operations passed with 100%% equivalence!\n");
    printf("========================================================================\n");

    bk_store_free(base);
    bha_free(a);
    bhb_free(b);
    bhc_free(c);
    bhh_free(hybrid);
    return 0;
}
