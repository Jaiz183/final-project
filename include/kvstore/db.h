// The public key-value store API.
#ifndef KVSTORE_DB_H_
#define KVSTORE_DB_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kvstore/buffer_pool.h"
#include "kvstore/iterator.h"
#include "kvstore/memtable.h"
#include "kvstore/options.h"
#include "kvstore/sst.h"
#include "kvstore/types.h"

namespace kvstore {

// An LSM-tree key-value store.
//
// Writes land in an in-memory AVL memtable.  When the memtable exceeds
// Options::memtable_size_bytes its contents are written, in key order, to a new
// immutable SST at level 0.  A level holding Options::size_ratio runs is merged
// into the level below, so level i always holds data strictly newer than level
// i+1.  Reads consult the memtable first and then the SSTs from youngest to
// oldest, stopping at the first version of the key they find.
//
// The store is single-threaded: one thread may use one Database at a time.
//
// Iterators observe the state of the database at the moment they are created and
// hold references into it.  The database must therefore not be modified while an
// iterator is alive; Put, Delete and Close throw an Error if one still is.
// Destroy the iterator first (let it leave scope) and then write.
class Database {
 public:
  explicit Database(const Options& options = Options());
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // Creates or reopens the database stored in directory `name`.  Every file of
  // the database lives in that one directory.  Reopening picks up all SSTs that
  // the previous session wrote.
  void Open(const std::string& name);

  // Stores `value` under `key`, replacing any previous version.
  void Put(KeyType key, ValueType value);

  // Reads the newest version of `key`.  Returns false if the key was never
  // written or its newest version is a delete marker.
  bool Get(KeyType key, ValueType* value);

  // Marks `key` as deleted by writing a tombstone.
  void Delete(KeyType key);

  // A cursor over the whole database in ascending key order, presenting each
  // live key once with its newest value.  Named NewIterator() rather than
  // Iterator() because Iterator is the name of the type it returns.
  std::unique_ptr<Iterator> NewIterator();

  // Flushes in-memory state so that the database can be reopened without losing
  // data, then releases all files.  Called automatically by the destructor.
  void Close();

  bool is_open() const { return open_; }
  const std::string& path() const { return dir_; }
  const Options& options() const { return options_; }

  // --- Introspection, for tests and experiments -----------------------------

  const BufferPool::Stats& buffer_pool_stats() const { return pool_->stats(); }
  void ResetBufferPoolStats() { pool_->ResetStats(); }

  // Whether O_DIRECT was actually granted by the filesystem.  False means the
  // OS page cache is still in play and absolute I/O numbers should be read with
  // that in mind.
  bool uses_direct_io() const { return direct_io_effective_; }

  // Number of SSTs at each LSM level, level 0 first.
  std::vector<std::size_t> LevelSizes() const;
  std::size_t NumSSTs() const;

  // Total number of entries stored in SSTs, tombstones included.  Compared
  // against the number of live keys this is the space amplification of the tree.
  std::uint64_t TotalSSTEntries() const;
  std::size_t memtable_entries() const { return memtable_.num_entries(); }

  // Number of compactions and flushes performed since Open().
  std::uint64_t num_flushes() const { return num_flushes_; }
  std::uint64_t num_compactions() const { return num_compactions_; }

 private:
  class DBIterator;

  void EnsureOpen() const;
  void EnsureNoLiveIterators(const char* operation) const;

  // Writes the memtable out as a new level-0 SST and empties it.
  void FlushMemtable();

  // Merges every run of `level` into `level + 1`.
  void CompactLevel(std::size_t level);

  // Compacts levels from the top down for as long as any of them is over the
  // size ratio.
  void MaybeCompact();

  // Child iterators for a merge, newest source first.  `include_memtable` is
  // false during compaction, which only merges SSTs.
  std::vector<std::unique_ptr<InternalIterator>> CollectIterators(
      bool include_memtable);

  std::string SSTPath(std::uint32_t level, std::uint64_t sequence) const;
  void SortLevel(std::size_t level);

  Options options_;
  std::string dir_;
  bool open_ = false;
  bool direct_io_effective_ = false;

  Memtable memtable_;
  std::unique_ptr<BufferPool> pool_;

  // levels_[i] holds the runs of LSM level i, youngest (largest sequence) first.
  std::vector<std::vector<std::unique_ptr<SSTable>>> levels_;

  std::uint64_t next_sequence_ = 1;
  std::uint64_t num_flushes_ = 0;
  std::uint64_t num_compactions_ = 0;
  std::size_t live_iterators_ = 0;
};

}  // namespace kvstore

#endif  // KVSTORE_DB_H_
