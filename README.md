# CSC443/CSC2234 — Baseline LSM-Tree Key-Value Store

This is the starting point for the group research project: a small, complete,
single-threaded key-value store built on an LSM-tree, written in C++17 with no
external dependencies.

It implements exactly the baseline the project specification asks for — memtable,
static B-tree SSTs, point queries, iterators, buffer pool with eviction,
compaction, updates, deletes and Bloom filters — and nothing beyond it. The
baseline is deliberately minimal: it exists so that you can spend the semester on
the research components (two paper-derived optimizations and one of your own)
rather than on infrastructure.

**You are expected to modify this code.** It is a platform, not a finished
product. Read [docs/DESIGN.md](docs/DESIGN.md) before you change anything
substantial — it explains why each component looks the way it does, which is also
what you will be asked about in the oral examination.

---

## Quick start

```sh
make            # builds the library, the tests, the example and the benchmark
make test       # builds and runs the whole test suite (should be all green)
make example    # builds and runs a guided tour of the API
```

Requirements: a C++17 compiler (g++ or clang++) and Linux. Nothing else.

`examples/example.cpp` is a commented, runnable walkthrough of every call —
opening, `Put`/`Get`, updates, deletes, full scans, `Seek`, range queries, the
iterator rule, introspection, and persistence across `Close`/`Open`. Read it
alongside this section. In miniature:

```cpp
#include "kvstore/db.h"

int main() {
  kvstore::Options options;
  options.memtable_size_bytes = 1 << 20;   // flush after ~1 MiB of entries
  options.bloom_bits_per_entry = 10;

  kvstore::Database db(options);
  db.Open("my_database");                  // a directory; created if missing

  db.Put(42, 1234);
  db.Delete(7);

  kvstore::ValueType value;
  if (db.Get(42, &value)) { /* value == 1234 */ }

  // A range query over [100, 200) is a Seek followed by Next calls.
  {
    auto it = db.NewIterator();
    for (it->Seek(100); it->Valid() && it->Key() < 200; it->Next()) {
      // it->Key(), it->Value()
    }
  }                                        // release the iterator before writing

  db.Close();                              // persists everything still in memory
}
```

Compile your own program against the built library:

```sh
g++ -std=c++17 -Iinclude my_program.cpp build/libkvstore.a -o my_program
```

Or drop it into `examples/` and `make` will build it to `build/<name>`.

---

## The API

`include/kvstore/db.h` is the public header. It provides the API named in the
project specification:

| Specification    | This implementation                          |
| ---------------- | -------------------------------------------- |
| `Open(name)`     | `void Database::Open(const std::string&)`     |
| `Put(k, v)`      | `void Database::Put(KeyType, ValueType)`      |
| `v = Get(k)`     | `bool Database::Get(KeyType, ValueType*)`     |
| `Delete(k)`      | `void Database::Delete(KeyType)`              |
| `Iterator()`     | `std::unique_ptr<Iterator> Database::NewIterator()` |
| `it.Seek(k)`     | `void Iterator::Seek(KeyType)`                |
| `it.Valid()`     | `bool Iterator::Valid() const`                |
| `it.Key()`       | `KeyType Iterator::Key() const`               |
| `it.Value()`     | `ValueType Iterator::Value() const`           |
| `it.Next()`      | `void Iterator::Next()`                       |
| `Close()`        | `void Database::Close()`                      |

The factory is called `NewIterator()` rather than `Iterator()` only because
`Iterator` is the name of the type it returns. `Iterator::SeekToFirst()` is also
provided, as the natural starting point for a full scan.

Keys and values are 8-byte signed integers (`kvstore::KeyType`,
`kvstore::ValueType`), as the specification's scope section allows.

Errors are reported by throwing `kvstore::Error` (a `std::runtime_error`).
`Get` returns `false` for a missing or deleted key; that is not an error.

**Iterators and writes.** An iterator holds references into the memtable and the
SSTs that existed when it was created. Writing to the database could rebalance
the memtable or compact away an SST underneath it, so `Put`, `Delete` and `Close`
throw if an iterator is still alive. Let the iterator go out of scope first. (If
your research direction needs concurrent reads and writes, snapshots are the
mechanism real systems use — that would be a change to the baseline, and a
defensible one.)

---

## Configuration

Everything tunable is in `kvstore::Options` (`include/kvstore/options.h`), so no
experiment ever needs to edit source code:

| Option | Meaning | Default |
| --- | --- | --- |
| `memtable_size_bytes` | On-disk size of buffered entries before a flush | 1 MiB |
| `buffer_pool_capacity_pages` | Buffer pool size, in 4 KiB pages | 1024 |
| `buffer_pool_hash_buckets` | Hash table buckets (0 = derive from capacity) | 0 |
| `bloom_bits_per_entry` | Filter budget; 0 disables filters entirely | 10 |
| `size_ratio` | LSM size ratio *T*: a level merges down at *T* runs | 2 |
| `use_direct_io` | Bypass the OS page cache with `O_DIRECT` | true |
| `create_if_missing` | Create the database directory if absent | true |

---

## Repository layout

```
include/kvstore/     public headers, one per component
  types.h            KeyType, ValueType, page size, the Error exception
  options.h          every tunable parameter
  db.h               the public Database and its iterator contract
  memtable.h         the in-memory AVL write buffer
  sst.h              the static B-tree SST: builder and reader
  format.h           the on-disk page structs
  buffer_pool.h      page cache: hash table + LRU
  bloom_filter.h     filter hashing and bit array
  iterator.h         the Iterator / InternalIterator interfaces
  merging_iterator.h N-way merge over ordered sources
  file.h             aligned buffers and page-granular pread/pwrite

src/                 the implementation of each of the above
tests/               the test suite and its ~100-line test framework
examples/example.cpp a commented tour of the whole API; `make example` runs it
tools/bench.cpp      kv_bench, a parameterised workload driver
docs/                DESIGN.md, FILE_FORMAT.md, EXPERIMENTS.md
```

---

## Testing

```sh
make test                      # everything
./build/kv_tests memtable      # only tests whose name contains "memtable"
./build/kv_tests db.iterator   # or any other substring
```

The suite covers the correctness properties the specification requires — `Put`,
`Get`, iterator creation and traversal, `Seek`, updates, deletes and persistence
across `Close`/`Open` — plus unit tests for each component and a randomized test
that compares a long mixed workload against `std::map` after every phase.

Before measuring any optimization, run the suite against both the baseline and
the optimized build. A faster system that returns different answers is not
faster.

Useful build variants while developing:

```sh
make BUILD_TYPE=debug                 # -O0 -g3, libstdc++ assertions active
make SANITIZE=address,undefined       # if the sanitizer runtimes are installed
valgrind -q --leak-check=full ./build/kv_tests
```

Add tests for every optimization you introduce. `tests/test_framework.h`
documents the macros.

---

## Benchmarking

`build/kv_bench` runs a load phase, a point-query phase and an optional scan
phase, and prints one CSV row. Every parameter is a flag, so a whole experiment
is a shell loop. See [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) for the flags,
the CSV columns, and the reproducibility rules the specification expects.

```sh
./build/kv_bench --help
./build/kv_bench --csv-header --num-keys 1000000 --num-queries 100000
```

---

## What is deliberately *not* here

The specification asks for a minimal baseline and warns against building
infrastructure you do not need. The following are absent on purpose. Several of
them are perfectly good research directions — which is the point.

- **Concurrency.** The store is single-threaded, as the scope section permits.
- **Variable-length keys and values.** Fixed 8-byte integers keep the page layout
  and the comparison logic trivial.
- **A write-ahead log.** Durability comes from `Close()`. A process killed before
  `Close()` loses whatever was still in the memtable; the SSTs already written
  stay valid and are recovered on the next `Open()`.
- **Compression, key-value separation, prefetching, learned or range filters,
  alternative compaction policies, alternative eviction policies.** All of these
  are listed in the specification as *research* directions, not baseline
  features.
- **Snapshots / MVCC.** Hence the rule that writes are refused while an iterator
  is alive.

Resist the urge to make the baseline more complete. Your grade comes from the
three research components.

---

## Where to start reading

1. [docs/DESIGN.md](docs/DESIGN.md) — the architecture, and the reasoning behind
   each design decision, including the invariants that make updates, deletes and
   compaction correct.
2. [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md) — the byte-level layout of an SST.
3. `include/kvstore/db.h`, then `src/db.cpp` — the write path
   (`Put` → `FlushMemtable` → `MaybeCompact` → `CompactLevel`) and the read path
   (`Get`, `NewIterator`).
4. `src/sst.cpp` — how a static B-tree is built in one streaming pass and
   searched in `O(height)` page reads.
