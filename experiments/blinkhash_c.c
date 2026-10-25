#define _POSIX_C_SOURCE 200809L

#include "blinkhash_c.h"
#include "hash.h"

#include <stdlib.h>
#include <string.h>

static inline uint32_t bhc_tag(uint64_t hash)
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

bhc_table *bhc_create(size_t initial_index_cap)
{
    size_t cap = next_pow2(initial_index_cap);
    if (cap < 16) cap = 16;

    bhc_table *table = calloc(1, sizeof(*table));
    if (!table) return NULL;

    table->index_cap = cap;
    table->index_mask = cap - 1;
    table->size = 0;

    table->index = calloc(table->index_cap, sizeof(bhc_slot));
    if (!table->index) {
        free(table);
        return NULL;
    }

    table->arena_cap = cap;
    table->arena_count = 1; /* handle 0 reserved */
    table->first_free = 0;

    table->arena = calloc(table->arena_cap, sizeof(bhc_record));
    if (!table->arena) {
        free(table->index);
        free(table);
        return NULL;
    }

    return table;
}

void bhc_free(bhc_table *table)
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

static inline size_t bhc_calc_dib(const bhc_table *table, size_t curr_idx, uint32_t handle)
{
    const bhc_record *rec = &table->arena[handle];
    uint64_t hash = bk_hash_fnv1a(rec->payload, rec->key_len);
    size_t ideal_idx = (size_t)hash & table->index_mask;
    return (curr_idx + table->index_cap - ideal_idx) & table->index_mask;
}

static void bhc_epoch_unlink(bhc_table *table, uint32_t handle)
{
    bhc_record *rec = &table->arena[handle];
    if (rec->prev_expire != 0) {
        table->arena[rec->prev_expire].next_expire = rec->next_expire;
    } else if (table->epoch_heads[rec->epoch_ring] == handle) {
        table->epoch_heads[rec->epoch_ring] = rec->next_expire;
    }

    if (rec->next_expire != 0) {
        table->arena[rec->next_expire].prev_expire = rec->prev_expire;
    }

    rec->prev_expire = 0;
    rec->next_expire = 0;
}

static void bhc_epoch_link(bhc_table *table, uint32_t handle, uint32_t epoch_ring)
{
    bhc_record *rec = &table->arena[handle];
    rec->epoch_ring = epoch_ring;
    rec->prev_expire = 0;
    rec->next_expire = table->epoch_heads[epoch_ring];

    if (table->epoch_heads[epoch_ring] != 0) {
        table->arena[table->epoch_heads[epoch_ring]].prev_expire = handle;
    }
    table->epoch_heads[epoch_ring] = handle;
}

static uint32_t bhc_arena_alloc(bhc_table *table)
{
    if (table->first_free != 0) {
        uint32_t handle = table->first_free;
        table->first_free = table->arena[handle].next_free;
        table->arena[handle].next_free = 0;
        return handle;
    }

    if (table->arena_count >= table->arena_cap) {
        size_t new_cap = table->arena_cap * 2;
        bhc_record *new_arena = realloc(table->arena, new_cap * sizeof(bhc_record));
        if (!new_arena) return 0;
        table->arena = new_arena;
        table->arena_cap = new_cap;
    }

    uint32_t handle = (uint32_t)table->arena_count++;
    table->arena[handle].payload = NULL;
    table->arena[handle].next_free = 0;
    table->arena[handle].prev_expire = 0;
    table->arena[handle].next_expire = 0;
    return handle;
}

static void bhc_arena_free(bhc_table *table, uint32_t handle)
{
    if (handle == 0 || handle >= table->arena_count) return;
    bhc_epoch_unlink(table, handle);
    free(table->arena[handle].payload);
    table->arena[handle].payload = NULL;
    table->arena[handle].next_free = table->first_free;
    table->first_free = handle;
}

static int bhc_resize_index(bhc_table *table, size_t new_cap);

int bhc_set(bhc_table *table, const uint8_t *key, size_t key_len,
            const uint8_t *val, size_t val_len, uint64_t expires_at_ms)
{
    if (!table || !key) return -1;

    if ((table->size + 1) * 5 >= table->index_cap * 4) {
        if (bhc_resize_index(table, table->index_cap * 2) != 0) {
            return -1;
        }
    }

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    uint32_t tag = bhc_tag(hash);
    uint32_t target_epoch = (uint32_t)((expires_at_ms / BHC_EPOCH_STEP_MS) % BHC_MAX_EPOCHS);
    size_t ideal_idx = (size_t)hash & table->index_mask;
    size_t curr_idx = ideal_idx;
    size_t curr_dib = 0;

    /* Check if key already exists */
    for (;;) {
        bhc_slot *slot = &table->index[curr_idx];
        if (slot->handle == BHC_HANDLE_EMPTY) {
            break;
        }

        size_t slot_dib = bhc_calc_dib(table, curr_idx, slot->handle);
        if (slot_dib < curr_dib) {
            break;
        }

        if (slot->tag == tag) {
            bhc_record *rec = &table->arena[slot->handle];
            if (rec->key_len == key_len &&
                (key_len == 0 || memcmp(rec->payload, key, key_len) == 0)) {
                /* Update existing */
                uint8_t *new_payload = malloc(key_len + val_len);
                if (!new_payload) return -1;
                if (key_len > 0) memcpy(new_payload, key, key_len);
                if (val_len > 0) memcpy(new_payload + key_len, val, val_len);

                free(rec->payload);
                rec->payload = new_payload;
                rec->val_len = (uint32_t)val_len;
                rec->expires_at_ms = expires_at_ms;

                /* Relink to new epoch */
                bhc_epoch_unlink(table, slot->handle);
                bhc_epoch_link(table, slot->handle, target_epoch);
                return 0;
            }
        }

        curr_idx = (curr_idx + 1) & table->index_mask;
        curr_dib++;
    }

    /* Allocate record */
    uint32_t handle = bhc_arena_alloc(table);
    if (handle == 0) return -1;

    uint8_t *payload = malloc(key_len + val_len);
    if (!payload) {
        bhc_arena_free(table, handle);
        return -1;
    }
    if (key_len > 0) memcpy(payload, key, key_len);
    if (val_len > 0) memcpy(payload + key_len, val, val_len);

    bhc_record *rec = &table->arena[handle];
    rec->key_len = (uint32_t)key_len;
    rec->val_len = (uint32_t)val_len;
    rec->expires_at_ms = expires_at_ms;
    rec->payload = payload;

    bhc_epoch_link(table, handle, target_epoch);

    /* Robin Hood index insertion */
    bhc_slot elem = { .tag = tag, .handle = handle };
    curr_idx = ideal_idx;
    curr_dib = 0;

    for (;;) {
        bhc_slot *slot = &table->index[curr_idx];
        if (slot->handle == BHC_HANDLE_EMPTY) {
            *slot = elem;
            table->size++;
            return 0;
        }

        size_t slot_dib = bhc_calc_dib(table, curr_idx, slot->handle);
        if (slot_dib < curr_dib) {
            bhc_slot tmp = *slot;
            *slot = elem;
            elem = tmp;
            curr_dib = slot_dib;
        }

        curr_idx = (curr_idx + 1) & table->index_mask;
        curr_dib++;
    }
}

int bhc_get(const bhc_table *table, const uint8_t *key, size_t key_len,
            uint64_t now_ms, const uint8_t **val_out, size_t *val_len_out)
{
    if (!table || !key) return -1;

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    uint32_t tag = bhc_tag(hash);
    size_t ideal_idx = (size_t)hash & table->index_mask;
    size_t curr_idx = ideal_idx;
    size_t curr_dib = 0;

    for (;;) {
        const bhc_slot *slot = &table->index[curr_idx];
        if (slot->handle == BHC_HANDLE_EMPTY) {
            return 1;
        }

        size_t slot_dib = bhc_calc_dib(table, curr_idx, slot->handle);
        if (slot_dib < curr_dib) {
            return 1;
        }

        if (slot->tag == tag) {
            const bhc_record *rec = &table->arena[slot->handle];
            if (rec->key_len == key_len &&
                (key_len == 0 || memcmp(rec->payload, key, key_len) == 0)) {
                if (now_ms >= rec->expires_at_ms) {
                    return 1; /* Expired */
                }
                if (val_out) *val_out = rec->payload + rec->key_len;
                if (val_len_out) *val_len_out = rec->val_len;
                return 0;
            }
        }

        curr_idx = (curr_idx + 1) & table->index_mask;
        curr_dib++;
    }
}

int bhc_exists(const bhc_table *table, const uint8_t *key, size_t key_len, uint64_t now_ms)
{
    return bhc_get(table, key, key_len, now_ms, NULL, NULL);
}

int bhc_del(bhc_table *table, const uint8_t *key, size_t key_len)
{
    if (!table || !key) return -1;

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    uint32_t tag = bhc_tag(hash);
    size_t ideal_idx = (size_t)hash & table->index_mask;
    size_t curr_idx = ideal_idx;
    size_t curr_dib = 0;

    for (;;) {
        bhc_slot *slot = &table->index[curr_idx];
        if (slot->handle == BHC_HANDLE_EMPTY) {
            return 1;
        }

        size_t slot_dib = bhc_calc_dib(table, curr_idx, slot->handle);
        if (slot_dib < curr_dib) {
            return 1;
        }

        if (slot->tag == tag) {
            bhc_record *rec = &table->arena[slot->handle];
            if (rec->key_len == key_len &&
                (key_len == 0 || memcmp(rec->payload, key, key_len) == 0)) {
                bhc_arena_free(table, slot->handle);

                /* Backward-shift deletion */
                size_t i = curr_idx;
                for (;;) {
                    size_t j = (i + 1) & table->index_mask;
                    if (table->index[j].handle == BHC_HANDLE_EMPTY) {
                        break;
                    }
                    size_t j_dib = bhc_calc_dib(table, j, table->index[j].handle);
                    if (j_dib == 0) {
                        break;
                    }
                    table->index[i] = table->index[j];
                    i = j;
                }
                table->index[i].handle = BHC_HANDLE_EMPTY;
                table->index[i].tag = 0;
                table->size--;
                return 0;
            }
        }

        curr_idx = (curr_idx + 1) & table->index_mask;
        curr_dib++;
    }
}

size_t bhc_purge_expired(bhc_table *table, uint64_t now_ms)
{
    if (!table) return 0;

    size_t purged = 0;

    for (size_t e = 0; e < BHC_MAX_EPOCHS; e++) {
        uint32_t curr = table->epoch_heads[e];
        while (curr != 0) {
            uint32_t next = table->arena[curr].next_expire;
            if (now_ms >= table->arena[curr].expires_at_ms) {
                /* Key is expired: delete from hash table */
                bhc_del(table, table->arena[curr].payload, table->arena[curr].key_len);
                purged++;
            }
            curr = next;
        }
    }

    return purged;
}

static int bhc_resize_index(bhc_table *table, size_t new_cap)
{
    bhc_slot *new_index = calloc(new_cap, sizeof(bhc_slot));
    if (!new_index) return -1;

    size_t new_mask = new_cap - 1;

    for (size_t i = 0; i < table->index_cap; i++) {
        uint32_t handle = table->index[i].handle;
        if (handle == BHC_HANDLE_EMPTY) continue;

        const bhc_record *rec = &table->arena[handle];
        uint64_t hash = bk_hash_fnv1a(rec->payload, rec->key_len);
        uint32_t tag = bhc_tag(hash);
        size_t ideal_idx = (size_t)hash & new_mask;

        bhc_slot elem = { .tag = tag, .handle = handle };
        size_t curr_idx = ideal_idx;
        size_t curr_dib = 0;

        for (;;) {
            bhc_slot *slot = &new_index[curr_idx];
            if (slot->handle == BHC_HANDLE_EMPTY) {
                *slot = elem;
                break;
            }

            const bhc_record *cur_rec = &table->arena[slot->handle];
            uint64_t cur_h = bk_hash_fnv1a(cur_rec->payload, cur_rec->key_len);
            size_t slot_ideal = (size_t)cur_h & new_mask;
            size_t slot_dib = (curr_idx + new_cap - slot_ideal) & new_mask;

            if (slot_dib < curr_dib) {
                bhc_slot tmp = *slot;
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

size_t bhc_size(const bhc_table *table)
{
    return table ? table->size : 0;
}

size_t bhc_memory_bytes(const bhc_table *table)
{
    if (!table) return 0;
    size_t total = sizeof(*table);
    total += table->index_cap * sizeof(bhc_slot);
    total += table->arena_cap * sizeof(bhc_record);
    return total;
}
