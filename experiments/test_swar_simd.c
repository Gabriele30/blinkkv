#define _POSIX_C_SOURCE 200809L

#include "blinkhash_hybrid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

static uint64_t g_rng = 0x987654321ULL;
static inline uint64_t next_rnd(void) {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1DULL;
}

int main(void)
{
    printf("========================================================================\n");
    printf(" BlinkHash Hybrid: SIMD vs SWAR Mask Equivalence Verification\n");
    printf("========================================================================\n");

    /* Test Case 1: The specific edge case reported in independent review:
     * ctrl[0] = 0x00, ctrl[1] = 0x01, rest 0xFF, searching for target 0x00.
     * The old broken SWAR produced 0x0003 instead of 0x0001! */
    printf("[Test 1/4] Edge-case from review: ctrl[0]=0x00, ctrl[1]=0x01, target 0x00...\n");
    uint8_t edge_ctrl[16];
    memset(edge_ctrl, 0xFF, 16);
    edge_ctrl[0] = 0x00;
    edge_ctrl[1] = 0x01;

    uint32_t m_simd = bhh_match_byte_test(edge_ctrl, 0x00, false);
    uint32_t m_swar = bhh_match_byte_test(edge_ctrl, 0x00, true);

    printf("  SIMD mask: 0x%04X, SWAR mask: 0x%04X\n", m_simd, m_swar);
    assert(m_simd == 0x0001);
    assert(m_swar == 0x0001);
    assert(m_simd == m_swar);
    printf("  [PASS] Exactly 0x0001 produced by both SIMD and SWAR!\n\n");

    /* Test Case 2: Control states EMPTY (0x80) and DELETED (0xFE) on all 16 positions */
    printf("[Test 2/4] Testing BHH_CTRL_EMPTY (0x80) and BHH_CTRL_DELETED (0xFE) across all 16 positions...\n");
    for (int pos = 0; pos < 16; pos++) {
        uint8_t c[16];
        memset(c, 0x00, 16);
        c[pos] = BHH_CTRL_EMPTY;
        uint32_t s_empty = bhh_match_byte_test(c, BHH_CTRL_EMPTY, false);
        uint32_t w_empty = bhh_match_byte_test(c, BHH_CTRL_EMPTY, true);
        assert(s_empty == (1u << pos));
        assert(w_empty == (1u << pos));
        assert(s_empty == w_empty);

        c[pos] = BHH_CTRL_DELETED;
        uint32_t s_del = bhh_match_byte_test(c, BHH_CTRL_DELETED, false);
        uint32_t w_del = bhh_match_byte_test(c, BHH_CTRL_DELETED, true);
        assert(s_del == (1u << pos));
        assert(w_del == (1u << pos));
        assert(s_del == w_del);
    }
    printf("  [PASS] All 16 positions verified for control states!\n\n");

    /* Test Case 3: All 128 7-bit fingerprints with adjacent byte permutations */
    printf("[Test 3/4] Testing all 128 fingerprint values with tricky neighbor bytes...\n");
    for (uint32_t fp = 0; fp < 0x80; fp++) {
        for (int pos = 0; pos < 16; pos++) {
            uint8_t c[16];
            memset(c, 0xFF, 16);
            c[pos] = (uint8_t)fp;
            if (pos > 0) c[pos - 1] = (uint8_t)(fp ^ 0x01); /* Potentially borrowing neighbor */
            if (pos < 15) c[pos + 1] = (uint8_t)(fp + 1);

            uint32_t s_mask = bhh_match_byte_test(c, (uint8_t)fp, false);
            uint32_t w_mask = bhh_match_byte_test(c, (uint8_t)fp, true);
            assert(s_mask == (1u << pos));
            assert(w_mask == (1u << pos));
            assert(s_mask == w_mask);
        }
    }
    printf("  [PASS] All 128 fingerprints verified!\n\n");

    /* Test Case 4: 100,000 randomized 16-byte arrays against random targets */
    printf("[Test 4/4] Testing 100,000 randomized 16-byte groups against random targets...\n");
    for (int trial = 0; trial < 100000; trial++) {
        uint8_t c[16];
        uint64_t r1 = next_rnd();
        uint64_t r2 = next_rnd();
        memcpy(c, &r1, 8);
        memcpy(c + 8, &r2, 8);

        uint8_t target = (uint8_t)(next_rnd() & 0xFF);

        uint32_t s_mask = bhh_match_byte_test(c, target, false);
        uint32_t w_mask = bhh_match_byte_test(c, target, true);

        if (s_mask != w_mask) {
            fprintf(stderr, "Mismatch at trial %d! target=0x%02X, SIMD=0x%04X, SWAR=0x%04X\n",
                    trial, target, s_mask, w_mask);
            assert(s_mask == w_mask);
        }
    }
    printf("  [PASS] 100,000 randomized trials matched 100%% identically!\n\n");

    printf("========================================================================\n");
    printf(" [ALL PASS] SIMD and SWAR produce 100%% identical 16-bit masks!\n");
    printf("========================================================================\n");
    return 0;
}
