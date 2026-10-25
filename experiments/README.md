# BlinkHash Research Initiative

**High-Performance, TTL-Native Hash Engine Exploration for BlinkKV**

This directory houses the experimental research prototypes, differential verification harnesses, stress test suites, and audited benchmarks developed during the exploration of next-generation indexing architectures for BlinkKV.

> [!WARNING]
> **Research Status: Preliminary Prototypes.**  
> Code in this directory is experimental and subject to ongoing technical validation. It has **not yet been merged** into BlinkKV's production core (`src/store.c`). Benchmark results are preliminary and environment-dependent; no absolute superiority claims are made.

---

## Architecture Exploration Overview

BlinkHash explores how to design an in-memory indexing engine optimized specifically for short-lived, high-churn key-value data with massive expirations.

| Candidate | Architecture Pattern | Key Mechanism | Probing / Collision | Expiration Strategy |
| :--- | :--- | :--- | :--- | :--- |
| **Candidate A** (`blinkhash_a`) | Flat Open-Addressing | SwissTable-style 16-slot groups, 7-bit vector fingerprints | SSE2 vector matching (`_mm_cmpeq_epi8`) | Lazy on-access |
| **Candidate B** (`blinkhash_b`) | Decoupled 2-Level | Compact 8-byte index table + Generational record arena | Robin Hood hashing + Backward-shift deletion | Lazy on-access + Handle validation |
| **Candidate C** (`blinkhash_c`) | Decoupled + Epoch Wheel | Compact 8-byte index table + Arena with epoch ring links | Robin Hood hashing + Backward-shift deletion | O(1) epoch bucket draining |
| **BlinkHash Hybrid** (`blinkhash_hybrid`) | Hardened Two-Level Hybrid | 80-byte SIMD index groups (16B ctrl + 64B handles) + Record arena | SSE2 vector probing (with verified 64-bit SWAR fallback) | Multi-turn intrusive timing wheel with O(1) backpointer unlinking |

---

## BlinkHash Hybrid: Deep Architectural Details

BlinkHash Hybrid combines vector-accelerated group probing with stable handle indirection and intrusive temporal ordering:

1. **Two-Level Decoupled Architecture**:
   - **80-byte Index Groups (`bhh_group`)**: 16 bytes of 7-bit metadata fingerprints (`ctrl`) + 64 bytes of 32-bit arena handles (`handles`). Index cost is strictly **5.0 bytes per slot**.
   - **Record Arena (`bhh_record`)**: 48-byte records storing TTL metadata, generational tags, intrusive wheel links, and back-pointers (`group_idx`, `slot_idx`) directly into the index.
   - **Packed Payload**: Single contiguous allocation for key + value, eliminating separate buffer allocations.
2. **Vectorized Probing**:
   - SSE2 SIMD instructions evaluate 16 slots concurrently in 1-2 CPU cycles.
   - Misses terminate on the first encountered empty slot.
   - A verified scalar 64-bit SWAR fallback guarantees identical bitmasks across platforms without false positives.
3. **64-bit ABA-Proof Stable Handles**:
   - Composed of a 32-bit generation counter + 32-bit arena index (`bhh_handle_make`).
   - Old handles are invalidated immediately upon deletion or upon post-expiration re-insertion (`bhh_set`).
4. **Multi-Turn Intrusive Timing Wheel**:
   - 1,024 buckets with 100 ms resolution (102.4 s horizon per lap).
   - Intrusive doubly-linked lists embedded directly in the arena records.
   - Back-pointers allow O(1) slot reclamation during expiration sweeps without hash table lookups.
   - Incremental budgeted purges (`bhh_purge_limit`, `bhh_purge_time_budget`) protect event loop latency.

---

## Independent Technical Audit & Hardening

An independent systems audit conducted in October 2026 identified several design and implementation issues, all of which have been resolved and verified with dedicated regression tests:

1. **Purge Cursor Resumption Bug (P0)**:
   - *Problem*: If an entry at `purge_record_cursor` was deleted via `bhh_del_handle()` during an incremental purge pause, the cursor became stale and pointed into the free-list, falsely incrementing the purged counter.
   - *Fix*: Implemented reactive unlinking in `bhh_wheel_unlink()` to advance the cursor, added generation validation on resumption, and conditioned the counter strictly on `bhh_del_handle() == 0`.
   - *Regression Test*: `test_cursor_regression.c` (Verified: `PASS`).
2. **SWAR Borrow-Overflow Bitmask Discrepancy (P1)**:
   - *Problem*: Classic SWAR zero-byte detection using subtraction (`x - 0x0101...`) suffered from borrow overflow across adjacent byte lanes (e.g. `ctrl[0]=0x00, ctrl[1]=0x01` produced mask `0x0003` under SWAR vs `0x0001` under SSE2).
   - *Fix*: Replaced borrow-prone subtraction with exact scalar byte extraction, verified across 100,000 randomized groups.
   - *Regression Test*: `test_swar_simd.c` (Verified: `PASS`, 100% mask equivalence).
3. **Handle Lifecycle on Post-Expiration SET (P1)**:
   - *Problem*: In-place overwrite of a logically expired key before physical purging reused the same generation, leaving stale handles valid.
   - *Fix*: Explicitly incremented record generation when overwriting an expired key, treating it as a new incarnation.
   - *Regression Test*: `test_handle_lifecycle.c` (Verified: `PASS`).
4. **Operational Equivalence in Benchmarks (P0)**:
   - *Problem*: Baseline `bk_get` allocated and copied memory on every call, whereas Hybrid returned an internal pointer.
   - *Fix*: Introduced audited benchmark (`bench_audited.c`) isolating Pure Hit Lookup (`exists`), Copy-Out (uniform `memcpy` into caller buffer), and Zero-Copy pointer resolution, with `fork()` process isolation for accurate RSS measurement.

---

## Preliminary Benchmark Summary (WSL2 / AMD Ryzen 5 8400F)

> [!NOTE]
> Tests executed on Linux 6.6 (Ubuntu 24.04), GCC 13.3.0 (`-O2`), with 1,000,000 keys (16-byte key, 32-byte value = 48 bytes payload). Median of 5 trials with process-isolated RSS.

| Engine | SET (Mops/s) | Pure Hit (Mops/s) | Copy-Out (Mops/s) | Zero-Copy (Mops/s) | DEL (Mops/s) | Delta RSS (MB) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Baseline (Chaining)** | 3.37 | 7.06 | 6.47 | N/A | 6.44 | 160.4 |
| **Candidate A (SIMD Flat)** | 3.87 | 6.13 | 6.15 | 6.17 | 6.09 | 133.7 |
| **Candidate B (Robin Hood)** | 3.40 | 5.36 | 5.47 | 5.58 | 3.99 | 113.2 |
| **Candidate C (Epoch)** | 3.51 | 5.74 | 5.61 | 5.65 | 3.81 | 121.2 |
| **BlinkHash Hybrid** | **5.15** | **9.20** | **8.80** | **9.00** | **7.96** | 175.6 |

*Disclaimer: Performance depends on memory subsystem bandwidth, cache topology, CPU architecture, and compiler flags. These numbers serve solely as directional research signals.*

---

## Known Limits & Open Research Questions

1. **Arena Sizing & Power-of-2 Boundaries**:
   Currently, `bhh_create` sizes the record arena to the full power-of-2 index capacity. When key counts lie just above a power-of-two boundary, substantial pre-allocated arena headroom remains idle (up to 51% of allocated RAM). Dynamic, incremental arena expansion via `realloc` is under active investigation.
2. **Page Fault Latency on First Allocation**:
   `bhh_create` initializes arena memory via `memset(0)`, forcing the kernel to commit pages immediately into RSS. Evaluating chunked or demand-paged zeroing could reduce initialization latency and memory pressure.
3. **Hash Function Seed**:
   Current prototypes use unseeded 64-bit FNV-1a. A cryptographically sound or seedable hash (e.g. SipHash or xxHash3) is necessary before production deployment against untrusted network inputs.

---

## Compiling and Running Experiments

All commands should be executed from the repository root:

### Run Regression & Unit Tests
```bash
# 1. Purge cursor regression test
gcc -std=c11 -Wall -Wextra -Iinclude -Iexperiments -fsanitize=address,undefined -g \
    src/hash.c experiments/blinkhash_hybrid.c experiments/test_cursor_regression.c -o build/test_cursor && ./build/test_cursor

# 2. SIMD vs SWAR mask equivalence test (100k trials)
gcc -std=c11 -Wall -Wextra -Iinclude -Iexperiments -fsanitize=address,undefined -g \
    src/hash.c experiments/blinkhash_hybrid.c experiments/test_swar_simd.c -o build/test_swar && ./build/test_swar

# 3. Handle lifecycle & post-TTL SET invalidation test
gcc -std=c11 -Wall -Wextra -Iinclude -Iexperiments -fsanitize=address,undefined -g \
    src/hash.c experiments/blinkhash_hybrid.c experiments/test_handle_lifecycle.c -o build/test_handle && ./build/test_handle
```

### Run Differential Correctness Verification
```bash
# 1,000,000-operation differential verification (Baseline vs A vs B vs C vs Hybrid)
gcc -std=c11 -Wall -Wextra -Iinclude -Iexperiments -fsanitize=address,undefined -O2 -g \
    src/hash.c src/store.c experiments/blinkhash_a.c experiments/blinkhash_b.c experiments/blinkhash_c.c \
    experiments/blinkhash_hybrid.c experiments/test_million_diff.c -o build/test_million && ./build/test_million
```

### Run 10-Million Operations Deep Stress Test
```bash
# 10,000,000 operations with 1,000,000 concurrent live keys, churn, and fault injection
gcc -std=c11 -Wall -Wextra -Iinclude -Iexperiments -fsanitize=address,undefined -O2 -g \
    src/hash.c experiments/blinkhash_hybrid.c experiments/test_stress_10m.c -o build/test_stress && ./build/test_stress
```

### Run Process-Isolated Audited Benchmark
```bash
gcc -std=c11 -Wall -Wextra -Iinclude -Iexperiments -O2 -march=native \
    src/hash.c src/store.c experiments/blinkhash_a.c experiments/blinkhash_b.c experiments/blinkhash_c.c \
    experiments/blinkhash_hybrid.c experiments/bench_audited.c -o build/bench_audited && ./build/bench_audited
```
