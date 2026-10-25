#ifndef BLINKKV_STORE_H
#define BLINKKV_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Status and error codes returned by BlinkKV operations.
 */
typedef enum bk_status {
    BK_OK = 0,               /**< Operation completed successfully */
    BK_ERR_NOT_FOUND = 1,    /**< Key not found or key has expired */
    BK_ERR_INVALID_ARG = -1, /**< Invalid argument (e.g. NULL pointer, invalid length) */
    BK_ERR_NOMEM = -2,       /**< Memory allocation failure */
    BK_ERR_INVALID_TTL = -3, /**< Invalid TTL (e.g. ttl_ms == 0) */
    BK_ERR_CLOCK = -4        /**< Monotonic clock provider failure */
} bk_status;

/**
 * Returns a static human-readable description for a given bk_status code.
 */
const char *bk_status_str(bk_status status);

/**
 * Clock function pointer type for monotonic time generation.
 * Must store the current monotonic time in milliseconds in *now_ms_out and return 0 on success.
 * If time retrieval fails, it should return non-zero.
 */
typedef int (*bk_clock_fn)(uint64_t *now_ms_out, void *user_data);

typedef struct bk_store bk_store;

/**
 * Creates a new BlinkKV store instance using the default monotonic system clock.
 * bucket_count must be > 0.
 * Returns NULL on failure (bucket_count == 0 or out of memory).
 */
bk_store *bk_store_create(size_t bucket_count);

/**
 * Creates a new BlinkKV store instance with a custom clock provider.
 * If clock_fn is NULL, the default monotonic system clock is used.
 * bucket_count must be > 0.
 * Returns NULL on failure.
 */
bk_store *bk_store_create_with_clock(size_t bucket_count, bk_clock_fn clock_fn, void *clock_ctx);

/**
 * Frees all resources associated with the store, including all allocated keys and values.
 * Safe to call with NULL.
 */
void bk_store_free(bk_store *store);

/**
 * Sets a key-value pair with a time-to-live in milliseconds.
 *
 * Requirements:
 * - store must not be NULL.
 * - key must be valid (non-NULL or key_len == 0).
 * - value must be valid (non-NULL or value_len == 0).
 * - ttl_ms must be > 0. Passing ttl_ms == 0 returns BK_ERR_INVALID_TTL.
 *
 * If the key already exists (and has not expired), its value and expiration are replaced.
 * If the key exists but is expired, it is treated as a new insertion.
 *
 * Returns:
 * - BK_OK on success.
 * - BK_ERR_INVALID_ARG if parameters are invalid.
 * - BK_ERR_INVALID_TTL if ttl_ms == 0.
 * - BK_ERR_NOMEM if allocation fails.
 * - BK_ERR_CLOCK if the monotonic clock query fails.
 */
bk_status bk_set(
    bk_store *store,
    const uint8_t *key,
    size_t key_len,
    const uint8_t *value,
    size_t value_len,
    uint64_t ttl_ms
);

/**
 * Retrieves the value associated with a key.
 *
 * If the key is found and not expired:
 * - *value_out receives a newly allocated buffer owned by the caller (must be freed with free()).
 *   For zero-length values, *value_out is NULL.
 * - *value_len_out receives the length of the value in bytes.
 * - Returns BK_OK.
 *
 * If the key does not exist or has expired:
 * - *value_out is set to NULL.
 * - *value_len_out is set to 0.
 * - Expired entries are reclaimed from the store.
 * - Returns BK_ERR_NOT_FOUND.
 *
 * Returns:
 * - BK_OK if found and not expired.
 * - BK_ERR_NOT_FOUND if missing or expired.
 * - BK_ERR_INVALID_ARG if arguments are invalid.
 * - BK_ERR_NOMEM if allocating value_out fails.
 * - BK_ERR_CLOCK if the monotonic clock query fails.
 */
bk_status bk_get(
    bk_store *store,
    const uint8_t *key,
    size_t key_len,
    uint8_t **value_out,
    size_t *value_len_out
);

/**
 * Deletes a key from the store.
 *
 * If the key is present and not expired, it is deleted and BK_OK is returned.
 * If the key is present but expired, it is reclaimed and BK_ERR_NOT_FOUND is returned.
 * If the key is missing, BK_ERR_NOT_FOUND is returned.
 *
 * Returns:
 * - BK_OK if the key was found and deleted.
 * - BK_ERR_NOT_FOUND if the key does not exist or was expired.
 * - BK_ERR_INVALID_ARG if arguments are invalid.
 * - BK_ERR_CLOCK if the monotonic clock query fails.
 */
bk_status bk_del(
    bk_store *store,
    const uint8_t *key,
    size_t key_len
);

/**
 * Checks if a key exists and is not expired.
 *
 * If the key exists but has expired, it is reclaimed and BK_ERR_NOT_FOUND is returned.
 *
 * Returns:
 * - BK_OK if the key exists and is active.
 * - BK_ERR_NOT_FOUND if the key does not exist or has expired.
 * - BK_ERR_INVALID_ARG if arguments are invalid.
 * - BK_ERR_CLOCK if the monotonic clock query fails.
 */
bk_status bk_exists(
    bk_store *store,
    const uint8_t *key,
    size_t key_len
);

/**
 * Retrieves the remaining TTL in milliseconds for a key.
 *
 * If the key exists and has not expired:
 * - *ttl_ms_out receives the remaining TTL in milliseconds.
 * - Returns BK_OK.
 *
 * If the key does not exist or has expired:
 * - *ttl_ms_out is set to 0.
 * - Expired entries are reclaimed.
 * - Returns BK_ERR_NOT_FOUND.
 *
 * Returns:
 * - BK_OK on success.
 * - BK_ERR_NOT_FOUND if the key does not exist or has expired.
 * - BK_ERR_INVALID_ARG if arguments are invalid.
 * - BK_ERR_CLOCK if the monotonic clock query fails.
 */
bk_status bk_ttl(
    bk_store *store,
    const uint8_t *key,
    size_t key_len,
    uint64_t *ttl_ms_out
);

#ifdef __cplusplus
}
#endif

#endif /* BLINKKV_STORE_H */
