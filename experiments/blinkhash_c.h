#ifndef BLINKHASH_C_H
#define BLINKHASH_C_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * Candidate C: BlinkHash-TTL (Compact Index + Intrusive Epoch Lists)
 * Designed explicitly for TTL-native workloads with massive expirations.
 *
 * Core architecture:
 * 1. Compact 8-byte Index Table with Robin Hood hashing and backward-shift deletion.
 * 2. Stable Record Arena with intrusive doubly-linked Epoch Buckets (temporal wheel).
 * 3. O(1) Expiration Bucket draining: purging all keys of an expired epoch is an
 *    O(K) traversal of only the expired keys, without scanning the whole hash table!
 * 4. O(1) TTL update: unlinking from old epoch bucket and relinking to new epoch bucket.
 */

#define BHC_EPOCH_STEP_MS 1000ULL /* 1 second per epoch */
#define BHC_MAX_EPOCHS 256        /* Number of epoch buckets */
#define BHC_HANDLE_EMPTY 0

typedef struct {
    uint32_t tag;    /* 32-bit hash fingerprint */
    uint32_t handle; /* Index into record arena (1-based, 0 = EMPTY) */
} bhc_slot;

typedef struct {
    uint64_t expires_at_ms;
    uint32_t key_len;
    uint32_t val_len;
    uint32_t epoch_ring;  /* Which epoch bucket this record belongs to */
    uint32_t prev_expire; /* Doubly linked list inside epoch bucket */
    uint32_t next_expire;
    uint32_t next_free;   /* Free-list link when record is inactive */
    uint8_t *payload;     /* packed key followed by value */
} bhc_record;

typedef struct {
    size_t index_cap;
    size_t index_mask;
    size_t size;
    bhc_slot *index;      /* Compact 8-byte index array */

    size_t arena_cap;
    size_t arena_count;
    uint32_t first_free;
    bhc_record *arena;    /* 1-based record array */

    uint32_t epoch_heads[BHC_MAX_EPOCHS]; /* Heads of epoch linked-lists */
} bhc_table;

bhc_table *bhc_create(size_t initial_index_cap);
void bhc_free(bhc_table *table);

int bhc_set(bhc_table *table, const uint8_t *key, size_t key_len,
            const uint8_t *val, size_t val_len, uint64_t expires_at_ms);

int bhc_get(const bhc_table *table, const uint8_t *key, size_t key_len,
            uint64_t now_ms, const uint8_t **val_out, size_t *val_len_out);

int bhc_exists(const bhc_table *table, const uint8_t *key, size_t key_len, uint64_t now_ms);

int bhc_del(bhc_table *table, const uint8_t *key, size_t key_len);

/**
 * Purges all keys whose expiration is <= now_ms by draining elapsed epoch buckets.
 * Returns the number of keys purged.
 */
size_t bhc_purge_expired(bhc_table *table, uint64_t now_ms);

size_t bhc_size(const bhc_table *table);
size_t bhc_memory_bytes(const bhc_table *table);

#endif /* BLINKHASH_C_H */
