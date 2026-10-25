#define _POSIX_C_SOURCE 200809L

#include "store.h"
#include "test_common.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Mock clock context for deterministic time simulation */
typedef struct {
    uint64_t current_time_ms;
    int fail_next;
} mock_clock;

static int mock_clock_fn(uint64_t *now_ms_out, void *user_data)
{
    mock_clock *clk = (mock_clock *)user_data;
    if (clk == NULL || now_ms_out == NULL || clk->fail_next) {
        return -1;
    }
    *now_ms_out = clk->current_time_ms;
    return 0;
}

static void test_create_free(void)
{
    bk_store *store = bk_store_create(16);
    TEST_ASSERT(store != NULL);
    bk_store_free(store);

    /* Zero bucket count must fail safely */
    bk_store *invalid = bk_store_create(0);
    TEST_ASSERT(invalid == NULL);

    /* Freeing NULL must not crash */
    bk_store_free(NULL);
}

static void test_set_get_basic(void)
{
    bk_store *store = bk_store_create(16);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "session:42";
    const uint8_t value[] = "payload_data_xyz";
    uint8_t *got = NULL;
    size_t got_len = 0;

    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, value, sizeof(value) - 1, 5000), BK_OK);
    TEST_ASSERT_STATUS(bk_get(store, key, sizeof(key) - 1, &got, &got_len), BK_OK);
    TEST_ASSERT_EQ_SIZE(got_len, sizeof(value) - 1);
    TEST_ASSERT(got != NULL);
    TEST_ASSERT_MEM_EQ(got, value, got_len);

    free(got);
    bk_store_free(store);
}

static void test_get_missing(void)
{
    bk_store *store = bk_store_create(16);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "nonexistent_key";
    uint8_t *got = (uint8_t *)0x1;
    size_t got_len = 999;

    TEST_ASSERT_STATUS(bk_get(store, key, sizeof(key) - 1, &got, &got_len), BK_ERR_NOT_FOUND);
    TEST_ASSERT(got == NULL);
    TEST_ASSERT_EQ_SIZE(got_len, 0);

    bk_store_free(store);
}

static void test_del_existing_and_missing(void)
{
    bk_store *store = bk_store_create(16);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "item_to_delete";
    const uint8_t value[] = "temporary_val";

    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, value, sizeof(value) - 1, 5000), BK_OK);
    TEST_ASSERT_STATUS(bk_del(store, key, sizeof(key) - 1), BK_OK);

    /* Second delete must return NOT_FOUND */
    TEST_ASSERT_STATUS(bk_del(store, key, sizeof(key) - 1), BK_ERR_NOT_FOUND);

    /* Exists must return NOT_FOUND */
    TEST_ASSERT_STATUS(bk_exists(store, key, sizeof(key) - 1), BK_ERR_NOT_FOUND);

    bk_store_free(store);
}

static void test_exists_existing_and_missing(void)
{
    bk_store *store = bk_store_create(16);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "active_item";
    const uint8_t value[] = "val";

    TEST_ASSERT_STATUS(bk_exists(store, key, sizeof(key) - 1), BK_ERR_NOT_FOUND);
    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, value, sizeof(value) - 1, 5000), BK_OK);
    TEST_ASSERT_STATUS(bk_exists(store, key, sizeof(key) - 1), BK_OK);

    bk_store_free(store);
}

static void test_set_replaces_existing_value(void)
{
    bk_store *store = bk_store_create(16);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "mutate_key";
    const uint8_t first_val[] = "first";
    const uint8_t second_val[] = "second_much_longer_value_string";
    uint8_t *got = NULL;
    size_t got_len = 0;

    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, first_val, sizeof(first_val) - 1, 1000), BK_OK);
    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, second_val, sizeof(second_val) - 1, 5000), BK_OK);

    TEST_ASSERT_STATUS(bk_get(store, key, sizeof(key) - 1, &got, &got_len), BK_OK);
    TEST_ASSERT_EQ_SIZE(got_len, sizeof(second_val) - 1);
    TEST_ASSERT_MEM_EQ(got, second_val, got_len);

    free(got);
    bk_store_free(store);
}

static void test_ttl_zero_rejected(void)
{
    bk_store *store = bk_store_create(16);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "zero_ttl_key";
    const uint8_t val[] = "val";

    /* ttl_ms == 0 must be rejected with BK_ERR_INVALID_TTL */
    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, val, sizeof(val) - 1, 0), BK_ERR_INVALID_TTL);

    /* Key must not exist in store */
    TEST_ASSERT_STATUS(bk_exists(store, key, sizeof(key) - 1), BK_ERR_NOT_FOUND);

    bk_store_free(store);
}

static void test_ttl_remaining_system_clock(void)
{
    bk_store *store = bk_store_create(16);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "ttl_test_key";
    const uint8_t val[] = "val";
    uint64_t remaining_ttl = 0;

    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, val, sizeof(val) - 1, 5000), BK_OK);
    TEST_ASSERT_STATUS(bk_ttl(store, key, sizeof(key) - 1, &remaining_ttl), BK_OK);
    TEST_ASSERT(remaining_ttl > 0);
    TEST_ASSERT(remaining_ttl <= 5000);

    bk_store_free(store);
}

static void test_deterministic_ttl_mock_clock(void)
{
    mock_clock clk = { .current_time_ms = 1000000ULL, .fail_next = 0 };
    bk_store *store = bk_store_create_with_clock(16, mock_clock_fn, &clk);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "mock_ttl_key";
    const uint8_t val[] = "mock_val";
    uint8_t *got = NULL;
    size_t got_len = 0;
    uint64_t remaining_ttl = 0;

    /* Insert key with TTL 100ms at T=1,000,000 (expires at T=1,000,100) */
    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, val, sizeof(val) - 1, 100), BK_OK);

    /* At T=1,000,050: still active, remaining TTL is 50ms */
    clk.current_time_ms = 1000050ULL;
    TEST_ASSERT_STATUS(bk_ttl(store, key, sizeof(key) - 1, &remaining_ttl), BK_OK);
    TEST_ASSERT_EQ_UINT64(remaining_ttl, 50);
    TEST_ASSERT_STATUS(bk_exists(store, key, sizeof(key) - 1), BK_OK);
    TEST_ASSERT_STATUS(bk_get(store, key, sizeof(key) - 1, &got, &got_len), BK_OK);
    TEST_ASSERT_EQ_SIZE(got_len, sizeof(val) - 1);
    TEST_ASSERT_MEM_EQ(got, val, got_len);
    free(got);
    got = NULL;

    /* At T=1,000,100: EXACT expiration timestamp */
    clk.current_time_ms = 1000100ULL;
    TEST_ASSERT_STATUS(bk_ttl(store, key, sizeof(key) - 1, &remaining_ttl), BK_ERR_NOT_FOUND);
    TEST_ASSERT_EQ_UINT64(remaining_ttl, 0);
    TEST_ASSERT_STATUS(bk_exists(store, key, sizeof(key) - 1), BK_ERR_NOT_FOUND);
    TEST_ASSERT_STATUS(bk_get(store, key, sizeof(key) - 1, &got, &got_len), BK_ERR_NOT_FOUND);
    TEST_ASSERT(got == NULL);

    /* At T=1,000,500: well past expiration */
    clk.current_time_ms = 1000500ULL;
    TEST_ASSERT_STATUS(bk_ttl(store, key, sizeof(key) - 1, &remaining_ttl), BK_ERR_NOT_FOUND);
    TEST_ASSERT_STATUS(bk_exists(store, key, sizeof(key) - 1), BK_ERR_NOT_FOUND);
    TEST_ASSERT_STATUS(bk_get(store, key, sizeof(key) - 1, &got, &got_len), BK_ERR_NOT_FOUND);

    bk_store_free(store);
}

static void test_del_expired_key_mock_clock(void)
{
    mock_clock clk = { .current_time_ms = 100000ULL, .fail_next = 0 };
    bk_store *store = bk_store_create_with_clock(16, mock_clock_fn, &clk);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "expire_del_key";
    const uint8_t val[] = "val";

    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, val, sizeof(val) - 1, 50), BK_OK);

    /* Advance time past expiration */
    clk.current_time_ms = 100100ULL;

    /* Deleting an expired key should reclaim it and return BK_ERR_NOT_FOUND */
    TEST_ASSERT_STATUS(bk_del(store, key, sizeof(key) - 1), BK_ERR_NOT_FOUND);

    bk_store_free(store);
}

static void test_overwrite_expired_key_mock_clock(void)
{
    mock_clock clk = { .current_time_ms = 50000ULL, .fail_next = 0 };
    bk_store *store = bk_store_create_with_clock(16, mock_clock_fn, &clk);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "reincarnated_key";
    const uint8_t old_val[] = "old_value";
    const uint8_t new_val[] = "new_value";
    uint8_t *got = NULL;
    size_t got_len = 0;

    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, old_val, sizeof(old_val) - 1, 50), BK_OK);

    /* Let it expire */
    clk.current_time_ms = 50100ULL;
    TEST_ASSERT_STATUS(bk_exists(store, key, sizeof(key) - 1), BK_ERR_NOT_FOUND);

    /* Overwrite after expiration: must succeed as a fresh entry */
    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, new_val, sizeof(new_val) - 1, 200), BK_OK);

    clk.current_time_ms = 50150ULL;
    TEST_ASSERT_STATUS(bk_get(store, key, sizeof(key) - 1, &got, &got_len), BK_OK);
    TEST_ASSERT_EQ_SIZE(got_len, sizeof(new_val) - 1);
    TEST_ASSERT_MEM_EQ(got, new_val, got_len);
    free(got);

    bk_store_free(store);
}

static void test_binary_safe_key_value(void)
{
    bk_store *store = bk_store_create(4);
    TEST_ASSERT(store != NULL);

    const uint8_t bin_key[] = { 0x00, 'b', 'i', 'n', 0x00, 0xFF, 0x42 };
    const uint8_t bin_val[] = { 0xFF, 0xFE, 0x00, 0x00, 0x01, 'x', 0x00 };
    uint8_t *got = NULL;
    size_t got_len = 0;

    TEST_ASSERT_STATUS(bk_set(store, bin_key, sizeof(bin_key), bin_val, sizeof(bin_val), 1000), BK_OK);
    TEST_ASSERT_STATUS(bk_get(store, bin_key, sizeof(bin_key), &got, &got_len), BK_OK);
    TEST_ASSERT_EQ_SIZE(got_len, sizeof(bin_val));
    TEST_ASSERT_MEM_EQ(got, bin_val, got_len);

    free(got);
    bk_store_free(store);
}

static void test_zero_length_key_and_value(void)
{
    bk_store *store = bk_store_create(16);
    TEST_ASSERT(store != NULL);

    const uint8_t empty_key[] = "";
    const uint8_t empty_val[] = "";
    uint8_t *got = (uint8_t *)0x1234;
    size_t got_len = 88;

    /* Zero-length key and zero-length value are valid */
    TEST_ASSERT_STATUS(bk_set(store, empty_key, 0, empty_val, 0, 1000), BK_OK);
    TEST_ASSERT_STATUS(bk_exists(store, empty_key, 0), BK_OK);

    TEST_ASSERT_STATUS(bk_get(store, empty_key, 0, &got, &got_len), BK_OK);
    TEST_ASSERT(got == NULL); /* Zero-length values return NULL buffer */
    TEST_ASSERT_EQ_SIZE(got_len, 0);

    TEST_ASSERT_STATUS(bk_del(store, empty_key, 0), BK_OK);
    TEST_ASSERT_STATUS(bk_exists(store, empty_key, 0), BK_ERR_NOT_FOUND);

    bk_store_free(store);
}

static void test_hash_collision_chain(void)
{
    /* Force all keys into a single bucket (bucket_count = 1) */
    bk_store *store = bk_store_create(1);
    TEST_ASSERT(store != NULL);

    char key_buf[32];
    char val_buf[32];

    /* Insert 40 keys with collisions */
    for (int i = 0; i < 40; i++) {
        snprintf(key_buf, sizeof(key_buf), "coll_key_%d", i);
        snprintf(val_buf, sizeof(val_buf), "coll_val_%d", i);
        TEST_ASSERT_STATUS(bk_set(store, (const uint8_t *)key_buf, strlen(key_buf),
                                  (const uint8_t *)val_buf, strlen(val_buf), 10000), BK_OK);
    }

    /* Verify all 40 can be retrieved */
    for (int i = 0; i < 40; i++) {
        snprintf(key_buf, sizeof(key_buf), "coll_key_%d", i);
        snprintf(val_buf, sizeof(val_buf), "coll_val_%d", i);
        uint8_t *got = NULL;
        size_t got_len = 0;

        TEST_ASSERT_STATUS(bk_get(store, (const uint8_t *)key_buf, strlen(key_buf),
                                  &got, &got_len), BK_OK);
        TEST_ASSERT_EQ_SIZE(got_len, strlen(val_buf));
        TEST_ASSERT_MEM_EQ(got, val_buf, got_len);
        free(got);
    }

    /* Delete even-numbered keys */
    for (int i = 0; i < 40; i += 2) {
        snprintf(key_buf, sizeof(key_buf), "coll_key_%d", i);
        TEST_ASSERT_STATUS(bk_del(store, (const uint8_t *)key_buf, strlen(key_buf)), BK_OK);
    }

    /* Verify odd-numbered keys still exist, even-numbered keys return NOT_FOUND */
    for (int i = 0; i < 40; i++) {
        snprintf(key_buf, sizeof(key_buf), "coll_key_%d", i);
        if (i % 2 == 0) {
            TEST_ASSERT_STATUS(bk_exists(store, (const uint8_t *)key_buf, strlen(key_buf)),
                               BK_ERR_NOT_FOUND);
        } else {
            TEST_ASSERT_STATUS(bk_exists(store, (const uint8_t *)key_buf, strlen(key_buf)),
                               BK_OK);
        }
    }

    bk_store_free(store);
}

static void test_invalid_arguments(void)
{
    bk_store *store = bk_store_create(16);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "valid_key";
    const uint8_t val[] = "valid_val";
    uint8_t *got = NULL;
    size_t got_len = 0;
    uint64_t ttl_out = 0;

    /* NULL store */
    TEST_ASSERT_STATUS(bk_set(NULL, key, sizeof(key) - 1, val, sizeof(val) - 1, 100), BK_ERR_INVALID_ARG);
    TEST_ASSERT_STATUS(bk_get(NULL, key, sizeof(key) - 1, &got, &got_len), BK_ERR_INVALID_ARG);
    TEST_ASSERT_STATUS(bk_del(NULL, key, sizeof(key) - 1), BK_ERR_INVALID_ARG);
    TEST_ASSERT_STATUS(bk_exists(NULL, key, sizeof(key) - 1), BK_ERR_INVALID_ARG);
    TEST_ASSERT_STATUS(bk_ttl(NULL, key, sizeof(key) - 1, &ttl_out), BK_ERR_INVALID_ARG);

    /* NULL buffers with non-zero length */
    TEST_ASSERT_STATUS(bk_set(store, NULL, 5, val, sizeof(val) - 1, 100), BK_ERR_INVALID_ARG);
    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, NULL, 5, 100), BK_ERR_INVALID_ARG);
    TEST_ASSERT_STATUS(bk_get(store, NULL, 5, &got, &got_len), BK_ERR_INVALID_ARG);
    TEST_ASSERT_STATUS(bk_del(store, NULL, 5), BK_ERR_INVALID_ARG);
    TEST_ASSERT_STATUS(bk_exists(store, NULL, 5), BK_ERR_INVALID_ARG);
    TEST_ASSERT_STATUS(bk_ttl(store, NULL, 5, &ttl_out), BK_ERR_INVALID_ARG);

    /* NULL output pointers */
    TEST_ASSERT_STATUS(bk_get(store, key, sizeof(key) - 1, NULL, &got_len), BK_ERR_INVALID_ARG);
    TEST_ASSERT_STATUS(bk_get(store, key, sizeof(key) - 1, &got, NULL), BK_ERR_INVALID_ARG);
    TEST_ASSERT_STATUS(bk_ttl(store, key, sizeof(key) - 1, NULL), BK_ERR_INVALID_ARG);

    bk_store_free(store);
}

static void test_clock_failure_handling(void)
{
    mock_clock clk = { .current_time_ms = 1000, .fail_next = 1 };
    bk_store *store = bk_store_create_with_clock(16, mock_clock_fn, &clk);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "clk_key";
    const uint8_t val[] = "clk_val";
    uint8_t *got = NULL;
    size_t got_len = 0;
    uint64_t ttl_out = 0;

    /* When clock fails, operations must return BK_ERR_CLOCK */
    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, val, sizeof(val) - 1, 100), BK_ERR_CLOCK);
    TEST_ASSERT_STATUS(bk_get(store, key, sizeof(key) - 1, &got, &got_len), BK_ERR_CLOCK);
    TEST_ASSERT_STATUS(bk_del(store, key, sizeof(key) - 1), BK_ERR_CLOCK);
    TEST_ASSERT_STATUS(bk_exists(store, key, sizeof(key) - 1), BK_ERR_CLOCK);
    TEST_ASSERT_STATUS(bk_ttl(store, key, sizeof(key) - 1, &ttl_out), BK_ERR_CLOCK);

    bk_store_free(store);
}

static void test_ttl_overflow_handling(void)
{
    mock_clock clk = { .current_time_ms = UINT64_MAX - 50ULL, .fail_next = 0 };
    bk_store *store = bk_store_create_with_clock(16, mock_clock_fn, &clk);
    TEST_ASSERT(store != NULL);

    const uint8_t key[] = "overflow_key";
    const uint8_t val[] = "overflow_val";

    /* Expiration will saturate at UINT64_MAX without overflow wrap-around */
    TEST_ASSERT_STATUS(bk_set(store, key, sizeof(key) - 1, val, sizeof(val) - 1, 1000), BK_OK);
    TEST_ASSERT_STATUS(bk_exists(store, key, sizeof(key) - 1), BK_OK);

    bk_store_free(store);
}

static void test_status_strings(void)
{
    TEST_ASSERT(strcmp(bk_status_str(BK_OK), "OK") == 0);
    TEST_ASSERT(strcmp(bk_status_str(BK_ERR_NOT_FOUND), "Key not found or expired") == 0);
    TEST_ASSERT(strcmp(bk_status_str(BK_ERR_INVALID_ARG), "Invalid argument") == 0);
    TEST_ASSERT(strcmp(bk_status_str(BK_ERR_NOMEM), "Out of memory") == 0);
    TEST_ASSERT(strcmp(bk_status_str(BK_ERR_INVALID_TTL), "Invalid TTL (must be > 0)") == 0);
    TEST_ASSERT(strcmp(bk_status_str(BK_ERR_CLOCK), "Clock provider failure") == 0);

    /* Out of range status code */
    TEST_ASSERT(strcmp(bk_status_str((bk_status)999), "Unknown status") == 0);
}

int main(void)
{
    printf("\nBlinkKV Test Suite (Milestone 1 Core Hardening)\n");
    printf("==================================================\n");

    TEST_RUN(test_create_free);
    TEST_RUN(test_set_get_basic);
    TEST_RUN(test_get_missing);
    TEST_RUN(test_del_existing_and_missing);
    TEST_RUN(test_exists_existing_and_missing);
    TEST_RUN(test_set_replaces_existing_value);
    TEST_RUN(test_ttl_zero_rejected);
    TEST_RUN(test_ttl_remaining_system_clock);
    TEST_RUN(test_deterministic_ttl_mock_clock);
    TEST_RUN(test_del_expired_key_mock_clock);
    TEST_RUN(test_overwrite_expired_key_mock_clock);
    TEST_RUN(test_binary_safe_key_value);
    TEST_RUN(test_zero_length_key_and_value);
    TEST_RUN(test_hash_collision_chain);
    TEST_RUN(test_invalid_arguments);
    TEST_RUN(test_clock_failure_handling);
    TEST_RUN(test_ttl_overflow_handling);
    TEST_RUN(test_status_strings);

    TEST_REPORT();

    return g_tests_failed > 0 ? 1 : 0;
}
