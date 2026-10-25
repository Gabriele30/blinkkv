#define _POSIX_C_SOURCE 200809L

#include "blinkhash_b.h"
#include "hash.h"

#include <stdlib.h>
#include <string.h>

static inline uint32_t bhb_tag(uint64_t hash)
{
    return (uint32_t)(hash >> 32);
}

static size_t next_pow2(size_t v)
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

bhb_table *bhb_create(size_t initial_capacity)
{
    size_t cap = next_pow2(initial_capacity);
    if (cap < 16) cap = 16;

    bhb_table *table = calloc(1, sizeof(*table));
    if (!table) return NULL;

    table->index_cap = cap;
    table->index_mask = cap - 1;
    table->size = 0;

    table->index = calloc(table->index_cap, sizeof(bhb_slot));
    if (!table->index) {
        free(table);
        return NULL;
    }

    /* Initial arena capacity matches index capacity */
    table->arena_cap = cap;
    table->arena_count = 1; /* index 0 reserved for BHB_HANDLE_EMPTY */
    table->first_free = 0;

    table->arena = calloc(table->arena_cap, sizeof(bhb_record));
    if (!table->arena) {
        free(table->index);
        free(table);
        return NULL;
    }

    return table;
}

void bhb_free(bhb_table *table)
{
    if (!table) return;

    for (size_t i = 1; i < table->arena_count; i++) {
        if (table->arena[i].payload != NULL) {
            free(table->arena[i].payload);
        }
    }

    free(table->index);
    free(table->arena);
    free(table);
}

static inline size_t bhb_calc_dib(const bhb_table *table, size_t curr_idx, uint32_t handle)
{
    const bhb_record *rec = &table->arena[handle];
    uint64_t hash = bk_hash_fnv1a(rec->payload, rec->key_len);
    size_t ideal_idx = (size_t)hash & table->index_mask;
    return (curr_idx + table->index_cap - ideal_idx) & table->index_mask;
}

static uint32_t bhb_arena_alloc(bhb_table *table)
{
    if (table->first_free != 0) {
        uint32_t handle = table->first_free;
        table->first_free = table->arena[handle].next_free;
        table->arena[handle].next_free = 0;
        return handle;
    }

    if (table->arena_count >= table->arena_cap) {
        size_t new_cap = table->arena_cap * 2;
        bhb_record *new_arena = realloc(table->arena, new_cap * sizeof(bhb_record));
        if (!new_arena) return 0;
        table->arena = new_arena;
        table->arena_cap = new_cap;
    }

    uint32_t handle = (uint32_t)table->arena_count++;
    table->arena[handle].generation = 1;
    table->arena[handle].next_free = 0;
    table->arena[handle].payload = NULL;
    return handle;
}

static void bhb_arena_free(bhb_table *table, uint32_t handle)
{
    if (handle == 0 || handle >= table->arena_count) return;
    free(table->arena[handle].payload);
    table->arena[handle].payload = NULL;
    table->arena[handle].generation++;
    table->arena[handle].next_free = table->first_free;
    table->first_free = handle;
}

static int bhb_resize_index(bhb_table *table, size_t new_cap);

int bhb_set(bhb_table *table, const uint8_t *key, size_t key_len,
            const uint8_t *val, size_t val_len, uint64_t expires_at_ms,
            uint32_t *handle_out)
{
    if (!table || !key) return -1;

    /* Resize index if load factor >= 80% */
    if ((table->size + 1) * 5 >= table->index_cap * 4) {
        if (bhb_resize_index(table, table->index_cap * 2) != 0) {
            return -1;
        }
    }

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    uint32_t tag = bhb_tag(hash);
    size_t ideal_idx = (size_t)hash & table->index_mask;

    /* Phase 1: Check if key already exists */
    size_t curr_idx = ideal_idx;
    size_t curr_dib = 0;

    for (;;) {
        bhb_slot *slot = &table->index[curr_idx];
        if (slot->handle == BHB_HANDLE_EMPTY) {
            break;
        }

        size_t slot_dib = bhb_calc_dib(table, curr_idx, slot->handle);
        if (slot_dib < curr_dib) {
            break; /* Key cannot be further down in Robin Hood layout */
        }

        if (slot->tag == tag) {
            bhb_record *rec = &table->arena[slot->handle];
            if (rec->key_len == key_len &&
                (key_len == 0 || memcmp(rec->payload, key, key_len) == 0)) {
                /* Key match: update in-place */
                uint8_t *new_payload = malloc(key_len + val_len);
                if (!new_payload) return -1;
                if (key_len > 0) memcpy(new_payload, key, key_len);
                if (val_len > 0) memcpy(new_payload + key_len, val, val_len);

                free(rec->payload);
                rec->payload = new_payload;
                rec->val_len = (uint32_t)val_len;
                rec->expires_at_ms = expires_at_ms;

                if (handle_out) *handle_out = slot->handle;
                return 0;
            }
        }

        curr_idx = (curr_idx + 1) & table->index_mask;
        curr_dib++;
    }

    /* Phase 2: Insert new record into arena */
    uint32_t handle = bhb_arena_alloc(table);
    if (handle == 0) return -1;

    uint8_t *payload = malloc(key_len + val_len);
    if (!payload) {
        bhb_arena_free(table, handle);
        return -1;
    }
    if (key_len > 0) memcpy(payload, key, key_len);
    if (val_len > 0) memcpy(payload + key_len, val, val_len);

    bhb_record *rec = &table->arena[handle];
    rec->key_len = (uint32_t)key_len;
    rec->val_len = (uint32_t)val_len;
    rec->expires_at_ms = expires_at_ms;
    rec->payload = payload;

    /* Phase 3: Insert slot into index via Robin Hood displacement */
    bhb_slot elem = { .tag = tag, .handle = handle };
    curr_idx = ideal_idx;
    curr_dib = 0;

    for (;;) {
        bhb_slot *slot = &table->index[curr_idx];
        if (slot->handle == BHB_HANDLE_EMPTY) {
            *slot = elem;
            table->size++;
            if (handle_out) *handle_out = handle;
            return 0;
        }

        size_t slot_dib = bhb_calc_dib(table, curr_idx, slot->handle);
        if (slot_dib < curr_dib) {
            /* Steal from rich: swap elem with slot */
            bhb_slot temp = *slot;
            *slot = elem;
            elem = temp;
            curr_dib = slot_dib;
        }

        curr_idx = (curr_idx + 1) & table->index_mask;
        curr_dib++;
    }
}

int bhb_get(const bhb_table *table, const uint8_t *key, size_t key_len,
            uint64_t now_ms, const uint8_t **val_out, size_t *val_len_out,
            uint32_t *handle_out)
{
    if (!table || !key) return -1;

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    uint32_t tag = bhb_tag(hash);
    size_t ideal_idx = (size_t)hash & table->index_mask;
    size_t curr_idx = ideal_idx;
    size_t curr_dib = 0;

    for (;;) {
        const bhb_slot *slot = &table->index[curr_idx];
        if (slot->handle == BHB_HANDLE_EMPTY) {
            return 1; /* Not found */
        }

        size_t slot_dib = bhb_calc_dib(table, curr_idx, slot->handle);
        if (slot_dib < curr_dib) {
            return 1; /* Not found (early Robin Hood termination) */
        }

        if (slot->tag == tag) {
            const bhb_record *rec = &table->arena[slot->handle];
            if (rec->key_len == key_len &&
                (key_len == 0 || memcmp(rec->payload, key, key_len) == 0)) {
                if (now_ms >= rec->expires_at_ms) {
                    return 1; /* Expired */
                }
                if (val_out) *val_out = rec->payload + rec->key_len;
                if (val_len_out) *val_len_out = rec->val_len;
                if (handle_out) *handle_out = slot->handle;
                return 0; /* Found */
            }
        }

        curr_idx = (curr_idx + 1) & table->index_mask;
        curr_dib++;
    }
}

int bhb_exists(const bhb_table *table, const uint8_t *key, size_t key_len, uint64_t now_ms)
{
    return bhb_get(table, key, key_len, now_ms, NULL, NULL, NULL);
}

int bhb_del(bhb_table *table, const uint8_t *key, size_t key_len)
{
    if (!table || !key) return -1;

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    uint32_t tag = bhb_tag(hash);
    size_t ideal_idx = (size_t)hash & table->index_mask;
    size_t curr_idx = ideal_idx;
    size_t curr_dib = 0;

    for (;;) {
        bhb_slot *slot = &table->index[curr_idx];
        if (slot->handle == BHB_HANDLE_EMPTY) {
            return 1; /* Not found */
        }

        size_t slot_dib = bhb_calc_dib(table, curr_idx, slot->handle);
        if (slot_dib < curr_dib) {
            return 1; /* Not found */
        }

        if (slot->tag == tag) {
            bhb_record *rec = &table->arena[slot->handle];
            if (rec->key_len == key_len &&
                (key_len == 0 || memcmp(rec->payload, key, key_len) == 0)) {
                /* Match found: reclaim arena record */
                bhb_arena_free(table, slot->handle);

                /* Backward-shift deletion on index slots (Tombstone-free!) */
                size_t i = curr_idx;
                for (;;) {
                    size_t j = (i + 1) & table->index_mask;
                    if (table->index[j].handle == BHB_HANDLE_EMPTY) {
                        break;
                    }
                    size_t j_dib = bhb_calc_dib(table, j, table->index[j].handle);
                    if (j_dib == 0) {
                        break;
                    }
                    table->index[i] = table->index[j];
                    i = j;
                }
                table->index[i].handle = BHB_HANDLE_EMPTY;
                table->index[i].tag = 0;
                table->size--;
                return 0;
            }
        }

        curr_idx = (curr_idx + 1) & table->index_mask;
        curr_dib++;
    }
}

const bhb_record *bhb_get_by_handle(const bhb_table *table, uint32_t handle)
{
    if (!table || handle == 0 || handle >= table->arena_count) return NULL;
    return &table->arena[handle];
}

static int bhb_resize_index(bhb_table *table, size_t new_cap)
{
    bhb_slot *new_index = calloc(new_cap, sizeof(bhb_slot));
    if (!new_index) return -1;

    size_t new_mask = new_cap - 1;

    /* Re-insert active handles into new index */
    for (size_t i = 0; i < table->index_cap; i++) {
        uint32_t handle = table->index[i].handle;
        if (handle == BHB_HANDLE_EMPTY) continue;

        const bhb_record *rec = &table->arena[handle];
        uint64_t hash = bk_hash_fnv1a(rec->payload, rec->key_len);
        uint32_t tag = bhb_tag(hash);
        size_t ideal_idx = (size_t)hash & new_mask;

        bhb_slot elem = { .tag = tag, .handle = handle };
        size_t curr_idx = ideal_idx;
        size_t curr_dib = 0;

        for (;;) {
            bhb_slot *slot = &new_index[curr_idx];
            if (slot->handle == BHB_HANDLE_EMPTY) {
                *slot = elem;
                break;
            }

            /* Calculate DIB in new index */
            const bhb_record *cur_rec = &table->arena[slot->handle];
            uint64_t cur_h = bk_hash_fnv1a(cur_rec->payload, cur_rec->key_len);
            size_t slot_ideal = (size_t)cur_h & new_mask;
            size_t slot_dib = (curr_idx + new_cap - slot_ideal) & new_mask;

            if (slot_dib < curr_dib) {
                bhb_slot tmp = *slot;
                *slot = elem;
                elem = tmp;
                curr_dib = slot_dib;
            }

            curr_idx = (curr_idx + 1) & new_mask;
            curr_dib++;
        }
    }

    free(table->index);
    table->index = new_index;
    table->index_cap = new_cap;
    table->index_mask = new_mask;
    return 0;
}

size_t bhb_size(const bhb_table *table)
{
    return table ? table->size : 0;
}

size_t bhb_memory_bytes(const bhb_table *table)
{
    if (!table) return 0;
    size_t total = sizeof(*table);
    total += table->index_cap * sizeof(bhb_slot);
    total += table->arena_cap * sizeof(bhb_record);
    return total;
}
