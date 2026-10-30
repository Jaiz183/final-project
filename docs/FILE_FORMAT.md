# SST file format

An SST is a single file made of 4 KiB pages. It is written once, bottom-up, and
never modified. The structs below are the real ones, declared in
`include/kvstore/format.h` and checked with `static_assert` to be exactly one
page each.

All integers are stored in the machine's native byte order. The format is
therefore not portable between machines of different endianness, which is fine
for this project — a database directory is only ever read on the machine that
wrote it. `format_version` in the footer exists so that an incompatible change
fails loudly instead of being misread.

---

## Page layout of a file

```
page index
  0                 +---------------------------+
                    |  leaf page 0              |  <- smallest keys
  1                 |  leaf page 1              |
  ...               |  ...                      |
  L-1               |  leaf page L-1            |  <- largest keys
                    +---------------------------+
  L                 |  index pages, level 1     |   each covers <= 255 children
  ...               |  index pages, level 2     |
  root_page_index   |  the root index page      |   the last index page written
                    +---------------------------+
  bloom_start_page  |  Bloom filter bits        |   bloom_num_pages pages
  ...               |                           |
                    +---------------------------+
  last page         |  Footer                   |
                    +---------------------------+
```

Two properties matter:

- **The leaf pages are a contiguous prefix**, pages `0 .. num_leaf_pages-1`, in
  ascending key order. A leaf's page index is therefore its ordinal, "the next
  leaf" is `+1`, and a full scan is a sequential read. (§4 of
  [DESIGN.md](DESIGN.md) explains what this costs the builder.)
- **The footer is the last page.** Its offset is `file_size - 4096`, so opening a
  table costs one read.

A table small enough to fit in a single leaf has no index pages at all: its
`tree_height` is 0 and `root_page_index` is 0, i.e. the root *is* the leaf.

---

## Leaf page — 4096 bytes

```c
struct DiskEntry {          // 24 bytes
  int64_t  key;
  int64_t  value;
  uint8_t  tombstone;       // 1 => delete marker; value is meaningless
  uint8_t  unused[7];       // keeps the next entry 8-byte aligned
};

struct LeafPage {
  uint32_t  num_entries;    // <= 170
  uint32_t  unused;
  DiskEntry entries[170];   // sorted by key, strictly increasing
  uint8_t   padding[8];
};
```

`kLeafCapacity = (4096 - 8) / 24 = 170`, leaving 8 bytes of padding. Only the
last leaf of a table may be partially filled.

A leaf is searched with a binary search for the first entry with `key >= target`;
if that entry's key is not the target, the key is absent from this table (either
a gap in the data, or a Bloom filter false positive).

---

## Internal (index) page — 4096 bytes

```c
struct ChildEntry {         // 16 bytes
  int64_t  max_key;         // largest key anywhere in this child's subtree
  uint32_t page_index;      // where that child lives in this file
  uint32_t unused;
};

struct InternalPage {
  uint32_t    num_children; // <= 255
  uint32_t    unused;
  ChildEntry  children[255];// sorted by max_key
  uint8_t     padding[8];
};
```

`kInternalCapacity = (4096 - 8) / 16 = 255`, the fan-out of the tree.

Storing each child's **maximum** key, rather than separator keys between
children, makes a descent a plain binary search for the first child with
`max_key >= target`, with no special case for the rightmost child. If no child
qualifies, the target is larger than every key in the subtree — which the caller
has already ruled out using the footer's `max_key`.

---

## Bloom filter pages

`bloom_num_pages` pages of raw bits, starting at `bloom_start_page`, holding
`bloom_num_bits = bloom_num_pages * 4096 * 8` bits in total. Bit *i* is bit
`i % 8` of byte `i / 8`, counted from the start of the filter region, so it lives
on page `bloom_start_page + (i / 8) / 4096`.

Because the filter is made of ordinary pages, a probe is an ordinary buffer-pool
fetch — the filter participates in caching and eviction exactly like index and
leaf pages, as the specification requires.

`bloom_num_bits == 0` means no filter was built (`bloom_bits_per_entry = 0`), and
every key is treated as "may be present".

---

## Footer — 4096 bytes, the last page

```c
struct Footer {
  uint64_t magic;            // kSSTMagic; wrong value => not one of our files
  uint32_t format_version;   // kFormatVersion
  uint32_t level;            // LSM level this SST belongs to
  uint64_t sequence;         // global creation order; higher == younger
  uint64_t num_entries;      // key-value pairs, tombstones included
  uint64_t num_leaf_pages;
  uint64_t root_page_index;
  uint32_t tree_height;      // 0 => the root is the single leaf page
  uint32_t bloom_num_hashes; // probes per lookup
  uint64_t bloom_start_page;
  uint64_t bloom_num_pages;
  uint64_t bloom_num_bits;   // 0 => no filter
  int64_t  min_key;          // smallest key in the table
  int64_t  max_key;          // largest key in the table
  uint8_t  padding[4000];
};
```

`level` and `sequence` in the footer are what `Database::Open` uses to rebuild the
LSM structure, so the database needs no manifest file. `min_key`/`max_key` let a
point query rule out a table with no I/O at all.

The footer is written last and the file is `fsync`ed before it is published, so a
file without a valid footer is an incomplete write and is refused by
`SSTable::Open`.

---

## File naming

```
sst_L<level>_S<sequence padded to 10 digits>.sst
```

for example `sst_L2_S0000000017.sst`. Sequence numbers are unique within a
database, so names are unique. The name duplicates information from the footer
purely so that a directory listing is readable; the footer is authoritative.

---

## Worked example

A table of 1,000 entries with 10 bits per entry:

| | |
| --- | --- |
| Leaf pages | `ceil(1000 / 170)` = **6** → pages 0–5 |
| Index pages | 6 children fit in one page → **1** → page 6 (the root) |
| `tree_height` | **1** |
| Filter | `1000 × 10` = 10,000 bits → `ceil(10000 / 32768)` = **1** page → page 7 |
| Footer | page **8** |
| File size | 9 × 4096 = **36,864 bytes** |

A lookup reads at most: up to 7 filter pages (usually one, since the filter is a
single page here), the root, and one leaf — and all but the first access of each
is a buffer-pool hit.
