// Tests for the buffer pool: hashing, hits, LRU replacement and pinning.
#include <cstring>
#include <string>
#include <vector>

#include "kvstore/buffer_pool.h"
#include "kvstore/file.h"
#include "test_framework.h"
#include "test_util.h"

using kvstore::AlignedBuffer;
using kvstore::BufferPool;
using kvstore::File;
using kvstore::kPageSize;
using kvstore::PageGuard;

namespace {

// Writes a file of `num_pages` pages where every byte of page i equals i % 251,
// so a reader can tell which page it got.
void WritePattern(const std::string& path, std::size_t num_pages) {
  File file = File::Create(path, /*direct=*/false);
  AlignedBuffer page(1);
  for (std::size_t i = 0; i < num_pages; ++i) {
    std::memset(page.data(), static_cast<int>(i % 251), kPageSize);
    file.WritePage(i, page.data());
  }
  file.Sync();
  file.Close();
}

bool PageHoldsPattern(const std::uint8_t* data, std::size_t page_index) {
  const auto expected = static_cast<std::uint8_t>(page_index % 251);
  for (std::size_t i = 0; i < kPageSize; ++i) {
    if (data[i] != expected) return false;
  }
  return true;
}

}  // namespace

TEST(buffer_pool, fetch_returns_the_right_page) {
  kvstore::test::TempDir dir("pool_fetch");
  const std::string path = dir.path() + "/pages";
  WritePattern(path, 32);

  BufferPool pool(8);
  File file = File::OpenForRead(path, /*direct=*/false);
  for (std::size_t i = 0; i < 32; ++i) {
    PageGuard guard = pool.Fetch(1, i, file);
    EXPECT_TRUE(PageHoldsPattern(guard.data(), i));
  }
}

TEST(buffer_pool, repeated_fetches_hit_the_cache) {
  kvstore::test::TempDir dir("pool_hits");
  const std::string path = dir.path() + "/pages";
  WritePattern(path, 4);

  BufferPool pool(8);
  File file = File::OpenForRead(path, /*direct=*/false);

  for (int round = 0; round < 10; ++round) {
    for (std::size_t i = 0; i < 4; ++i) {
      PageGuard guard = pool.Fetch(1, i, file);
      EXPECT_TRUE(PageHoldsPattern(guard.data(), i));
    }
  }

  // Four cold misses, then everything comes from the pool.
  EXPECT_EQ(pool.stats().misses, 4u);
  EXPECT_EQ(pool.stats().page_reads, 4u);
  EXPECT_EQ(pool.stats().hits, 36u);
  EXPECT_EQ(pool.stats().evictions, 0u);
  EXPECT_EQ(pool.num_cached_pages(), 4u);
}

TEST(buffer_pool, evicts_the_least_recently_used_page) {
  kvstore::test::TempDir dir("pool_lru");
  const std::string path = dir.path() + "/pages";
  WritePattern(path, 8);

  BufferPool pool(3);
  File file = File::OpenForRead(path, /*direct=*/false);

  // Fill the pool with pages 0, 1, 2, then touch 0 so that 1 is the oldest.
  for (std::size_t i = 0; i < 3; ++i) pool.Fetch(1, i, file);
  pool.Fetch(1, 0, file);
  EXPECT_EQ(pool.stats().misses, 3u);
  EXPECT_EQ(pool.stats().hits, 1u);

  // Page 3 must evict page 1, the least recently used.
  pool.Fetch(1, 3, file);
  EXPECT_EQ(pool.stats().evictions, 1u);
  EXPECT_EQ(pool.num_cached_pages(), 3u);

  pool.Fetch(1, 0, file);  // still cached
  pool.Fetch(1, 2, file);  // still cached
  EXPECT_EQ(pool.stats().hits, 3u);
  pool.Fetch(1, 1, file);  // was evicted, so this must miss
  EXPECT_EQ(pool.stats().misses, 5u);
}

TEST(buffer_pool, pinned_pages_are_not_evicted) {
  kvstore::test::TempDir dir("pool_pin");
  const std::string path = dir.path() + "/pages";
  WritePattern(path, 16);

  BufferPool pool(2);
  File file = File::OpenForRead(path, /*direct=*/false);

  PageGuard pinned = pool.Fetch(1, 0, file);
  // Repeatedly fetch other pages: they can only ever reuse the second frame.
  for (std::size_t i = 1; i < 10; ++i) {
    PageGuard other = pool.Fetch(1, i, file);
    EXPECT_TRUE(PageHoldsPattern(other.data(), i));
    // The pinned page must still be intact behind our back.
    EXPECT_TRUE(PageHoldsPattern(pinned.data(), 0));
  }

  // With every frame pinned there is nothing left to evict, and the pool says so
  // instead of corrupting a page in use.
  PageGuard second = pool.Fetch(1, 1, file);
  EXPECT_THROWS(pool.Fetch(1, 2, file));

  // Releasing a guard makes its frame reusable again.
  second.Reset();
  PageGuard third = pool.Fetch(1, 2, file);
  EXPECT_TRUE(PageHoldsPattern(third.data(), 2));
}

TEST(buffer_pool, distinguishes_pages_of_different_files) {
  kvstore::test::TempDir dir("pool_files");
  const std::string path_a = dir.path() + "/a";
  const std::string path_b = dir.path() + "/b";
  WritePattern(path_a, 4);
  WritePattern(path_b, 4);

  BufferPool pool(8);
  File a = File::OpenForRead(path_a, /*direct=*/false);
  File b = File::OpenForRead(path_b, /*direct=*/false);

  // Page 1 of file 7 and page 1 of file 9 are different pages and must occupy
  // different frames, even though their page index is the same.
  PageGuard ga = pool.Fetch(7, 1, a);
  PageGuard gb = pool.Fetch(9, 1, b);
  EXPECT_NE(ga.data(), gb.data());
  EXPECT_EQ(pool.stats().misses, 2u);
  EXPECT_EQ(pool.num_cached_pages(), 2u);
}

TEST(buffer_pool, evict_file_drops_only_that_file) {
  kvstore::test::TempDir dir("pool_evict_file");
  const std::string path = dir.path() + "/pages";
  WritePattern(path, 8);

  BufferPool pool(16);
  File file = File::OpenForRead(path, /*direct=*/false);
  for (std::size_t i = 0; i < 4; ++i) pool.Fetch(1, i, file);
  for (std::size_t i = 0; i < 4; ++i) pool.Fetch(2, i, file);
  EXPECT_EQ(pool.num_cached_pages(), 8u);

  pool.EvictFile(1);
  EXPECT_EQ(pool.num_cached_pages(), 4u);

  pool.ResetStats();
  for (std::size_t i = 0; i < 4; ++i) pool.Fetch(2, i, file);
  EXPECT_EQ(pool.stats().hits, 4u);  // file 2 survived
  for (std::size_t i = 0; i < 4; ++i) pool.Fetch(1, i, file);
  EXPECT_EQ(pool.stats().misses, 4u);  // file 1 is gone
}

TEST(buffer_pool, survives_many_collisions) {
  // One bucket per frame and a working set far larger than the pool exercises
  // chaining, eviction and hash removal together.
  kvstore::test::TempDir dir("pool_collisions");
  const std::string path = dir.path() + "/pages";
  WritePattern(path, 200);

  BufferPool pool(16, /*hash_buckets=*/4);
  File file = File::OpenForRead(path, /*direct=*/false);
  for (int round = 0; round < 5; ++round) {
    for (std::size_t i = 0; i < 200; ++i) {
      PageGuard guard = pool.Fetch(1, i, file);
      EXPECT_TRUE(PageHoldsPattern(guard.data(), i));
    }
  }
  EXPECT_LE(pool.num_cached_pages(), pool.capacity_pages());
}

TEST(buffer_pool, rejects_a_zero_sized_pool) {
  EXPECT_THROWS(BufferPool(0));
}
