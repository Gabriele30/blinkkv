#ifndef BLINKHASH_B_H
#define BLINKHASH_B_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * Candidate B: BlinkHash-Handle
 * Two-level architecture:
 * 1. Compact 8-byte Index Table with Robin Hood hashing and Tombstone-Free Backward-Shift Deletion.
 * 2. Generational Record Arena with stable 32-bit handles and free-list recycling.
 */

#define BHB_HANDLE_EMPTY 0

typedef struct {
    uint32_t tag;    /* 32-bit hash fingerprint */
    uint32_t handle; /* Index into record arena (1-based, 0 = EMPTY) */
} bhb_slot;

typedef struct {
    uint64_t expires_at_ms;
    uint32_t key_len;
    uint32_t val_len;
    uint32_t generation; /* To validate handle freshness */
    uint32_t next_free;  /* Free list pointer when inactive */
    uint8_t *payload;    /* Single allocation: key followed by value */
} bhb_record;

typedef struct {
    size_t index_cap;        /* Index table capacity (power of 2) */
    size_t index_mask;       /* index_cap - 1 */
    size_t size;             /* Number of active entries */
    bhb_slot *index;         /* Array of 8-byte index slots */

    size_t arena_cap;        /* Record arena capacity */
    size_t arena_count;      /* Total records allocated */
    uint32_t first_free;     /* Head of free-list (0 = none) */
    bhb_record *arena;       /* 1-based array of records (index 0 unused) */
} bhb_table;

bhb_table *bhb_create(size_t initial_capacity);
void bhb_free(bhb_table *table);

int bhb_set(bhb_table *table, const uint8_t *key, size_t key_len,
            const uint8_t *val, size_t val_len, uint64_t expires_at_ms,
            uint32_t *handle_out);

int bhb_get(const bhb_table *table, const uint8_t *key, size_t key_len,
            uint64_t now_ms, const uint8_t **val_out, size_t *val_len_out,
            uint32_t *handle_out);

int bhb_exists(const bhb_table *table, const uint8_t *key, size_t key_len, uint64_t now_ms);

int bhb_del(bhb_table *table, const uint8_t *key, size_t key_len);

int bhb_del_handle(bhb_table *table, uint32_t handle);

/* Direct record lookup by stable handle */
const bhb_record *bhb_get_by_handle(const bhb_table *table, uint32_t handle);

size_t bhb_size(const bhb_table *table);
size_t bhb_memory_bytes(const bhb_table *table);

#endif /* BLINKHASH_B_H */
