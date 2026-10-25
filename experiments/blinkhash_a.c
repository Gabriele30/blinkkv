#define _POSIX_C_SOURCE 200809L

#include "blinkhash_a.h"
#include "hash.h"

#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define BHA_USE_SSE2 1
#else
#define BHA_USE_SSE2 0
#endif

static inline uint8_t bha_h2(uint64_t hash)
{
    /* Use highest 7 bits as fingerprint (0x00..0x7F) */
    return (uint8_t)((hash >> 57) & 0x7F);
}

static inline uint32_t bha_match_byte(const bha_group *group, uint8_t byte)
{
#if BHA_USE_SSE2
    __m128i ctrl = _mm_loadu_si128((const __m128i *)group->ctrl);
    __m128i target = _mm_set1_epi8((char)byte);
    __m128i cmp = _mm_cmpeq_epi8(ctrl, target);
    return (uint32_t)_mm_movemask_epi8(cmp);
#else
    uint32_t mask = 0;
    for (int i = 0; i < BHA_GROUP_WIDTH; i++) {
        if (group->ctrl[i] == byte) {
            mask |= (1u << i);
        }
    }
    return mask;
#endif
}

static inline uint32_t bha_match_empty(const bha_group *group)
{
    return bha_match_byte(group, BHA_CTRL_EMPTY);
}

static inline uint32_t bha_match_deleted(const bha_group *group)
{
    return bha_match_byte(group, BHA_CTRL_DELETED);
}

static inline uint32_t bha_match_fingerprint(const bha_group *group, uint8_t fp)
{
    return bha_match_byte(group, fp);
}

static size_t next_power_of_two(size_t v)
{
    if (v < 16) return 16;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;
    return v;
}

bha_table *bha_create(size_t initial_capacity)
{
    size_t cap = next_power_of_two(initial_capacity);
    if (cap < 16) cap = 16;

    bha_table *table = calloc(1, sizeof(*table));
    if (!table) return NULL;

    table->capacity = cap;
    table->group_count = cap / BHA_GROUP_WIDTH;
    table->size = 0;
    table->tombstones = 0;

    table->groups = malloc(table->group_count * sizeof(bha_group));
    table->slots = calloc(table->capacity, sizeof(bha_slot));
    if (!table->groups || !table->slots) {
        free(table->groups);
        free(table->slots);
        free(table);
        return NULL;
    }

    for (size_t g = 0; g < table->group_count; g++) {
        memset(table->groups[g].ctrl, BHA_CTRL_EMPTY, BHA_GROUP_WIDTH);
    }

    return table;
}

void bha_free(bha_table *table)
{
    if (!table) return;

    for (size_t i = 0; i < table->capacity; i++) {
        size_t g = i / BHA_GROUP_WIDTH;
        size_t s = i % BHA_GROUP_WIDTH;
        if (table->groups[g].ctrl[s] != BHA_CTRL_EMPTY &&
            table->groups[g].ctrl[s] != BHA_CTRL_DELETED) {
            free(table->slots[i].payload);
        }
    }

    free(table->groups);
    free(table->slots);
    free(table);
}

static int bha_resize(bha_table *table, size_t new_cap);

int bha_set(bha_table *table, const uint8_t *key, size_t key_len,
            const uint8_t *val, size_t val_len, uint64_t expires_at_ms)
{
    if (!table || !key) return -1;

    /* Resize if load factor + tombstones >= 75% */
    if ((table->size + table->tombstones + 1) * 4 >= table->capacity * 3) {
        size_t target_cap = table->capacity * 2;
        /* If many tombstones, rehash in place without doubling */
        if (table->tombstones > table->size) {
            target_cap = table->capacity;
        }
        if (bha_resize(table, target_cap) != 0) {
            return -1;
        }
    }

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    uint8_t fp = bha_h2(hash);
    size_t group_mask = table->group_count - 1;
    size_t group_idx = (size_t)hash & group_mask;
    size_t step = 0;

    int64_t first_deleted_slot = -1;

    for (;;) {
        const bha_group *group = &table->groups[group_idx];

        /* Check for existing key match */
        uint32_t fp_mask = bha_match_fingerprint(group, fp);
        while (fp_mask != 0) {
            int bit = __builtin_ctz(fp_mask);
            fp_mask &= fp_mask - 1;
            size_t slot_idx = group_idx * BHA_GROUP_WIDTH + (size_t)bit;
            bha_slot *slot = &table->slots[slot_idx];
            if (slot->hash == hash && slot->key_len == key_len) {
                if (key_len == 0 || memcmp(slot->payload, key, key_len) == 0) {
                    /* Key match: update in place */
                    uint8_t *new_payload = malloc(key_len + val_len);
                    if (!new_payload) return -1;
                    if (key_len > 0) memcpy(new_payload, key, key_len);
                    if (val_len > 0) memcpy(new_payload + key_len, val, val_len);

                    free(slot->payload);
                    slot->payload = new_payload;
                    slot->val_len = (uint32_t)val_len;
                    slot->expires_at_ms = expires_at_ms;
                    return 0;
                }
            }
        }

        /* Record first tombstone for reuse */
        if (first_deleted_slot < 0) {
            uint32_t del_mask = bha_match_deleted(group);
            if (del_mask != 0) {
                int bit = __builtin_ctz(del_mask);
                first_deleted_slot = (int64_t)(group_idx * BHA_GROUP_WIDTH + (size_t)bit);
            }
        }

        /* Check for empty slot */
        uint32_t empty_mask = bha_match_empty(group);
        if (empty_mask != 0) {
            /* Insertion position found */
            size_t insert_slot;
            if (first_deleted_slot >= 0) {
                insert_slot = (size_t)first_deleted_slot;
                table->tombstones--;
            } else {
                int bit = __builtin_ctz(empty_mask);
                insert_slot = group_idx * BHA_GROUP_WIDTH + (size_t)bit;
            }

            uint8_t *payload = malloc(key_len + val_len);
            if (!payload) return -1;
            if (key_len > 0) memcpy(payload, key, key_len);
            if (val_len > 0) memcpy(payload + key_len, val, val_len);

            bha_slot *slot = &table->slots[insert_slot];
            slot->hash = hash;
            slot->expires_at_ms = expires_at_ms;
            slot->key_len = (uint32_t)key_len;
            slot->val_len = (uint32_t)val_len;
            slot->payload = payload;

            size_t g = insert_slot / BHA_GROUP_WIDTH;
            size_t s = insert_slot % BHA_GROUP_WIDTH;
            table->groups[g].ctrl[s] = fp;
            table->size++;
            return 0;
        }

        /* Quadratic probe to next group */
        step++;
        group_idx = (group_idx + step) & group_mask;
    }
}

int bha_get(const bha_table *table, const uint8_t *key, size_t key_len,
            uint64_t now_ms, const uint8_t **val_out, size_t *val_len_out)
{
    if (!table || !key) return -1;

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    uint8_t fp = bha_h2(hash);
    size_t group_mask = table->group_count - 1;
    size_t group_idx = (size_t)hash & group_mask;
    size_t step = 0;

    for (;;) {
        const bha_group *group = &table->groups[group_idx];

        uint32_t fp_mask = bha_match_fingerprint(group, fp);
        while (fp_mask != 0) {
            int bit = __builtin_ctz(fp_mask);
            fp_mask &= fp_mask - 1;
            size_t slot_idx = group_idx * BHA_GROUP_WIDTH + (size_t)bit;
            const bha_slot *slot = &table->slots[slot_idx];

            if (slot->hash == hash && slot->key_len == key_len) {
                if (key_len == 0 || memcmp(slot->payload, key, key_len) == 0) {
                    if (now_ms >= slot->expires_at_ms) {
                        return 1; /* Expired */
                    }
                    if (val_out) *val_out = slot->payload + slot->key_len;
                    if (val_len_out) *val_len_out = slot->val_len;
                    return 0; /* Found */
                }
            }
        }

        /* If there's an empty slot in this group, the key cannot be further down */
        if (bha_match_empty(group) != 0) {
            return 1; /* Not found */
        }

        step++;
        group_idx = (group_idx + step) & group_mask;
    }
}

int bha_exists(const bha_table *table, const uint8_t *key, size_t key_len, uint64_t now_ms)
{
    return bha_get(table, key, key_len, now_ms, NULL, NULL);
}

int bha_del(bha_table *table, const uint8_t *key, size_t key_len)
{
    if (!table || !key) return -1;

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    uint8_t fp = bha_h2(hash);
    size_t group_mask = table->group_count - 1;
    size_t group_idx = (size_t)hash & group_mask;
    size_t step = 0;

    for (;;) {
        bha_group *group = &table->groups[group_idx];

        uint32_t fp_mask = bha_match_fingerprint(group, fp);
        while (fp_mask != 0) {
            int bit = __builtin_ctz(fp_mask);
            fp_mask &= fp_mask - 1;
            size_t slot_idx = group_idx * BHA_GROUP_WIDTH + (size_t)bit;
            bha_slot *slot = &table->slots[slot_idx];

            if (slot->hash == hash && slot->key_len == key_len) {
                if (key_len == 0 || memcmp(slot->payload, key, key_len) == 0) {
                    free(slot->payload);
                    slot->payload = NULL;
                    group->ctrl[bit] = BHA_CTRL_DELETED;
                    table->size--;
                    table->tombstones++;
                    return 0;
                }
            }
        }

        if (bha_match_empty(group) != 0) {
            return 1; /* Not found */
        }

        step++;
        group_idx = (group_idx + step) & group_mask;
    }
}

static int bha_resize(bha_table *table, size_t new_cap)
{
    bha_table *new_tbl = bha_create(new_cap);
    if (!new_tbl) return -1;

    for (size_t i = 0; i < table->capacity; i++) {
        size_t g = i / BHA_GROUP_WIDTH;
        size_t s = i % BHA_GROUP_WIDTH;
        uint8_t ctrl = table->groups[g].ctrl[s];
        if (ctrl != BHA_CTRL_EMPTY && ctrl != BHA_CTRL_DELETED) {
            bha_slot *slot = &table->slots[i];
            const uint8_t *key = slot->payload;
            const uint8_t *val = slot->payload + slot->key_len;
            if (bha_set(new_tbl, key, slot->key_len, val, slot->val_len, slot->expires_at_ms) != 0) {
                bha_free(new_tbl);
                return -1;
            }
            free(slot->payload);
            slot->payload = NULL;
        }
    }

    free(table->groups);
    free(table->slots);

    table->capacity = new_tbl->capacity;
    table->group_count = new_tbl->group_count;
    table->size = new_tbl->size;
    table->tombstones = 0;
    table->groups = new_tbl->groups;
    table->slots = new_tbl->slots;

    free(new_tbl);
    return 0;
}

size_t bha_size(const bha_table *table)
{
    return table ? table->size : 0;
}

size_t bha_memory_bytes(const bha_table *table)
{
    if (!table) return 0;
    size_t total = sizeof(*table);
    total += table->group_count * sizeof(bha_group);
    total += table->capacity * sizeof(bha_slot);
    /* Payloads are dynamically allocated */
    return total;
}
