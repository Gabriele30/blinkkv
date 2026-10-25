#define _POSIX_C_SOURCE 200809L

#include "blinkhash_hybrid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

int main(void)
{
    printf("========================================================================\n");
    printf(" BlinkHash Hybrid: Handle Lifecycle & Post-Expiration SET Invalidation\n");
    printf("========================================================================\n");

    bhh_table *table = bhh_create(16);
    assert(table != NULL);

    const uint8_t key[] = "session_token_xyz";
    size_t klen = sizeof(key) - 1;

    const uint8_t val1[] = "payload_v1";
    size_t vlen1 = sizeof(val1) - 1;

    const uint8_t val2[] = "payload_v2_reincarnated";
    size_t vlen2 = sizeof(val2) - 1;

    uint64_t clock_ms = 1000ULL;
    uint64_t expire_ms = clock_ms + 100ULL; /* Expire at 1100 */

    bhh_handle h1 = BHH_HANDLE_NULL;
    int rc = bhh_set(table, key, klen, val1, vlen1, expire_ms, &h1);
    assert(rc == 0);
    assert(h1 != BHH_HANDLE_NULL);
    printf("1. Key inserted with TTL=100ms. Handle 1: generation=%u, index=%u\n",
           bhh_handle_gen(h1), bhh_handle_idx(h1));

    /* Verify handle 1 is currently valid */
    const bhh_record *rec1 = bhh_resolve_handle(table, h1);
    assert(rec1 != NULL);
    assert(rec1->key_len == klen);

    /* Advance clock to 1200ms: Key is now logically expired, but NOT physically purged yet */
    clock_ms = 1200ULL;
    const uint8_t *out_val = NULL;
    size_t out_vlen = 0;
    bhh_handle out_h = BHH_HANDLE_NULL;
    rc = bhh_get(table, key, klen, clock_ms, &out_val, &out_vlen, &out_h);
    assert(rc == 1); /* Logically expired */
    printf("2. Clock advanced to 1200ms. bhh_get confirmed key is logically EXPIRED.\n");

    /* Re-insert key with new value BEFORE purge occurs */
    bhh_handle h2 = BHH_HANDLE_NULL;
    rc = bhh_set(table, key, klen, val2, vlen2, clock_ms + 1000ULL, &h2);
    assert(rc == 0);
    assert(h2 != BHH_HANDLE_NULL);
    printf("3. SET performed on expired key (new incarnation). Handle 2: generation=%u, index=%u\n",
           bhh_handle_gen(h2), bhh_handle_idx(h2));

    /* VERIFICATION:
     * - Handle 1 MUST NOT equal Handle 2!
     * - Handle 1 MUST be completely invalid (old handle cannot access new incarnation).
     * - Handle 2 MUST be valid and point to new incarnation.
     */
    int handle_equal_after_expiration = (h1 == h2);
    const bhh_record *rec_old = bhh_resolve_handle(table, h1);
    int old_handle_still_valid = (rec_old != NULL);

    printf("   handle_equal_after_expiration = %d (expected: 0)\n", handle_equal_after_expiration);
    printf("   old_handle_still_valid        = %d (expected: 0)\n", old_handle_still_valid);

    assert(handle_equal_after_expiration == 0);
    assert(old_handle_still_valid == 0);

    /* Deleting with stale handle h1 must fail with error 1 */
    rc = bhh_del_handle(table, h1);
    assert(rc == 1);

    /* Resolving and getting with new handle h2 must succeed */
    const bhh_record *rec_new = bhh_resolve_handle(table, h2);
    assert(rec_new != NULL);
    assert(rec_new->val_len == vlen2);

    rc = bhh_get(table, key, klen, clock_ms, &out_val, &out_vlen, &out_h);
    assert(rc == 0);
    assert(out_h == h2);
    assert(out_vlen == vlen2);
    assert(memcmp(out_val, val2, vlen2) == 0);
    printf("4. New handle h2 resolves correctly. Old handle h1 is strictly rejected.\n");

    bhh_free(table);

    printf("========================================================================\n");
    printf(" [ALL PASS] Handle lifecycle & post-expiration reincarnation verified!\n");
    printf("========================================================================\n");
    return 0;
}
