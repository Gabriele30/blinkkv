# BlinkKV Architecture & Core API (Milestone 1)

## 1. Overview & Philosophy

BlinkKV is a lightweight, in-memory key-value store written in C11, engineered specifically for ephemeral, short-lived data (sessions, OTPs, rate-limit counters, temporary locks).

The core design principle is **minimalism with mechanical sympathy**:
- Written in clean, standards-compliant C11 (`-std=c11 -Wall -Wextra -Wpedantic`).
- Zero unnecessary external dependencies (relies only on the C standard library and OS monotonic clock facilities).
- Explicit error handling via `bk_status` enum.
- Deterministic, testable time semantics via pluggable clock interfaces.

---

## 2. Component Layout

The public interface is exposed through [include/blinkkv.h](file:///c:/Users/gabriele/Documents/blinkkv/include/blinkkv.h), which aggregates the store and hash components:

```
include/
  blinkkv.h       # Umbrella header (versioning, public aggregate)
  store.h         # Core in-memory store API, bk_status, clock typedefs
  hash.h          # Hashing utilities (FNV-1a 64-bit)
src/
  store.c         # Hash table, bucket management, lazy expiration
  hash.c          # FNV-1a 64-bit implementation
tests/
  test_common.h   # Assertion framework independent of NDEBUG
  test_store.c    # Deterministic test suite
```

---

## 3. Data Structures & Hash Table

The primary store abstraction is `bk_store`, an opaque pointer representing an in-memory hash table with separate chaining for collision resolution:

```c
typedef struct bk_entry {
    uint8_t *key;
    size_t key_len;
    uint8_t *value;
    size_t value_len;
    uint64_t hash;            /* Cached 64-bit hash to short-circuit memcmp */
    uint64_t expires_at_ms;   /* Monotonic millisecond timestamp */
    struct bk_entry *next;
} bk_entry;

struct bk_store {
    size_t bucket_count;
    bk_entry **buckets;
    bk_clock_fn clock_fn;     /* Pluggable monotonic clock */
    void *clock_ctx;          /* User context for clock */
};
```

### Key Optimizations (Milestone 1)
- **Single Hash Calculation**: In `bk_set()`, `bk_get()`, `bk_del()`, `bk_exists()`, and `bk_ttl()`, `bk_hash_fnv1a` is calculated exactly once per operation.
- **Hash Caching**: Each node caches its 64-bit hash value in `entry->hash`. Bucket traversals compare the 64-bit hash first; string `memcmp` is only invoked if the hashes and lengths match.
- **Safe Overflow Handling**: Expiration time calculation saturates at `UINT64_MAX` instead of wrapping around if `now_ms + ttl_ms` overflows.

---

## 4. Error Handling & API Contract

Every operation returns an explicit `bk_status` code:

| Status Code | Value | Description |
| :--- | :--- | :--- |
| `BK_OK` | `0` | Operation succeeded. |
| `BK_ERR_NOT_FOUND` | `1` | Key does not exist or has expired. |
| `BK_ERR_INVALID_ARG` | `-1` | Invalid argument (NULL pointer, invalid buffer length). |
| `BK_ERR_NOMEM` | `-2` | Memory allocation (`calloc`/`malloc`) failed. |
| `BK_ERR_INVALID_TTL` | `-3` | TTL value is invalid (`ttl_ms == 0` is strictly rejected). |
| `BK_ERR_CLOCK` | `-4` | The monotonic clock provider returned an error. |

A helper function `bk_status_str(status)` returns static human-readable diagnostic messages.

---

## 5. Memory Ownership Model

1. **Write Operations (`bk_set`)**:
   - The store makes deep defensive copies of both the `key` and `value` buffers.
   - The caller retains ownership of the input buffers and can free or reuse them immediately after `bk_set` returns.
2. **Read Operations (`bk_get`)**:
   - On success (`BK_OK`), `*value_out` receives a freshly allocated memory buffer containing a copy of the stored value.
   - **The caller is responsible for freeing `*value_out` with `free()`**.
   - For zero-length values (`value_len == 0`), `*value_out` is set to `NULL` and `*value_len_out` is set to `0`.
   - On failure or if the key is missing/expired, `*value_out` is set to `NULL` and `*value_len_out` is set to `0`.
3. **Deallocation (`bk_store_free`)**:
   - Frees all active buckets, entry nodes, keys, and values.
   - Calling `bk_store_free(NULL)` is safe and performs no operation.

---

## 6. Monotonic Time & Deterministic Testing

To guarantee determinism in test environments without relying on fragile `sleep()` calls, BlinkKV abstracts monotonic time behind an injectable function pointer:

```c
typedef int (*bk_clock_fn)(uint64_t *now_ms_out, void *user_data);

bk_store *bk_store_create(size_t bucket_count);
bk_store *bk_store_create_with_clock(size_t bucket_count, bk_clock_fn clock_fn, void *clock_ctx);
```

- When `clock_fn` is `NULL` (or created via `bk_store_create`), BlinkKV defaults to:
  - POSIX: `clock_gettime(CLOCK_MONOTONIC)`
  - Windows: `GetTickCount64()`
- Tests can pass a mock clock function advancing simulated time instantaneously with sub-millisecond precision.

---

## 7. Current Architectural Limitations (To Address in M2/M3)

1. **Lazy Expiration Only**: In Milestone 1, keys are reclaimed only when accessed via `set`, `get`, `del`, `exists`, or `ttl`. A background active expiration mechanism (Timing Wheel) is planned for Milestone 3.
2. **Static Bucket Count**: The hash table size is fixed at creation time (`bucket_count`). Dynamic rehashing and load factor management are scheduled for Milestone 2.
3. **Allocation Count per Entry**: An entry currently requires 3 heap allocations (node, key, value). Packing key and value into a single contiguous block with the node is scheduled for Milestone 2.
4. **Single-Threaded**: The store does not implement locking; concurrent access must be synchronized externally or mediated by a single-threaded event loop.
