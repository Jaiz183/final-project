// On-disk layout of an SST file.
//
// An SST is a *static* B-tree: it is built once, bottom-up, and never modified.
// The file is a sequence of 4 KiB pages laid out in this order:
//
//   page 0 .. num_leaf_pages-1        leaf pages, holding the sorted entries
//   ...                               internal index pages, level 1 upwards
//   root_page_index                   the last index page written (the root)
//   bloom_start_page ..               the Bloom filter bit array
//   last page of the file             the Footer
//
// Because leaves occupy a contiguous prefix of the file, a full scan of an SST
// is simply "read leaf pages 0, 1, 2, ..." which is what SSTableIterator does.
// Every page is written at a 4 KiB-aligned offset, and the root's location is
// recorded in the footer, as required by the project specification.
#ifndef KVSTORE_FORMAT_H_
#define KVSTORE_FORMAT_H_

#include <cstdint>

#include "kvstore/types.h"

namespace kvstore {

// One key-value pair as stored inside a leaf page.  24 bytes: the 8-byte key,
// the 8-byte value, a tombstone flag, and padding that keeps the following
// entry 8-byte aligned.
struct DiskEntry {
  KeyType key;
  ValueType value;
  std::uint8_t tombstone;  // 1 => this entry is a delete marker
  std::uint8_t unused[7];
};
static_assert(sizeof(DiskEntry) == 24, "unexpected DiskEntry layout");

// Number of entries that fit in one leaf page (170 for 4 KiB pages).
constexpr std::size_t kLeafCapacity = (kPageSize - 8) / sizeof(DiskEntry);

struct LeafPage {
  std::uint32_t num_entries;
  std::uint32_t unused;
  DiskEntry entries[kLeafCapacity];
  std::uint8_t padding[kPageSize - 8 - kLeafCapacity * sizeof(DiskEntry)];
};
static_assert(sizeof(LeafPage) == kPageSize, "LeafPage must be exactly one page");

// One child pointer inside an internal index page.  `max_key` is the largest
// key stored anywhere in that child's subtree, which makes descent a single
// binary search for the first child with max_key >= target.
struct ChildEntry {
  KeyType max_key;
  std::uint32_t page_index;
  std::uint32_t unused;
};
static_assert(sizeof(ChildEntry) == 16, "unexpected ChildEntry layout");

// Fan-out of an internal page (255 for 4 KiB pages), so a three-level index
// already covers 255 * 255 * 170 ~ 11M entries.
constexpr std::size_t kInternalCapacity = (kPageSize - 8) / sizeof(ChildEntry);

struct InternalPage {
  std::uint32_t num_children;
  std::uint32_t unused;
  ChildEntry children[kInternalCapacity];
  std::uint8_t padding[kPageSize - 8 - kInternalCapacity * sizeof(ChildEntry)];
};
static_assert(sizeof(InternalPage) == kPageSize, "InternalPage must be exactly one page");

constexpr std::uint64_t kSSTMagic = 0x5453534b56534431ULL;  // "KVSSTD1" -ish
constexpr std::uint32_t kFormatVersion = 1;

// The last page of every SST file.  Reading it is enough to learn the whole
// geometry of the file, so opening a database only costs one page read per SST.
struct Footer {
  std::uint64_t magic;
  std::uint32_t format_version;
  std::uint32_t level;          // LSM-tree level this SST belongs to
  std::uint64_t sequence;       // global creation order; higher == younger
  std::uint64_t num_entries;    // number of key-value pairs (incl. tombstones)
  std::uint64_t num_leaf_pages;
  std::uint64_t root_page_index;
  std::uint32_t tree_height;    // 0 => the root *is* the single leaf page
  std::uint32_t bloom_num_hashes;
  std::uint64_t bloom_start_page;
  std::uint64_t bloom_num_pages;
  std::uint64_t bloom_num_bits;  // 0 => no filter was built
  KeyType min_key;
  KeyType max_key;
  std::uint8_t padding[kPageSize - 96];
};
static_assert(sizeof(Footer) == kPageSize, "Footer must be exactly one page");

}  // namespace kvstore

#endif  // KVSTORE_FORMAT_H_
