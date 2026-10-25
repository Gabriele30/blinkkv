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

## 4-Phase Synchronized Lifecycle Memory Snapshots (Milestone M2.1 Audited Methodology)

To eliminate desynchronized state comparisons, memory and capacities are captured in **four distinct lifecycle snapshots** across 5 fork-isolated rounds, comparing BlinkHash Hybrid M2.1 (1.5x dynamic growth) against Baseline (`bk_store`) under identical active keys and 48-byte payloads (16-byte key, 32-byte value):

1. **Store Vuoto**: Immediately after table creation.
2. **N Chiavi Vive**: Fully populated working set.
3. **N/2 Vive (dopo DELETE)**: Half of the keys deleted.
4. **N Vive (Reinserimento)**: Re-insertion of the deleted keys into recycled slots.

Slots are strictly classified into **Mai Allocati (Headroom)** vs **Free-List (Recycled)**:

### 1,000,000 Keys Lifecycle Breakdown

| Engine | Fase Lifecycle | Live Keys | SIMD Cap | Arena Cap | Mai Alloc. | Free-List | Tombstone | Payload Act. | Mem Interna | RSS Reale | Overhead/Key |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Hybrid M2.1** | 1. Store Vuoto | 0 | 2,097,152 | 64 | 63 | 0 | 0 | 0.00 MB | 10.01 MB | 11.00 MB | 0.0 B |
| **Hybrid M2.1** | 2. N Chiavi Vive | 1,000,000 | 2,097,152 | 1,076,167 | 76,166 | 0 | 0 | 45.78 MB | 105.04 MB | 129.20 MB | **62.1 B** |
| **Hybrid M2.1** | 3. N/2 Vive (DELETE) | 500,000 | 2,097,152 | 1,076,167 | 76,166 | 500,000 | 500,000 | 22.89 MB | 82.16 MB | 98.81 MB | 124.3 B |
| **Hybrid M2.1** | 4. N Vive (Reinserito)| 1,000,000 | 2,097,152 | 1,076,167 | 76,166 | 0 | 0 | 45.78 MB | 105.04 MB | 132.93 MB | **62.1 B** |
| **Baseline** | 1. Store Vuoto | 0 | 2,000,000 | N/A | 0 | 0 | 0 | 0.00 MB | 15.26 MB | 1.00 MB | 0.0 B |
| **Baseline** | 2. N Chiavi Vive | 1,000,000 | 2,000,000 | N/A | 0 | 0 | 0 | 45.78 MB | 114.44 MB | 161.48 MB | 72.0 B |
| **Baseline** | 3. N/2 Vive (DELETE) | 500,000 | 2,000,000 | N/A | 0 | 0 | 0 | 22.89 MB | 64.85 MB | 169.11 MB | 88.0 B |
| **Baseline** | 4. N Vive (Reinserito)| 1,000,000 | 2,000,000 | N/A | 0 | 0 | 0 | 45.78 MB | 114.44 MB | 169.11 MB | 72.0 B |

**Key State-Equivalence Takeaways**:
- **Real RAM Savings vs Baseline**: At 1,000,000 keys, Hybrid uses **129.20 MB RSS vs 161.48 MB Baseline (-20.0% RAM reduction)**. At 300,000 keys, Hybrid uses **41.46 MB RSS vs 49.32 MB Baseline (-15.9% RAM reduction)**.
- **Strict Free-List Recycling**: In Phase 3, 500,000 deleted slots move to the free-list (`freelist_count = 500,000`). In Phase 4, all 500,000 slots are recycled without growing the arena (`never_allocated_slots` remains constant at 76,166).

---

## Real Arena Growth Benchmark: 1.5× vs 2.0×

Executed **strictly through the public API** (`bhh_set_growth_policy` and `bhh_set`), measuring the complete SET latency including internal reallocations across 5 rounds with strictly alternating execution order:

| Scale | Strategy | Reallocs | Subject Bytes | Final Arena Cap | Headroom (Unused) | Internal Mem | Peak RSS (VmHWM) | SET QPS | p50 | p95 | p99 | p99.9 | Max Spike |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **100k** | 1.5× (M2.1) | 20 | 12.87 MB | 141,718 | 41,717 | 12.32 MB | 16.38 MB | 4.71 Mops/s | 0.05 µs | 0.13 µs | 1.85 µs | 4.63 µs | 1.20 ms |
| **100k** | 2.0× | 12 | 12.00 MB | 131,072 | 31,071 | 11.83 MB | 15.88 MB | 5.10 Mops/s | 0.05 µs | 0.11 µs | 1.83 µs | 4.48 µs | 1.67 ms |
| **300k** | **1.5× (M2.1)** | **21** | **29.19 MB** | **318,865** | **18,864** | **33.33 MB** | **43.38 MB** | **4.38 Mops/s** | **0.05 µs** | **0.16 µs** | **1.88 µs** | **4.60 µs** | **2.77 ms** |
| **300k** | 2.0× | 13 | 24.00 MB | 524,288 | 224,287 | 42.74 MB | 52.88 MB | 4.07 Mops/s | 0.07 µs | 0.20 µs | 1.89 µs | 4.53 µs | 6.89 ms |
| **1M** | **1.5× (M2.1)** | **24** | **98.52 MB** | **1,076,167** | **76,166** | **105.04 MB** | **136.50 MB** | **3.65 Mops/s** | **0.11 µs** | **0.23 µs** | **1.94 µs** | **5.04 µs** | **10.94 ms** |
| **1M** | 2.0× | 14 | 48.00 MB | 1,048,576 | 48,575 | 103.78 MB | 135.25 MB | 3.92 Mops/s | 0.11 µs | 0.23 µs | 1.92 µs | 4.59 µs | 13.47 ms |

> [!NOTE]
> **Architectural Note on Realloc Non-Determinism**:  
> "Subject Bytes" indicates the cumulative sum of existing arena buffers passed to `realloc()`. Under Linux/glibc, large allocations (>128 KB) utilize `mremap(MREMAP_MAYMOVE)`. If adjacent virtual address space is free or can be remapped at the page-table level, the kernel updates virtual mappings without physically copying memory pages on the CPU. Reallocation latency is inherently non-deterministic and depends on address space fragmentation and kernel page-table topology.

---

## Architectural Trade-Off Analysis: M2 (Static Preallocated) vs M2.1 (Decoupled Dynamic)

A complete empirical comparison between M2 (statically coupled preallocated arena) and M2.1 (decoupled dynamic geometric arena) illuminates the precise engineering trade-offs:

| Metric / Dimension | M2 (Static Preallocation) | M2.1 (Decoupled Dynamic 1.5x) | Trade-Off Impact |
| :--- | :---: | :---: | :--- |
| **Internal Memory (300k keys)** | 66.74 MB | **33.33 MB** | **-50.1% RAM reduction** |
| **Peak RSS (300k keys)** | 76.88 MB | **43.38 MB** | **-43.6% RAM reduction (-33.4 MB)** |
| **Internal Memory (1M keys)** | 151.78 MB | **105.04 MB** | **-30.8% RAM reduction** |
| **Peak RSS (1M keys)** | 183.25 MB | **136.50 MB** | **-25.5% RAM reduction (-46.7 MB)** |
| **Populate SET Throughput (300k)**| 4.70 Mops/s | 4.21 Mops/s | -10.4% during initial expansion |
| **Populate SET p50 / p95** | 0.06 µs / 0.18 µs | 0.06 µs / 0.18 µs | Identical fast path (0% change) |
| **Populate SET p99 (Ramp-up Tail)**| 2.06 µs | 2.04 µs | Controlled tail during reallocations |
| **Populate SET Worst Spike** | 48.66 µs (300k) / 248.68 µs (1M)| **3,007 µs (300k) / 9,764 µs (1M)**| **Reallocation pause spikes** |
| **Steady-State GET Throughput** | 7.17 Mops/s | 7.18 Mops/s | **100% equivalent** |
| **Steady-State GET p99** | **0.25 µs** | **0.26 µs** | **Sub-microsecond tail** |
| **Continuous Churn SET QPS** | 3.67 Mops/s | 4.10 Mops/s | +11.7% throughput |
| **Continuous Churn Reallocs** | 0 | **0** | **Zero reallocations under churn** |
| **Continuous Churn Memory Growth** | 0.00 MB | **0.00 MB** | **Strictly bounded memory** |

### Explicit Systems Trade-Off Summary
1. **The Cost**: Dynamic geometric growth introduces reallocation pauses during initial population. While median latency (`p50 = 0.06 µs`) and fast-path lookup are completely unaffected, individual SET operations that trigger `realloc()` suffer latency spikes (worst latency reaches ~3.0 ms at 300k and ~9.8 ms at 1M keys).
2. **The Benefit**: Eliminates the severe over-allocation of static coupling, cutting RSS by up to **-43.6% to -50.1%**, and bringing BlinkHash Hybrid from +13.5% above baseline to **-20.0% below baseline RSS**.
3. **Steady-State & Churn Purity**: Once populated to the working set size, tail latency returns immediately to **0.26 µs** (10x faster than during ramp-up). During continuous key churn (delete + insert), the intrusive free-list absorbs 100% of mutations with zero additional reallocations and zero memory drift.

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

### Run Audited Benchmarks
```bash
# 1. Audited 4-phase lifecycle memory benchmark (Baseline vs Hybrid M2.1)
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -march=native -Iinclude -Isrc -Iexperiments \
    src/hash.c src/store.c experiments/blinkhash_hybrid.c experiments/bench_m21_measure.c -o build/bench_m21_measure && ./build/bench_m21_measure

# 2. Real arena growth benchmark (1.5x vs 2.0x public API)
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -march=native -Iinclude -Isrc -Iexperiments \
    src/hash.c experiments/blinkhash_hybrid.c experiments/bench_arena_growth_cmp.c -o build/bench_arena_growth_cmp && ./build/bench_arena_growth_cmp

# 3. M2 vs M2.1 architectural trade-off validation
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -march=native -Iinclude -Isrc -Iexperiments \
    src/hash.c experiments/blinkhash_hybrid.c experiments/bench_m2_vs_m21_tradeoff.c -o build/bench_m2_vs_m21_tradeoff && ./build/bench_m2_vs_m21_tradeoff
```

