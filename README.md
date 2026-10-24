# lsm-engine-c
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT) [![CI](https://github.com/sohamshewani/lsm-engine-c/actions/workflows/ci.yml/badge.svg)](https://github.com/sohamshewani/lsm-engine-c/actions) [![Security: Hardened](https://img.shields.io/badge/Security-Hardened-blue.svg)](https://github.com/sohamshewani/lsm-engine-c) [![Stars](https://img.shields.io/github/stars/sohamshewani/lsm-engine-c?style=flat)](https://github.com/sohamshewani/lsm-engine-c)

## Executive Summary & Problem Space

`lsm-engine-c` is a production-grade, embeddable Log-Structured Merge-tree (LSM) storage engine engineered from the ground up in modern C (C11). Designed for high-throughput, latency-sensitive distributed systems, it addresses the fundamental mechanical limitations of traditional in-place update databases (e.g., B-Trees) by transforming random disk I/O into sequential write operations.

In high-write workloads, traditional storage engines suffer from devastating write amplification and lock contention on page caches. `lsm-engine-c` resolves these bottlenecks through:
- **Append-Only Write Paths:** High-frequency mutations bypass immediate disk seeks by streaming directly to a Write-Ahead Log (`src/lsm_wal.c`) and an in-memory mutable buffer (`src/lsm_memtable.c`).
- **Zero-Copy Serialization & Bounded Allocations:** Custom memory arenas and slab allocators (`src/lsm_alloc.c`) eliminate heap fragmentation and minimize syscall overhead.
- **Background Compaction Pipelines:** Non-blocking multi-level compaction (`src/lsm_compaction.c`) reorganizes immutable Sorted String Tables (`src/lsm_sstable.c`) to bound read amplification and reclaim reclaimed storage space deterministically.

---

## Structural ASCII Architecture / Dataflow Diagram

```
 +-------------------------------------------------------------------------+
 |                              Client Thread                              |
 +-------------------------------------------------------------------------+
       |                                           |
       | (1) Put(Key, Value)                       | (2) Get(Key)
       v                                           v
 +-----------------------+               +---------------------------------+
 |      lsm_wal.c        |               |        lsm_memtable.c           |
 | (Append-Only Log Sync)|               | (Concurrent SkipList / Buffer)  |
 +-----------------------+               +---------------------------------+
       |                                           |
       | (Flush)                                   | (Miss / Fallback)
       v                                           v
 +-------------------------------------------------------------------------+
 |                           lsm_sstable.c (L0 - Ln)                       |
 |  +-------------------+  +-------------------+  +---------------------+  |
 |  | Bloom Filters     |  | Index Blocks      |  | Data Blocks         |  |
 |  +-------------------+  +-------------------+  +---------------------+  |
 +-------------------------------------------------------------------------+
       ^                                           |
       |                                           | (Background Worker)
       +-------------------------------------------+
                     lsm_compaction.c
           (Size-Tiered / Leveled Merge Pipeline)
```

---

## Concurrency Model & Memory Invariants

`lsm-engine-c` employs a fine-grained concurrency model optimized for multicore architectures:
- **Lock-Free Read Paths:** Reads traverse immutable snapshots of the memtable and SSTables without acquiring global mutexes, leveraging atomic pointer swaps and memory barriers (`stdatomic.h`).
- **Mutex-Protected Write Frontiers:** The active memtable utilizes lightweight read-write locks or mutexes strictly scoped to memory insertion boundaries, immediately yielding to background flushing workers when capacity thresholds are reached.
- **Memory Ownership & Arenas (`src/lsm_alloc.c`):** All runtime blocks are allocated via thread-local or arena-based allocators. Explicit lifecycle ownership transfer ensures zero dangling pointers: memory allocated during compaction is strictly scoped to worker threads and freed atomically upon descriptor detachment.

---

## Performance Characteristics & Algorithmic Complexity Table

| Operation | Average Case | Worst Case | Operational Characteristics & Latency Estimates |
| :--- | :--- | :--- | :--- |
| **Write (`Put`)** | $O(1)$ | $O(\log N)$ | Append to WAL and insert into Memtable. Latency: $< 15\mu\text{s}$ (with fsync batching). |
| **Read (`Get`)** | $O(\log N)$ | $O(D \cdot \log N)$ | Memtable check $\rightarrow$ Bloom filter check $\rightarrow$ SSTable binary search. Latency: $10 - 50\mu\text{s}$. |
| **Delete** | $O(1)$ | $O(\log N)$ | Tombstone record appended to memtable; resolved during compaction. |
| **Range Scan** | $O(K + \log N)$ | $O(K + D \cdot \log N)$ | Iterator merge across memtable and active SSTables ($K$ = keys returned). |
| **Compaction** | $O(N)$ | $O(N \log N)$ | Background multi-way merge sort of SSTables, bounded I/O bandwidth. |

---

## Security Architecture & Threat Model

Designed to operate in untrusted environments and multi-tenant nodes, `lsm-engine-c` incorporates strict defensive programming paradigms:
- **Input Boundaries & Sanitization:** All incoming byte arrays, keys, and values are strictly bounded by explicit length parameters defined in `include/lsm_types.h`. No raw string operations (`strcpy`, `sprintf`) are permitted.
- **Memory Corruption Protections:** Buffer overflows are mitigated via static bounds checking, explicit allocator guards, and rigorous use of safe memory copy primitives (`memcpy_s` equivalents).
- **Fault Tolerance & Crash Consistency:** The WAL (`src/lsm_wal.c`) enforces atomic durability boundaries using checksum-verified log records, guaranteeing zero data corruption upon unexpected power loss or process termination.

---

## Build, Verification & Benchmarking Recipes

To compile the engine, execute the test suite, and run the micro-benchmark suite, use the exact terminal recipes below.

### Prerequisites
- POSIX-compliant system (Linux/macOS)
- GCC (>= 11.0) or Clang (>= 13.0)
- GNU Make

### 1. Compile the Engine and Test Suite
```bash
make clean
make all
```

### 2. Execute Invariant and Unit Tests
Verify structural safety, concurrency correctness, and memory integrity:
```bash
make test
./tests/test_invariants
```

### 3. Run Performance Benchmarks
Execute throughput and latency profiling across read/write workloads:
```bash
make benchmark
./benchmarks/bench_engine --threads=4 --operations=1000000