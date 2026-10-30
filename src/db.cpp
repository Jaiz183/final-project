#include "kvstore/db.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <utility>

#include "kvstore/merging_iterator.h"

namespace fs = std::filesystem;

namespace kvstore {

// ---------------------------------------------------------------------------
// DBIterator: turns the raw merged stream into the client-visible view
// ---------------------------------------------------------------------------

// The merged stream delivers every version of every key, ordered by key and then
// by age (newest first).  For each key this iterator therefore keeps the first
// version it sees, skips the rest, and hides the key entirely when that newest
// version is a tombstone.  That single rule satisfies all three requirements the
// specification places on iterators: sorted order, one entry per key, and newest
// version wins with deletes suppressed.
class Database::DBIterator : public Iterator {
 public:
  DBIterator(Database* db, std::unique_ptr<InternalIterator> merged)
      : db_(db), merged_(std::move(merged)) {
    ++db_->live_iterators_;
  }

  ~DBIterator() override { --db_->live_iterators_; }

  void SeekToFirst() override {
    merged_->SeekToFirst();
    AdvanceToVisible();
  }

  void Seek(KeyType key) override {
    merged_->Seek(key);
    AdvanceToVisible();
  }

  bool Valid() const override { return valid_; }

  void Next() override {
    if (!valid_) return;
    AdvanceToVisible();
  }

  KeyType Key() const override {
    if (!valid_) throw Error("Iterator::Key on an invalid iterator");
    return key_;
  }

  ValueType Value() const override {
    if (!valid_) throw Error("Iterator::Value on an invalid iterator");
    return value_;
  }

 private:
  // Consumes whole keys from the merged stream until a live one is found.  On
  // return the merged stream is positioned on the first version of the *next*
  // distinct key, so Next() is just another call to this function.
  void AdvanceToVisible() {
    while (merged_->Valid()) {
      const KeyType key = merged_->Key();
      const ValueType value = merged_->Value();
      const bool tombstone = merged_->IsTombstone();

      // Drop the obsolete versions of this key.
      do {
        merged_->Next();
      } while (merged_->Valid() && merged_->Key() == key);

      if (!tombstone) {
        key_ = key;
        value_ = value;
        valid_ = true;
        return;
      }
    }
    valid_ = false;
  }

  Database* db_;
  std::unique_ptr<InternalIterator> merged_;
  KeyType key_ = 0;
  ValueType value_ = 0;
  bool valid_ = false;
};

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

Database::Database(const Options& options) : options_(options) {
  if (options_.size_ratio < 2) throw Error("Options::size_ratio must be >= 2");
  if (options_.memtable_size_bytes < sizeof(DiskEntry)) {
    throw Error("Options::memtable_size_bytes must hold at least one entry");
  }
  pool_ = std::make_unique<BufferPool>(options_.buffer_pool_capacity_pages,
                                       options_.buffer_pool_hash_buckets);
}

Database::~Database() {
  // A destructor must not throw, but silently losing the memtable would be a
  // nasty way to find out that Close() failed -- so say so on stderr.  The usual
  // cause is an iterator that is still alive; the data it was protecting is
  // exactly the data that has now been dropped.
  try {
    Close();
  } catch (const std::exception& e) {
    std::fprintf(stderr,
                 "kvstore: ~Database could not close cleanly, unflushed writes "
                 "have been lost: %s\n",
                 e.what());
  } catch (...) {
    std::fprintf(stderr, "kvstore: ~Database could not close cleanly\n");
  }
}

void Database::EnsureOpen() const {
  if (!open_) throw Error("database is not open");
}

void Database::EnsureNoLiveIterators(const char* operation) const {
  if (live_iterators_ != 0) {
    throw Error(std::string(operation) +
                " is not allowed while an iterator is alive: iterators hold "
                "references into the memtable and the SSTs. Destroy the "
                "iterator first.");
  }
}

std::string Database::SSTPath(std::uint32_t level,
                              std::uint64_t sequence) const {
  return (fs::path(dir_) / SSTFileName(level, sequence)).string();
}

void Database::SortLevel(std::size_t level) {
  // Youngest run first, so that a point query inside a level stops at the newest
  // version it can find.
  std::sort(levels_[level].begin(), levels_[level].end(),
            [](const std::unique_ptr<SSTable>& a,
               const std::unique_ptr<SSTable>& b) {
              return a->sequence() > b->sequence();
            });
}

void Database::Open(const std::string& name) {
  if (open_) throw Error("database is already open");

  if (!fs::exists(name)) {
    if (!options_.create_if_missing) {
      throw Error("database directory does not exist: " + name);
    }
    fs::create_directories(name);
  } else if (!fs::is_directory(name)) {
    throw Error("database path exists but is not a directory: " + name);
  }
  dir_ = name;

  // Find out whether this filesystem actually honours O_DIRECT, by trying it once
  // on a throwaway file.  Experiments that report I/O counts need to know, since
  // a silent fallback leaves the OS page cache acting as a second buffer pool.
  direct_io_effective_ = false;
  if (options_.use_direct_io) {
    const std::string probe = (fs::path(dir_) / ".direct_io_probe").string();
    {
      File f = File::Create(probe, /*direct=*/true);
      direct_io_effective_ = f.is_direct();
    }
    RemoveFile(probe);
  }

  // Recover the LSM structure by reading the footer of every SST in the
  // directory.  The footers carry the level and the creation sequence, so no
  // separate manifest file is needed.
  levels_.clear();
  std::vector<std::unique_ptr<SSTable>> tables;
  for (const auto& entry : fs::directory_iterator(dir_)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".sst") continue;
    tables.push_back(
        SSTable::Open(entry.path().string(), pool_.get(), options_.use_direct_io));
  }

  next_sequence_ = 1;
  for (auto& table : tables) {
    const std::size_t level = table->level();
    if (levels_.size() <= level) levels_.resize(level + 1);
    next_sequence_ = std::max(next_sequence_, table->sequence() + 1);
    levels_[level].push_back(std::move(table));
  }
  for (std::size_t level = 0; level < levels_.size(); ++level) SortLevel(level);

  open_ = true;
  num_flushes_ = 0;
  num_compactions_ = 0;
  // A well-formed directory is already within the size ratio; this only matters
  // if a previous run was killed between writing an SST and deleting its inputs.
  MaybeCompact();
}

void Database::Close() {
  if (!open_) return;
  EnsureNoLiveIterators("Close");
  FlushMemtable();  // no-op when the memtable is empty
  // Release the cached pages before the tables go away, so that a Database
  // reopened on the same buffer pool starts from a clean cache.
  for (auto& level : levels_) {
    for (auto& table : level) table->DropFromCache();
  }
  levels_.clear();  // closes every file descriptor
  open_ = false;
}

void Database::Put(KeyType key, ValueType value) {
  EnsureOpen();
  EnsureNoLiveIterators("Put");
  memtable_.Put(key, value, /*tombstone=*/false);
  if (memtable_.ApproximateSizeBytes() >= options_.memtable_size_bytes) {
    FlushMemtable();
    MaybeCompact();
  }
}

void Database::Delete(KeyType key) {
  EnsureOpen();
  EnsureNoLiveIterators("Delete");
  // A delete is an ordinary write of a tombstone: the key may live in an SST that
  // we must not rewrite, so the deletion has to be recorded as a new version and
  // resolved on the read path and during compaction.
  memtable_.Put(key, 0, /*tombstone=*/true);
  if (memtable_.ApproximateSizeBytes() >= options_.memtable_size_bytes) {
    FlushMemtable();
    MaybeCompact();
  }
}

bool Database::Get(KeyType key, ValueType* value) {
  EnsureOpen();

  bool tombstone = false;
  if (memtable_.Get(key, value, &tombstone)) {
    return !tombstone;  // the memtable always holds the newest version
  }

  // Youngest to oldest: lower levels are newer, and within a level runs are
  // sorted by descending sequence number.  The first version found is the
  // newest one, so the search stops there -- including when it is a tombstone.
  for (auto& level : levels_) {
    for (auto& table : level) {
      switch (table->Get(key, value)) {
        case SSTable::LookupResult::kFound:
          return true;
        case SSTable::LookupResult::kTombstone:
          return false;
        case SSTable::LookupResult::kNotFound:
          break;
      }
    }
  }
  return false;
}

std::vector<std::unique_ptr<InternalIterator>> Database::CollectIterators(
    bool include_memtable) {
  std::vector<std::unique_ptr<InternalIterator>> children;
  if (include_memtable && !memtable_.empty()) {
    children.push_back(memtable_.NewIterator());
  }
  for (auto& level : levels_) {
    for (auto& table : level) children.push_back(table->NewIterator());
  }
  return children;
}

std::unique_ptr<Iterator> Database::NewIterator() {
  EnsureOpen();
  auto merged =
      std::make_unique<MergingIterator>(CollectIterators(/*include_memtable=*/true));
  return std::make_unique<DBIterator>(this, std::move(merged));
}

void Database::FlushMemtable() {
  if (memtable_.empty()) return;

  const std::uint64_t sequence = next_sequence_++;
  const std::string path = SSTPath(0, sequence);
  SSTableBuilder builder(path, /*level=*/0, sequence,
                         options_.bloom_bits_per_entry, memtable_.num_entries(),
                         options_.use_direct_io);

  // The memtable is already sorted and already holds at most one version of each
  // key, so a single ordered walk is all the flush needs.  Tombstones are written
  // out as well: older versions of those keys may still exist in lower levels,
  // and only a compaction that reaches the bottom of the tree can drop them.
  auto it = memtable_.NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    builder.Add(it->Key(), it->Value(), it->IsTombstone());
  }
  it.reset();
  builder.Finish();

  memtable_.Clear();
  if (levels_.empty()) levels_.resize(1);
  levels_[0].push_back(SSTable::Open(path, pool_.get(), options_.use_direct_io));
  SortLevel(0);
  ++num_flushes_;
}

void Database::MaybeCompact() {
  // Working downwards is enough: compacting level i can only overfill level
  // i + 1, which is visited later in the same sweep.
  for (std::size_t level = 0; level < levels_.size(); ++level) {
    // CompactLevel can shrink levels_ (when a merge produces nothing, the
    // trailing empty levels are dropped), so the bound is rechecked here too.
    while (level < levels_.size() &&
           levels_[level].size() >= options_.size_ratio) {
      CompactLevel(level);
    }
  }
}

void Database::CompactLevel(std::size_t level) {
  const std::size_t target = level + 1;
  if (levels_.size() <= target) levels_.resize(target + 1);

  std::vector<std::unique_ptr<SSTable>> inputs = std::move(levels_[level]);
  levels_[level].clear();

  // A tombstone may only be discarded once nothing older can be hiding beneath
  // it.  Since the merge consumes every run of `level`, the only places an older
  // version could survive are `target` and the levels below it.  If those are all
  // empty the output becomes the oldest data in the tree and tombstones can go.
  bool anything_below = false;
  for (std::size_t l = target; l < levels_.size(); ++l) {
    if (!levels_[l].empty()) anything_below = true;
  }
  const bool drop_tombstones = !anything_below;

  std::uint64_t expected_entries = 0;
  for (const auto& table : inputs) expected_entries += table->num_entries();

  const std::uint64_t sequence = next_sequence_++;
  const std::string path = SSTPath(static_cast<std::uint32_t>(target), sequence);
  std::uint64_t written = 0;
  {
    SSTableBuilder builder(path, static_cast<std::uint32_t>(target), sequence,
                           options_.bloom_bits_per_entry, expected_entries,
                           options_.use_direct_io);

    // Merge the runs youngest first, so that for every key the merged stream
    // yields the newest version before the obsolete ones.
    std::vector<std::unique_ptr<InternalIterator>> children;
    children.reserve(inputs.size());
    for (auto& table : inputs) children.push_back(table->NewIterator());
    MergingIterator merged(std::move(children));

    // Streaming merge: the only buffers involved are one leaf page per input
    // cursor and one page per level of the output tree, so an SST much larger
    // than memory is merged without ever being held in memory.
    for (merged.SeekToFirst(); merged.Valid();) {
      const KeyType key = merged.Key();
      const ValueType value = merged.Value();
      const bool tombstone = merged.IsTombstone();
      do {
        merged.Next();
      } while (merged.Valid() && merged.Key() == key);

      if (tombstone && drop_tombstones) continue;
      builder.Add(key, value, tombstone);
    }
    written = builder.Finish();
  }  // the cursors are gone here, so no input page is pinned any more

  // Retire the inputs: drop their cached pages, close them, delete the files.
  for (auto& table : inputs) {
    table->DropFromCache();
    const std::string input_path = table->path();
    table.reset();
    RemoveFile(input_path);
  }
  inputs.clear();

  if (written == 0) {
    // Everything merged away (all tombstones at the bottom level).
    RemoveFile(path);
  } else {
    levels_[target].push_back(
        SSTable::Open(path, pool_.get(), options_.use_direct_io));
    SortLevel(target);
  }

  // Drop trailing empty levels so that LevelSizes() and the tombstone check
  // reflect the real shape of the tree.
  while (!levels_.empty() && levels_.back().empty()) levels_.pop_back();

  ++num_compactions_;
}

std::vector<std::size_t> Database::LevelSizes() const {
  std::vector<std::size_t> sizes;
  sizes.reserve(levels_.size());
  for (const auto& level : levels_) sizes.push_back(level.size());
  return sizes;
}

std::uint64_t Database::TotalSSTEntries() const {
  std::uint64_t n = 0;
  for (const auto& level : levels_) {
    for (const auto& table : level) n += table->num_entries();
  }
  return n;
}

std::size_t Database::NumSSTs() const {
  std::size_t n = 0;
  for (const auto& level : levels_) n += level.size();
  return n;
}

}  // namespace kvstore
