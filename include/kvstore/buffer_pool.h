// Page cache sitting between the SSTs and the filesystem.
#ifndef KVSTORE_BUFFER_POOL_H_
#define KVSTORE_BUFFER_POOL_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kvstore/file.h"
#include "kvstore/types.h"

namespace kvstore {

class BufferPool;

// RAII pin on a cached page.  While a guard is alive the page cannot be evicted,
// so the pointer returned by data() stays valid.  Guards are deliberately short
// lived: the read path pins a page, reads what it needs, and releases it before
// fetching the next one.
class PageGuard {
 public:
  PageGuard() = default;
  ~PageGuard();

  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;
  PageGuard(PageGuard&& other) noexcept;
  PageGuard& operator=(PageGuard&& other) noexcept;

  bool valid() const { return pool_ != nullptr; }
  const std::uint8_t* data() const { return data_; }
  void Reset();

 private:
  friend class BufferPool;
  PageGuard(BufferPool* pool, std::size_t frame, const std::uint8_t* data)
      : pool_(pool), frame_(frame), data_(data) {}

  BufferPool* pool_ = nullptr;
  std::size_t frame_ = 0;
  const std::uint8_t* data_ = nullptr;
};

// A fixed-size pool of 4 KiB frames, indexed by a hash table that maps a page
// identifier to the frame holding it, with LRU replacement.
//
// A page is identified by the pair (file id, page index within that file), as
// the specification suggests.  File ids are handed out by SSTable::Open() and
// never reused, so a page of a compacted-away SST can never be confused with a
// page of a new one.
//
// Collisions are resolved by chaining: each bucket is the head of a singly
// linked list threaded through the frames themselves (Frame::hash_next), so the
// table needs no per-entry allocation.  LRU order is kept in a second intrusive
// list over the same frames.  Fetch() therefore costs O(1) expected time and
// allocates nothing.
class BufferPool {
 public:
  struct Stats {
    std::uint64_t hits = 0;        // page found in the pool
    std::uint64_t misses = 0;      // page had to be read from the file
    std::uint64_t evictions = 0;   // pages thrown out to make room
    std::uint64_t page_reads = 0;  // pread() calls issued (== misses)
  };

  // `capacity_pages` frames are allocated up front as one aligned slab.
  // `hash_buckets` is rounded up to a power of two; 0 picks 2 * capacity.
  BufferPool(std::size_t capacity_pages, std::size_t hash_buckets = 0);

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // Returns a pinned guard on page `page_index` of `file`, reading it through
  // `file` on a miss.  `file_id` must uniquely identify `file`.
  PageGuard Fetch(std::uint32_t file_id, std::uint64_t page_index, File& file);

  // Drops every cached page belonging to `file_id`.  Called when an SST is
  // deleted by a compaction so that its frames become reusable immediately.
  void EvictFile(std::uint32_t file_id);

  std::size_t capacity_pages() const { return frames_.size(); }
  std::size_t num_cached_pages() const { return num_cached_; }
  const Stats& stats() const { return stats_; }
  void ResetStats() { stats_ = Stats(); }

 private:
  friend class PageGuard;

  static constexpr std::size_t kInvalid = static_cast<std::size_t>(-1);

  struct Frame {
    std::uint32_t file_id = 0;
    std::uint64_t page_index = 0;
    bool occupied = false;
    std::uint32_t pin_count = 0;
    std::size_t hash_next = kInvalid;  // next frame in this hash bucket
    std::size_t lru_prev = kInvalid;   // towards the most recently used end
    std::size_t lru_next = kInvalid;   // towards the least recently used end
  };

  std::size_t BucketOf(std::uint32_t file_id, std::uint64_t page_index) const;
  std::size_t Lookup(std::uint32_t file_id, std::uint64_t page_index) const;
  void HashInsert(std::size_t frame);
  void HashRemove(std::size_t frame);
  void LruPushFront(std::size_t frame);
  void LruRemove(std::size_t frame);
  // Returns a free frame, evicting the least recently used unpinned page if the
  // pool is full.
  std::size_t AcquireFrame();
  void Unpin(std::size_t frame);

  std::vector<Frame> frames_;
  AlignedBuffer pages_;
  std::vector<std::size_t> buckets_;
  std::size_t bucket_mask_ = 0;
  std::vector<std::size_t> free_frames_;
  std::size_t lru_head_ = kInvalid;  // most recently used
  std::size_t lru_tail_ = kInvalid;  // least recently used (evicted first)
  std::size_t num_cached_ = 0;
  Stats stats_;
};

}  // namespace kvstore

#endif  // KVSTORE_BUFFER_POOL_H_
