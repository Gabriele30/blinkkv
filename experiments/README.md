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
| **BlinkHash Hybrid** (`blinkhash_hybrid`) | Hardened Decoupled Dynamic Hybrid | 80-byte SIMD index groups (16B ctrl + 64B handles) + Decoupled Dynamic Arena | SSE2 vector probing (with verified 64-bit SWAR fallback) | Multi-turn intrusive timing wheel with O(1) backpointer unlinking |

---

## BlinkHash Hybrid: Deep Architectural Details

BlinkHash Hybrid combines vector-accelerated group probing with stable handle indirection, intrusive temporal ordering, and dynamically decoupled memory pools:

1. **Two-Level Decoupled Architecture**:
   - **80-byte Index Groups (`bhh_group`)**: 16 bytes of 7-bit metadata fingerprints (`ctrl`) + 64 bytes of 32-bit arena handles (`handles`). Index cost is strictly **5.0 bytes per slot**.
   - **Decoupled Dynamic Record Arena (`bhh_record`)**: 48-byte records storing TTL metadata, generational tags, intrusive wheel links, and back-pointers (`group_idx`, `slot_idx`) directly into the index.
   - **Packed Payload**: Single contiguous allocation for key + value, eliminating separate buffer allocations.
2. **Decoupled Arena Capacity (Milestone M2.1)**:
   - Arena capacity is strictly independent of index capacity. Index groups scale on power-of-two boundaries for mask-based indexing, while the arena starts conservatively (`BHH_INITIAL_ARENA_CAP = 64`) and grows geometrically (1.5x by default).
   - Only newly allocated slots are zeroed, preventing premature physical memory pagination.
   - Recycled slots in the intrusive free-list (`first_free`) are strictly prioritized, resulting in zero reallocations during key churn.
   - Comprehensive rollback guarantees: if `realloc()` fails, the table remains 100% uncorrupted with zero data loss.
3. **Vectorized Probing**:
   - SSE2 SIMD instructions evaluate 16 slots concurrently in 1-2 CPU cycles.
   - Misses terminate on the first encountered empty slot.
   - A verified scalar 64-bit SWAR fallback guarantees identical bitmasks across platforms without false positives.
4. **64-bit ABA-Proof Stable Handles**:
   - Composed of a 32-bit generation counter + 32-bit arena index (`bhh_handle_make`).
   - Old handles are invalidated immediately upon deletion or upon post-expiration re-insertion (`bhh_set`).
5. **Multi-Turn Intrusive Timing Wheel**:
   - 1,024 buckets with 100 ms resolution (102.4 s horizon per lap).
   - Intrusive doubly-linked lists embedded directly in the arena records.
   - Back-pointers allow O(1) slot reclamation during expiration sweeps without hash table lookups.
   - Incremental budgeted purges (`bhh_purge_limit`, `bhh_purge_time_budget`) protect event loop latency.

---

## Arena Growth Strategy: 1.5× vs 2.0× Empirical Evaluation

During Milestone M2.1, 1.5× and 2.0× geometric expansion factors were benchmarked across 100k, 300k, and 1M keys from a 1,024-slot start:

| Scale | Strategy | Final Arena Cap | Unused Slots (Waste) | Reallocs | Cumulative Realloc Time | Peak RSS |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **100k** | 1.5× | 132,859 | 32,858 (24.7%) | 12 | 0.05 ms | **14.56 MB** |
| **100k** | 2.0× | 131,072 | 31,071 (23.7%) | 7 | 0.04 ms | 14.31 MB |
| **300k** | **1.5× (Chosen)** | **448,398** | **148,397 (33.1%)** | **15** | **0.08 ms** | **54.00 MB** |
| **300k** | 2.0× | 524,288 | 224,287 (42.8%) | 9 | 0.08 ms | 54.00 MB |
| **1M** | **1.5× (Chosen)** | **1,008,895** | **8,894 (0.9%)** | **17** | **0.11 ms** | **118.31 MB** |
| **1M** | 2.0× | 1,048,576 | 48,575 (4.6%) | 10 | 0.07 ms | 120.06 MB |

**Findings**: 1.5× geometric growth achieves substantially tighter memory bounds (wasting only 0.9% of the arena at 1M keys vs 4.6% with 2.0×, and 33.1% at 300k vs 42.8% with 2.0×). Cumulative reallocation time across 1,000,000 keys is negligible (0.11 ms total, or ~0.1 ns/key), while peak SET spikes remain well within event-loop tolerance. 1.5× is set as the default policy.

---

## Audited Comparative Benchmark Summary (WSL2 / AMD Ryzen 5 8400F)

> [!NOTE]
> Tests executed on Linux 6.6 (Ubuntu 24.04), GCC 13.3.0 (`-O2`), with 1,000,000 keys (16-byte key, 32-byte value = 48 bytes payload). Median of 5 trials with complete process isolation via `fork()`.

| Engine | SET (Mops/s) | Pure Hit (Mops/s) | Copy-Out (Mops/s) | Zero-Copy (Mops/s) | DEL (Mops/s) | Mem/Key | Delta RSS/Key |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Baseline (Chaining)** | 3.02 | 6.34 | 5.90 | N/A | 6.04 | 96.0 B | 160.4 B |
| **Candidate A (SIMD Flat)** | 3.19 | 5.93 | 6.03 | 5.74 | 5.71 | 117.2 B | 133.7 B |
| **Candidate B (Robin Hood)** | 3.71 | 5.99 | 5.85 | 5.92 | 4.24 | 131.9 B | 113.2 B |
| **Candidate C (Epoch)** | 3.60 | 5.99 | 6.15 | 6.01 | 4.11 | 148.7 B | 121.2 B |
| **Hybrid (M2.1 Decoupled)** | **4.77** | **10.74** | **11.29** | **10.78** | **9.18** | **86.1 B** | **126.6 B** |

*Disclaimer: Performance depends on memory subsystem bandwidth, cache topology, CPU architecture, and compiler flags. These numbers serve solely as directional research signals.*

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

# 4. Decoupled dynamic arena growth & fault injection test (Milestone M2.1)
gcc -std=c11 -Wall -Wextra -Iinclude -Iexperiments -fsanitize=address,undefined -g \
    src/hash.c experiments/blinkhash_hybrid.c experiments/test_arena_growth.c -o build/test_arena && ./build/test_arena
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
