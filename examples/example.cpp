// A guided tour of the key-value store's API.
//
// Build and run it with:
//
//     make
//     ./build/example
//
// Every section below is independent, so you can read it top to bottom or jump
// to the operation you care about.  The point of the program is to show the
// intended usage of each call, including the two rules that are easy to get
// wrong: releasing an iterator before writing, and calling Close() so that data
// survives.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "kvstore/db.h"

namespace fs = std::filesystem;

using kvstore::Database;
using kvstore::KeyType;
using kvstore::Options;
using kvstore::ValueType;

namespace {

void Heading(const char* title) { std::printf("\n=== %s ===\n", title); }

// Prints [start, end) by seeking once and walking forward.  This is how a range
// query is expressed: the database has no separate range API, because it does
// not need one.
void PrintRange(Database* db, KeyType start, KeyType end) {
  auto it = db->NewIterator();
  std::printf("  keys in [%lld, %lld):", static_cast<long long>(start),
              static_cast<long long>(end));
  for (it->Seek(start); it->Valid() && it->Key() < end; it->Next()) {
    std::printf(" %lld=%lld", static_cast<long long>(it->Key()),
                static_cast<long long>(it->Value()));
  }
  std::printf("\n");
}

}  // namespace

int main() try {
  const std::string db_path = "example_db";
  fs::remove_all(db_path);  // start from a clean slate

  // -------------------------------------------------------------------------
  // 1. Opening a database
  // -------------------------------------------------------------------------
  // Options carries every tunable parameter; see include/kvstore/options.h.
  // The defaults are sensible, but a deliberately tiny memtable is used here so
  // that this small example still flushes and compacts, and section 8 has
  // something interesting to report.
  Options options;
  options.memtable_size_bytes = 64 * sizeof(kvstore::DiskEntry);  // ~64 entries
  options.buffer_pool_capacity_pages = 64;                        // 256 KiB
  options.bloom_bits_per_entry = 10;
  options.size_ratio = 2;

  Database db(options);
  db.Open(db_path);  // a *directory*, created if it does not exist

  Heading("1. Open");
  std::printf("  opened %s (direct I/O: %s)\n", db.path().c_str(),
              db.uses_direct_io() ? "yes" : "no — the OS page cache is active");

  // -------------------------------------------------------------------------
  // 2. Put and Get
  // -------------------------------------------------------------------------
  Heading("2. Put and Get");
  db.Put(10, 100);
  db.Put(20, 200);
  db.Put(30, 300);

  ValueType value = 0;
  // Get returns false for a key that is absent; that is a normal answer, not an
  // error, and *value is left alone.
  if (db.Get(20, &value)) std::printf("  Get(20) -> %lld\n", (long long)value);
  if (!db.Get(25, &value)) std::printf("  Get(25) -> not found\n");

  // -------------------------------------------------------------------------
  // 3. Updates
  // -------------------------------------------------------------------------
  // Writing an existing key stores a new version.  Nothing is overwritten on
  // disk; the read path simply resolves to the newest version.
  Heading("3. Updates");
  db.Put(20, 999);
  db.Get(20, &value);
  std::printf("  after Put(20, 999), Get(20) -> %lld\n", (long long)value);

  // -------------------------------------------------------------------------
  // 4. Deletes
  // -------------------------------------------------------------------------
  // A delete writes a tombstone, because the key may live in an immutable file
  // that cannot be edited.  Get and the iterators both hide it.
  Heading("4. Deletes");
  db.Delete(30);
  std::printf("  after Delete(30), Get(30) -> %s\n",
              db.Get(30, &value) ? "found (wrong!)" : "not found");
  db.Delete(12345);  // deleting an absent key is harmless
  db.Put(30, 3030);  // and a deleted key can be written again
  db.Get(30, &value);
  std::printf("  after Put(30, 3030), Get(30) -> %lld\n", (long long)value);

  // -------------------------------------------------------------------------
  // 5. Iterating over everything, in sorted order
  // -------------------------------------------------------------------------
  // Load enough data to span several SSTs, so that the iterator really has to
  // merge the memtable with files on disk.
  for (KeyType key = 0; key < 500; ++key) db.Put(key, key * 2);
  for (KeyType key = 0; key < 500; key += 5) db.Delete(key);

  Heading("5. Full scan");
  {
    auto it = db.NewIterator();
    std::size_t count = 0;
    KeyType first = 0;
    KeyType last = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      if (count == 0) first = it->Key();
      last = it->Key();
      ++count;
    }
    // 500 keys were written, every fifth one deleted -> 400 remain.  Each live
    // key appears exactly once, with its newest value, in ascending order.
    std::printf("  %zu live keys, from %lld to %lld\n", count,
                (long long)first, (long long)last);
  }  // the iterator is released here — see section 7

  // -------------------------------------------------------------------------
  // 6. Seek and range queries
  // -------------------------------------------------------------------------
  Heading("6. Seek and range queries");
  PrintRange(&db, 20, 40);  // note 20, 25, 30 and 35 are missing: deleted above

  {
    auto it = db.NewIterator();

    // Seek lands on the first key >= the target, so it works whether or not the
    // target itself exists.  35 was deleted above, hence the answer is 36.
    it->Seek(35);
    std::printf("  Seek(35) -> %lld (35 was deleted)\n", (long long)it->Key());

    it->Seek(100000);  // seeking past the end leaves the iterator invalid
    std::printf("  Seek(100000) -> Valid() == %s\n",
                it->Valid() ? "true" : "false");

    // An iterator is repositionable: Seek may be called again, in any direction.
    it->Seek(1);
    std::printf("  Seek(1) -> %lld\n", (long long)it->Key());
  }

  // -------------------------------------------------------------------------
  // 7. The one rule: no writes while an iterator is alive
  // -------------------------------------------------------------------------
  // An iterator holds references into the memtable and the SSTs that existed
  // when it was created.  A write could rebalance the memtable or compact those
  // files away, so Put, Delete and Close refuse to run and throw instead.
  Heading("7. Writes are refused while an iterator is alive");
  {
    auto it = db.NewIterator();
    it->SeekToFirst();
    try {
      db.Put(1, 1);
      std::printf("  (unreachable)\n");
    } catch (const kvstore::Error& e) {
      std::printf("  Put threw, as it should: %s\n", e.what());
    }
  }  // once the iterator is gone, writing works again
  db.Put(1, 1);
  std::printf("  after the iterator was released, Put(1, 1) succeeded\n");

  // -------------------------------------------------------------------------
  // 8. Inspecting the shape of the tree
  // -------------------------------------------------------------------------
  // These accessors exist for tests and experiments.  They are the quickest way
  // to see what the LSM-tree is actually doing.
  Heading("8. Introspection");
  std::printf("  flushes: %llu, compactions: %llu\n",
              (unsigned long long)db.num_flushes(),
              (unsigned long long)db.num_compactions());
  std::printf("  runs per level (level 0 first):");
  for (std::size_t runs : db.LevelSizes()) std::printf(" %zu", runs);
  std::printf("\n");
  std::printf("  %zu SSTs holding %llu entries, %zu still in the memtable\n",
              db.NumSSTs(), (unsigned long long)db.TotalSSTEntries(),
              db.memtable_entries());

  // Entries on disk exceed the number of live keys: obsolete versions and
  // tombstones survive until a compaction reaches the bottom of the tree.  That
  // difference is the space amplification of the tree.
  db.ResetBufferPoolStats();
  for (KeyType key = 0; key < 500; ++key) db.Get(key, &value);
  const auto& stats = db.buffer_pool_stats();
  std::printf("  500 lookups: %llu buffer pool hits, %llu misses (%llu page reads)\n",
              (unsigned long long)stats.hits,
              (unsigned long long)stats.misses,
              (unsigned long long)stats.page_reads);

  // -------------------------------------------------------------------------
  // 9. Close and reopen
  // -------------------------------------------------------------------------
  // Close() flushes whatever is still in memory, so the database can be reopened
  // without losing data.  There is no write-ahead log: a process that dies
  // before Close() loses the memtable (the SSTs already written are fine).
  Heading("9. Persistence across Close and Open");
  db.Close();

  Database reopened(options);
  reopened.Open(db_path);  // recovers the SSTs by reading their footers
  reopened.Get(1, &value);
  std::printf("  reopened: Get(1) -> %lld\n", (long long)value);
  std::printf("  reopened: Get(0) -> %s (it was deleted before closing)\n",
              reopened.Get(0, &value) ? "found (wrong!)" : "not found");
  std::printf("  reopened: runs per level:");
  for (std::size_t runs : reopened.LevelSizes()) std::printf(" %zu", runs);
  std::printf("\n");
  reopened.Close();

  fs::remove_all(db_path);
  std::printf("\nDone.\n");
  return 0;
} catch (const std::exception& e) {
  // Every unrecoverable error is reported as an exception (kvstore::Error, which
  // derives from std::runtime_error).
  std::fprintf(stderr, "example failed: %s\n", e.what());
  return 1;
}
