#ifndef BLINKHASH_HYBRID_H
#define BLINKHASH_HYBRID_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * BlinkHash Hybrid: Hardened High-Performance TTL-Native Hash Engine
 *
 * Architectural highlights:
 * 1. Two-Level Decoupled Architecture:
 *    - 80-byte Index Groups: 16-byte SIMD metadata (7-bit fingerprints) + 64-byte 32-bit arena handles.
 *    - Separated Record Arena with single contiguous payload allocations (key + val in 1 chunk).
 * 2. Vectorized SIMD Group Probing:
 *    - SSE2 vector instructions (_mm_cmpeq_epi8, _mm_movemask_epi8) with verified 64-bit SWAR fallback.
 *    - Lookup misses terminate in 1 vector check if group contains an Empty slot.
 * 3. 64-bit ABA-Proof Stable Handles:
 *    - Upper 32-bit generation counter + lower 32-bit arena slot index.
 *    - Guarantees zero dangling references and instant detection of stale handles.
 * 4. Native Multi-Turn Intrusive Timing Wheel:
 *    - Doubly-linked expiration lists embedded directly in arena records.
 *    - Direct back-pointers (group_idx, slot_idx) allow O(1) index removal during expiration storms
 *      without performing any hash table lookups!
 *    - Supports multi-turn laps for long-lived keys beyond the wheel horizon.
 *    - Incremental budgeted purging (count-based and microsecond-based) to prevent event loop stalls.
 * 5. Bounded Memory Under Churn:
 *    - Configurable tombstone recovery policies: Naive Doubling vs In-Place Rehash scrubbing.
 * 6. Hardened Allocation and Arithmetic:
 *    - Safe overflow checks on size_t and uint32_t.
 *    - Clean rollback on memory allocation failures.
 */

#define BHH_GROUP_WIDTH    16
#define BHH_CTRL_EMPTY     0x80
#define BHH_CTRL_DELETED   0xFE
#define BHH_WHEEL_BUCKETS  1024
#define BHH_WHEEL_STEP_MS  100ULL /* 100ms granularity per bucket: 102.4s horizon per lap */

typedef uint64_t bhh_handle;
#define BHH_HANDLE_NULL 0ULL

static inline bhh_handle bhh_handle_make(uint32_t gen, uint32_t idx) {
    return ((uint64_t)gen << 32) | (uint64_t)idx;
}

static inline uint32_t bhh_handle_gen(bhh_handle h) {
    return (uint32_t)(h >> 32);
}

static inline uint32_t bhh_handle_idx(bhh_handle h) {
    return (uint32_t)(h & 0xFFFFFFFFULL);
}

typedef enum {
    BHH_POLICY_DOUBLING = 0,       /* Naive doubling whenever (size + tombstones) >= 75% */
    BHH_POLICY_INPLACE_REHASH = 1  /* In-place rehash to scrub tombstones if size <= capacity / 2 */
} bhh_tombstone_policy;

typedef enum {
    BHH_GROWTH_1_5X = 0,           /* 1.5x geometric growth (tighter memory bounds & page reuse) */
    BHH_GROWTH_2X   = 1            /* 2.0x doubling */
} bhh_growth_policy;

#define BHH_INITIAL_ARENA_CAP 64
#define BHH_MAX_ARENA_CAP     0xFFFFFFFEU

typedef struct {
    uint8_t  ctrl[BHH_GROUP_WIDTH];     /* 16 bytes: 0x80 = empty, 0xFE = deleted, 0x00..0x7F = fp */
    uint32_t handles[BHH_GROUP_WIDTH];  /* 64 bytes: 32-bit arena indices (0 = empty) */
} bhh_group;

typedef struct {
    uint64_t expires_at_ms;
    uint32_t key_len;
    uint32_t val_len;
    uint32_t generation;
    uint32_t prev_expire;  /* Intrusive timing wheel link */
    uint32_t next_expire;
    uint32_t wheel_bucket; /* Current wheel bucket */
    uint32_t group_idx;    /* Back-pointer to index group for O(1) purge removal */
    uint8_t  slot_idx;     /* Back-pointer to slot within group (0..15) */
    uint8_t  flags;
    uint16_t reserved;
    uint8_t *payload;      /* Contiguous key followed by value */
} bhh_record;

typedef struct {
    size_t size;                   /* Active key count */
    size_t tombstones;             /* Active tombstone count */
    size_t capacity;               /* Index capacity */
    size_t arena_cap;              /* Arena capacity */
    size_t arena_count;            /* Arena high watermark */
    size_t never_allocated_slots;  /* Arena capacity never reached */
    size_t freelist_count;         /* Slots currently in free-list */
    size_t payload_bytes;          /* Exact active payload bytes */
    uint64_t rehash_count;         /* Total resizes performed */
    uint64_t inplace_rehash_count; /* In-place tombstone scrub operations */
    uint64_t arena_realloc_count;  /* Total arena reallocations performed */
    uint64_t arena_bytes_copied;   /* Total bytes copied during arena reallocations */
    double   avg_probe_groups;     /* Average probe groups traversed in lookups */
    uint32_t max_probe_groups;     /* Maximum probe groups traversed in lookups */
} bhh_stats;

/* Memory allocator hooks for fault injection and testing */
typedef struct {
    void *(*malloc_fn)(size_t size);
    void *(*realloc_fn)(void *ptr, size_t size);
    void  (*free_fn)(void *ptr);
} bhh_allocator;

typedef struct {
    /* Index State */
    size_t capacity;        /* Total slot capacity (multiple of 16) */
    size_t group_count;     /* capacity / 16 (power of 2) */
    size_t group_mask;      /* group_count - 1 */
    size_t size;            /* Active key count */
    size_t tombstones;      /* Deleted slot count */
    bhh_group *groups;      /* Array of 80-byte index groups */

    /* Arena State */
    size_t arena_cap;
    size_t arena_count;     /* Highest allocated record index */
    uint32_t first_free;    /* Free list head */
    bhh_record *arena;      /* 1-based record array (slot 0 reserved) */

    /* Timing Wheel State */
    uint32_t wheel_heads[BHH_WHEEL_BUCKETS];
    uint64_t last_tick;           /* Wheel cursor in 100ms units */
    uint64_t last_now_ms;         /* Latest observed monotonic timestamp */
    uint32_t purge_bucket_cursor; /* Active bucket being processed */
    uint32_t purge_record_cursor; /* Arena index of next record to inspect */
    uint32_t purge_record_gen;    /* Generation of record at cursor to verify validity */

    /* Memory Accounting */
    size_t payload_bytes;         /* Exact memory allocated for active key + value payloads */

    /* Configuration & Policies */
    bhh_tombstone_policy policy;
    bhh_growth_policy growth_policy;
    bool force_swar;              /* If true, forces scalar SWAR path for testing */
    uint64_t seed;                /* Hash seed */

    /* Telemetry */
    uint64_t rehash_count;
    uint64_t inplace_rehash_count;
    uint64_t arena_realloc_count;
    uint64_t arena_bytes_copied;
    uint64_t total_lookups;
    uint64_t total_probe_steps;
    uint32_t max_probe_steps;

    /* Allocator injection */
    bhh_allocator alloc;
} bhh_table;

bhh_table *bhh_create(size_t initial_capacity);
bhh_table *bhh_create_with_allocator(size_t initial_capacity, const bhh_allocator *alloc);
void bhh_free(bhh_table *table);

/* Core KV Operations */
int bhh_set(bhh_table *table, const uint8_t *key, size_t key_len,
            const uint8_t *val, size_t val_len, uint64_t expires_at_ms,
            bhh_handle *handle_out);

int bhh_get(bhh_table *table, const uint8_t *key, size_t key_len,
            uint64_t now_ms, const uint8_t **val_out, size_t *val_len_out,
            bhh_handle *handle_out);

int bhh_exists(bhh_table *table, const uint8_t *key, size_t key_len, uint64_t now_ms);

int bhh_del(bhh_table *table, const uint8_t *key, size_t key_len);

int bhh_del_handle(bhh_table *table, bhh_handle handle);

/**
 * Direct handle access.
 *
 * NOTE on Lifetime:
 * Returns a direct pointer to the record in the arena. The pointer remains valid
 * ONLY until the next mutating call (set/del/purge/resize) which may reallocate
 * or modify the arena. The bhh_handle itself remains stable indefinitely.
 *
 * NOTE on Generation Wrap-around:
 * Handles use a 32-bit generation counter. When recycled 2^32 times, generation 0
 * is skipped. An ABA collision on a specific slot would require exactly 4,294,967,295
 * deallocations and reallocations of that same arena slot.
 */
const bhh_record *bhh_resolve_handle(const bhh_table *table, bhh_handle handle);

/* Expiration Strategies */
size_t bhh_purge_all(bhh_table *table, uint64_t now_ms);
size_t bhh_purge_limit(bhh_table *table, uint64_t now_ms, size_t max_keys);
size_t bhh_purge_time_budget(bhh_table *table, uint64_t now_ms, uint64_t budget_us);
size_t bhh_purge_expired(bhh_table *table, uint64_t now_ms); /* Convenience alias for bhh_purge_all */

/* Tombstone & Growth management */
void bhh_set_tombstone_policy(bhh_table *table, bhh_tombstone_policy policy);
int bhh_scrub_tombstones(bhh_table *table);
void bhh_set_growth_policy(bhh_table *table, bhh_growth_policy policy);
bhh_growth_policy bhh_get_growth_policy(const bhh_table *table);

/* Diagnostics & Telemetry */
size_t bhh_size(const bhh_table *table);
size_t bhh_memory_bytes(const bhh_table *table);
void bhh_get_stats(const bhh_table *table, bhh_stats *stats_out);
void bhh_set_force_swar(bhh_table *table, bool enable);
uint32_t bhh_match_byte_test(const uint8_t ctrl[16], uint8_t target, bool force_swar);

#endif /* BLINKHASH_HYBRID_H */

