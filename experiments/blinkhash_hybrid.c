#define _POSIX_C_SOURCE 200809L

#include "blinkhash_hybrid.h"
#include "hash.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define BHH_HAS_SSE2 1
#else
#define BHH_HAS_SSE2 0
#endif

static inline void *bhh_alloc_mem(bhh_table *table, size_t size)
{
    if (table && table->alloc.malloc_fn) {
        return table->alloc.malloc_fn(size);
    }
    return malloc(size);
}

static inline void *bhh_realloc_mem(bhh_table *table, void *ptr, size_t size)
{
    if (table && table->alloc.realloc_fn) {
        return table->alloc.realloc_fn(ptr, size);
    }
    return realloc(ptr, size);
}

static inline void bhh_free_mem(bhh_table *table, void *ptr)
{
    if (!ptr) return;
    if (table && table->alloc.free_fn) {
        table->alloc.free_fn(ptr);
    } else {
        free(ptr);
    }
}

static inline uint8_t bhh_h2(uint64_t hash)
{
    return (uint8_t)((hash >> 57) & 0x7F);
}

/* 100% exact scalar byte match without false positives or cross-byte borrowing */
static inline uint32_t bhh_swar_match_byte(const uint8_t *ctrl, uint8_t target)
{
    uint32_t mask = 0;
    for (int i = 0; i < BHH_GROUP_WIDTH; i++) {
        if (ctrl[i] == target) {
            mask |= (1u << i);
        }
    }
    return mask;
}

static inline uint32_t bhh_match_byte(const bhh_group *group, uint8_t target, bool force_swar)
{
#if BHH_HAS_SSE2
    if (!force_swar) {
        __m128i ctrl = _mm_loadu_si128((const __m128i *)group->ctrl);
        __m128i cmp = _mm_cmpeq_epi8(ctrl, _mm_set1_epi8((char)target));
        return (uint32_t)_mm_movemask_epi8(cmp);
    }
#else
    (void)force_swar;
#endif
    return bhh_swar_match_byte(group->ctrl, target);
}

static inline uint32_t bhh_match_empty(const bhh_group *group, bool force_swar)
{
    return bhh_match_byte(group, BHH_CTRL_EMPTY, force_swar);
}

static inline uint32_t bhh_match_deleted(const bhh_group *group, bool force_swar)
{
    return bhh_match_byte(group, BHH_CTRL_DELETED, force_swar);
}

static inline uint32_t bhh_match_fp(const bhh_group *group, uint8_t fp, bool force_swar)
{
    return bhh_match_byte(group, fp, force_swar);
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

bhh_table *bhh_create(size_t initial_capacity)
{
    return bhh_create_with_allocator(initial_capacity, NULL);
}

bhh_table *bhh_create_with_allocator(size_t initial_capacity, const bhh_allocator *alloc)
{
    /* Reject incomplete custom allocator hooks to prevent mixing with system allocators */
    if (alloc != NULL) {
        if (!alloc->malloc_fn || !alloc->realloc_fn || !alloc->free_fn) {
            return NULL;
        }
    }

    bhh_table *table = NULL;
    if (alloc && alloc->malloc_fn) {
        table = alloc->malloc_fn(sizeof(*table));
        if (table) memset(table, 0, sizeof(*table));
    } else {
        table = calloc(1, sizeof(*table));
    }
    if (!table) return NULL;

    if (alloc) {
        table->alloc = *alloc;
    }

    size_t cap = next_pow2(initial_capacity);
    if (cap < 16) cap = 16;

    table->capacity = cap;
    table->group_count = cap / BHH_GROUP_WIDTH;
    table->group_mask = table->group_count - 1;
    table->size = 0;
    table->tombstones = 0;
    table->payload_bytes = 0;
    table->policy = BHH_POLICY_INPLACE_REHASH;
    table->growth_policy = BHH_GROWTH_1_5X;
    table->force_swar = false;

    table->groups = bhh_alloc_mem(table, table->group_count * sizeof(bhh_group));
    if (!table->groups) {
        if (table->alloc.free_fn) table->alloc.free_fn(table);
        else free(table);
        return NULL;
    }

    for (size_t g = 0; g < table->group_count; g++) {
        memset(table->groups[g].ctrl, BHH_CTRL_EMPTY, BHH_GROUP_WIDTH);
        memset(table->groups[g].handles, 0, sizeof(table->groups[g].handles));
    }

    /* Decoupled Dynamic Arena: start small with conservative capacity */
    table->arena_cap = BHH_INITIAL_ARENA_CAP;
    table->arena_count = 1; /* 1-based, 0 is NULL */
    table->first_free = 0;
    table->arena_realloc_count = 0;
    table->arena_bytes_copied = 0;

    table->arena = bhh_alloc_mem(table, table->arena_cap * sizeof(bhh_record));
    if (!table->arena) {
        bhh_free_mem(table, table->groups);
        if (table->alloc.free_fn) table->alloc.free_fn(table);
        else free(table);
        return NULL;
    }
    memset(table->arena, 0, table->arena_cap * sizeof(bhh_record));

    return table;
}

void bhh_free(bhh_table *table)
{
    if (!table) return;

    for (size_t i = 1; i < table->arena_count; i++) {
        if (table->arena[i].payload != NULL) {
            bhh_free_mem(table, table->arena[i].payload);
        }
    }

    bhh_free_mem(table, table->groups);
    bhh_free_mem(table, table->arena);

    if (table->alloc.free_fn) {
        table->alloc.free_fn(table);
    } else {
        free(table);
    }
}

static inline void bhh_wheel_unlink(bhh_table *table, uint32_t handle_idx)
{
    bhh_record *rec = &table->arena[handle_idx];
    if (rec->prev_expire != 0) {
        table->arena[rec->prev_expire].next_expire = rec->next_expire;
    } else if (table->wheel_heads[rec->wheel_bucket] == handle_idx) {
        table->wheel_heads[rec->wheel_bucket] = rec->next_expire;
    }

    if (rec->next_expire != 0) {
        table->arena[rec->next_expire].prev_expire = rec->prev_expire;
    }

    /* P0 Architectural Fix: Advance purge cursor automatically if unlinked record was cursor target */
    if (table->purge_record_cursor == handle_idx) {
        table->purge_record_cursor = rec->next_expire;
        table->purge_record_gen = rec->next_expire ? table->arena[rec->next_expire].generation : 0;
    }

    rec->prev_expire = 0;
    rec->next_expire = 0;
}

static inline void bhh_wheel_link(bhh_table *table, uint32_t handle_idx, uint64_t expires_at_ms)
{
    bhh_record *rec = &table->arena[handle_idx];
    uint32_t bucket = (uint32_t)((expires_at_ms / BHH_WHEEL_STEP_MS) % BHH_WHEEL_BUCKETS);

    rec->wheel_bucket = bucket;
    rec->prev_expire = 0;
    rec->next_expire = table->wheel_heads[bucket];

    if (table->wheel_heads[bucket] != 0) {
        table->arena[table->wheel_heads[bucket]].prev_expire = handle_idx;
    }
    table->wheel_heads[bucket] = handle_idx;
}

static uint32_t bhh_arena_alloc(bhh_table *table)
{
    /* Prioritize reusing recycled slots from free list */
    if (table->first_free != 0) {
        uint32_t idx = table->first_free;
        table->first_free = table->arena[idx].next_expire;
        table->arena[idx].next_expire = 0;
        table->arena[idx].generation++;
        if (table->arena[idx].generation == 0) {
            table->arena[idx].generation = 1;
        }
        return idx;
    }

    /* Grow arena dynamically when full */
    if (table->arena_count >= table->arena_cap) {
        size_t old_cap = table->arena_cap;
        if (old_cap >= BHH_MAX_ARENA_CAP) {
            return 0; /* Exceeded 32-bit slot addressing limit */
        }

        size_t new_cap = 0;
        if (table->growth_policy == BHH_GROWTH_2X) {
            if (old_cap > (BHH_MAX_ARENA_CAP / 2)) {
                new_cap = BHH_MAX_ARENA_CAP;
            } else {
                new_cap = old_cap * 2;
            }
        } else {
            /* Default: 1.5x geometric growth */
            size_t inc = old_cap >> 1;
            if (inc < 32) inc = 32;
            if (BHH_MAX_ARENA_CAP - old_cap < inc) {
                new_cap = BHH_MAX_ARENA_CAP;
            } else {
                new_cap = old_cap + inc;
            }
        }

        if (new_cap <= old_cap) {
            return 0;
        }

        /* Check for size_t multiplication overflow */
        if (new_cap > (SIZE_MAX / sizeof(bhh_record))) {
            return 0;
        }

        bhh_record *new_arena = bhh_realloc_mem(table, table->arena, new_cap * sizeof(bhh_record));
        if (!new_arena) {
            return 0; /* Rollback: old arena and cap remain 100% intact */
        }

        /* Initialize ONLY the newly allocated slots */
        memset(new_arena + old_cap, 0, (new_cap - old_cap) * sizeof(bhh_record));
        table->arena = new_arena;
        table->arena_cap = new_cap;
        table->arena_realloc_count++;
        table->arena_bytes_copied += (old_cap * sizeof(bhh_record));
    }

    uint32_t idx = (uint32_t)table->arena_count++;
    table->arena[idx].generation = 1;
    table->arena[idx].payload = NULL;
    table->arena[idx].prev_expire = 0;
    table->arena[idx].next_expire = 0;
    return idx;
}

static void bhh_arena_free(bhh_table *table, uint32_t handle_idx)
{
    if (handle_idx == 0 || handle_idx >= table->arena_count) return;

    bhh_wheel_unlink(table, handle_idx);
    if (table->arena[handle_idx].payload != NULL) {
        table->payload_bytes -= (table->arena[handle_idx].key_len + table->arena[handle_idx].val_len);
        bhh_free_mem(table, table->arena[handle_idx].payload);
        table->arena[handle_idx].payload = NULL;
    }

    table->arena[handle_idx].generation++;
    if (table->arena[handle_idx].generation == 0) {
        table->arena[handle_idx].generation = 1;
    }

    table->arena[handle_idx].next_expire = table->first_free;
    table->first_free = handle_idx;
}

static int bhh_resize(bhh_table *table, size_t new_cap);

int bhh_set(bhh_table *table, const uint8_t *key, size_t key_len,
            const uint8_t *val, size_t val_len, uint64_t expires_at_ms,
            bhh_handle *handle_out)
{
    if (!table || !key) return -1;
    if (val == NULL && val_len > 0) return -1;
    if (key_len > UINT32_MAX || val_len > UINT32_MAX || (SIZE_MAX - key_len) < val_len) {
        return -1;
    }

    /* Resize or in-place rehash when total entries (size + tombstones) >= 75% of capacity */
    if ((table->size + table->tombstones + 1) * 4 >= table->capacity * 3) {
        size_t target_cap = table->capacity * 2;
        if (table->policy == BHH_POLICY_INPLACE_REHASH && (table->size * 2 <= table->capacity)) {
            target_cap = table->capacity;
        }
        if (bhh_resize(table, target_cap) != 0) {
            return -1;
        }
    }

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    uint8_t fp = bhh_h2(hash);
    size_t group_mask = table->group_mask;
    size_t group_idx = (size_t)hash & group_mask;
    size_t step = 0;

    int64_t first_del_group = -1;
    int first_del_slot = -1;

    for (;;) {
        bhh_group *group = &table->groups[group_idx];

        /* Step 1: Probe for existing key match */
        uint32_t fp_mask = bhh_match_fp(group, fp, table->force_swar);
        while (fp_mask != 0) {
            int bit = __builtin_ctz(fp_mask);
            fp_mask &= fp_mask - 1;

            uint32_t h_idx = group->handles[bit];
            if (h_idx > 0 && h_idx < table->arena_count) {
                bhh_record *rec = &table->arena[h_idx];
                if (rec->key_len == key_len &&
                    (key_len == 0 || memcmp(rec->payload, key, key_len) == 0)) {
                    /* Match found: overwrite payload in-place */
                    uint8_t *new_payload = bhh_alloc_mem(table, key_len + val_len);
                    if (!new_payload) return -1;
                    if (key_len > 0) memcpy(new_payload, key, key_len);
                    if (val_len > 0) memcpy(new_payload + key_len, val, val_len);

                    table->payload_bytes -= (rec->key_len + rec->val_len);
                    table->payload_bytes += (key_len + val_len);

                    bhh_free_mem(table, rec->payload);
                    rec->payload = new_payload;
                    rec->key_len = (uint32_t)key_len;
                    rec->val_len = (uint32_t)val_len;

                    /* P1 Lifecycle Contract: if the key had expired, advance generation to invalidate old handles */
                    if (table->last_now_ms > 0 && rec->expires_at_ms <= table->last_now_ms) {
                        rec->generation++;
                        if (rec->generation == 0) rec->generation = 1;
                    }

                    rec->expires_at_ms = expires_at_ms;

                    bhh_wheel_unlink(table, h_idx);
                    bhh_wheel_link(table, h_idx, expires_at_ms);

                    if (handle_out) *handle_out = bhh_handle_make(rec->generation, h_idx);
                    return 0;
                }
            }
        }

        /* Step 2: Record first tombstone for reuse */
        if (first_del_group < 0) {
            uint32_t del_mask = bhh_match_deleted(group, table->force_swar);
            if (del_mask != 0) {
                first_del_group = (int64_t)group_idx;
                first_del_slot = __builtin_ctz(del_mask);
            }
        }

        /* Step 3: Check for empty slot */
        uint32_t empty_mask = bhh_match_empty(group, table->force_swar);
        if (empty_mask != 0) {
            uint32_t h_idx = bhh_arena_alloc(table);
            if (h_idx == 0) return -1;

            uint8_t *payload = bhh_alloc_mem(table, key_len + val_len);
            if (!payload) {
                bhh_arena_free(table, h_idx);
                return -1;
            }
            if (key_len > 0) memcpy(payload, key, key_len);
            if (val_len > 0) memcpy(payload + key_len, val, val_len);

            size_t ins_group;
            int ins_slot;

            if (first_del_group >= 0) {
                ins_group = (size_t)first_del_group;
                ins_slot = first_del_slot;
                table->tombstones--;
            } else {
                ins_group = group_idx;
                ins_slot = __builtin_ctz(empty_mask);
            }

            bhh_record *rec = &table->arena[h_idx];
            rec->key_len = (uint32_t)key_len;
            rec->val_len = (uint32_t)val_len;
            rec->expires_at_ms = expires_at_ms;
            rec->payload = payload;
            rec->group_idx = (uint32_t)ins_group;
            rec->slot_idx = (uint8_t)ins_slot;

            table->payload_bytes += (key_len + val_len);

            bhh_wheel_link(table, h_idx, expires_at_ms);

            table->groups[ins_group].ctrl[ins_slot] = fp;
            table->groups[ins_group].handles[ins_slot] = h_idx;
            table->size++;

            if (handle_out) *handle_out = bhh_handle_make(rec->generation, h_idx);
            return 0;
        }

        step++;
        group_idx = (group_idx + step) & group_mask;
    }
}

int bhh_get(bhh_table *table, const uint8_t *key, size_t key_len,
            uint64_t now_ms, const uint8_t **val_out, size_t *val_len_out,
            bhh_handle *handle_out)
{
    if (!table || !key) return -1;
    if (now_ms > table->last_now_ms) table->last_now_ms = now_ms;

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    uint8_t fp = bhh_h2(hash);
    size_t group_mask = table->group_mask;
    size_t group_idx = (size_t)hash & group_mask;
    size_t step = 0;

    table->total_lookups++;

    for (;;) {
        const bhh_group *group = &table->groups[group_idx];

        uint32_t fp_mask = bhh_match_fp(group, fp, table->force_swar);
        while (fp_mask != 0) {
            int bit = __builtin_ctz(fp_mask);
            fp_mask &= fp_mask - 1;

            uint32_t h_idx = group->handles[bit];
            if (h_idx > 0 && h_idx < table->arena_count) {
                const bhh_record *rec = &table->arena[h_idx];
                if (rec->key_len == key_len &&
                    (key_len == 0 || memcmp(rec->payload, key, key_len) == 0)) {
                    table->total_probe_steps += (step + 1);
                    if ((step + 1) > table->max_probe_steps) {
                        table->max_probe_steps = (uint32_t)(step + 1);
                    }
                    if (now_ms >= rec->expires_at_ms) {
                        return 1; /* Logically expired */
                    }
                    if (val_out) *val_out = rec->payload + rec->key_len;
                    if (val_len_out) *val_len_out = rec->val_len;
                    if (handle_out) *handle_out = bhh_handle_make(rec->generation, h_idx);
                    return 0; /* Found */
                }
            }
        }

        if (bhh_match_empty(group, table->force_swar) != 0) {
            table->total_probe_steps += (step + 1);
            if ((step + 1) > table->max_probe_steps) {
                table->max_probe_steps = (uint32_t)(step + 1);
            }
            return 1; /* Not found */
        }

        step++;
        group_idx = (group_idx + step) & group_mask;
    }
}

int bhh_exists(bhh_table *table, const uint8_t *key, size_t key_len, uint64_t now_ms)
{
    return bhh_get(table, key, key_len, now_ms, NULL, NULL, NULL);
}

int bhh_del_handle(bhh_table *table, bhh_handle handle)
{
    if (!table || handle == BHH_HANDLE_NULL) return -1;

    uint32_t gen = bhh_handle_gen(handle);
    uint32_t idx = bhh_handle_idx(handle);

    if (idx == 0 || idx >= table->arena_count) return 1;

    bhh_record *rec = &table->arena[idx];
    if (rec->generation != gen || rec->payload == NULL) {
        return 1; /* Stale or inactive handle! */
    }

    uint32_t g_idx = rec->group_idx;
    uint8_t s_idx = rec->slot_idx;

    table->groups[g_idx].ctrl[s_idx] = BHH_CTRL_DELETED;
    table->groups[g_idx].handles[s_idx] = 0;
    table->size--;
    table->tombstones++;

    bhh_arena_free(table, idx);
    return 0;
}

int bhh_del(bhh_table *table, const uint8_t *key, size_t key_len)
{
    if (!table || !key) return -1;

    uint64_t hash = bk_hash_fnv1a(key, key_len);
    uint8_t fp = bhh_h2(hash);
    size_t group_mask = table->group_mask;
    size_t group_idx = (size_t)hash & group_mask;
    size_t step = 0;

    for (;;) {
        bhh_group *group = &table->groups[group_idx];

        uint32_t fp_mask = bhh_match_fp(group, fp, table->force_swar);
        while (fp_mask != 0) {
            int bit = __builtin_ctz(fp_mask);
            fp_mask &= fp_mask - 1;

            uint32_t h_idx = group->handles[bit];
            if (h_idx > 0 && h_idx < table->arena_count) {
                bhh_record *rec = &table->arena[h_idx];
                if (rec->key_len == key_len &&
                    (key_len == 0 || memcmp(rec->payload, key, key_len) == 0)) {
                    bhh_handle h = bhh_handle_make(rec->generation, h_idx);
                    return bhh_del_handle(table, h);
                }
            }
        }

        if (bhh_match_empty(group, table->force_swar) != 0) {
            return 1; /* Not found */
        }

        step++;
        group_idx = (group_idx + step) & group_mask;
    }
}

const bhh_record *bhh_resolve_handle(const bhh_table *table, bhh_handle handle)
{
    if (!table || handle == BHH_HANDLE_NULL) return NULL;
    uint32_t gen = bhh_handle_gen(handle);
    uint32_t idx = bhh_handle_idx(handle);
    if (idx == 0 || idx >= table->arena_count) return NULL;

    const bhh_record *rec = &table->arena[idx];
    if (rec->generation != gen || rec->payload == NULL) return NULL;
    return rec;
}

void bhh_set_tombstone_policy(bhh_table *table, bhh_tombstone_policy policy)
{
    if (table) table->policy = policy;
}

int bhh_scrub_tombstones(bhh_table *table)
{
    if (!table) return -1;
    if (table->tombstones == 0) return 0;
    return bhh_resize(table, table->capacity);
}

/* Helper to safely purge an arena handle while traversing linked list */
static inline uint32_t bhh_purge_entry(bhh_table *table, uint32_t curr, uint64_t now_ms)
{
    uint32_t next = table->arena[curr].next_expire;
    if (now_ms >= table->arena[curr].expires_at_ms) {
        bhh_handle h = bhh_handle_make(table->arena[curr].generation, curr);
        bhh_del_handle(table, h);
    }
    return next;
}

/* Strategy A: Full purge across all elapsed buckets */
size_t bhh_purge_all(bhh_table *table, uint64_t now_ms)
{
    if (!table) return 0;
    if (now_ms > table->last_now_ms) table->last_now_ms = now_ms;

    size_t purged_before = table->size;
    uint64_t now_tick = now_ms / BHH_WHEEL_STEP_MS;

    size_t scan_buckets = BHH_WHEEL_BUCKETS;
    if (table->last_tick > 0 && (now_tick - table->last_tick) < BHH_WHEEL_BUCKETS) {
        scan_buckets = (size_t)(now_tick - table->last_tick + 1);
    }

    size_t start_b = table->last_tick % BHH_WHEEL_BUCKETS;
    for (size_t i = 0; i < scan_buckets; i++) {
        size_t b = (start_b + i) % BHH_WHEEL_BUCKETS;
        uint32_t curr = table->wheel_heads[b];
        while (curr != 0) {
            curr = bhh_purge_entry(table, curr, now_ms);
        }
    }

    table->last_tick = now_tick;
    table->purge_bucket_cursor = 0;
    table->purge_record_cursor = 0;
    table->purge_record_gen = 0;

    return (purged_before >= table->size) ? (purged_before - table->size) : 0;
}

/* Strategy B: Count-budgeted purge with bounded node traversal and cursor safety */
size_t bhh_purge_limit(bhh_table *table, uint64_t now_ms, size_t max_keys)
{
    if (!table || max_keys == 0) return 0;
    if (now_ms > table->last_now_ms) table->last_now_ms = now_ms;

    size_t purged = 0;
    uint64_t now_tick = now_ms / BHH_WHEEL_STEP_MS;
    size_t buckets_checked = 0;

    /* P1 Fix: Limit inspected nodes to bound event-loop latency even with 100k future-lap keys */
    size_t nodes_inspected = 0;
    size_t max_nodes_to_inspect = max_keys * 8;
    if (max_nodes_to_inspect < 64) max_nodes_to_inspect = 64;

    while (purged < max_keys && buckets_checked < BHH_WHEEL_BUCKETS && nodes_inspected < max_nodes_to_inspect) {
        size_t b = table->purge_bucket_cursor % BHH_WHEEL_BUCKETS;
        uint32_t curr = table->purge_record_cursor;

        /* P0 Fix: Validate that curr is still an active node in bucket b with matching generation */
        if (curr != 0) {
            if (curr >= table->arena_count ||
                table->arena[curr].payload == NULL ||
                table->arena[curr].wheel_bucket != b ||
                table->arena[curr].generation != table->purge_record_gen) {
                curr = table->wheel_heads[b];
            }
        } else {
            curr = table->wheel_heads[b];
        }

        while (curr != 0 && purged < max_keys && nodes_inspected < max_nodes_to_inspect) {
            nodes_inspected++;
            uint32_t next = table->arena[curr].next_expire;

            if (now_ms >= table->arena[curr].expires_at_ms) {
                bhh_handle h = bhh_handle_make(table->arena[curr].generation, curr);
                if (bhh_del_handle(table, h) == 0) {
                    purged++;
                }
            }

            curr = next;
        }

        if (curr != 0 && (purged >= max_keys || nodes_inspected >= max_nodes_to_inspect)) {
            /* Paused mid-bucket: record cursor index and generation */
            table->purge_record_cursor = curr;
            table->purge_record_gen = table->arena[curr].generation;
            break;
        }

        /* Bucket completed */
        table->purge_record_cursor = 0;
        table->purge_record_gen = 0;
        table->purge_bucket_cursor = (table->purge_bucket_cursor + 1) % BHH_WHEEL_BUCKETS;
        buckets_checked++;
    }

    table->last_tick = now_tick;
    return purged;
}

/* Safe timespec difference helper preventing unsigned underflow */
static inline uint64_t bhh_timespec_elapsed_us(struct timespec start, struct timespec end)
{
    int64_t sec = (int64_t)end.tv_sec - (int64_t)start.tv_sec;
    int64_t nsec = (int64_t)end.tv_nsec - (int64_t)start.tv_nsec;
    int64_t diff = sec * 1000000LL + nsec / 1000LL;
    return diff > 0 ? (uint64_t)diff : 0;
}

/* Strategy C: Time-budgeted purge with safe timespec arithmetic and cursor validation */
size_t bhh_purge_time_budget(bhh_table *table, uint64_t now_ms, uint64_t budget_us)
{
    if (!table || budget_us == 0) return 0;
    if (now_ms > table->last_now_ms) table->last_now_ms = now_ms;

    struct timespec start_ts, cur_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    size_t purged = 0;
    size_t buckets_checked = 0;
    uint64_t check_counter = 0;

    while (buckets_checked < BHH_WHEEL_BUCKETS) {
        size_t b = table->purge_bucket_cursor % BHH_WHEEL_BUCKETS;
        uint32_t curr = table->purge_record_cursor;

        if (curr != 0) {
            if (curr >= table->arena_count ||
                table->arena[curr].payload == NULL ||
                table->arena[curr].wheel_bucket != b ||
                table->arena[curr].generation != table->purge_record_gen) {
                curr = table->wheel_heads[b];
            }
        } else {
            curr = table->wheel_heads[b];
        }

        while (curr != 0) {
            uint32_t next = table->arena[curr].next_expire;

            if (now_ms >= table->arena[curr].expires_at_ms) {
                bhh_handle h = bhh_handle_make(table->arena[curr].generation, curr);
                if (bhh_del_handle(table, h) == 0) {
                    purged++;
                }
            }

            curr = next;

            if ((++check_counter & 0x0F) == 0) {
                clock_gettime(CLOCK_MONOTONIC, &cur_ts);
                if (bhh_timespec_elapsed_us(start_ts, cur_ts) >= budget_us) {
                    table->purge_record_cursor = curr;
                    table->purge_record_gen = curr ? table->arena[curr].generation : 0;
                    return purged;
                }
            }
        }

        /* Bucket completed */
        table->purge_record_cursor = 0;
        table->purge_record_gen = 0;
        table->purge_bucket_cursor = (table->purge_bucket_cursor + 1) % BHH_WHEEL_BUCKETS;
        buckets_checked++;

        clock_gettime(CLOCK_MONOTONIC, &cur_ts);
        if (bhh_timespec_elapsed_us(start_ts, cur_ts) >= budget_us) {
            break;
        }
    }

    return purged;
}

size_t bhh_purge_expired(bhh_table *table, uint64_t now_ms)
{
    return bhh_purge_all(table, now_ms);
}

static int bhh_resize(bhh_table *table, size_t new_cap)
{
    size_t new_group_count = new_cap / BHH_GROUP_WIDTH;
    size_t new_group_mask = new_group_count - 1;

    bhh_group *new_groups = bhh_alloc_mem(table, new_group_count * sizeof(bhh_group));
    if (!new_groups) return -1;

    for (size_t g = 0; g < new_group_count; g++) {
        memset(new_groups[g].ctrl, BHH_CTRL_EMPTY, BHH_GROUP_WIDTH);
        memset(new_groups[g].handles, 0, sizeof(new_groups[g].handles));
    }

    /* Rehash active handles into new groups (ZERO PAYLOAD REALLOCATIONS!) */
    for (size_t g = 0; g < table->group_count; g++) {
        for (int s = 0; s < BHH_GROUP_WIDTH; s++) {
            uint8_t c = table->groups[g].ctrl[s];
            if (c != BHH_CTRL_EMPTY && c != BHH_CTRL_DELETED) {
                uint32_t h_idx = table->groups[g].handles[s];
                bhh_record *rec = &table->arena[h_idx];
                uint64_t hash = bk_hash_fnv1a(rec->payload, rec->key_len);
                uint8_t fp = bhh_h2(hash);

                size_t ng_idx = (size_t)hash & new_group_mask;
                size_t step = 0;

                for (;;) {
                    bhh_group *ng = &new_groups[ng_idx];
                    uint32_t empty_mask = bhh_match_empty(ng, table->force_swar);
                    if (empty_mask != 0) {
                        int bit = __builtin_ctz(empty_mask);
                        ng->ctrl[bit] = fp;
                        ng->handles[bit] = h_idx;

                        rec->group_idx = (uint32_t)ng_idx;
                        rec->slot_idx = (uint8_t)bit;
                        break;
                    }
                    step++;
                    ng_idx = (ng_idx + step) & new_group_mask;
                }
            }
        }
    }

    bhh_free_mem(table, table->groups);
    table->groups = new_groups;

    if (new_cap == table->capacity) {
        table->inplace_rehash_count++;
    } else {
        table->rehash_count++;
    }

    table->capacity = new_cap;
    table->group_count = new_group_count;
    table->group_mask = new_group_mask;
    table->tombstones = 0;
    return 0;
}

size_t bhh_size(const bhh_table *table)
{
    return table ? table->size : 0;
}

size_t bhh_memory_bytes(const bhh_table *table)
{
    if (!table) return 0;
    size_t total = sizeof(*table);
    total += table->group_count * sizeof(bhh_group);
    total += table->arena_cap * sizeof(bhh_record);
    total += table->payload_bytes;
    return total;
}

void bhh_get_stats(const bhh_table *table, bhh_stats *stats_out)
{
    if (!table || !stats_out) return;
    stats_out->size = table->size;
    stats_out->tombstones = table->tombstones;
    stats_out->capacity = table->capacity;
    stats_out->arena_cap = table->arena_cap;
    stats_out->arena_count = table->arena_count;
    stats_out->rehash_count = table->rehash_count;
    stats_out->inplace_rehash_count = table->inplace_rehash_count;
    stats_out->arena_realloc_count = table->arena_realloc_count;
    stats_out->arena_bytes_copied = table->arena_bytes_copied;
    stats_out->max_probe_groups = table->max_probe_steps;
    stats_out->avg_probe_groups = (table->total_lookups > 0)
        ? ((double)table->total_probe_steps / (double)table->total_lookups)
        : 1.0;
}

void bhh_set_growth_policy(bhh_table *table, bhh_growth_policy policy)
{
    if (table) table->growth_policy = policy;
}

bhh_growth_policy bhh_get_growth_policy(const bhh_table *table)
{
    return table ? table->growth_policy : BHH_GROWTH_1_5X;
}

void bhh_set_force_swar(bhh_table *table, bool enable)
{
    if (table) table->force_swar = enable;
}

uint32_t bhh_match_byte_test(const uint8_t ctrl[16], uint8_t target, bool force_swar)
{
    bhh_group g;
    memcpy(g.ctrl, ctrl, 16);
    return bhh_match_byte(&g, target, force_swar);
}
