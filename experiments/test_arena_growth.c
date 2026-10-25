#define _POSIX_C_SOURCE 200809L

#include "blinkhash_hybrid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* Fault injection allocator state */
static int g_fail_realloc_after = -1;
static int g_realloc_call_count = 0;
static int g_fail_malloc_after = -1;
static int g_malloc_call_count = 0;

static void *test_malloc(size_t size)
{
    if (g_fail_malloc_after >= 0 && ++g_malloc_call_count > g_fail_malloc_after) {
        return NULL;
    }
    return malloc(size);
}

static void *test_realloc(void *ptr, size_t size)
{
    if (g_fail_realloc_after >= 0 && ++g_realloc_call_count > g_fail_realloc_after) {
        return NULL;
    }
    return realloc(ptr, size);
}

static void test_free(void *ptr)
{
    free(ptr);
}

/* -------------------------------------------------------------------------
 * Test 1: Incomplete Allocator Contract Validation
 * ------------------------------------------------------------------------- */
static void test_allocator_contract(void)
{
    printf("1. Testing strict allocator contract (incomplete hooks rejected)... ");
    bhh_allocator partial;

    /* Missing free_fn */
    partial.malloc_fn = test_malloc;
    partial.realloc_fn = test_realloc;
    partial.free_fn = NULL;
    bhh_table *t1 = bhh_create_with_allocator(64, &partial);
    assert(t1 == NULL);

    /* Missing realloc_fn */
    partial.malloc_fn = test_malloc;
    partial.realloc_fn = NULL;
    partial.free_fn = test_free;
    bhh_table *t2 = bhh_create_with_allocator(64, &partial);
    assert(t2 == NULL);

    /* Missing malloc_fn */
    partial.malloc_fn = NULL;
    partial.realloc_fn = test_realloc;
    partial.free_fn = test_free;
    bhh_table *t3 = bhh_create_with_allocator(64, &partial);
    assert(t3 == NULL);

    /* Complete contract succeeds */
    partial.malloc_fn = test_malloc;
    partial.realloc_fn = test_realloc;
    partial.free_fn = test_free;
    bhh_table *t4 = bhh_create_with_allocator(64, &partial);
    assert(t4 != NULL);
    bhh_free(t4);

    puts("[PASS]");
}

/* -------------------------------------------------------------------------
 * Test 2: Repeated Dynamic Growth & Handle Stability across Reallocs
 * ------------------------------------------------------------------------- */
static void test_repeated_growth_and_handle_stability(bhh_growth_policy policy, const char *pname)
{
    printf("2. Testing repeated growth (%s) and handle stability across reallocs... ", pname);
    bhh_table *table = bhh_create(100);
    assert(table != NULL);
    bhh_set_growth_policy(table, policy);

    /* Starts with BHH_INITIAL_ARENA_CAP */
    assert(table->arena_cap == BHH_INITIAL_ARENA_CAP);

    const size_t N = 50000;
    bhh_handle *handles = malloc(N * sizeof(bhh_handle));
    assert(handles != NULL);

    uint8_t kbuf[32], vbuf[32];
    for (uint32_t i = 0; i < N; i++) {
        snprintf((char *)kbuf, sizeof(kbuf), "key_%08u", i);
        snprintf((char *)vbuf, sizeof(vbuf), "val_%08u", i);
        int rc = bhh_set(table, kbuf, strlen((char *)kbuf), vbuf, strlen((char *)vbuf), 2000000ULL, &handles[i]);
        assert(rc == 0);
        assert(handles[i] != BHH_HANDLE_NULL);
    }

    /* Verify multiple reallocations took place */
    bhh_stats st;
    bhh_get_stats(table, &st);
    assert(st.arena_realloc_count > 0);
    assert(st.arena_cap >= N);

    /* Verify that EVERY handle from the earliest (capacity 64) to the latest resolves to identical data */
    for (uint32_t i = 0; i < N; i++) {
        const bhh_record *rec = bhh_resolve_handle(table, handles[i]);
        assert(rec != NULL);

        char expected_val[32];
        snprintf(expected_val, sizeof(expected_val), "val_%08u", i);
        assert(rec->val_len == strlen(expected_val));
        assert(memcmp(rec->payload + rec->key_len, expected_val, rec->val_len) == 0);
    }

    free(handles);
    bhh_free(table);
    puts("[PASS]");
}

/* -------------------------------------------------------------------------
 * Test 3: Free-List Priority (Zero Reallocations on Key Churn)
 * ------------------------------------------------------------------------- */
static void test_freelist_priority_no_realloc(void)
{
    printf("3. Testing free-list recycling priority under churn... ");
    bhh_table *table = bhh_create(100);
    assert(table != NULL);

    /* Insert 1,000 keys */
    uint8_t kbuf[32], vbuf[32];
    for (uint32_t i = 0; i < 1000; i++) {
        snprintf((char *)kbuf, sizeof(kbuf), "churn_key_%05u", i);
        snprintf((char *)vbuf, sizeof(vbuf), "churn_val_%05u", i);
        bhh_set(table, kbuf, strlen((char *)kbuf), vbuf, strlen((char *)vbuf), 5000000ULL, NULL);
    }

    size_t reallocs_before = table->arena_realloc_count;
    size_t cap_before = table->arena_cap;

    /* Delete 500 keys, populating first_free */
    for (uint32_t i = 0; i < 500; i++) {
        snprintf((char *)kbuf, sizeof(kbuf), "churn_key_%05u", i);
        int rc = bhh_del(table, kbuf, strlen((char *)kbuf));
        assert(rc == 0);
    }
    assert(table->first_free != 0);

    /* Re-insert 500 new keys */
    for (uint32_t i = 5000; i < 5500; i++) {
        snprintf((char *)kbuf, sizeof(kbuf), "churn_key_%05u", i);
        snprintf((char *)vbuf, sizeof(vbuf), "churn_val_%05u", i);
        int rc = bhh_set(table, kbuf, strlen((char *)kbuf), vbuf, strlen((char *)vbuf), 5000000ULL, NULL);
        assert(rc == 0);
    }

    /* Zero new reallocations must have occurred! */
    assert(table->arena_realloc_count == reallocs_before);
    assert(table->arena_cap == cap_before);

    bhh_free(table);
    puts("[PASS]");
}

/* -------------------------------------------------------------------------
 * Test 4: Fault Injection during Arena Realloc (Rollback & Consistency)
 * ------------------------------------------------------------------------- */
static void test_fault_injection_arena_realloc(void)
{
    printf("4. Testing fault injection during arena realloc (clean rollback)... ");

    bhh_allocator alloc;
    alloc.malloc_fn = test_malloc;
    alloc.realloc_fn = test_realloc;
    alloc.free_fn = test_free;

    /* Reset fault state */
    g_realloc_call_count = 0;
    g_fail_realloc_after = -1;
    g_malloc_call_count = 0;
    g_fail_malloc_after = -1;

    bhh_table *table = bhh_create_with_allocator(100, &alloc);
    assert(table != NULL);

    /* Populate up to current arena capacity (64 slots) */
    uint8_t kbuf[32], vbuf[32];
    for (uint32_t i = 1; i < BHH_INITIAL_ARENA_CAP; i++) {
        snprintf((char *)kbuf, sizeof(kbuf), "safe_key_%04u", i);
        snprintf((char *)vbuf, sizeof(vbuf), "safe_val_%04u", i);
        int rc = bhh_set(table, kbuf, strlen((char *)kbuf), vbuf, strlen((char *)vbuf), 10000000ULL, NULL);
        assert(rc == 0);
    }

    size_t size_before = bhh_size(table);
    size_t arena_cap_before = table->arena_cap;
    assert(size_before == BHH_INITIAL_ARENA_CAP - 1);

    /* Arm fault injector: NEXT realloc will fail */
    g_realloc_call_count = 0;
    g_fail_realloc_after = 0;

    /* This SET must trigger arena growth and fail gracefully */
    snprintf((char *)kbuf, sizeof(kbuf), "overflow_key");
    snprintf((char *)vbuf, sizeof(vbuf), "overflow_val");
    int fail_rc = bhh_set(table, kbuf, strlen((char *)kbuf), vbuf, strlen((char *)vbuf), 10000000ULL, NULL);
    assert(fail_rc == -1);

    /* Table state must remain 100% intact and uncorrupted */
    assert(bhh_size(table) == size_before);
    assert(table->arena_cap == arena_cap_before);

    /* All previously inserted keys must still exist and be readable */
    for (uint32_t i = 1; i < BHH_INITIAL_ARENA_CAP; i++) {
        snprintf((char *)kbuf, sizeof(kbuf), "safe_key_%04u", i);
        int exists = bhh_exists(table, kbuf, strlen((char *)kbuf), 1000);
        assert(exists == 0);
    }

    /* Disarm fault injector: subsequent allocations must recover and succeed */
    g_fail_realloc_after = -1;
    snprintf((char *)kbuf, sizeof(kbuf), "overflow_key");
    snprintf((char *)vbuf, sizeof(vbuf), "overflow_val");
    int recover_rc = bhh_set(table, kbuf, strlen((char *)kbuf), vbuf, strlen((char *)vbuf), 10000000ULL, NULL);
    assert(recover_rc == 0);
    assert(bhh_size(table) == size_before + 1);
    assert(table->arena_cap > arena_cap_before);

    bhh_free(table);
    puts("[PASS]");
}

/* -------------------------------------------------------------------------
 * Test 5: Clean Rollback on Payload Malloc Failure
 * ------------------------------------------------------------------------- */
static void test_fault_injection_payload_malloc(void)
{
    printf("5. Testing fault injection on payload allocation failure... ");

    bhh_allocator alloc;
    alloc.malloc_fn = test_malloc;
    alloc.realloc_fn = test_realloc;
    alloc.free_fn = test_free;

    g_realloc_call_count = 0;
    g_fail_realloc_after = -1;
    g_malloc_call_count = 0;
    g_fail_malloc_after = -1;

    bhh_table *table = bhh_create_with_allocator(100, &alloc);
    assert(table != NULL);

    /* Insert 10 keys cleanly */
    uint8_t kbuf[32], vbuf[32];
    for (uint32_t i = 0; i < 10; i++) {
        snprintf((char *)kbuf, sizeof(kbuf), "k_%u", i);
        snprintf((char *)vbuf, sizeof(vbuf), "v_%u", i);
        bhh_set(table, kbuf, strlen((char *)kbuf), vbuf, strlen((char *)vbuf), 1000000ULL, NULL);
    }
    size_t size_before = bhh_size(table);

    /* Fail the next malloc (payload allocation inside bhh_set) */
    g_malloc_call_count = 0;
    g_fail_malloc_after = 0;

    snprintf((char *)kbuf, sizeof(kbuf), "fail_k");
    snprintf((char *)vbuf, sizeof(vbuf), "fail_v");
    int rc = bhh_set(table, kbuf, strlen((char *)kbuf), vbuf, strlen((char *)vbuf), 1000000ULL, NULL);
    assert(rc == -1);
    assert(bhh_size(table) == size_before);

    /* Arena allocated slot must have been rolled back to first_free */
    assert(table->first_free != 0);

    /* Disarm: insertion into recycled slot succeeds */
    g_fail_malloc_after = -1;
    rc = bhh_set(table, kbuf, strlen((char *)kbuf), vbuf, strlen((char *)vbuf), 1000000ULL, NULL);
    assert(rc == 0);
    assert(bhh_size(table) == size_before + 1);

    bhh_free(table);
    puts("[PASS]");
}

int main(void)
{
    printf("========================================================================\n");
    printf(" BlinkHash Hybrid: Decoupled Dynamic Arena Deep Verification Suite\n");
    printf("========================================================================\n");

    test_allocator_contract();
    test_repeated_growth_and_handle_stability(BHH_GROWTH_1_5X, "1.5x Policy");
    test_repeated_growth_and_handle_stability(BHH_GROWTH_2X, "2.0x Policy");
    test_freelist_priority_no_realloc();
    test_fault_injection_arena_realloc();
    test_fault_injection_payload_malloc();

    printf("========================================================================\n");
    printf(" [ALL PASS] All decoupled arena growth and fault tests passed cleanly!\n");
    printf("========================================================================\n");
    return 0;
}
