#define _POSIX_C_SOURCE 200809L

#include "store.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void sleep_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;

    while (nanosleep(&ts, &ts) != 0) {
    }
}

static void test_create_free(void)
{
    bk_store *store = bk_store_create(16);

    assert(store != NULL);
    bk_store_free(store);
    bk_store_free(NULL);
}

static void test_set_get(void)
{
    bk_store *store = bk_store_create(16);
    const uint8_t key[] = "key";
    const uint8_t value[] = "value";
    uint8_t *got = NULL;
    size_t got_len = 0;

    assert(store != NULL);
    assert(bk_set(store, key, sizeof(key) - 1, value, sizeof(value) - 1, 1000) == 0);
    assert(bk_get(store, key, sizeof(key) - 1, &got, &got_len) == 0);
    assert(got_len == sizeof(value) - 1);
    assert(memcmp(got, value, got_len) == 0);

    free(got);
    bk_store_free(store);
}

static void test_get_missing(void)
{
    bk_store *store = bk_store_create(16);
    const uint8_t key[] = "missing";
    uint8_t *got = (uint8_t *)1;
    size_t got_len = 42;

    assert(store != NULL);
    assert(bk_get(store, key, sizeof(key) - 1, &got, &got_len) == 1);
    assert(got == NULL);
    assert(got_len == 0);

    bk_store_free(store);
}

static void test_del_existing(void)
{
    bk_store *store = bk_store_create(16);
    const uint8_t key[] = "key";
    const uint8_t value[] = "value";

    assert(store != NULL);
    assert(bk_set(store, key, sizeof(key) - 1, value, sizeof(value) - 1, 1000) == 0);
    assert(bk_del(store, key, sizeof(key) - 1) == 0);
    assert(bk_exists(store, key, sizeof(key) - 1) == 1);

    bk_store_free(store);
}

static void test_del_missing(void)
{
    bk_store *store = bk_store_create(16);
    const uint8_t key[] = "missing";

    assert(store != NULL);
    assert(bk_del(store, key, sizeof(key) - 1) == 1);

    bk_store_free(store);
}

static void test_exists_existing(void)
{
    bk_store *store = bk_store_create(16);
    const uint8_t key[] = "key";
    const uint8_t value[] = "value";

    assert(store != NULL);
    assert(bk_set(store, key, sizeof(key) - 1, value, sizeof(value) - 1, 1000) == 0);
    assert(bk_exists(store, key, sizeof(key) - 1) == 0);

    bk_store_free(store);
}

static void test_exists_missing(void)
{
    bk_store *store = bk_store_create(16);
    const uint8_t key[] = "missing";

    assert(store != NULL);
    assert(bk_exists(store, key, sizeof(key) - 1) == 1);

    bk_store_free(store);
}

static void test_set_replaces_existing_value(void)
{
    bk_store *store = bk_store_create(16);
    const uint8_t key[] = "key";
    const uint8_t first[] = "first";
    const uint8_t second[] = "second";
    uint8_t *got = NULL;
    size_t got_len = 0;
    uint64_t ttl_ms = 0;

    assert(store != NULL);
    assert(bk_set(store, key, sizeof(key) - 1, first, sizeof(first) - 1, 1) == 0);
    assert(bk_set(store, key, sizeof(key) - 1, second, sizeof(second) - 1, 5000) == 0);
    assert(bk_get(store, key, sizeof(key) - 1, &got, &got_len) == 0);
    assert(got_len == sizeof(second) - 1);
    assert(memcmp(got, second, got_len) == 0);
    assert(bk_ttl(store, key, sizeof(key) - 1, &ttl_ms) == 0);
    assert(ttl_ms > 1000);
    assert(ttl_ms <= 5000);

    free(got);
    bk_store_free(store);
}

static void test_ttl_remaining(void)
{
    bk_store *store = bk_store_create(16);
    const uint8_t key[] = "key";
    const uint8_t value[] = "value";
    uint64_t ttl_ms = 0;

    assert(store != NULL);
    assert(bk_set(store, key, sizeof(key) - 1, value, sizeof(value) - 1, 5000) == 0);
    assert(bk_ttl(store, key, sizeof(key) - 1, &ttl_ms) == 0);
    assert(ttl_ms > 0);
    assert(ttl_ms <= 5000);

    bk_store_free(store);
}

static void test_key_expiration_after_ttl(void)
{
    bk_store *store = bk_store_create(16);
    const uint8_t key[] = "key";
    const uint8_t value[] = "value";
    uint64_t ttl_ms = 1;

    assert(store != NULL);
    assert(bk_set(store, key, sizeof(key) - 1, value, sizeof(value) - 1, 20) == 0);
    sleep_ms(60);
    assert(bk_ttl(store, key, sizeof(key) - 1, &ttl_ms) == 1);

    bk_store_free(store);
}

static void test_expired_key_not_returned_by_get(void)
{
    bk_store *store = bk_store_create(16);
    const uint8_t key[] = "key";
    const uint8_t value[] = "value";
    uint8_t *got = NULL;
    size_t got_len = 0;

    assert(store != NULL);
    assert(bk_set(store, key, sizeof(key) - 1, value, sizeof(value) - 1, 20) == 0);
    sleep_ms(60);
    assert(bk_get(store, key, sizeof(key) - 1, &got, &got_len) == 1);
    assert(got == NULL);
    assert(got_len == 0);

    bk_store_free(store);
}

static void test_expired_key_not_returned_by_exists(void)
{
    bk_store *store = bk_store_create(16);
    const uint8_t key[] = "key";
    const uint8_t value[] = "value";

    assert(store != NULL);
    assert(bk_set(store, key, sizeof(key) - 1, value, sizeof(value) - 1, 20) == 0);
    sleep_ms(60);
    assert(bk_exists(store, key, sizeof(key) - 1) == 1);

    bk_store_free(store);
}

static void test_binary_safe_key_value(void)
{
    bk_store *store = bk_store_create(4);
    const uint8_t key[] = {0x00, 'k', 0xff, 0x00};
    const uint8_t value[] = {0x00, 'v', 0x00, 'x', 0xff};
    uint8_t *got = NULL;
    size_t got_len = 0;

    assert(store != NULL);
    assert(bk_set(store, key, sizeof(key), value, sizeof(value), 1000) == 0);
    assert(bk_get(store, key, sizeof(key), &got, &got_len) == 0);
    assert(got_len == sizeof(value));
    assert(memcmp(got, value, got_len) == 0);

    free(got);
    bk_store_free(store);
}

int main(void)
{
    test_create_free();
    test_set_get();
    test_get_missing();
    test_del_existing();
    test_del_missing();
    test_exists_existing();
    test_exists_missing();
    test_set_replaces_existing_value();
    test_ttl_remaining();
    test_key_expiration_after_ttl();
    test_expired_key_not_returned_by_get();
    test_expired_key_not_returned_by_exists();
    test_binary_safe_key_value();

    return 0;
}
