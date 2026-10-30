// Sorted string tables: immutable static B-trees on disk.
#ifndef KVSTORE_SST_H_
#define KVSTORE_SST_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kvstore/bloom_filter.h"
#include "kvstore/buffer_pool.h"
#include "kvstore/file.h"
#include "kvstore/format.h"
#include "kvstore/iterator.h"
#include "kvstore/types.h"

namespace kvstore {

// Writes one SST file.  Entries must be added in strictly increasing key order,
// which both the memtable flush and the compaction merge naturally produce.
//
// The tree is built bottom-up in a single streaming pass.  Leaf pages are written
// out as soon as they fill up, so the entries themselves are never held in
// memory: an SST far larger than RAM can be produced from a streaming input,
// which is what the specification requires of compaction.  What the builder does
// keep is one (largest key, page index) pair per finished leaf page -- 16 bytes
// per 4 KiB of data, about 0.4% of the output size -- and at Finish() it folds
// that list into index pages, then into their parents, until a single root page
// is left.
//
// Buffering the index entries rather than writing them as they fill is what keeps
// the leaf pages a contiguous prefix of the file, which in turn makes a full scan
// a sequential read and makes a leaf's page index equal to its ordinal.  Writing
// index pages eagerly would need only O(height) memory but would interleave them
// with the leaves and cost both of those properties.
class SSTableBuilder {
 public:
  // `expected_entries` only sizes the Bloom filter and may be an upper bound.
  SSTableBuilder(const std::string& path, std::uint32_t level,
                 std::uint64_t sequence, std::uint32_t bloom_bits_per_entry,
                 std::uint64_t expected_entries, bool use_direct_io);

  SSTableBuilder(const SSTableBuilder&) = delete;
  SSTableBuilder& operator=(const SSTableBuilder&) = delete;

  void Add(KeyType key, ValueType value, bool tombstone);

  // Writes the remaining pages, the filter and the footer, then closes the file.
  // Returns the number of entries written.  Must be called exactly once; if no
  // entry was ever added nothing is written and the caller should delete the
  // (empty) file.
  std::uint64_t Finish();

  std::uint64_t num_entries() const { return num_entries_; }

 private:
  // Writes the staged leaf page and remembers its (largest key, page index).
  void FlushLeafPage();
  // Writes `children` out as index pages and returns one entry per page written,
  // to be folded again by the caller.
  std::vector<ChildEntry> BuildIndexLevel(const std::vector<ChildEntry>& children);
  std::uint64_t AllocatePage() { return next_page_index_++; }

  std::string path_;
  File file_;
  std::uint32_t level_;
  std::uint64_t sequence_;
  BloomFilterBuilder bloom_;

  AlignedBuffer leaf_page_{1};
  std::uint32_t leaf_count_ = 0;   // entries staged in leaf_page_
  // One entry per finished leaf page, in key order: the input to the index build.
  std::vector<ChildEntry> leaf_entries_;

  std::uint64_t next_page_index_ = 0;
  std::uint64_t num_entries_ = 0;
  KeyType min_key_ = 0;
  KeyType max_key_ = 0;
  bool has_entries_ = false;
  bool finished_ = false;
};

// Read-only handle on one SST file.  Every page it touches -- index pages, leaf
// pages and filter pages alike -- is fetched through the shared buffer pool.
class SSTable {
 public:
  enum class LookupResult {
    kNotFound,   // the key is not in this SST; keep looking in older ones
    kFound,      // `*value` holds the newest version stored here
    kTombstone,  // the newest version here is a delete marker
  };

  static std::unique_ptr<SSTable> Open(const std::string& path, BufferPool* pool,
                                       bool use_direct_io);

  SSTable(const SSTable&) = delete;
  SSTable& operator=(const SSTable&) = delete;

  LookupResult Get(KeyType key, ValueType* value);

  // Cursor over every entry in this SST, in key order, including tombstones.
  std::unique_ptr<InternalIterator> NewIterator();

  // True if the filter says the key may be present (or there is no filter).
  // Exposed separately so that tests and experiments can measure filter
  // effectiveness directly.
  bool MayContain(KeyType key);

  const std::string& path() const { return path_; }
  std::uint32_t file_id() const { return file_id_; }
  std::uint32_t level() const { return footer_.level; }
  std::uint64_t sequence() const { return footer_.sequence; }
  std::uint64_t num_entries() const { return footer_.num_entries; }
  std::uint64_t num_leaf_pages() const { return footer_.num_leaf_pages; }
  std::uint32_t tree_height() const { return footer_.tree_height; }
  KeyType min_key() const { return footer_.min_key; }
  KeyType max_key() const { return footer_.max_key; }
  const Footer& footer() const { return footer_; }

  // Drops this table's pages from the buffer pool.  Called before the file is
  // deleted by a compaction.
  void DropFromCache();

 private:
  friend class SSTableIterator;

  SSTable() = default;

  PageGuard FetchPage(std::uint64_t page_index);
  // Index of the leaf page that would contain `key`, or num_leaf_pages() if
  // `key` is larger than every key in the table.
  std::uint64_t FindLeafPage(KeyType key);

  std::string path_;
  File file_;
  std::uint32_t file_id_ = 0;
  BufferPool* pool_ = nullptr;
  Footer footer_{};
};

// Name of the file backing an SST at `level` with creation number `sequence`.
// The level and sequence are also stored in the footer; putting them in the name
// as well makes a database directory readable at a glance.
std::string SSTFileName(std::uint32_t level, std::uint64_t sequence);

}  // namespace kvstore

#endif  // KVSTORE_SST_H_
