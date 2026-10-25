#ifndef BLINKHASH_A_H
#define BLINKHASH_A_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * Candidate A: BlinkHash-Flat
 * Open-addressing table with 16-slot groups, 7-bit SIMD fingerprints (SwissTable-style),
 * and inlined metadata.
 */

#define BHA_GROUP_WIDTH 16
#define BHA_CTRL_EMPTY    0x80
#define BHA_CTRL_DELETED  0xFE

typedef struct {
    uint64_t hash;
    uint64_t expires_at_ms;
    uint32_t key_len;
    uint32_t val_len;
    uint8_t *payload; /* packed key followed by value */
} bha_slot;

typedef struct {
    uint8_t ctrl[BHA_GROUP_WIDTH];
} bha_group;

typedef struct {
    size_t capacity;     /* total slot capacity (multiple of 16) */
    size_t group_count;  /* capacity / 16 */
    size_t size;         /* active item count */
    size_t tombstones;   /* deleted slot count */
    bha_group *groups;   /* array of 16-byte metadata control blocks */
    bha_slot *slots;     /* array of slots */
} bha_table;

bha_table *bha_create(size_t initial_capacity);
void bha_free(bha_table *table);

int bha_set(bha_table *table, const uint8_t *key, size_t key_len,
            const uint8_t *val, size_t val_len, uint64_t expires_at_ms);

int bha_get(const bha_table *table, const uint8_t *key, size_t key_len,
            uint64_t now_ms, const uint8_t **val_out, size_t *val_len_out);

int bha_exists(const bha_table *table, const uint8_t *key, size_t key_len, uint64_t now_ms);

int bha_del(bha_table *table, const uint8_t *key, size_t key_len);

size_t bha_size(const bha_table *table);
size_t bha_memory_bytes(const bha_table *table);

#endif /* BLINKHASH_A_H */
