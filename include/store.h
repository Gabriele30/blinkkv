#ifndef BLINKKV_STORE_H
#define BLINKKV_STORE_H

#include <stddef.h>
#include <stdint.h>

typedef struct bk_store bk_store;

bk_store *bk_store_create(size_t bucket_count);
void bk_store_free(bk_store *store);

int bk_set(
    bk_store *store,
    const uint8_t *key,
    size_t key_len,
    const uint8_t *value,
    size_t value_len,
    uint64_t ttl_ms
);

/*
 * On success, value_out receives a newly allocated buffer owned by the caller.
 * The caller must free it with free(). For zero-length values, value_out is NULL.
 */
int bk_get(
    bk_store *store,
    const uint8_t *key,
    size_t key_len,
    uint8_t **value_out,
    size_t *value_len_out
);

int bk_del(
    bk_store *store,
    const uint8_t *key,
    size_t key_len
);

int bk_exists(
    bk_store *store,
    const uint8_t *key,
    size_t key_len
);

int bk_ttl(
    bk_store *store,
    const uint8_t *key,
    size_t key_len,
    uint64_t *ttl_ms_out
);

#endif
