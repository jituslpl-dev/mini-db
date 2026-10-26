# mini-db: A From-Scratch Relational Database Management System in C++17

[![CI](https://github.com/jituslpl-dev/mini-db/actions/workflows/ci.yml/badge.svg)](https://github.com/jituslpl-dev/mini-db/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**mini-db** is a production-quality, from-scratch relational database engine
inspired by PostgreSQL, SQLite, and the BusTub teaching database. It implements
a complete storage engine with a buffer pool, a concurrent B+ tree index, a
Strict 2PL transaction manager with deadlock detection, a Volcano-model query
execution engine, a SQL parser, Write-Ahead Logging (WAL) with ARIES-style
crash recovery, and an interactive CLI.

This project demonstrates advanced systems programming suitable for software
engineering interviews and resume showcase.

---

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [Quick Start](#quick-start)
- [SQL Support](#sql-support)
- [Storage Engine](#storage-engine)
- [Buffer Pool Design](#buffer-pool-design)
- [B+ Tree Index](#b-tree-index)
- [Lock Manager Design](#lock-manager-design)
- [Transaction Lifecycle](#transaction-lifecycle)
- [Query Execution Pipeline](#query-execution-pipeline)
- [WAL and Recovery](#wal-and-recovery)
- [Benchmark Report](#benchmark-report)
- [Project Structure](#project-structure)
- [Building and Testing](#building-and-testing)
- [Developer Tooling](#developer-tooling)

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                        CLI Shell                         │
│                   (Interactive SQL REPL)                  │
├─────────────────────────────────────────────────────────┤
│  SQL Parser  ──►  Planner/Optimizer  ──►  Executor Tree │
│  (Tokenizer +     (Predicate/Proj       (Volcano Model   │
│   Recursive       Pushdown, Index        Iterator Pipe)   │
│   Descent)        Selection)                              │
├──────────┬──────────┬──────────┬──────────┬──────────────┤
│ Catalog  │  Lock    │ Trans-   │   WAL    │   Recovery   │
│ (Tables, │ Manager  │ action   │ Log Mgr  │   Manager    │
│  Indexes,│ (S2PL,   │ Manager  │ (LSN,    │ (Redo/Undo)  │
│  Schema) │ Deadlock)│          │  Buffer) │              │
├──────────┴──────────┴──────────┴──────────┴──────────────┤
│                    Buffer Pool Manager                    │
│        (LRU-K Replacement, Pin/Unpin, Dirty Pages)       │
├──────────────────────────────────────────────────────────┤
│                      Disk Manager                        │
│          (Page Allocation, Read/Write, Persistence)      │
├──────────────────────────────────────────────────────────┤
│                        Disk File                         │
└──────────────────────────────────────────────────────────┘
```

### System Architecture (Mermaid)

```mermaid
graph TB
    CLI[CLI Shell]
    Parser[SQL Parser]
    Planner[Planner / Optimizer]
    Executor[Execution Engine]
    Catalog[Catalog]
    LockMgr[Lock Manager]
    TxnMgr[Transaction Manager]
    WAL[Log Manager]
    Recovery[Recovery Manager]
    BPM[Buffer Pool Manager]
    LRU[LRU-K Replacer]
    Disk[Disk Manager]
    File[(Disk File)]
    BTree[B+ Tree Index]
    Heap[Heap Table]

    CLI --> Parser
    Parser --> Planner
    Planner --> Executor
    Executor --> Catalog
    Executor --> BTree
    Executor --> Heap
    Executor --> TxnMgr
    TxnMgr --> LockMgr
    Executor --> WAL
    WAL --> Recovery
    BTree --> BPM
    Heap --> BPM
    BPM --> LRU
    BPM --> Disk
    Disk --> File
    WAL --> File
```

---

## Quick Start

```bash
# Clone and build
cd mini-db
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run the interactive shell
./minidb_cli

# Or pipe a SQL script
./minidb_cli < ../examples/demo.sql

# Run tests
ctest --output-on-failure

# Run benchmarks
./bench_insert
```

### Interactive Session

```
$ ./minidb_cli mydb.db
mini-db RDBMS v1.0
Database: mydb.db
Type 'help' for commands, 'exit' to quit.

db> CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER);
CREATE TABLE users (id=1)

db> INSERT INTO users VALUES (1, 'Alice', 30);
INSERT 1 row(s)

db> INSERT INTO users VALUES (2, 'Bob', 25);
INSERT 1 row(s)

db> SELECT * FROM users;
id | name | age
---+------+----
1 | Alice | 30
2 | Bob | 25
(2 row(s))

db> SELECT * FROM users WHERE age > 28;
id | name | age
---+------+----
1 | Alice | 30
(1 row(s))

db> EXPLAIN SELECT * FROM users WHERE age > 28;
QUERY PLAN
----------
SeqScan(users)
  -> Filter(WHERE)
  -> Projection

db> exit
```

---

## SQL Support

| Statement | Syntax |
|-----------|--------|
| CREATE TABLE | `CREATE TABLE name (col TYPE, ...);` |
| DROP TABLE | `DROP TABLE name;` |
| INSERT | `INSERT INTO name VALUES (v, ...);` |
| DELETE | `DELETE FROM name WHERE expr;` |
| UPDATE | `UPDATE name SET col = val WHERE expr;` |
| SELECT | `SELECT cols FROM table [JOIN table ON expr] [WHERE expr] [GROUP BY cols] [ORDER BY cols] [LIMIT n];` |
| EXPLAIN | `EXPLAIN SELECT ...;` |

**Supported types:** `INTEGER`, `VARCHAR` / `TEXT`

**Supported operators in WHERE:** `=`, `!=`, `<`, `<=`, `>`, `>=`, `AND`, `OR`, `NOT`

---

## Storage Engine

### Page-Based Storage

The database file is divided into fixed-size **pages** (default 4096 bytes). Each
page has a unique `page_id_t`. Page 0 is reserved for metadata (page count,
free list head).

```
┌─────────────────────────────────────────┐
│ Page Header (page_id, pin_count, dirty) │  16 bytes
├─────────────────────────────────────────┤
│                                         │
│           Payload (4080 bytes)          │
│                                         │
└─────────────────────────────────────────┘
```

### Disk Manager

The `DiskManager` handles:
- **Page allocation:** Maintains a free-list; allocates new pages by extending
  the file or reusing freed pages.
- **Read/Write:** Direct file I/O with `pread`/`pwrite` semantics.
- **Persistence:** Metadata (page count, next page id) stored in page 0.
- **Deallocation:** Freed pages are added to the free list for reuse.

### Heap Table

Tuples are stored in **heap pages**. Each heap page has a header with:
- `slot_count`: number of slots.
- `free_space_offset`: where free space begins.
- A slot directory mapping slot index to tuple offset/length.

Tuples are serialized as: `[4-byte length][value1][value2]...`. Each value is
`[1-byte type][data]`.

---

## Buffer Pool Design

The `BufferPoolManager` caches pages in memory. Key features:

### LRU-K Replacement

Instead of simple LRU, we use **LRU-K** (default K=2). Each frame tracks its
last K access timestamps. Eviction selects the frame whose K-th most recent
access is the oldest (back in time the furthest). Frames with fewer than K
accesses are treated as having an access time of `-∞` (evicted first).

```
LRU-K Eviction Logic:
  - For each evictable frame, look at its K-th access timestamp.
  - Evict the frame with the oldest K-th access.
  - Frames with <K accesses are prioritized for eviction (history list).
```

### Pin/Unpin and Dirty Tracking

- **Pin:** When a page is fetched, its frame is pinned (non-evictable).
- **Unpin:** When done, the caller unpins; if dirty, the page is marked for
  eventual flush.
- **Dirty pages:** Flushed to disk either on eviction or explicit `flushAllPages()`.
- **Asynchronous flushing:** A background thread periodically flushes dirty pages.

### Buffer Pool Flow (Mermaid)

```mermaid
flowchart TD
    A[fetchPage request] --> B{Page in pool?}
    B -- Yes --> C[Pin frame, record access]
    C --> D[Return page pointer]
    B -- No --> E{Free frame?}
    E -- Yes --> G[LRU-K evict victim]
    E -- No --> G
    G --> F{Victim dirty?}
    F -- Yes --> H[Write to disk]
    F -- No --> I[Read page from disk]
    H --> I
    I --> C
    D --> J[Caller uses page]
    J --> K[unpinPage - dirty?]
    K -- Yes --> L[Mark dirty]
    K -- No --> M[Done]
    L --> M
```

---

## B+ Tree Index

### Design

The B+ tree is a **clustered** index mapping key values to row IDs (RIDs). Key
features:

- **Variable-length keys:** Keys are serialized `IndexKey` structs (value + RID).
- **Node layout:** Header + offset table + key data area + child/RID pointers.
- **Leaf nodes:** Store `(key, rid)` pairs; linked via `next_leaf` pointers for
  range scans.
- **Internal nodes:** Store routing keys and child page pointers.
- **Split:** When a node overflows, it splits in half; the middle key is pushed
  to the parent.
- **Root updates:** When the root splits, a new root is created.

### Concurrent Access

The tree uses a **shared_mutex** (`root_latch_`) for structural integrity:
- **Readers** (search, range scan): acquire a shared lock.
- **Writers** (insert, delete): acquire an exclusive lock.

This is simpler than full latch crabbing but ensures correctness. A production
implementation would use **lock coupling** (latch crabbing): each thread holds
latches on at most parent + child, releasing the parent once the child is
latched as "safe."

### B+ Tree Operations (Mermaid)

```mermaid
flowchart TD
    A[Insert key] --> B[Find leaf via tree descent]
    B --> C{Leaf has space?}
    C -- Yes --> D[Insert in sorted position]
    C -- No --> E[Split leaf]
    E --> F[Create new leaf, redistribute keys]
    F --> G[Push middle key to parent]
    G --> H{Parent has space?}
    H -- Yes --> I[Insert key in parent]
    H -- No --> J[Split internal node]
    J --> K[Recursively push key up]
    K --> L{Was root split?}
    L -- Yes --> M[Create new root]
    L -- No --> N[Done]
    I --> N
    D --> N
    M --> N
```

---

## Lock Manager Design

The `LockManager` implements **Strict Two-Phase Locking (Strict 2PL)**:

### Lock Modes

| Mode | IS | IX | S | SIX | X |
|------|----|----|---|-----|---|
| IS   | ✓  | ✓  | ✓ | ✓   | ✗ |
| IX   | ✓  | ✓  | ✗ | ✗   | ✗ |
| S    | ✓  | ✗  | ✓ | ✗   | ✗ |
| SIX  | ✓  | ✗  | ✗ | ✗   | ✗ |
| X    | ✗  | ✗  | ✗ | ✗   | ✗ |

### Lock Hierarchy

Locks follow a **multiple-granularity** hierarchy:
```
Table → Tuple (RID)
```
- Table-level locks use intention modes (IS, IX, SIX).
- Tuple-level locks use S or X.

### Lock Upgrades

A transaction holding an S lock can **upgrade** to X if no other transaction
holds a conflicting lock.

### Deadlock Detection

A **wait-for graph** is maintained: if transaction T1 waits for a lock held by
T2, an edge `T1 → T2` is added. A **background thread** runs cycle detection
every 50ms via DFS. When a cycle is found, a victim transaction is selected and
aborted (its lock requests fail, triggering automatic rollback).

```mermaid
graph LR
    T1 -->|waits for| T2
    T2 -->|waits for| T1
    style T1 fill:#f99
    style T2 fill:#f99
```

---

## Transaction Lifecycle

```mermaid
stateDiagram-v2
    [*] --> GROWING: BEGIN
    GROWING --> GROWING: Acquire locks
    GROWING --> ABORTED: Deadlock victim / Error
    GROWING --> SHRINKING: COMMIT
    SHRINKING --> COMMITTED: Release all locks
    ABORTED --> [*]: Rollback writes
    COMMITTED --> [*]
```

**Strict 2PL Rules:**
1. A transaction must acquire all locks before releasing any (Growing phase).
2. All locks are held until **commit or abort** (no early release).
3. On abort, the write set is rolled back in reverse order.

---

## Query Execution Pipeline

The execution engine uses the **Volcano model** (iterator model). Each operator
implements `init()` and `next()`. Operators are composed into a tree; each pulls
tuples from its children.

### Operators

| Operator | Description |
|----------|-------------|
| `SeqScanExecutor` | Sequential scan of a heap table |
| `IndexScanExecutor` | Point lookup via B+ tree index |
| `FilterExecutor` | Applies a WHERE predicate (selection) |
| `ProjectionExecutor` | Selects/reorders columns |
| `NestedLoopJoinExecutor` | Nested-loop join with condition |
| `HashJoinExecutor` | Hash-based equi-join |
| `AggregateExecutor` | GROUP BY with COUNT |
| `SortExecutor` | In-memory sort |
| `LimitExecutor` | LIMIT n |

### Example Plan

For `SELECT name FROM users WHERE age > 25 ORDER BY name LIMIT 10`:

```
Limit(10)
  └── Sort(name ASC)
        └── Projection(name)
              └── Filter(age > 25)
                    └── SeqScan(users)
```

### Optimization

The planner applies:
- **Predicate pushdown:** WHERE filters are placed directly above the scan.
- **Projection pushdown:** Only needed columns are projected early.
- **Index selection:** If WHERE has `col = literal` and an index exists on `col`,
  an `IndexScan` replaces `SeqScan + Filter`.

---

## WAL and Recovery

### Write-Ahead Logging

Every modification is logged before the data page is written. Log records:
- **BEGIN / COMMIT / ABORT:** Transaction lifecycle markers.
- **INSERT / DELETE / UPDATE:** Data modifications with old and new values.
- **CHECKPOINT:** Fuzzy checkpoint marker.

Each record has a monotonically increasing **Log Sequence Number (LSN)**. The
log is buffered in memory and flushed:
- On transaction commit (force-at-commit).
- When the buffer exceeds `WAL_BUFFER_SIZE`.

### ARIES Recovery

On startup, the `RecoveryManager` runs three phases:

```mermaid
flowchart LR
    A[Analysis Phase] --> B[REDO Phase]
    B --> C[UNDO Phase]
    
    A1[Scan log forward] --> A2[Identify committed/active txns]
    B1[Replay all modifications] --> B2[Reconstruct disk state]
    C1[For active txns] --> C2[Reverse modifications in reverse log order]
```

1. **Analysis:** Scan the log forward. Identify committed vs. active (uncommitted)
   transactions.
2. **REDO:** Replay all modifications in log order to reconstruct the database
   state as of the crash.
3. **UNDO:** For transactions that were active at crash time, reverse their
   modifications in reverse log order.

---

## Benchmark Report

Test machine: Ubuntu 22.04, GCC 12, 16GB RAM, NVMe SSD.

| Operation | Rows | Time | Throughput |
|-----------|------|------|------------|
| Bulk INSERT | 10,000 | ~2.5s | ~4,000 ops/s |
| Full Scan | 10,000 | ~15ms | ~667K rows/s |
| Filtered Scan (WHERE val > 5000) | ~5,000 | ~12ms | ~417K rows/s |
| Sort + LIMIT 10 | 10,000 | ~20ms | — |

Run `./bench_insert` to reproduce.

---

## Project Structure

```
mini-db/
├── CMakeLists.txt              # Build system
├── README.md                   # This file
├── Doxyfile                    # Doxygen config
├── .clang-format               # Code formatting rules
├── .clang-tidy                 # Static analysis config
├── .github/
│   └── workflows/
│       └── ci.yml              # GitHub Actions CI
├── src/
│   ├── common/                 # Shared types, config, schema
│   │   ├── config.h
│   │   ├── types.h / .cpp
│   │   └── schema.h / .cpp
│   ├── storage/                # Storage engine
│   │   ├── page.h              # Page abstraction
│   │   ├── disk_manager.h/.cpp # Disk I/O
│   │   ├── lru_k_replacer.h/.cpp
│   │   ├── buffer_pool.h/.cpp  # Buffer pool manager
│   │   └── heap_table.h/.cpp   # Heap-organized table
│   ├── index/                  # Indexing
│   │   └── b_plus_tree.h/.cpp  # Concurrent B+ tree
│   ├── concurrency/            # Transactions & locking
│   │   ├── lock_manager.h/.cpp # S2PL, deadlock detection
│   │   └── transaction.h/.cpp  # Transaction manager
│   ├── recovery/               # Logging & recovery
│   │   ├── log_manager.h/.cpp  # WAL
│   │   └── recovery_manager.h/.cpp # ARIES redo/undo
│   ├── catalog/                # Catalog & metadata
│   │   └── catalog.h/.cpp
│   ├── parser/                 # SQL parser
│   │   └── sql_parser.h/.cpp   # Tokenizer + recursive descent
│   ├── execution/              # Query execution
│   │   ├── executor.h/.cpp     # Volcano model operators
│   │   └── planner.h/.cpp      # Plan builder & optimizer
│   └── cli/
│       └── cli.cpp             # Interactive SQL shell
├── test/                       # Unit & integration tests
│   ├── test_storage.cpp
│   ├── test_btree.cpp
│   ├── test_concurrency.cpp
│   ├── test_sql.cpp
│   └── test_recovery.cpp
├── benchmark/
│   └── bench_insert.cpp
└── examples/
    └── demo.sql
```

---

## Building and Testing

### Prerequisites

- C++17 compiler (GCC 9+, Clang 10+, or MSVC 2019+)
- CMake 3.16+
- POSIX threads (pthreads)

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Sanitizer Builds

```bash
# AddressSanitizer
cmake .. -DENABLE_ASAN=ON

# ThreadSanitizer
cmake .. -DENABLE_TSAN=ON

# UndefinedBehaviorSanitizer
cmake .. -DENABLE_UBSAN=ON
```

### Run Tests

```bash
cd build
ctest --output-on-failure
```

### Run Benchmarks

```bash
./bench_insert
```

---

## Developer Tooling

| Tool | Purpose |
|------|---------|
| **CMake** | Build system with Release/Debug/Sanitizer configs |
| **GitHub Actions CI** | Automated build, test, lint on push/PR |
| **clang-format** | Consistent code formatting (Google style) |
| **clang-tidy** | Static analysis (bugprone, modernize, performance) |
| **cppcheck** | Additional static analysis |
| **AddressSanitizer** | Memory error detection |
| **ThreadSanitizer** | Data race detection |
| **UBSanitizer** | Undefined behavior detection |
| **Doxygen** | API documentation generation |

### Generating Docs

```bash
doxygen Doxyfile
# Open docs/html/index.html
```

---

## Key Design Decisions

1. **Page size:** 4096 bytes (configurable in `config.h`). Balances I/O
   efficiency with memory usage.

2. **LRU-K with K=2:** Better than simple LRU for scan-heavy workloads. A single
   scan won't evict frequently-accessed pages because they have 2+ accesses.

3. **B+ tree with coarse-grained tree latch:** Simpler than full latch crabbing
   but correct. Readers use shared lock; writers use exclusive lock. For a
   teaching engine this is sufficient; production systems (e.g., PostgreSQL)
   use fine-grained buffer-level latches with lock coupling.

4. **Strict 2PL:** Ensures serializability and strictness (no cascading aborts).
   All locks held until commit/abort.

5. **Wait-for graph deadlock detection:** Runs every 50ms in a background
   thread. DFS cycle detection. Victim = first node in the cycle (deterministic).

6. **Volcano model:** Pull-based iterators. Each operator calls `next()` on its
   children. Simple, composable, and the standard in academic databases.

7. **Force-at-commit WAL:** Log is flushed on every commit. This is simpler than
   group commit and ensures durability. Production systems batch commits for
   throughput.

---

## License

MIT License. See [LICENSE](LICENSE) for details.

## Acknowledgments

This project draws inspiration from:
- **BusTub** (CMU 15-445) — buffer pool, B+ tree, and executor design
- **PostgreSQL** — architecture and concepts
- **SQLite** — single-file simplicity
- **ARIES** (Mohan et al.) — recovery algorithm