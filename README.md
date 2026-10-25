# BlinkKV

**An experimental, TTL-native in-memory key-value engine written in C11.**

> Redis is general-purpose. BlinkKV explores an engine where expiration is a first-class citizen.

BlinkKV is an experimental in-memory key-value store developed in C11, focused on volatile, short-lived workloads: session tokens, login OTPs, API rate-limiting buckets, temporary locks, and volatile cache entries.

> [!WARNING]
> **Project Status: Active Research & Development (Pre-Alpha).**  
> BlinkKV is **not production-ready**. The repository contains the hardened core key-value engine (Milestone 1). Advanced indexing architectures, TCP networking, and CLI tools are actively being researched and developed.

---

## Current Architecture & Status

Development is structured into milestone phases. We strictly distinguish between verified, working code and planned components:

### Implemented in Core (`main` branch — Milestone 1)
- **C11 In-Memory Engine**: Hash table with separate chaining and deterministic memory lifecycle (`src/store.c`, `src/hash.c`).
- **Explicit Error Model (`bk_status`)**: Predictable status codes (`BK_OK`, `BK_ERR_NOT_FOUND`, `BK_ERR_INVALID_ARG`, `BK_ERR_NOMEM`, `BK_ERR_INVALID_TTL`, `BK_ERR_CLOCK`).
- **Strict TTL Contract**: Zero TTL (`ttl_ms == 0`) is explicitly rejected with `BK_ERR_INVALID_TTL`; keys cannot be allocated into a pre-expired state.
- **Injectable Monotonic Clock**: Deterministic time injection (`bk_store_create_with_clock`) allowing multi-hour TTL scenarios to be tested instantly without sleeps.
- **Binary-Safe Storage**: Arbitrary byte arrays for both keys and values, handling embedded null bytes and zero-length buffers cleanly.
- **Hardened Test Harness**: Custom assertion harness running 18 comprehensive unit tests (`tests/test_store.c`) with 100% PASS under both standard compilation and `-fsanitize=address,undefined`.

### Research & High-Performance Prototypes (`research/blinkhash` branch)
A dedicated research initiative (**BlinkHash**) is exploring cache-friendly, SIMD-accelerated, and tombstone-free indexing:
- **Candidate A (Flat SIMD)**: 16-slot groups with 7-bit vector fingerprints (SwissTable style).
- **Candidate B (Robin Hood + Arena)**: Compact 8-byte index with backward-shift deletion and stable generational handles.
- **Candidate C (Epoch Timing Wheel)**: Intrusive doubly-linked epoch lists for O(1) bucket expiration draining.
- **BlinkHash Hybrid**: Two-level decoupled architecture combining SSE2/SWAR vectorized probing, 64-bit ABA-proof handles, and an intrusive multi-turn timing wheel.

> [!NOTE]
> BlinkHash Hybrid is currently an experimental prototype housed exclusively in the `research/blinkhash` branch for technical validation and benchmarking. It is **not yet integrated** into `src/store.c`.

### Planned for Subsequent Milestones
- **Event-Driven TCP Server**: Non-blocking I/O event loop (`src/net.c`, `src/main.c`).
- **Wire Protocols**: Minimal text protocol (`src/protocol.c`) and high-efficiency binary protocol.
- **CLI Client**: Standalone CLI tool (`cli/blinkkv-cli.c`).
- **Engine Migration**: Production integration of validated BlinkHash Hybrid into `src/store.c` (Milestone 2).

---

## Known Limitations & Open Engineering Items

In keeping with rigorous engineering transparency, the following characteristics and open items are documented:
1. **Memory Allocator Overhead**: In the Milestone 1 chaining store, each key requires 3 distinct heap allocations (`bk_entry`, key buffer, value buffer), incurring `ptmalloc` chunk header and 16-byte alignment overheads.
2. **Hybrid Power-of-2 Arena Headroom**: In the experimental Hybrid prototype, sizing up to the next power of 2 (`next_pow2`) can leave pre-allocated arena headroom when key counts fall just above a power-of-two boundary. Dynamic incremental growth is planned.
3. **Hash Function Seed**: The default hash function is 64-bit FNV-1a. Before opening TCP ports to untrusted network input, a seeded hash algorithm (e.g. SipHash or xxHash3) will be introduced to mitigate algorithmic HashDoS collision risks.

---

## Building and Testing

### Prerequisites
- POSIX-compliant environment (Linux, macOS, or WSL2 on Windows)
- C11 compiler (`gcc` or `clang`)
- GNU Make

### Build the Core Library
```bash
make
```
Produces `build/libblinkkv.a`.

### Run the Core Test Suite
```bash
make test
```

### Run Sanitizers (AddressSanitizer + UndefinedBehaviorSanitizer)
```bash
make sanitize
```

### Run Baseline Benchmark
```bash
make bench
```

### Clean Build Artifacts
```bash
make clean
```

---

## Roadmap

| Milestone | Scope | Status |
| :--- | :--- | :---: |
| **Milestone 1** | Hardened C11 Core Engine, `bk_status`, injectable clock, 18-test harness, ASan/UBSan clean | **Completed** |
| **BlinkHash R&D** | Hybrid prototype (SIMD + generational arena + timing wheel), differential & stress tests | **In Review (`research/blinkhash`)** |
| **Milestone 2** | BlinkHash Hybrid core integration into `src/store.c`, dynamic arena scaling | Planned |
| **Milestone 3** | Event loop daemon, TCP networking, text protocol, CLI client | Planned |
| **Milestone 4** | Expiration storm benchmarking, binary protocol, telemetry | Planned |

---

## License

MIT License. See [LICENSE](LICENSE) for details.