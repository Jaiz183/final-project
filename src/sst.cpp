#include "kvstore/sst.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace kvstore {
namespace {

// File ids are process-wide and never reused, so a page cached for a table that
// a compaction has since deleted can never be mistaken for a page of a new table
// that happens to reuse the same path.
std::uint32_t NextFileId() {
  static std::uint32_t counter = 0;
  return ++counter;
}

// Index of the first entry in a leaf page with key >= `key`, or `count` if there
// is none.
std::uint32_t LowerBoundInLeaf(const LeafPage& leaf, KeyType key) {
  std::uint32_t lo = 0;
  std::uint32_t hi = leaf.num_entries;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo) / 2;
    if (leaf.entries[mid].key < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

// Index of the first child whose subtree may contain `key`, i.e. the first child
// with max_key >= key, or `num_children` if `key` is past the whole subtree.
std::uint32_t LowerBoundInIndex(const InternalPage& index, KeyType key) {
  std::uint32_t lo = 0;
  std::uint32_t hi = index.num_children;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo) / 2;
    if (index.children[mid].max_key < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

}  // namespace

std::string SSTFileName(std::uint32_t level, std::uint64_t sequence) {
  std::ostringstream os;
  os << "sst_L" << level << "_S" << std::setw(10) << std::setfill('0')
     << sequence << ".sst";
  return os.str();
}

// ---------------------------------------------------------------------------
// SSTableBuilder
// ---------------------------------------------------------------------------

SSTableBuilder::SSTableBuilder(const std::string& path, std::uint32_t level,
                               std::uint64_t sequence,
                               std::uint32_t bloom_bits_per_entry,
                               std::uint64_t expected_entries,
                               bool use_direct_io)
    : path_(path),
      file_(File::Create(path, use_direct_io)),
      level_(level),
      sequence_(sequence),
      bloom_(expected_entries, bloom_bits_per_entry) {}

void SSTableBuilder::Add(KeyType key, ValueType value, bool tombstone) {
  if (finished_) throw Error("SSTableBuilder::Add after Finish");
  if (has_entries_ && key <= max_key_) {
    throw Error("SSTableBuilder::Add requires strictly increasing keys");
  }

  auto* page = reinterpret_cast<LeafPage*>(leaf_page_.data());
  page->entries[leaf_count_].key = key;
  page->entries[leaf_count_].value = value;
  page->entries[leaf_count_].tombstone = tombstone ? 1 : 0;
  ++leaf_count_;

  bloom_.Add(key);
  if (!has_entries_) min_key_ = key;
  max_key_ = key;
  has_entries_ = true;
  ++num_entries_;

  if (leaf_count_ == kLeafCapacity) FlushLeafPage();
}

void SSTableBuilder::FlushLeafPage() {
  if (leaf_count_ == 0) return;

  auto* page = reinterpret_cast<LeafPage*>(leaf_page_.data());
  page->num_entries = leaf_count_;
  const KeyType page_max_key = page->entries[leaf_count_ - 1].key;

  const std::uint64_t page_index = AllocatePage();
  file_.WritePage(page_index, leaf_page_.data());
  leaf_entries_.push_back(
      ChildEntry{page_max_key, static_cast<std::uint32_t>(page_index), 0});

  leaf_count_ = 0;
  // Zero the staging page so that the unused tail of the next page written from
  // it is deterministic rather than holding stale bytes.
  std::memset(leaf_page_.data(), 0, kPageSize);
}

std::vector<ChildEntry> SSTableBuilder::BuildIndexLevel(
    const std::vector<ChildEntry>& children) {
  std::vector<ChildEntry> parents;
  parents.reserve((children.size() + kInternalCapacity - 1) / kInternalCapacity);

  AlignedBuffer buffer(1);
  for (std::size_t first = 0; first < children.size(); first += kInternalCapacity) {
    const std::size_t count =
        std::min(kInternalCapacity, children.size() - first);
    std::memset(buffer.data(), 0, kPageSize);
    auto* page = reinterpret_cast<InternalPage*>(buffer.data());
    page->num_children = static_cast<std::uint32_t>(count);
    for (std::size_t i = 0; i < count; ++i) {
      page->children[i] = children[first + i];
    }

    const std::uint64_t page_index = AllocatePage();
    file_.WritePage(page_index, buffer.data());
    // The subtree rooted at this page covers everything up to its last child.
    parents.push_back(ChildEntry{children[first + count - 1].max_key,
                                 static_cast<std::uint32_t>(page_index), 0});
  }
  return parents;
}

std::uint64_t SSTableBuilder::Finish() {
  if (finished_) throw Error("SSTableBuilder::Finish called twice");
  finished_ = true;
  if (!has_entries_) {
    // Nothing to store.  The caller removes the empty file.
    file_.Close();
    return 0;
  }

  FlushLeafPage();
  const std::uint64_t num_leaf_pages = leaf_entries_.size();

  // Fold the leaf list into index pages, then those into their parents, until a
  // single page is left: that page is the root.  A table small enough to fit in
  // one leaf skips the loop entirely and its only leaf *is* the root.
  std::vector<ChildEntry> level = std::move(leaf_entries_);
  std::uint32_t tree_height = 0;
  while (level.size() > 1) {
    level = BuildIndexLevel(level);
    ++tree_height;
  }
  const std::uint64_t root_page_index = level.front().page_index;

  // The filter follows the tree, page aligned so that it can be read back
  // through the buffer pool like any other page.
  const std::uint64_t bloom_start_page = next_page_index_;
  AlignedBuffer scratch(1);
  for (std::size_t p = 0; p < bloom_.num_pages(); ++p) {
    std::memcpy(scratch.data(), bloom_.bits().data() + p * kPageSize, kPageSize);
    file_.WritePage(AllocatePage(), scratch.data());
  }
  const std::uint64_t bloom_num_pages = bloom_.num_pages();

  std::memset(scratch.data(), 0, kPageSize);
  auto* footer = reinterpret_cast<Footer*>(scratch.data());
  footer->magic = kSSTMagic;
  footer->format_version = kFormatVersion;
  footer->level = level_;
  footer->sequence = sequence_;
  footer->num_entries = num_entries_;
  footer->num_leaf_pages = num_leaf_pages;
  footer->root_page_index = root_page_index;
  footer->tree_height = tree_height;
  footer->bloom_num_hashes = bloom_.num_hashes();
  footer->bloom_start_page = bloom_start_page;
  footer->bloom_num_pages = bloom_num_pages;
  footer->bloom_num_bits = bloom_.num_bits();
  footer->min_key = min_key_;
  footer->max_key = max_key_;
  file_.WritePage(AllocatePage(), scratch.data());

  file_.Sync();
  file_.Close();
  return num_entries_;
}

// ---------------------------------------------------------------------------
// SSTable
// ---------------------------------------------------------------------------

std::unique_ptr<SSTable> SSTable::Open(const std::string& path, BufferPool* pool,
                                       bool use_direct_io) {
  auto table = std::unique_ptr<SSTable>(new SSTable());
  table->path_ = path;
  table->pool_ = pool;
  table->file_ = File::OpenForRead(path, use_direct_io);
  table->file_id_ = NextFileId();

  const std::uint64_t num_pages = table->file_.NumPages();
  if (num_pages == 0) throw Error("not an SST file (empty): " + path);

  // The footer is always the last page, so one read reveals the whole geometry.
  AlignedBuffer scratch(1);
  table->file_.ReadPage(num_pages - 1, scratch.data());
  std::memcpy(&table->footer_, scratch.data(), sizeof(Footer));
  if (table->footer_.magic != kSSTMagic) {
    throw Error("bad SST magic in " + path);
  }
  if (table->footer_.format_version != kFormatVersion) {
    throw Error("unsupported SST format version in " + path);
  }
  return table;
}

PageGuard SSTable::FetchPage(std::uint64_t page_index) {
  return pool_->Fetch(file_id_, page_index, file_);
}

void SSTable::DropFromCache() { pool_->EvictFile(file_id_); }

bool SSTable::MayContain(KeyType key) {
  if (footer_.bloom_num_bits == 0) return true;  // filters disabled
  for (std::uint32_t probe = 0; probe < footer_.bloom_num_hashes; ++probe) {
    const std::uint64_t bit =
        bloom::BitIndex(key, probe, footer_.bloom_num_bits);
    const std::uint64_t byte = bit / 8;
    // The filter bits live in ordinary pages of the SST, fetched through the
    // buffer pool exactly like index and leaf pages.
    PageGuard guard = FetchPage(footer_.bloom_start_page + byte / kPageSize);
    const std::uint8_t b = guard.data()[byte % kPageSize];
    if ((b & (1u << (bit % 8))) == 0) return false;
  }
  return true;
}

std::uint64_t SSTable::FindLeafPage(KeyType key) {
  if (footer_.num_leaf_pages == 0) return 0;
  if (key > footer_.max_key) return footer_.num_leaf_pages;

  std::uint64_t page_index = footer_.root_page_index;
  for (std::uint32_t level = footer_.tree_height; level > 0; --level) {
    PageGuard guard = FetchPage(page_index);
    const auto* index = reinterpret_cast<const InternalPage*>(guard.data());
    std::uint32_t slot = LowerBoundInIndex(*index, key);
    // key <= max_key of the table guarantees a child exists; the clamp is pure
    // defence against a corrupt page.
    if (slot >= index->num_children) slot = index->num_children - 1;
    page_index = index->children[slot].page_index;
  }
  return page_index;
}

SSTable::LookupResult SSTable::Get(KeyType key, ValueType* value) {
  // Cheapest checks first: the key range of the table, then the filter, then the
  // tree traversal that actually costs I/O.
  if (key < footer_.min_key || key > footer_.max_key) return LookupResult::kNotFound;
  if (!MayContain(key)) return LookupResult::kNotFound;

  const std::uint64_t leaf_index = FindLeafPage(key);
  if (leaf_index >= footer_.num_leaf_pages) return LookupResult::kNotFound;

  PageGuard guard = FetchPage(leaf_index);
  const auto* leaf = reinterpret_cast<const LeafPage*>(guard.data());
  const std::uint32_t slot = LowerBoundInLeaf(*leaf, key);
  if (slot >= leaf->num_entries || leaf->entries[slot].key != key) {
    return LookupResult::kNotFound;  // Bloom filter false positive, or a gap
  }
  if (leaf->entries[slot].tombstone) return LookupResult::kTombstone;
  *value = leaf->entries[slot].value;
  return LookupResult::kFound;
}

// Cursor over one SST.
//
// The current leaf page is copied out of the buffer pool into a private page
// buffer and the pin is released immediately.  A merging iterator keeps one
// cursor per SST alive at the same time, and holding a pin for the whole life of
// each cursor would both pressure the pool and force its capacity to exceed the
// number of open tables.  The read still goes through the pool, so the page stays
// cached for whoever needs it next; the only extra cost is one 4 KiB copy per
// page, which is negligible next to the I/O it replaces.
class SSTableIterator : public InternalIterator {
 public:
  explicit SSTableIterator(SSTable* table) : table_(table), page_(1) {}

  void SeekToFirst() override {
    if (table_->num_leaf_pages() == 0) {
      valid_ = false;
      return;
    }
    LoadLeaf(0);
    position_ = 0;
    SkipEmptyLeaves();
  }

  void Seek(KeyType key) override {
    const std::uint64_t leaf = table_->FindLeafPage(key);
    if (leaf >= table_->num_leaf_pages()) {
      valid_ = false;
      return;
    }
    LoadLeaf(leaf);
    position_ = LowerBoundInLeaf(*leaf_page(), key);
    SkipEmptyLeaves();
  }

  bool Valid() const override { return valid_; }

  void Next() override {
    if (!valid_) return;
    ++position_;
    SkipEmptyLeaves();
  }

  KeyType Key() const override { return Current().key; }
  ValueType Value() const override { return Current().value; }
  bool IsTombstone() const override { return Current().tombstone != 0; }

 private:
  const LeafPage* leaf_page() const {
    return reinterpret_cast<const LeafPage*>(page_.data());
  }
  const DiskEntry& Current() const { return leaf_page()->entries[position_]; }

  void LoadLeaf(std::uint64_t leaf_index) {
    PageGuard guard = table_->FetchPage(leaf_index);
    std::memcpy(page_.data(), guard.data(), kPageSize);
    leaf_index_ = leaf_index;
    valid_ = true;
  }

  // Moves forward to the next leaf while the position has run off the end of the
  // current one.  Only the last leaf of a table can be short, so this loops at
  // most once in practice; the loop keeps it correct regardless.
  void SkipEmptyLeaves() {
    while (valid_ && position_ >= leaf_page()->num_entries) {
      if (leaf_index_ + 1 >= table_->num_leaf_pages()) {
        valid_ = false;
        return;
      }
      LoadLeaf(leaf_index_ + 1);
      position_ = 0;
    }
  }

  SSTable* table_;
  AlignedBuffer page_;
  std::uint64_t leaf_index_ = 0;
  std::uint32_t position_ = 0;
  bool valid_ = false;
};

std::unique_ptr<InternalIterator> SSTable::NewIterator() {
  return std::make_unique<SSTableIterator>(this);
}

}  // namespace kvstore
