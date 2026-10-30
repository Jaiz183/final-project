// End-to-end tests for the database: the API required by the specification
// (Put, Get, Delete, iterators, Seek, persistence) plus the LSM-tree behaviour
// that sits behind it.
#include <algorithm>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "kvstore/db.h"
#include "test_framework.h"
#include "test_util.h"

using kvstore::Database;
using kvstore::KeyType;
using kvstore::Options;
using kvstore::ValueType;

namespace {

// Options that flush after roughly `entries` writes, so that tests can create
// many SSTs and force compactions without writing much data.  Direct I/O is off
// in the tests: it is not available on every filesystem (including some CI
// temporary directories) and none of these tests measure I/O volume.
Options SmallOptions(std::size_t entries) {
  Options options;
  options.memtable_size_bytes = entries * sizeof(kvstore::DiskEntry);
  options.buffer_pool_capacity_pages = 256;
  options.use_direct_io = false;
  return options;
}

// Collects the whole visible contents of the database through an iterator.
std::vector<std::pair<KeyType, ValueType>> Scan(Database* db) {
  std::vector<std::pair<KeyType, ValueType>> out;
  auto it = db->NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out.emplace_back(it->Key(), it->Value());
  }
  return out;
}

}  // namespace

TEST(db, put_and_get_from_the_memtable) {
  kvstore::test::TempDir dir("db_basic");
  Database db(SmallOptions(1000));
  db.Open(dir.path());

  for (KeyType key = 0; key < 100; ++key) db.Put(key, key * 3);

  ValueType value = 0;
  for (KeyType key = 0; key < 100; ++key) {
    ASSERT_TRUE(db.Get(key, &value));
    EXPECT_EQ(value, key * 3);
  }
  EXPECT_FALSE(db.Get(100, &value));
  EXPECT_FALSE(db.Get(-1, &value));
  EXPECT_EQ(db.NumSSTs(), 0u);  // nothing was large enough to flush
  db.Close();
}

TEST(db, put_and_get_across_many_ssts) {
  kvstore::test::TempDir dir("db_ssts");
  Database db(SmallOptions(50));
  db.Open(dir.path());

  const int n = 2000;
  for (KeyType key = 0; key < n; ++key) db.Put(key, key + 1);

  EXPECT_GT(db.num_flushes(), 0u);
  EXPECT_GT(db.num_compactions(), 0u);

  ValueType value = 0;
  for (KeyType key = 0; key < n; ++key) {
    ASSERT_TRUE(db.Get(key, &value));
    EXPECT_EQ(value, key + 1);
  }
  for (KeyType key = n; key < n + 100; ++key) EXPECT_FALSE(db.Get(key, &value));
  db.Close();
}

TEST(db, updates_return_the_newest_version) {
  kvstore::test::TempDir dir("db_updates");
  Database db(SmallOptions(40));
  db.Open(dir.path());

  const int n = 500;
  for (KeyType key = 0; key < n; ++key) db.Put(key, 1);
  // Three more rounds, each overwriting every key.  The older versions end up in
  // older SSTs and must never win.
  for (int round = 2; round <= 4; ++round) {
    for (KeyType key = 0; key < n; ++key) db.Put(key, round);
  }

  ValueType value = 0;
  for (KeyType key = 0; key < n; ++key) {
    ASSERT_TRUE(db.Get(key, &value));
    EXPECT_EQ(value, 4);
  }

  // The iterator must also report each key exactly once, with the newest value.
  const auto scan = Scan(&db);
  ASSERT_EQ(scan.size(), static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    EXPECT_EQ(scan[i].first, i);
    EXPECT_EQ(scan[i].second, 4);
  }
  db.Close();
}

TEST(db, deletes_hide_keys_from_get_and_iterators) {
  kvstore::test::TempDir dir("db_deletes");
  Database db(SmallOptions(40));
  db.Open(dir.path());

  const int n = 400;
  for (KeyType key = 0; key < n; ++key) db.Put(key, key);
  for (KeyType key = 0; key < n; key += 2) db.Delete(key);

  ValueType value = 0;
  for (KeyType key = 0; key < n; ++key) {
    if (key % 2 == 0) {
      EXPECT_FALSE(db.Get(key, &value));
    } else {
      ASSERT_TRUE(db.Get(key, &value));
      EXPECT_EQ(value, key);
    }
  }

  const auto scan = Scan(&db);
  ASSERT_EQ(scan.size(), static_cast<std::size_t>(n / 2));
  for (const auto& entry : scan) EXPECT_EQ(entry.first % 2, 1);
  db.Close();
}

TEST(db, deleting_an_absent_key_is_harmless) {
  kvstore::test::TempDir dir("db_del_absent");
  Database db(SmallOptions(1000));
  db.Open(dir.path());

  db.Delete(42);  // tombstone for a key that was never written
  ValueType value = 0;
  EXPECT_FALSE(db.Get(42, &value));
  EXPECT_EQ(Scan(&db).size(), 0u);

  // And writing it afterwards resurrects it.
  db.Put(42, 7);
  ASSERT_TRUE(db.Get(42, &value));
  EXPECT_EQ(value, 7);
  db.Close();
}

TEST(db, a_key_can_be_deleted_and_rewritten_repeatedly) {
  kvstore::test::TempDir dir("db_churn");
  Database db(SmallOptions(30));
  db.Open(dir.path());

  ValueType value = 0;
  for (int round = 0; round < 40; ++round) {
    db.Put(7, round);
    // Push the write into an SST so the next round's versions land in a
    // different component of the tree.
    for (KeyType filler = 0; filler < 40; ++filler) db.Put(1000 + filler, filler);
    ASSERT_TRUE(db.Get(7, &value));
    EXPECT_EQ(value, round);

    db.Delete(7);
    for (KeyType filler = 0; filler < 40; ++filler) db.Put(2000 + filler, filler);
    EXPECT_FALSE(db.Get(7, &value));
  }
  db.Close();
}

TEST(db, iterator_traversal_is_sorted_and_complete) {
  kvstore::test::TempDir dir("db_iter");
  Database db(SmallOptions(60));
  db.Open(dir.path());

  // Insert in a scrambled order and across several SSTs.
  std::vector<KeyType> keys;
  for (KeyType key = 0; key < 1000; ++key) keys.push_back(key);
  std::shuffle(keys.begin(), keys.end(), std::mt19937_64(7));
  for (KeyType key : keys) db.Put(key, key * 2);

  const auto scan = Scan(&db);
  ASSERT_EQ(scan.size(), 1000u);
  for (std::size_t i = 0; i < scan.size(); ++i) {
    EXPECT_EQ(scan[i].first, static_cast<KeyType>(i));
    EXPECT_EQ(scan[i].second, static_cast<KeyType>(i) * 2);
    if (i > 0) EXPECT_LT(scan[i - 1].first, scan[i].first);
  }
  db.Close();
}

TEST(db, iterator_seek) {
  kvstore::test::TempDir dir("db_seek");
  Database db(SmallOptions(60));
  db.Open(dir.path());
  for (KeyType key = 0; key < 1000; key += 10) db.Put(key, key);

  auto it = db.NewIterator();

  it->Seek(500);  // exact match
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 500);

  it->Seek(501);  // gap: the next larger key
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 510);

  it->Seek(-1);  // before the start
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 0);

  it->Seek(990);  // the last key
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 990);
  it->Next();
  EXPECT_FALSE(it->Valid());

  it->Seek(991);  // past the end
  EXPECT_FALSE(it->Valid());

  // Seeking backwards must work: an iterator is repositionable, not one-shot.
  it->Seek(100);
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 100);
}

TEST(db, range_query_via_seek_and_next) {
  // The specification asks for range queries to be expressible on top of the
  // iterator rather than as their own API; this is that pattern.
  kvstore::test::TempDir dir("db_range");
  Database db(SmallOptions(60));
  db.Open(dir.path());
  for (KeyType key = 0; key < 1000; ++key) db.Put(key, key);
  db.Delete(300);
  db.Delete(400);

  const KeyType start = 250;
  const KeyType end = 450;  // exclusive
  std::vector<KeyType> found;
  auto it = db.NewIterator();
  for (it->Seek(start); it->Valid() && it->Key() < end; it->Next()) {
    found.push_back(it->Key());
    EXPECT_EQ(it->Value(), it->Key());
  }

  EXPECT_EQ(found.size(), static_cast<std::size_t>(end - start - 2));
  EXPECT_EQ(found.front(), start);
  EXPECT_EQ(found.back(), end - 1);
  EXPECT_TRUE(std::find(found.begin(), found.end(), 300) == found.end());
  EXPECT_TRUE(std::find(found.begin(), found.end(), 400) == found.end());
}

TEST(db, iterator_merges_memtable_with_ssts) {
  kvstore::test::TempDir dir("db_merge");
  Database db(SmallOptions(50));
  db.Open(dir.path());

  // Even keys first; they will be flushed into SSTs.
  for (KeyType key = 0; key < 400; key += 2) db.Put(key, key);
  // Odd keys last, few enough to stay in the memtable.
  const std::size_t before = db.NumSSTs();
  for (KeyType key = 1; key < 40; key += 2) db.Put(key, key);
  EXPECT_EQ(db.NumSSTs(), before);
  EXPECT_GT(db.memtable_entries(), 0u);

  // The interleaving of the two sources is exactly what the merge must get right.
  const auto scan = Scan(&db);
  ASSERT_EQ(scan.size(), 200u + 20u);
  for (std::size_t i = 0; i + 1 < scan.size(); ++i) {
    EXPECT_LT(scan[i].first, scan[i + 1].first);
  }
  EXPECT_EQ(scan[0].first, 0);
  EXPECT_EQ(scan[1].first, 1);
  EXPECT_EQ(scan[2].first, 2);
  db.Close();
}

TEST(db, iterator_on_an_empty_database) {
  kvstore::test::TempDir dir("db_empty_iter");
  Database db(SmallOptions(100));
  db.Open(dir.path());

  auto it = db.NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->Seek(0);
  EXPECT_FALSE(it->Valid());
  EXPECT_THROWS(it->Key());
  EXPECT_THROWS(it->Value());
}

TEST(db, iterator_hides_a_key_deleted_in_a_newer_component) {
  // The tombstone lives in the memtable while the value lives in an SST, which is
  // the case the merge has to resolve across sources.
  kvstore::test::TempDir dir("db_tomb_merge");
  Database db(SmallOptions(50));
  db.Open(dir.path());

  for (KeyType key = 0; key < 300; ++key) db.Put(key, key);
  EXPECT_GT(db.NumSSTs(), 0u);
  db.Delete(150);
  db.Put(151, 999);

  ValueType value = 0;
  EXPECT_FALSE(db.Get(150, &value));
  ASSERT_TRUE(db.Get(151, &value));
  EXPECT_EQ(value, 999);

  const auto scan = Scan(&db);
  EXPECT_EQ(scan.size(), 299u);
  for (const auto& entry : scan) {
    EXPECT_NE(entry.first, 150);
    if (entry.first == 151) EXPECT_EQ(entry.second, 999);
  }
  db.Close();
}

TEST(db, persistence_across_close_and_open) {
  kvstore::test::TempDir dir("db_persist");
  const int n = 3000;

  {
    Database db(SmallOptions(100));
    db.Open(dir.path());
    for (KeyType key = 0; key < n; ++key) db.Put(key, key * 7);
    for (KeyType key = 0; key < n; key += 3) db.Delete(key);
    db.Put(n, 4242);
    // This last write has not been flushed yet; Close() must persist it anyway.
    EXPECT_GT(db.memtable_entries(), 0u);
    db.Close();
  }

  {
    Database db(SmallOptions(100));
    db.Open(dir.path());
    ValueType value = 0;
    for (KeyType key = 0; key < n; ++key) {
      if (key % 3 == 0) {
        EXPECT_FALSE(db.Get(key, &value));
      } else {
        ASSERT_TRUE(db.Get(key, &value));
        EXPECT_EQ(value, key * 7);
      }
    }
    ASSERT_TRUE(db.Get(n, &value));
    EXPECT_EQ(value, 4242);

    // And the iterator view survives too.
    const auto scan = Scan(&db);
    EXPECT_EQ(scan.size(), static_cast<std::size_t>(n - (n + 2) / 3) + 1);

    // Writes continue on top of the recovered state.
    db.Put(0, 12345);
    ASSERT_TRUE(db.Get(0, &value));
    EXPECT_EQ(value, 12345);
    db.Close();
  }

  // A third session sees the update written by the second.
  {
    Database db(SmallOptions(100));
    db.Open(dir.path());
    ValueType value = 0;
    ASSERT_TRUE(db.Get(0, &value));
    EXPECT_EQ(value, 12345);
    db.Close();
  }
}

TEST(db, reopening_preserves_the_level_layout) {
  kvstore::test::TempDir dir("db_layout");
  std::vector<std::size_t> layout;
  {
    Database db(SmallOptions(40));
    db.Open(dir.path());
    for (KeyType key = 0; key < 2000; ++key) db.Put(key, key);
    db.Close();
  }
  {
    Database db(SmallOptions(40));
    db.Open(dir.path());
    layout = db.LevelSizes();
    // With a size ratio of 2 every level holds at most one run.
    for (std::size_t size : layout) EXPECT_LE(size, 1u);
    EXPECT_GT(layout.size(), 1u);
    db.Close();
  }
}

TEST(db, compaction_keeps_levels_within_the_size_ratio) {
  kvstore::test::TempDir dir("db_compact");
  Options options = SmallOptions(40);
  options.size_ratio = 3;
  Database db(options);
  db.Open(dir.path());

  for (KeyType key = 0; key < 4000; ++key) {
    db.Put(key, key);
    for (std::size_t size : db.LevelSizes()) EXPECT_LT(size, options.size_ratio);
  }

  ValueType value = 0;
  for (KeyType key = 0; key < 4000; ++key) {
    ASSERT_TRUE(db.Get(key, &value));
    EXPECT_EQ(value, key);
  }
  db.Close();
}

TEST(db, compaction_reclaims_space_from_updates) {
  // Rewriting the same small key range over and over produces many obsolete
  // versions.  Compaction must collapse them, so the total number of entries
  // stored on disk stays close to the number of live keys.
  kvstore::test::TempDir dir("db_space");
  Database db(SmallOptions(100));
  db.Open(dir.path());

  const int distinct_keys = 200;
  for (int round = 0; round < 60; ++round) {
    for (KeyType key = 0; key < distinct_keys; ++key) db.Put(key, round);
  }
  db.Close();

  // Reopen and count what actually survived on disk.
  Database reopened(SmallOptions(100));
  reopened.Open(dir.path());
  EXPECT_EQ(Scan(&reopened).size(), static_cast<std::size_t>(distinct_keys));
  ValueType value = 0;
  ASSERT_TRUE(reopened.Get(0, &value));
  EXPECT_EQ(value, 59);
  reopened.Close();
}

TEST(db, tombstones_are_dropped_at_the_bottom_level) {
  // Set up the smallest situation in which dropping is legal.  Forty writes fill
  // the memtable exactly and flush as one run; forty deletes flush as a second.
  // Level 0 now holds two runs, so they merge into level 1 -- and because level 1
  // and everything below it is empty, no older version can be hiding under those
  // tombstones and they can be discarded.  Every key in the merge is a dead
  // tombstone, so the output is empty and no SST is written at all.
  kvstore::test::TempDir dir("db_tomb_drop");
  const std::size_t entries_per_flush = 40;
  Database db(SmallOptions(entries_per_flush));
  db.Open(dir.path());

  for (KeyType key = 0; key < 40; ++key) db.Put(key, key);
  ASSERT_EQ(db.NumSSTs(), 1u);
  EXPECT_EQ(db.memtable_entries(), 0u);

  for (KeyType key = 0; key < 40; ++key) db.Delete(key);
  EXPECT_EQ(db.num_compactions(), 1u);
  EXPECT_EQ(db.NumSSTs(), 0u);
  EXPECT_EQ(db.TotalSSTEntries(), 0u);

  ValueType value = 0;
  for (KeyType key = 0; key < 40; ++key) EXPECT_FALSE(db.Get(key, &value));
  db.Close();
}

TEST(db, deleting_everything_leaves_an_empty_database) {
  kvstore::test::TempDir dir("db_delete_all");
  Database db(SmallOptions(40));
  db.Open(dir.path());

  const int n = 600;
  for (KeyType key = 0; key < n; ++key) db.Put(key, key);
  for (KeyType key = 0; key < n; ++key) db.Delete(key);
  // Compaction has collapsed the obsolete versions: far fewer than the 1200
  // entries that were written are still stored.
  EXPECT_LT(db.TotalSSTEntries(), 1200u);
  db.Close();

  Database reopened(SmallOptions(40));
  reopened.Open(dir.path());
  ValueType value = 0;
  for (KeyType key = 0; key < n; ++key) EXPECT_FALSE(reopened.Get(key, &value));
  EXPECT_EQ(Scan(&reopened).size(), 0u);
  reopened.Close();
}

TEST(db, reads_go_through_the_buffer_pool) {
  kvstore::test::TempDir dir("db_pool");
  Database db(SmallOptions(200));
  db.Open(dir.path());
  for (KeyType key = 0; key < 4000; ++key) db.Put(key, key);

  // A first pass warms the pool; the second must find most pages already there.
  ValueType value = 0;
  for (KeyType key = 0; key < 4000; ++key) db.Get(key, &value);
  db.ResetBufferPoolStats();
  for (KeyType key = 0; key < 4000; ++key) db.Get(key, &value);

  const auto& stats = db.buffer_pool_stats();
  EXPECT_GT(stats.hits, 0u);
  EXPECT_GT(stats.hits, stats.misses);
  EXPECT_EQ(stats.page_reads, stats.misses);
  db.Close();
}

TEST(db, mutating_while_an_iterator_is_alive_is_rejected) {
  kvstore::test::TempDir dir("db_guard");
  Database db(SmallOptions(100));
  db.Open(dir.path());
  db.Put(1, 1);

  {
    auto it = db.NewIterator();
    it->SeekToFirst();
    // Writing here would rebalance the memtable and could compact away the SSTs
    // the iterator is reading, so it is refused rather than silently corrupting.
    EXPECT_THROWS(db.Put(2, 2));
    EXPECT_THROWS(db.Delete(1));
    EXPECT_THROWS(db.Close());
  }

  // Once the iterator is gone, writing works again.
  db.Put(2, 2);
  ValueType value = 0;
  EXPECT_TRUE(db.Get(2, &value));
  db.Close();
}

TEST(db, operations_on_a_closed_database_are_rejected) {
  kvstore::test::TempDir dir("db_closed");
  Database db(SmallOptions(100));
  ValueType value = 0;
  EXPECT_THROWS(db.Put(1, 1));
  EXPECT_THROWS(db.Get(1, &value));
  EXPECT_THROWS(db.Delete(1));
  EXPECT_THROWS(db.NewIterator());

  db.Open(dir.path());
  EXPECT_THROWS(db.Open(dir.path()));  // already open
  db.Close();
  db.Close();  // idempotent
  EXPECT_THROWS(db.Put(1, 1));
}

TEST(db, rejects_invalid_options) {
  Options bad_ratio;
  bad_ratio.size_ratio = 1;
  EXPECT_THROWS(Database{bad_ratio});

  Options tiny_memtable;
  tiny_memtable.memtable_size_bytes = 1;
  EXPECT_THROWS(Database{tiny_memtable});

  Options no_pool;
  no_pool.buffer_pool_capacity_pages = 0;
  EXPECT_THROWS(Database{no_pool});
}

TEST(db, open_refuses_a_missing_directory_when_asked) {
  kvstore::test::TempDir dir("db_missing");
  Options options = SmallOptions(100);
  options.create_if_missing = false;
  Database db(options);
  EXPECT_THROWS(db.Open(dir.path() + "/does_not_exist"));
}

TEST(db, works_without_filters) {
  // Disabling the filters must change performance only, never answers.
  kvstore::test::TempDir dir("db_nofilter");
  Options options = SmallOptions(60);
  options.bloom_bits_per_entry = 0;
  Database db(options);
  db.Open(dir.path());

  for (KeyType key = 0; key < 1000; ++key) db.Put(key, key);
  for (KeyType key = 0; key < 1000; key += 5) db.Delete(key);

  ValueType value = 0;
  for (KeyType key = 0; key < 1000; ++key) {
    if (key % 5 == 0) {
      EXPECT_FALSE(db.Get(key, &value));
    } else {
      ASSERT_TRUE(db.Get(key, &value));
      EXPECT_EQ(value, key);
    }
  }
  db.Close();
}

TEST(db, handles_negative_and_extreme_keys) {
  kvstore::test::TempDir dir("db_extremes");
  Database db(SmallOptions(40));
  db.Open(dir.path());

  const std::vector<KeyType> keys = {
      std::numeric_limits<KeyType>::min(), -1000000, -1, 0, 1, 1000000,
      std::numeric_limits<KeyType>::max()};
  for (KeyType key : keys) db.Put(key, key);
  for (KeyType filler = 0; filler < 200; ++filler) db.Put(filler + 10, filler);

  ValueType value = 0;
  for (KeyType key : keys) {
    ASSERT_TRUE(db.Get(key, &value));
    EXPECT_EQ(value, key);
  }

  auto it = db.NewIterator();
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), std::numeric_limits<KeyType>::min());
  it->Seek(std::numeric_limits<KeyType>::max());
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), std::numeric_limits<KeyType>::max());
  it->Next();
  EXPECT_FALSE(it->Valid());
  it.reset();
  db.Close();
}

TEST(db, matches_a_reference_map_under_random_operations) {
  // The strongest correctness check in the suite: a long random mix of writes,
  // updates and deletes, with the whole database compared against std::map after
  // every phase.  The memtable is tiny, so this runs through many flushes and
  // compactions.
  kvstore::test::TempDir dir("db_model");
  Options options = SmallOptions(30);
  Database db(options);
  db.Open(dir.path());

  std::map<KeyType, ValueType> model;
  std::mt19937_64 rng(2026);
  std::uniform_int_distribution<KeyType> key_dist(0, 800);

  for (int phase = 0; phase < 12; ++phase) {
    for (int i = 0; i < 700; ++i) {
      const KeyType key = key_dist(rng);
      if (rng() % 5 == 0) {
        db.Delete(key);
        model.erase(key);
      } else {
        const ValueType value = static_cast<ValueType>(rng() % 100000);
        db.Put(key, value);
        model[key] = value;
      }
    }

    // Point lookups, including keys that were never written.
    ValueType value = 0;
    for (KeyType key = 0; key <= 850; ++key) {
      const auto found = model.find(key);
      if (found == model.end()) {
        EXPECT_FALSE(db.Get(key, &value));
      } else {
        ASSERT_TRUE(db.Get(key, &value));
        EXPECT_EQ(value, found->second);
      }
    }

    // Full scan.
    const auto scan = Scan(&db);
    ASSERT_EQ(scan.size(), model.size());
    std::size_t i = 0;
    for (const auto& expected : model) {
      EXPECT_EQ(scan[i].first, expected.first);
      EXPECT_EQ(scan[i].second, expected.second);
      ++i;
    }

    // Seek to a handful of random positions and compare with lower_bound.
    {
      auto it = db.NewIterator();
      for (int probe = 0; probe < 50; ++probe) {
        const KeyType target = key_dist(rng);
        it->Seek(target);
        const auto expected = model.lower_bound(target);
        if (expected == model.end()) {
          EXPECT_FALSE(it->Valid());
        } else {
          ASSERT_TRUE(it->Valid());
          EXPECT_EQ(it->Key(), expected->first);
          EXPECT_EQ(it->Value(), expected->second);
        }
      }
    }
  }

  // And everything survives a restart.
  db.Close();
  Database reopened(options);
  reopened.Open(dir.path());
  const auto scan = Scan(&reopened);
  ASSERT_EQ(scan.size(), model.size());
  std::size_t i = 0;
  for (const auto& expected : model) {
    EXPECT_EQ(scan[i].first, expected.first);
    EXPECT_EQ(scan[i].second, expected.second);
    ++i;
  }
  reopened.Close();
}

TEST(db, handles_data_larger_than_the_buffer_pool) {
  // A pool of 16 pages cannot hold anything like the whole dataset, so this
  // exercises eviction under a real workload rather than in isolation.
  kvstore::test::TempDir dir("db_small_pool");
  Options options = SmallOptions(100);
  options.buffer_pool_capacity_pages = 16;
  Database db(options);
  db.Open(dir.path());

  const int n = 20000;
  for (KeyType key = 0; key < n; ++key) db.Put(key, key ^ 0x5a5a);

  ValueType value = 0;
  for (KeyType key = 0; key < n; key += 7) {
    ASSERT_TRUE(db.Get(key, &value));
    EXPECT_EQ(value, key ^ 0x5a5a);
  }
  EXPECT_GT(db.buffer_pool_stats().evictions, 0u);

  const auto scan = Scan(&db);
  EXPECT_EQ(scan.size(), static_cast<std::size_t>(n));
  db.Close();
}

TEST(db, matches_a_reference_map_under_varied_configurations) {
  // The same correctness check as above, repeated across combinations of the
  // parameters that change how the system behaves structurally: the size ratio
  // (how often and how widely levels merge), the filter budget (including 0,
  // which turns the filters off), a buffer pool small enough to thrash, and
  // reopening the database part way through.  A bug that only shows up for, say,
  // T = 4 or for a filterless build should not survive this.
  struct Variant {
    std::size_t memtable_entries;
    std::size_t pool_pages;
    std::uint32_t bloom_bits;
    std::uint32_t size_ratio;
  };
  const std::vector<Variant> variants = {
      {37, 13, 5, 3}, {54, 18, 8, 4},  {20, 8, 0, 2},
      {71, 28, 14, 3}, {88, 33, 17, 4}, {45, 10, 0, 2},
  };

  int seed = 0;
  for (const Variant& variant : variants) {
    ++seed;
    kvstore::test::TempDir dir("db_variant" + std::to_string(seed));
    Options options = SmallOptions(variant.memtable_entries);
    options.buffer_pool_capacity_pages = variant.pool_pages;
    options.bloom_bits_per_entry = variant.bloom_bits;
    options.size_ratio = variant.size_ratio;

    Database db(options);
    db.Open(dir.path());

    std::map<KeyType, ValueType> model;
    std::mt19937_64 rng(static_cast<std::uint64_t>(seed) * 7919);
    std::uniform_int_distribution<KeyType> key_dist(-400, 400);

    for (int phase = 0; phase < 4; ++phase) {
      for (int i = 0; i < 600; ++i) {
        const KeyType key = key_dist(rng);
        if (rng() % 4 == 0) {
          db.Delete(key);
          model.erase(key);
        } else {
          const ValueType value = static_cast<ValueType>(rng() % 1000);
          db.Put(key, value);
          model[key] = value;
        }
      }
      // Every other phase, check that the state survives a restart as well.
      if (phase % 2 == 1) {
        db.Close();
        db.Open(dir.path());
      }

      ValueType value = 0;
      for (KeyType key = -450; key <= 450; ++key) {
        const auto expected = model.find(key);
        if (expected == model.end()) {
          ASSERT_FALSE(db.Get(key, &value));
        } else {
          ASSERT_TRUE(db.Get(key, &value));
          ASSERT_EQ(value, expected->second);
        }
      }

      const auto scan = Scan(&db);
      ASSERT_EQ(scan.size(), model.size());
      std::size_t i = 0;
      for (const auto& expected : model) {
        ASSERT_EQ(scan[i].first, expected.first);
        ASSERT_EQ(scan[i].second, expected.second);
        ++i;
      }
    }
    db.Close();
  }
}
