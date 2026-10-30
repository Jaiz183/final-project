// Tests for the static B-tree SST format: building, point lookups and scans.
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "kvstore/buffer_pool.h"
#include "kvstore/format.h"
#include "kvstore/sst.h"
#include "test_framework.h"
#include "test_util.h"

namespace fs = std::filesystem;

using kvstore::BufferPool;
using kvstore::KeyType;
using kvstore::SSTable;
using kvstore::SSTableBuilder;
using kvstore::ValueType;

namespace {

// Builds an SST holding keys `stride`, 2*stride, ... `count`*stride with value
// 10*key, and returns it opened on `pool`.
std::unique_ptr<SSTable> BuildRange(const std::string& path, BufferPool* pool,
                                   int count, KeyType stride = 1,
                                   std::uint32_t bits_per_entry = 10) {
  SSTableBuilder builder(path, /*level=*/0, /*sequence=*/1, bits_per_entry,
                         static_cast<std::uint64_t>(count), /*use_direct_io=*/false);
  for (int i = 1; i <= count; ++i) {
    const KeyType key = i * stride;
    builder.Add(key, key * 10, /*tombstone=*/false);
  }
  builder.Finish();
  return SSTable::Open(path, pool, /*use_direct_io=*/false);
}

}  // namespace

TEST(sst, single_page_table) {
  kvstore::test::TempDir dir("sst_small");
  BufferPool pool(64);
  auto table = BuildRange(dir.path() + "/t.sst", &pool, 10);

  EXPECT_EQ(table->num_entries(), 10u);
  EXPECT_EQ(table->num_leaf_pages(), 1u);
  // Ten entries fit in one leaf, so that leaf is the root and the tree has no
  // index levels at all.
  EXPECT_EQ(table->tree_height(), 0u);
  EXPECT_EQ(table->min_key(), 1);
  EXPECT_EQ(table->max_key(), 10);

  ValueType value = 0;
  for (KeyType key = 1; key <= 10; ++key) {
    ASSERT_EQ(static_cast<int>(table->Get(key, &value)),
              static_cast<int>(SSTable::LookupResult::kFound));
    EXPECT_EQ(value, key * 10);
  }
}

TEST(sst, lookups_of_absent_keys) {
  kvstore::test::TempDir dir("sst_absent");
  BufferPool pool(64);
  // Even keys only, so every odd key inside the range must be reported absent.
  auto table = BuildRange(dir.path() + "/t.sst", &pool, 500, /*stride=*/2);

  ValueType value = 0;
  EXPECT_EQ(static_cast<int>(table->Get(0, &value)),
            static_cast<int>(SSTable::LookupResult::kNotFound));  // below min
  EXPECT_EQ(static_cast<int>(table->Get(100000, &value)),
            static_cast<int>(SSTable::LookupResult::kNotFound));  // above max
  for (KeyType key = 1; key < 1000; key += 2) {
    EXPECT_EQ(static_cast<int>(table->Get(key, &value)),
              static_cast<int>(SSTable::LookupResult::kNotFound));
  }
  for (KeyType key = 2; key <= 1000; key += 2) {
    ASSERT_EQ(static_cast<int>(table->Get(key, &value)),
              static_cast<int>(SSTable::LookupResult::kFound));
    EXPECT_EQ(value, key * 10);
  }
}

TEST(sst, multi_level_tree) {
  kvstore::test::TempDir dir("sst_deep");
  BufferPool pool(256);
  // More than kLeafCapacity * kInternalCapacity entries forces a second index
  // level, so the descent really does traverse an interior node.
  const int count = static_cast<int>(kvstore::kLeafCapacity *
                                     kvstore::kInternalCapacity + 5);
  auto table = BuildRange(dir.path() + "/t.sst", &pool, count);

  EXPECT_EQ(table->num_entries(), static_cast<std::uint64_t>(count));
  EXPECT_GE(table->tree_height(), 2u);
  EXPECT_EQ(table->num_leaf_pages(),
            (count + kvstore::kLeafCapacity - 1) / kvstore::kLeafCapacity);

  ValueType value = 0;
  for (KeyType key = 1; key <= count; key += 97) {
    ASSERT_EQ(static_cast<int>(table->Get(key, &value)),
              static_cast<int>(SSTable::LookupResult::kFound));
    EXPECT_EQ(value, key * 10);
  }
  // First and last key, the classic off-by-one boundaries of a B-tree descent.
  EXPECT_EQ(static_cast<int>(table->Get(1, &value)),
            static_cast<int>(SSTable::LookupResult::kFound));
  EXPECT_EQ(static_cast<int>(table->Get(count, &value)),
            static_cast<int>(SSTable::LookupResult::kFound));
}

TEST(sst, point_query_reads_only_a_few_pages) {
  kvstore::test::TempDir dir("sst_io");
  BufferPool pool(1024);
  const int count = 50000;
  auto table = BuildRange(dir.path() + "/t.sst", &pool, count);

  pool.ResetStats();
  ValueType value = 0;
  table->Get(count / 2, &value);
  // One page per index level, one leaf, plus at most one filter page per probe.
  // The point is that a lookup does *not* read the whole file.
  EXPECT_LE(pool.stats().page_reads, table->tree_height() + 1 + 12);
  EXPECT_LT(pool.stats().page_reads, table->num_leaf_pages());
}

TEST(sst, filter_rejects_keys_outside_the_table) {
  kvstore::test::TempDir dir("sst_filter");
  BufferPool pool(1024);
  const int count = 20000;
  auto table = BuildRange(dir.path() + "/t.sst", &pool, count, /*stride=*/2);

  // Every stored key must pass the filter: no false negatives.
  for (KeyType key = 2; key <= 2 * count; key += 2) {
    ASSERT_TRUE(table->MayContain(key));
  }
  // Most absent keys inside the range should be rejected by the filter alone.
  int rejected = 0;
  int tried = 0;
  for (KeyType key = 1; key < 2 * count; key += 2) {
    ++tried;
    if (!table->MayContain(key)) ++rejected;
  }
  EXPECT_GT(rejected, tried * 9 / 10);
}

TEST(sst, filter_can_be_disabled) {
  kvstore::test::TempDir dir("sst_nofilter");
  BufferPool pool(256);
  auto table = BuildRange(dir.path() + "/t.sst", &pool, 1000, /*stride=*/2,
                          /*bits_per_entry=*/0);

  EXPECT_EQ(table->footer().bloom_num_bits, 0u);
  EXPECT_EQ(table->footer().bloom_num_pages, 0u);
  // With no filter every key "may" be present, and lookups still give the right
  // answers -- only the skipping optimisation is lost.
  EXPECT_TRUE(table->MayContain(3));
  ValueType value = 0;
  EXPECT_EQ(static_cast<int>(table->Get(3, &value)),
            static_cast<int>(SSTable::LookupResult::kNotFound));
  EXPECT_EQ(static_cast<int>(table->Get(4, &value)),
            static_cast<int>(SSTable::LookupResult::kFound));
}

TEST(sst, tombstones_round_trip) {
  kvstore::test::TempDir dir("sst_tombstone");
  BufferPool pool(64);
  const std::string path = dir.path() + "/t.sst";
  {
    SSTableBuilder builder(path, 0, 1, 10, 3, false);
    builder.Add(1, 100, false);
    builder.Add(2, 0, /*tombstone=*/true);
    builder.Add(3, 300, false);
    EXPECT_EQ(builder.Finish(), 3u);
  }
  auto table = SSTable::Open(path, &pool, false);

  ValueType value = 0;
  EXPECT_EQ(static_cast<int>(table->Get(1, &value)),
            static_cast<int>(SSTable::LookupResult::kFound));
  EXPECT_EQ(static_cast<int>(table->Get(2, &value)),
            static_cast<int>(SSTable::LookupResult::kTombstone));
  EXPECT_EQ(static_cast<int>(table->Get(3, &value)),
            static_cast<int>(SSTable::LookupResult::kFound));
}

TEST(sst, iterator_scans_everything_in_order) {
  kvstore::test::TempDir dir("sst_scan");
  BufferPool pool(256);
  const int count = 5000;
  auto table = BuildRange(dir.path() + "/t.sst", &pool, count, /*stride=*/3);

  auto it = table->NewIterator();
  int seen = 0;
  KeyType previous = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ++seen;
    EXPECT_GT(it->Key(), previous);
    previous = it->Key();
    EXPECT_EQ(it->Value(), it->Key() * 10);
    EXPECT_FALSE(it->IsTombstone());
  }
  EXPECT_EQ(seen, count);
  EXPECT_EQ(previous, static_cast<KeyType>(count) * 3);
}

TEST(sst, iterator_seek) {
  kvstore::test::TempDir dir("sst_seek");
  BufferPool pool(256);
  auto table = BuildRange(dir.path() + "/t.sst", &pool, 2000, /*stride=*/10);

  auto it = table->NewIterator();

  it->Seek(10);  // the very first key
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 10);

  it->Seek(5000);  // exact match in the middle
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 5000);

  it->Seek(5001);  // between two keys
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 5010);

  it->Seek(-100);  // before the first key
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 10);

  it->Seek(20000);  // the very last key
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), 20000);
  it->Next();
  EXPECT_FALSE(it->Valid());

  it->Seek(20001);  // past the end
  EXPECT_FALSE(it->Valid());
}

TEST(sst, iterator_crosses_page_boundaries) {
  kvstore::test::TempDir dir("sst_pages");
  BufferPool pool(256);
  // Exactly two full leaves plus one entry: checks the last-page and
  // page-boundary cases at once.
  const int count = static_cast<int>(kvstore::kLeafCapacity) * 2 + 1;
  auto table = BuildRange(dir.path() + "/t.sst", &pool, count);
  EXPECT_EQ(table->num_leaf_pages(), 3u);

  auto it = table->NewIterator();
  it->Seek(static_cast<KeyType>(kvstore::kLeafCapacity));  // last key of leaf 0
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), static_cast<KeyType>(kvstore::kLeafCapacity));
  it->Next();  // must step into leaf 1
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), static_cast<KeyType>(kvstore::kLeafCapacity) + 1);

  it->Seek(count);  // the single entry on the short last leaf
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->Key(), count);
  it->Next();
  EXPECT_FALSE(it->Valid());
}

TEST(sst, builder_rejects_out_of_order_keys) {
  kvstore::test::TempDir dir("sst_order");
  const std::string path = dir.path() + "/t.sst";
  SSTableBuilder builder(path, 0, 1, 10, 4, false);
  builder.Add(5, 50, false);
  EXPECT_THROWS(builder.Add(5, 50, false));  // duplicates are not allowed either
  EXPECT_THROWS(builder.Add(4, 40, false));
  builder.Add(6, 60, false);
  builder.Finish();
  EXPECT_THROWS(builder.Finish());
}

TEST(sst, empty_builder_writes_nothing) {
  kvstore::test::TempDir dir("sst_empty");
  const std::string path = dir.path() + "/t.sst";
  SSTableBuilder builder(path, 0, 1, 10, 0, false);
  EXPECT_EQ(builder.Finish(), 0u);
  // The file exists but has no footer, so opening it must fail rather than
  // silently produce a garbage table.  The database deletes such files instead.
  BufferPool pool(8);
  EXPECT_THROWS(SSTable::Open(path, &pool, false));
}

TEST(sst, footer_records_the_geometry) {
  kvstore::test::TempDir dir("sst_footer");
  BufferPool pool(256);
  auto table = BuildRange(dir.path() + "/t.sst", &pool, 1000);

  const kvstore::Footer& footer = table->footer();
  EXPECT_EQ(footer.magic, kvstore::kSSTMagic);
  EXPECT_EQ(footer.format_version, kvstore::kFormatVersion);
  EXPECT_EQ(footer.level, 0u);
  EXPECT_EQ(footer.sequence, 1u);
  EXPECT_EQ(footer.min_key, 1);
  EXPECT_EQ(footer.max_key, 1000);

  // This is the worked example in docs/FILE_FORMAT.md; keeping it as an
  // assertion means the document cannot drift away from the code.
  // 1000 entries at 170 per leaf -> 6 leaves (pages 0-5); 6 children fit in one
  // index page (page 6, the root); 1000*10 bits of filter fit in one page
  // (page 7); the footer is page 8.
  EXPECT_EQ(footer.num_entries, 1000u);
  EXPECT_EQ(footer.num_leaf_pages, 6u);
  EXPECT_EQ(footer.tree_height, 1u);
  EXPECT_EQ(footer.root_page_index, 6u);
  EXPECT_EQ(footer.bloom_start_page, 7u);
  EXPECT_EQ(footer.bloom_num_pages, 1u);
  EXPECT_EQ(footer.bloom_num_bits, kvstore::kPageSize * 8);
  EXPECT_EQ(fs::file_size(dir.path() + "/t.sst"), 9 * kvstore::kPageSize);
}

TEST(sst, leaf_pages_are_a_contiguous_prefix_of_the_file) {
  // The layout invariant the SST iterator depends on: leaves occupy pages
  // 0 .. num_leaf_pages-1, so "the next leaf" is simply the next page.  A large
  // table is used because that is where index pages would interleave with
  // leaves if they were written as soon as they filled up.
  kvstore::test::TempDir dir("sst_prefix");
  BufferPool pool(512);
  const int count = static_cast<int>(kvstore::kLeafCapacity *
                                     kvstore::kInternalCapacity + 5);
  auto table = BuildRange(dir.path() + "/t.sst", &pool, count);

  EXPECT_GE(table->footer().root_page_index, table->num_leaf_pages());
  EXPECT_GE(table->footer().bloom_start_page, table->footer().root_page_index);

  // Every leaf ordinal must really be a leaf page holding the expected keys.
  auto it = table->NewIterator();
  it->SeekToFirst();
  for (KeyType key = 1; key <= count; ++key) {
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(it->Key(), key);
    it->Next();
  }
  EXPECT_FALSE(it->Valid());
}

TEST(sst, file_name_encodes_level_and_sequence) {
  EXPECT_EQ(kvstore::SSTFileName(0, 1), std::string("sst_L0_S0000000001.sst"));
  EXPECT_EQ(kvstore::SSTFileName(3, 42), std::string("sst_L3_S0000000042.sst"));
}
