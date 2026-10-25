#define _POSIX_C_SOURCE 200809L

#include "store.h"
#include "hash.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef struct bk_entry {
    uint8_t *key;
    size_t key_len;
    uint8_t *value;
    size_t value_len;
    uint64_t hash;
    uint64_t expires_at_ms;
    struct bk_entry *next;
} bk_entry;

struct bk_store {
    size_t bucket_count;
    bk_entry **buckets;
    bk_clock_fn clock_fn;
    void *clock_ctx;
};

const char *bk_status_str(bk_status status)
{
    switch (status) {
        case BK_OK:
            return "OK";
        case BK_ERR_NOT_FOUND:
            return "Key not found or expired";
        case BK_ERR_INVALID_ARG:
            return "Invalid argument";
        case BK_ERR_NOMEM:
            return "Out of memory";
        case BK_ERR_INVALID_TTL:
            return "Invalid TTL (must be > 0)";
        case BK_ERR_CLOCK:
            return "Clock provider failure";
        default:
            return "Unknown status";
    }
}

static int bk_default_clock(uint64_t *now_ms_out, void *user_data)
{
    (void)user_data;
    if (now_ms_out == NULL) {
        return -1;
    }

#if defined(_WIN32)
    *now_ms_out = (uint64_t)GetTickCount64();
    return 0;
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }

    *now_ms_out = ((uint64_t)ts.tv_sec * 1000ULL) +
                  ((uint64_t)ts.tv_nsec / 1000000ULL);
    return 0;
#else
    return -1;
#endif
}

static int bk_get_now_ms(const bk_store *store, uint64_t *now_ms_out)
{
    if (store == NULL || now_ms_out == NULL) {
        return -1;
    }

    if (store->clock_fn != NULL) {
        return store->clock_fn(now_ms_out, store->clock_ctx);
    }

    return bk_default_clock(now_ms_out, NULL);
}

static int bk_valid_buffer(const uint8_t *buffer, size_t len)
{
    return buffer != NULL || len == 0;
}

static uint64_t bk_expiration_from_ttl(uint64_t now_ms, uint64_t ttl_ms)
{
    if (UINT64_MAX - now_ms < ttl_ms) {
        return UINT64_MAX;
    }

    return now_ms + ttl_ms;
}

static int bk_entry_is_expired(const bk_entry *entry, uint64_t now_ms)
{
    return now_ms >= entry->expires_at_ms;
}

static int bk_key_equals(
    const bk_entry *entry,
    uint64_t hash,
    const uint8_t *key,
    size_t key_len
)
{
    if (entry->hash != hash || entry->key_len != key_len) {
        return 0;
    }

    return key_len == 0 || memcmp(entry->key, key, key_len) == 0;
}

static int bk_copy_buffer(const uint8_t *src, size_t len, uint8_t **dst_out)
{
    uint8_t *dst = NULL;

    if (dst_out == NULL || !bk_valid_buffer(src, len)) {
        return -1;
    }

    if (len > 0) {
        dst = malloc(len);
        if (dst == NULL) {
            return -1;
        }

        memcpy(dst, src, len);
    }

    *dst_out = dst;
    return 0;
}

static void bk_entry_free(bk_entry *entry)
{
    if (entry == NULL) {
        return;
    }

    free(entry->key);
    free(entry->value);
    free(entry);
}

static bk_entry **bk_find_entry_link(
    bk_store *store,
    size_t bucket_index,
    uint64_t hash,
    const uint8_t *key,
    size_t key_len
)
{
    bk_entry **link = &store->buckets[bucket_index];

    while (*link != NULL) {
        if (bk_key_equals(*link, hash, key, key_len)) {
            return link;
        }

        link = &(*link)->next;
    }

    return NULL;
}

static void bk_remove_entry_link(bk_entry **link)
{
    bk_entry *entry = *link;

    *link = entry->next;
    bk_entry_free(entry);
}

bk_store *bk_store_create_with_clock(
    size_t bucket_count,
    bk_clock_fn clock_fn,
    void *clock_ctx
)
{
    bk_store *store = NULL;

    if (bucket_count == 0) {
        return NULL;
    }

    store = calloc(1, sizeof(*store));
    if (store == NULL) {
        return NULL;
    }

    store->buckets = calloc(bucket_count, sizeof(*store->buckets));
    if (store->buckets == NULL) {
        free(store);
        return NULL;
    }

    store->bucket_count = bucket_count;
    store->clock_fn = clock_fn;
    store->clock_ctx = clock_ctx;
    return store;
}

bk_store *bk_store_create(size_t bucket_count)
{
    return bk_store_create_with_clock(bucket_count, NULL, NULL);
}

void bk_store_free(bk_store *store)
{
    if (store == NULL) {
        return;
    }

    for (size_t i = 0; i < store->bucket_count; i++) {
        bk_entry *entry = store->buckets[i];

        while (entry != NULL) {
            bk_entry *next = entry->next;
            bk_entry_free(entry);
            entry = next;
        }
    }

    free(store->buckets);
    free(store);
}

bk_status bk_set(
    bk_store *store,
    const uint8_t *key,
    size_t key_len,
    const uint8_t *value,
    size_t value_len,
    uint64_t ttl_ms
)
{
    uint64_t now_ms = 0;
    uint64_t expires_at_ms = 0;
    bk_entry **link = NULL;
    uint8_t *value_copy = NULL;

    if (store == NULL ||
        !bk_valid_buffer(key, key_len) ||
        !bk_valid_buffer(value, value_len)) {
        return BK_ERR_INVALID_ARG;
    }

    if (ttl_ms == 0) {
        return BK_ERR_INVALID_TTL;
    }

    if (bk_get_now_ms(store, &now_ms) != 0) {
        return BK_ERR_CLOCK;
    }

    expires_at_ms = bk_expiration_from_ttl(now_ms, ttl_ms);
    uint64_t hash = bk_hash_fnv1a(key, key_len);
    size_t bucket_index = hash % store->bucket_count;

    link = bk_find_entry_link(store, bucket_index, hash, key, key_len);

    if (link != NULL && bk_entry_is_expired(*link, now_ms)) {
        bk_remove_entry_link(link);
        link = NULL;
    }

    if (link != NULL) {
        if (bk_copy_buffer(value, value_len, &value_copy) != 0) {
            return BK_ERR_NOMEM;
        }

        free((*link)->value);
        (*link)->value = value_copy;
        (*link)->value_len = value_len;
        (*link)->expires_at_ms = expires_at_ms;
        return BK_OK;
    }

    bk_entry *entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return BK_ERR_NOMEM;
    }

    if (bk_copy_buffer(key, key_len, &entry->key) != 0 ||
        bk_copy_buffer(value, value_len, &entry->value) != 0) {
        bk_entry_free(entry);
        return BK_ERR_NOMEM;
    }

    entry->key_len = key_len;
    entry->value_len = value_len;
    entry->hash = hash;
    entry->expires_at_ms = expires_at_ms;

    entry->next = store->buckets[bucket_index];
    store->buckets[bucket_index] = entry;

    return BK_OK;
}

bk_status bk_get(
    bk_store *store,
    const uint8_t *key,
    size_t key_len,
    uint8_t **value_out,
    size_t *value_len_out
)
{
    uint64_t now_ms = 0;
    bk_entry **link = NULL;
    uint8_t *value_copy = NULL;

    if (store == NULL ||
        value_out == NULL ||
        value_len_out == NULL ||
        !bk_valid_buffer(key, key_len)) {
        return BK_ERR_INVALID_ARG;
    }

    *value_out = NULL;
    *value_len_out = 0;

    if (bk_get_now_ms(store, &now_ms) != 0) {
        return BK_ERR_CLOCK;
    }

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    size_t bucket_index = hash % store->bucket_count;

    link = bk_find_entry_link(store, bucket_index, hash, key, key_len);
    if (link == NULL) {
        return BK_ERR_NOT_FOUND;
    }

    if (bk_entry_is_expired(*link, now_ms)) {
        bk_remove_entry_link(link);
        return BK_ERR_NOT_FOUND;
    }

    if (bk_copy_buffer((*link)->value, (*link)->value_len, &value_copy) != 0) {
        return BK_ERR_NOMEM;
    }

    *value_out = value_copy;
    *value_len_out = (*link)->value_len;
    return BK_OK;
}

bk_status bk_del(
    bk_store *store,
    const uint8_t *key,
    size_t key_len
)
{
    uint64_t now_ms = 0;
    bk_entry **link = NULL;

    if (store == NULL || !bk_valid_buffer(key, key_len)) {
        return BK_ERR_INVALID_ARG;
    }

    if (bk_get_now_ms(store, &now_ms) != 0) {
        return BK_ERR_CLOCK;
    }

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    size_t bucket_index = hash % store->bucket_count;

    link = bk_find_entry_link(store, bucket_index, hash, key, key_len);
    if (link == NULL) {
        return BK_ERR_NOT_FOUND;
    }

    if (bk_entry_is_expired(*link, now_ms)) {
        bk_remove_entry_link(link);
        return BK_ERR_NOT_FOUND;
    }

    bk_remove_entry_link(link);
    return BK_OK;
}

bk_status bk_exists(
    bk_store *store,
    const uint8_t *key,
    size_t key_len
)
{
    uint64_t now_ms = 0;
    bk_entry **link = NULL;

    if (store == NULL || !bk_valid_buffer(key, key_len)) {
        return BK_ERR_INVALID_ARG;
    }

    if (bk_get_now_ms(store, &now_ms) != 0) {
        return BK_ERR_CLOCK;
    }

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    size_t bucket_index = hash % store->bucket_count;

    link = bk_find_entry_link(store, bucket_index, hash, key, key_len);
    if (link == NULL) {
        return BK_ERR_NOT_FOUND;
    }

    if (bk_entry_is_expired(*link, now_ms)) {
        bk_remove_entry_link(link);
        return BK_ERR_NOT_FOUND;
    }

    return BK_OK;
}

bk_status bk_ttl(
    bk_store *store,
    const uint8_t *key,
    size_t key_len,
    uint64_t *ttl_ms_out
)
{
    uint64_t now_ms = 0;
    bk_entry **link = NULL;

    if (store == NULL ||
        ttl_ms_out == NULL ||
        !bk_valid_buffer(key, key_len)) {
        return BK_ERR_INVALID_ARG;
    }

    *ttl_ms_out = 0;

    if (bk_get_now_ms(store, &now_ms) != 0) {
        return BK_ERR_CLOCK;
    }

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    size_t bucket_index = hash % store->bucket_count;

    link = bk_find_entry_link(store, bucket_index, hash, key, key_len);
    if (link == NULL) {
        return BK_ERR_NOT_FOUND;
    }

    if (bk_entry_is_expired(*link, now_ms)) {
        bk_remove_entry_link(link);
        return BK_ERR_NOT_FOUND;
    }

    *ttl_ms_out = (*link)->expires_at_ms - now_ms;
    return BK_OK;
}
