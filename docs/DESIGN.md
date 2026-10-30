# Design of the baseline key-value store

This document explains how the system is put together and, more importantly, *why*
each piece looks the way it does. The oral examination asks about design
decisions, corner cases and complexity, so everything of that kind is written
down here rather than left implicit in the code.

---

## 1. Architecture at a glance

```
                 Put / Delete                      Get / NewIterator
                      |                                    |
                      v                                    v
             +-------------------+   read first   +-------------------+
             |     Memtable      |<---------------|    read path      |
             |  (AVL tree, RAM)  |                |  newest -> oldest |
             +-------------------+                +-------------------+
                      | full                               |
                      v                                    |
             +-------------------+                         |
   level 0   |   SST (run)       |<------------------------+
             +-------------------+                         |
                      | T runs at a level                  |
                      v  (merge)                           |
   level 1   +-------------------+<------------------------+
             |   SST (run)       |                         |
             +-------------------+                         |
                      |                                    |
   level 2            ...                                  |
                                                           |
             +----------------------------------------+    |
             | Buffer pool (hash table + LRU, 4 KiB)  |<---+
             +----------------------------------------+
                      | miss
                      v
             +----------------------------------------+
             |  pread() on SST files, O_DIRECT        |
             +----------------------------------------+
```

Writes are appended to an in-memory tree. When it is full it becomes an
immutable file. Files accumulate into levels and are merged. Reads consult the
components in newest-to-oldest order and stop at the first version of the key
they find. Every page read on the way goes through one shared cache.

**Source map**

| Component | Header | Implementation |
| --- | --- | --- |
| Public API, LSM levels, flush, compaction | `include/kvstore/db.h` | `src/db.cpp` |
| Memtable (AVL) | `include/kvstore/memtable.h` | `src/memtable.cpp` |
| SST builder and reader | `include/kvstore/sst.h` | `src/sst.cpp` |
| On-disk page structs | `include/kvstore/format.h` | — |
| Buffer pool | `include/kvstore/buffer_pool.h` | `src/buffer_pool.cpp` |
| Bloom filter | `include/kvstore/bloom_filter.h` | `src/bloom_filter.cpp` |
| N-way merge | `include/kvstore/merging_iterator.h` | `src/merging_iterator.cpp` |
| File and aligned buffer helpers | `include/kvstore/file.h` | `src/file.cpp` |

---

## 2. Data model and versions

A key-value pair is an `(int64 key, int64 value)` plus a **tombstone** flag. The
store never modifies data in place, so an update and a delete are both just new
versions of a key:

- `Put(k, v)` writes the version `(k, v, live)`.
- `Delete(k)` writes the version `(k, -, tombstone)`.

Several versions of one key can therefore exist at once, in different components
of the tree. Correctness rests on one rule, applied in three places:

> **For each key, only the newest version counts. If that version is a
> tombstone, the key does not exist.**

The three places are the point-query path (§6), the iterator (§7) and compaction
(§8). Everything else follows from being able to order versions by age, which is
what the next section establishes.

### The recency ordering

The components of the tree, from newest to oldest, are:

1. the memtable;
2. the runs of level 0, newest first;
3. the runs of level 1, newest first;
4. ...

Two invariants make this ordering correct.

**Within a level**, runs are kept sorted by descending *sequence number* — a
counter incremented every time an SST is created (`Database::SortLevel`). A run
created later contains data written later.

**Across levels**, level *i* is always newer than level *i+1*. This holds because
data only ever moves downwards, and only by a merge that consumes *every* run of
level *i* at once (`Database::CompactLevel`). Nothing can be left behind at level
*i* that is older than what was just pushed to level *i+1*.

That second invariant is worth remembering: it is why a point query may stop at
the first version it finds, and why `MergingIterator` can resolve duplicates
purely by child index.

---

## 3. Memtable — an AVL tree

`Memtable` is a balanced binary search tree of live entries, ordered by key.

- `Put` — `O(log n)`. If the key is already present its value and tombstone flag
  are overwritten *in place*, so **the memtable holds at most one version of each
  key**. That is what lets a flush write its contents out directly without any
  deduplication.
- `Get` — `O(log n)`.
- Ordered iteration — `O(1)` amortised per step, via an explicit stack of the
  ancestors whose subtrees are not yet exhausted. The stack depth is the tree
  height, so iteration costs `O(log n)` extra memory and no allocation per step.
- `Seek(key)` — `O(log n)`: descend from the root and keep on the stack exactly
  those nodes whose key is `>= key`; the top of the stack is then the smallest
  such key.

AVL was chosen over a red-black tree because its invariant — the heights of two
siblings differ by at most one — is the easiest to state and to test. The test
suite checks it indirectly by inserting 10,000 ascending keys (the worst case for
an unbalanced BST) and asserting that the height stays within `1.44 · log₂(n+2)`.
A red-black tree, a skip list or a B-tree would all be reasonable alternatives;
the specification asks only for "a reasonable balanced search tree".

`memtable_size_bytes` is compared against the *on-disk* footprint of the buffered
entries (`num_entries · sizeof(DiskEntry)`), not against the memory the AVL nodes
actually occupy. That makes the parameter mean something concrete — it directly
sets the size of the SSTs a flush produces — but it does mean the process uses
noticeably more than `memtable_size_bytes` of RAM for the memtable (each node also
carries two pointers and a height).

---

## 4. SSTs — static B-trees

When the memtable fills, its contents are written in key order to a new immutable
file. Because the file is never updated afterwards, the B-tree inside it needs no
support for insertion, splitting or deletion — it is built once, bottom-up, and
only ever searched.

[docs/FILE_FORMAT.md](FILE_FORMAT.md) has the byte-level layout. The shape:

- **Leaf pages** hold up to 170 sorted `(key, value, tombstone)` entries each.
- **Internal pages** hold up to 255 `(max_key, child_page)` pairs, where
  `max_key` is the largest key anywhere in that child's subtree. A descent is
  therefore a binary search for the first child with `max_key >= target`, with no
  special casing for the rightmost child.
- The **footer** is the last page and records the whole geometry: the root's page
  index, the height, the leaf count, the filter's location and the key range.
  Opening an SST costs exactly one page read.

With a fan-out of 255 and 170 entries per leaf, a two-level index already covers
about 11 million entries, so a point query almost never costs more than three
page reads plus the filter probes.

### Building in one streaming pass

`SSTableBuilder::Add` fills a single leaf page and writes it out as soon as it is
full, so **the entries are never held in memory**: an SST far larger than RAM can
be produced from a streaming input, which is exactly what compaction needs.

What the builder *does* accumulate is one 16-byte `(max_key, page_index)` entry
per finished leaf page. At `Finish()` that list is folded into index pages, those
into their parents, and so on until a single page is left — the root
(`BuildIndexLevel`). The cost is 16 bytes of memory per 4 KiB of output, about
0.4%.

That choice is deliberate and worth understanding, because it is the one place
where the builder trades memory for layout. Writing index pages eagerly, as soon
as they fill, would need only `O(height)` memory — but it would interleave index
pages with leaf pages in the file. Keeping the leaves as a **contiguous prefix**
of the file buys two properties the rest of the system relies on:

- a leaf's page index equals its ordinal, so "the next leaf" is `+1` and an SST
  iterator needs no parent pointers or sibling links;
- a full scan is a purely sequential read.

If your research direction produces SSTs so large that 0.4% is a real cost, this
is a sensible thing to revisit — and the alternative sketched above is the fix.

### Searching

`SSTable::Get` applies three filters, cheapest first:

1. **Key range.** The footer's `min_key`/`max_key` rule out the table with no
   I/O at all.
2. **Bloom filter** (§9). A negative answer rules out the table.
3. **Tree descent.** One page read per level, then a binary search in the leaf.

It returns a three-valued result — `kFound`, `kTombstone`, `kNotFound` — because
the caller must distinguish "this table says the key is deleted" (stop, the key
does not exist) from "this table does not know about the key" (keep looking in
older tables).

---

## 5. Buffer pool

A fixed array of 4 KiB frames, allocated once as a single page-aligned slab, with
two intrusive linked lists threaded through the frames:

- a **hash chain** per bucket, for lookup;
- a **recency list**, most recently used at the head.

A page is identified by `(file id, page index)`, as the specification suggests.
File ids come from a process-wide counter and are never reused, so a page cached
for a table that compaction has since deleted can never be confused with a page of
a new table — even one that reuses the same path.

Collisions are resolved by **chaining**, with the chain links stored in the frames
themselves, so `Fetch` allocates nothing and costs `O(1)` expected time.

Eviction is **LRU**: `AcquireFrame` walks from the least recently used end to the
first unpinned frame. Pages are pinned only while a `PageGuard` is alive, and the
read paths hold guards for a few instructions at a time, so the scan stops almost
immediately. If *every* frame is pinned the pool raises an error rather than
corrupting a page that is in use — that means the pool is too small for the
number of pages the caller is holding at once.

Everything an SST reads goes through the pool: internal index pages, leaf pages
**and filter pages**.

### Why SST iterators copy their leaf page

`SSTableIterator` fetches its current leaf page through the pool, copies the 4 KiB
out into a private buffer, and releases the pin immediately. A merging iterator
keeps one cursor per SST alive simultaneously; if each held a pin for its whole
lifetime, the pool's capacity would have to exceed the number of open tables and
long scans would pin pages for a long time. The read still goes through the pool
— so the page stays cached for whoever needs it next — and the only extra cost is
a 4 KiB `memcpy` per page, negligible next to the I/O it replaces.

---

## 6. Point queries

`Database::Get` walks the components in the recency order of §2:

1. the memtable — if the key is there, that *is* the newest version, so the
   answer is immediate (a tombstone means "not found");
2. each level in turn, and within a level each run newest first;
3. the first table that returns `kFound` or `kTombstone` decides the answer.

Cost: one `O(log n)` memtable lookup, then per SST a range check, a filter probe,
and — only if both pass — `height + 1` page reads, most of which hit the buffer
pool. With `T = 2` there is one run per level and `O(log(N/memtable))` levels, so
a lookup touches `O(log N)` filters and, thanks to them, usually reads a leaf from
only one of them.

---

## 7. Iterators

A client iterator must present the *logical* contents of the database: sorted
order, each live key exactly once, with its newest value, and deleted keys
absent. That is built in two layers.

**`MergingIterator`** merges N ordered sources with a binary min-heap of child
indices — the memtable first, then every SST from youngest to oldest. Its
comparator orders by key ascending and, on ties, by child index ascending. Since
child index *is* age (§2), **the first entry produced for a key is always its
newest version**. Each step costs `O(log N)` comparisons and copies no data. The
merge deliberately hides nothing: duplicates and tombstones still come through,
because compaction needs to see them.

**`DBIterator`** applies the visibility rule on top. For each key it keeps the
first version it sees, consumes the rest, and if that first version was a
tombstone it moves on to the next key instead of returning it. `Next()` is the
same operation as `Seek()` — advance until a live key appears — which is why both
are a single call to `AdvanceToVisible()`.

A range query needs no API of its own: `Seek(start)` and then `Next()` while
`Key() < end`.

Memory while a scan is running: one copied leaf page per SST, plus the heap.

---

## 8. Compaction

The policy is the simple one the specification suggests, generalised by one
parameter. A level is merged into the next as soon as it holds `size_ratio` runs
(default 2). `Database::MaybeCompact` sweeps levels from the top down, which is
enough because compacting level *i* can only overfill level *i+1*, which is
visited later in the same sweep.

`CompactLevel(i)`:

1. take **all** runs of level *i* (this is what preserves the across-level
   invariant of §2);
2. merge them with a `MergingIterator` ordered youngest-first;
3. for each key, keep the first (newest) version and drop the rest;
4. write the survivors into a new SST at level *i+1*;
5. drop the input pages from the buffer pool, close the files, unlink them.

With `T = 2` this behaves like a binary counter: each level holds zero or one
run, and a flush cascades exactly as carries do in binary addition.

**Memory.** The merge is fully streaming. The only buffers are one leaf page per
input cursor and the builder's staging page plus its index list (§4). A set of
SSTs much larger than RAM merges without ever being held in RAM, as required.

**When may a tombstone be discarded?** Only when no older version of that key can
be hiding beneath it. Since the merge consumes every run of level *i*, the only
places an older version could survive are level *i+1* and below. So tombstones
are dropped exactly when level *i+1* and every level below it is empty — the
output then becomes the oldest data in the tree. `CompactLevel` computes this as
`drop_tombstones`. If every key in the merge turns out to be a dead tombstone the
output is empty, no file is written, and the level simply disappears.

This is the conservative version of the rule. Real systems drop tombstones
earlier, using per-key range information about the levels below. That is a
legitimate thing to improve.

---

## 9. Bloom filters

One filter per SST, stored inside the SST file, read back through the buffer
pool like any other page.

- `m = n · bits_per_entry` bits, rounded up to a whole number of pages.
- `k = round(ln 2 · bits_per_entry)` probes, clamped to `[1, 12]`.
- Probe positions come from a single SplitMix64 hash of the key, expanded to the
  `k` positions as `h1 + i·h2` with `h2` forced odd (Kirsch–Mitzenmacher double
  hashing). One hash computation per lookup, not `k`.
- `bits_per_entry = 0` disables filters entirely — useful as an experimental
  control, since it must change performance and nothing else.

During a point query a negative answer skips the SST completely; a positive
answer means the tree still has to be searched. At 10 bits per entry the false
positive rate is about 1%, which the test suite checks.

**A subtlety worth knowing:** during a compaction the exact number of output
entries is not known until the merge has finished, so the filter is sized from an
*upper bound* (the sum of the input entry counts). Over-estimating only ever gives
the filter more bits per entry than configured, never fewer — so the configured
budget is a lower bound on what is delivered, and the filter is never weaker than
asked for. It does mean that filter memory reported for a compacted SST can
exceed `bits_per_entry · num_entries`. If you are studying filter-memory
allocation, this is the first thing to fix.

---

## 10. Persistence, recovery and file naming

`Open(name)` creates or reopens a directory holding every file of the database.
Recovery reads the **footer of each `*.sst` file** to learn its level and
sequence number, groups the files into levels and sorts each level. There is no
manifest file: the footers are the metadata, which is one fewer thing that can
become inconsistent. `next_sequence` resumes above the highest sequence seen.

`Close()` flushes the memtable, releases the cached pages and closes every file,
so the database can be reopened without losing data.

Files are named `sst_L<level>_S<sequence>.sst`. The name is only for humans — the
authoritative level and sequence live in the footer — but it makes a database
directory readable at a glance.

**Durability limits, stated plainly.** There is no write-ahead log. A process
killed before `Close()` loses whatever was still in the memtable. SSTs already
written are complete and are recovered normally, because an SST file only becomes
reachable once `Finish()` has written its footer and `fsync`ed it. A crash *during*
a compaction can leave both the inputs and a complete output on disk; `Open()`
would then see an over-full level and re-run the merge, which is why it calls
`MaybeCompact()`. It can also leave a partial output file with no footer, which
`Open()` refuses to load. Full crash safety is not a baseline requirement.

---

## 11. Direct I/O

All I/O is page-granular `pread`/`pwrite` at 4 KiB-aligned offsets into 4 KiB
-aligned buffers (`posix_memalign`), which is what `O_DIRECT` requires.

With `use_direct_io` the store bypasses the OS page cache, so the buffer pool is
the *only* cache in the system — essential when the experiment is measuring the
buffer pool. Some filesystems reject `O_DIRECT`; rather than failing, `File`
falls back to buffered I/O and `Database::uses_direct_io()` reports what actually
happened. **Check that flag before trusting an I/O measurement**, because a silent
fallback leaves the page cache acting as a second, invisible buffer pool.
`kv_bench` prints it as a CSV column for exactly this reason.

---

## 12. Complexity summary

Let *N* be the number of entries in the database, *m* the memtable capacity in
entries, *T* the size ratio and *B* = 170 entries per page.

| Operation | Cost |
| --- | --- |
| `Put` / `Delete` | `O(log m)` in memory; amortised `O((T/B)·log_T(N/m))` page writes from flushing and merging |
| `Get` | `O(log m)` + one filter probe per run + `O(log_B N)` page reads in the one run that has the key |
| `Seek` | `O(log m)` + `O(log_B N)` page reads per run |
| `Next` | `O(log R)` for the heap, with *R* the number of runs; one page read per *B* entries per run |
| Flush | `O(m)` entries written sequentially |
| Compaction of a level | `O(n)` entries read and written, `O(1)` pages of memory plus 16 bytes per output page |

Number of levels with default settings: `log_T(N/m)`.

---

## 13. Known limitations

These are stated so you can decide which ones are worth attacking, and so that
nothing here surprises you in the oral examination.

- **Single-threaded.** No locking anywhere.
- **No write-ahead log** (§10).
- **No snapshots**, hence writes are refused while an iterator is alive.
- **Tombstones are dropped conservatively** (§8).
- **Filters are sized from an upper bound** after compaction (§9), and every level
  gets the same bits per entry.
- **Compaction merges a whole level at once**, so one merge can be long. Real
  systems partition a level into files and merge a piece at a time.
- **The buffer pool holds read-only pages.** SSTs are written directly to the
  file, not through the pool, because they are written once, sequentially, and
  never modified.
- **Fixed 8-byte keys and values.**

Each of these is a legitimate place to start a research question — but read the
project specification's warning first: a change that requires only a small
modification may not have enough substance for a paper component.
