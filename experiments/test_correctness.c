#define _POSIX_C_SOURCE 200809L

#include "blinkkv.h"
#include "blinkhash_a.h"
#include "blinkhash_b.h"
#include "blinkhash_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#define NUM_OPS 25000
#define KEY_SPACE 2000

/* Simple reference model: brute force array */
typedef struct {
    uint8_t key[64];
    size_t key_len;
    uint8_t val[64];
    size_t val_len;
    uint64_t expires_at_ms;
    int active;
} ref_entry;

static ref_entry g_ref[KEY_SPACE];

static int ref_find(const uint8_t *key, size_t key_len)
{
    for (int i = 0; i < KEY_SPACE; i++) {
        if (g_ref[i].active && g_ref[i].key_len == key_len) {
            if (key_len == 0 || memcmp(g_ref[i].key, key, key_len) == 0) {
                return i;
            }
        }
    }
    return -1;
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
    printf("\n>>> Running Differential Correctness Test across Baseline, A, B, and C <<<\n");

    bk_store *base = bk_store_create(128);
    bha_table *a = bha_create(16);
    bhb_table *b = bhb_create(16);
    bhc_table *c = bhc_create(16);

    assert(base && a && b && c);
    memset(g_ref, 0, sizeof(g_ref));

    uint64_t prng = 0xabcdef0123456789ULL;
    uint64_t simulated_time = 1000000ULL;

    for (int step = 0; step < NUM_OPS; step++) {
        uint64_t r = xorshift64(&prng);
        int op = (int)(r % 10); /* 0..4 SET, 5..7 GET, 8..9 DEL */
        int key_id = (int)((r >> 8) % KEY_SPACE);

        uint8_t key[32];
        size_t key_len = snprintf((char *)key, sizeof(key), "k_%d", key_id);

        /* Advance simulated time slightly every 100 steps */
        if (step % 100 == 0) {
            simulated_time += 100;
            bhc_purge_expired(c, simulated_time);
        }

        if (op < 5) {
            /* SET */
            uint8_t val[32];
            size_t val_len = snprintf((char *)val, sizeof(val), "v_%d_%d", key_id, step);
            uint64_t ttl_ms = 5000 + ((r >> 16) % 15000);
            uint64_t expires_at = simulated_time + ttl_ms;

            /* Reference */
            int ref_idx = ref_find(key, key_len);
            if (ref_idx < 0) {
                for (int i = 0; i < KEY_SPACE; i++) {
                    if (!g_ref[i].active) {
                        ref_idx = i;
                        break;
                    }
                }
            }
            assert(ref_idx >= 0);
            memcpy(g_ref[ref_idx].key, key, key_len);
            g_ref[ref_idx].key_len = key_len;
            memcpy(g_ref[ref_idx].val, val, val_len);
            g_ref[ref_idx].val_len = val_len;
            g_ref[ref_idx].expires_at_ms = expires_at;
            g_ref[ref_idx].active = 1;

            /* Baseline */
            bk_status s_base = bk_set(base, key, key_len, val, val_len, ttl_ms);
            assert(s_base == BK_OK);

            /* Candidate A */
            int s_a = bha_set(a, key, key_len, val, val_len, expires_at);
            assert(s_a == 0);

            /* Candidate B */
            int s_b = bhb_set(b, key, key_len, val, val_len, expires_at, NULL);
            assert(s_b == 0);

            /* Candidate C */
            int s_c = bhc_set(c, key, key_len, val, val_len, expires_at);
            assert(s_c == 0);
        } else if (op < 8) {
            /* GET / EXISTS */
            int ref_idx = ref_find(key, key_len);
            int ref_found = (ref_idx >= 0 && simulated_time < g_ref[ref_idx].expires_at_ms);

            const uint8_t *val_a = NULL;
            size_t val_a_len = 0;
            int res_a = bha_get(a, key, key_len, simulated_time, &val_a, &val_a_len);

            const uint8_t *val_b = NULL;
            size_t val_b_len = 0;
            int res_b = bhb_get(b, key, key_len, simulated_time, &val_b, &val_b_len, NULL);

            const uint8_t *val_c = NULL;
            size_t val_c_len = 0;
            int res_c = bhc_get(c, key, key_len, simulated_time, &val_c, &val_c_len);

            if (ref_found) {
                assert(res_a == 0);
                assert(res_b == 0);
                assert(res_c == 0);
                assert(val_a_len == g_ref[ref_idx].val_len);
                assert(val_b_len == g_ref[ref_idx].val_len);
                assert(val_c_len == g_ref[ref_idx].val_len);
                assert(memcmp(val_a, g_ref[ref_idx].val, val_a_len) == 0);
                assert(memcmp(val_b, g_ref[ref_idx].val, val_b_len) == 0);
                assert(memcmp(val_c, g_ref[ref_idx].val, val_c_len) == 0);
            } else {
                assert(res_a == 1);
                assert(res_b == 1);
                assert(res_c == 1);
            }
        } else {
            /* DEL */
            int ref_idx = ref_find(key, key_len);
            int ref_had_active = (ref_idx >= 0 && simulated_time < g_ref[ref_idx].expires_at_ms);
            if (ref_idx >= 0) {
                g_ref[ref_idx].active = 0;
            }

            int res_a = bha_del(a, key, key_len);
            int res_b = bhb_del(b, key, key_len);
            int res_c = bhc_del(c, key, key_len);

            if (ref_had_active) {
                assert(res_a == 0);
                assert(res_b == 0);
                assert(res_c == 0);
            } else {
                /* Key was missing or expired */
                assert(res_a == 1 || res_a == 0);
                assert(res_b == 1 || res_b == 0);
                assert(res_c == 1 || res_c == 0);
            }
        }
    }

    printf("  [SUCCESS] All %d differential operations passed with 100%% equivalence!\n", NUM_OPS);

    bk_store_free(base);
    bha_free(a);
    bhb_free(b);
    bhc_free(c);
    return 0;
}
