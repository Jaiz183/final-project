// Tests for the AVL memtable.
#include <cmath>
#include <random>
#include <vector>

#include "kvstore/memtable.h"
#include "test_framework.h"

using kvstore::KeyType;
using kvstore::Memtable;
using kvstore::ValueType;

TEST(memtable, put_and_get) {
  Memtable table;
  EXPECT_TRUE(table.empty());

  table.Put(10, 100);
  table.Put(5, 50);
  table.Put(20, 200);
  EXPECT_FALSE(table.empty());
  EXPECT_EQ(table.num_entries(), 3u);

  ValueType value = 0;
  bool tombstone = true;
  ASSERT_TRUE(table.Get(10, &value, &tombstone));
  EXPECT_EQ(value, 100);
  EXPECT_FALSE(tombstone);

  EXPECT_TRUE(table.Get(5, &value, &tombstone));
  EXPECT_EQ(value, 50);
  EXPECT_TRUE(table.Get(20, &value, &tombstone));
  EXPECT_EQ(value, 200);
  EXPECT_FALSE(table.Get(7, &value, &tombstone));
}

TEST(memtable, put_overwrites_in_place) {
  Memtable table;
  table.Put(1, 1);
  table.Put(1, 2);
  table.Put(1, 3);
  // Rewriting a key must not grow the memtable: it holds one version per key.
  EXPECT_EQ(table.num_entries(), 1u);

  ValueType value = 0;
  bool tombstone = true;
  EXPECT_TRUE(table.Get(1, &value, &tombstone));
  EXPECT_EQ(value, 3);
  EXPECT_FALSE(tombstone);
}

TEST(memtable, tombstones_are_visible_to_get) {
  Memtable table;
  table.Put(1, 42);
  table.Put(1, 0, /*tombstone=*/true);

  ValueType value = 0;
  bool tombstone = false;
  ASSERT_TRUE(table.Get(1, &value, &tombstone));
  EXPECT_TRUE(tombstone);

  // Writing the key again clears the tombstone.
  table.Put(1, 7);
  EXPECT_TRUE(table.Get(1, &value, &tombstone));
  EXPECT_FALSE(tombstone);
  EXPECT_EQ(value, 7);
}

TEST(memtable, iteration_is_sorted) {
  Memtable table;
  const std::vector<KeyType> keys = {50, 10, 90, 30, 70, 20, 80, 40, 60};
  for (KeyType key : keys) table.Put(key, key * 2);

  auto it = table.NewIterator();
  std::vector<KeyType> seen;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    seen.push_back(it->Key());
    EXPECT_EQ(it->Value(), it->Key() * 2);
  }

  ASSERT_EQ(seen.size(), keys.size());
  for (std::size_t i = 0; i + 1 < seen.size(); ++i) EXPECT_LT(seen[i], seen[i + 1]);
  EXPECT_EQ(seen.front(), 10);
  EXPECT_EQ(seen.back(), 90);
}

TEST(memtable, seek) {
  Memtable table;
  for (KeyType key = 0; key < 100; key += 10) table.Put(key, key);

  auto it = table.NewIterator();

  it->Seek(30);  // exact match
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 30);

  it->Seek(31);  // between keys: first key greater than the target
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 40);

  it->Seek(-5);  // before everything
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 0);

  it->Seek(1000);  // past everything
  EXPECT_FALSE(it->Valid());
}

TEST(memtable, iteration_over_empty_table) {
  Memtable table;
  auto it = table.NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->Seek(0);
  EXPECT_FALSE(it->Valid());
}

TEST(memtable, clear_and_size_accounting) {
  Memtable table;
  for (KeyType key = 0; key < 100; ++key) table.Put(key, key);
  EXPECT_EQ(table.num_entries(), 100u);
  EXPECT_EQ(table.ApproximateSizeBytes(), 100u * sizeof(kvstore::DiskEntry));

  table.Clear();
  EXPECT_TRUE(table.empty());
  EXPECT_EQ(table.num_entries(), 0u);
  EXPECT_EQ(table.ApproximateSizeBytes(), 0u);

  ValueType value = 0;
  bool tombstone = false;
  EXPECT_FALSE(table.Get(5, &value, &tombstone));
}

TEST(memtable, stays_balanced_under_sorted_inserts) {
  // Ascending keys are the worst case for an unbalanced BST, which would grow to
  // height n.  AVL must keep the height within 1.44 * log2(n + 2).
  Memtable table;
  const int n = 10000;
  for (KeyType key = 0; key < n; ++key) table.Put(key, key);

  const double bound = 1.4405 * std::log2(static_cast<double>(n) + 2.0);
  EXPECT_LE(static_cast<double>(table.height()), bound);

  // And the ordering is still intact.
  auto it = table.NewIterator();
  KeyType expected = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) EXPECT_EQ(it->Key(), expected++);
  EXPECT_EQ(expected, n);
}

TEST(memtable, random_operations_match_a_reference_map) {
  std::mt19937_64 rng(12345);
  std::uniform_int_distribution<KeyType> keys(0, 500);
  Memtable table;
  std::vector<std::pair<bool, ValueType>> reference(501, {false, 0});

  for (int i = 0; i < 20000; ++i) {
    const KeyType key = keys(rng);
    if (rng() % 4 == 0) {
      table.Put(key, 0, /*tombstone=*/true);
      reference[key] = {true, -1};
    } else {
      const ValueType value = static_cast<ValueType>(rng() % 1000);
      table.Put(key, value);
      reference[key] = {true, value};
    }
  }

  for (KeyType key = 0; key <= 500; ++key) {
    ValueType value = 0;
    bool tombstone = false;
    const bool present = table.Get(key, &value, &tombstone);
    EXPECT_EQ(present, reference[key].first);
    if (!present) continue;
    if (reference[key].second == -1) {
      EXPECT_TRUE(tombstone);
    } else {
      EXPECT_FALSE(tombstone);
      EXPECT_EQ(value, reference[key].second);
    }
  }
}
