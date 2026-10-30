#include "kvstore/buffer_pool.h"

#include <utility>

namespace kvstore {

PageGuard::~PageGuard() { Reset(); }

PageGuard::PageGuard(PageGuard&& other) noexcept
    : pool_(other.pool_), frame_(other.frame_), data_(other.data_) {
  other.pool_ = nullptr;
  other.data_ = nullptr;
}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
  if (this != &other) {
    Reset();
    pool_ = other.pool_;
    frame_ = other.frame_;
    data_ = other.data_;
    other.pool_ = nullptr;
    other.data_ = nullptr;
  }
  return *this;
}

void PageGuard::Reset() {
  if (pool_ != nullptr) {
    pool_->Unpin(frame_);
    pool_ = nullptr;
    data_ = nullptr;
  }
}

namespace {

std::size_t RoundUpPow2(std::size_t n) {
  std::size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

}  // namespace

BufferPool::BufferPool(std::size_t capacity_pages, std::size_t hash_buckets) {
  if (capacity_pages == 0) throw Error("buffer pool capacity must be positive");
  frames_.resize(capacity_pages);
  pages_ = AlignedBuffer(capacity_pages);

  const std::size_t buckets =
      RoundUpPow2(hash_buckets == 0 ? capacity_pages * 2 : hash_buckets);
  buckets_.assign(buckets, kInvalid);
  bucket_mask_ = buckets - 1;

  free_frames_.reserve(capacity_pages);
  for (std::size_t i = capacity_pages; i > 0; --i) free_frames_.push_back(i - 1);
}

std::size_t BufferPool::BucketOf(std::uint32_t file_id,
                                 std::uint64_t page_index) const {
  // Mixing the two components rather than concatenating them keeps consecutive
  // pages of one file from clustering into neighbouring buckets.
  std::uint64_t h = (static_cast<std::uint64_t>(file_id) << 40) ^ page_index;
  h *= 0x9e3779b97f4a7c15ULL;
  h ^= h >> 29;
  return static_cast<std::size_t>(h) & bucket_mask_;
}

std::size_t BufferPool::Lookup(std::uint32_t file_id,
                               std::uint64_t page_index) const {
  for (std::size_t f = buckets_[BucketOf(file_id, page_index)]; f != kInvalid;
       f = frames_[f].hash_next) {
    if (frames_[f].file_id == file_id && frames_[f].page_index == page_index) {
      return f;
    }
  }
  return kInvalid;
}

void BufferPool::HashInsert(std::size_t frame) {
  const std::size_t b = BucketOf(frames_[frame].file_id, frames_[frame].page_index);
  frames_[frame].hash_next = buckets_[b];
  buckets_[b] = frame;
}

void BufferPool::HashRemove(std::size_t frame) {
  const std::size_t b = BucketOf(frames_[frame].file_id, frames_[frame].page_index);
  std::size_t* link = &buckets_[b];
  while (*link != kInvalid) {
    if (*link == frame) {
      *link = frames_[frame].hash_next;
      frames_[frame].hash_next = kInvalid;
      return;
    }
    link = &frames_[*link].hash_next;
  }
  throw Error("buffer pool hash table corruption: frame not in its bucket");
}

void BufferPool::LruPushFront(std::size_t frame) {
  frames_[frame].lru_prev = kInvalid;
  frames_[frame].lru_next = lru_head_;
  if (lru_head_ != kInvalid) frames_[lru_head_].lru_prev = frame;
  lru_head_ = frame;
  if (lru_tail_ == kInvalid) lru_tail_ = frame;
}

void BufferPool::LruRemove(std::size_t frame) {
  const std::size_t prev = frames_[frame].lru_prev;
  const std::size_t next = frames_[frame].lru_next;
  if (prev != kInvalid) {
    frames_[prev].lru_next = next;
  } else {
    lru_head_ = next;
  }
  if (next != kInvalid) {
    frames_[next].lru_prev = prev;
  } else {
    lru_tail_ = prev;
  }
  frames_[frame].lru_prev = kInvalid;
  frames_[frame].lru_next = kInvalid;
}

std::size_t BufferPool::AcquireFrame() {
  if (!free_frames_.empty()) {
    const std::size_t f = free_frames_.back();
    free_frames_.pop_back();
    return f;
  }
  // Walk from the least recently used end until an unpinned page is found.  Pages
  // are only pinned while a PageGuard is alive, so in the single-threaded
  // baseline the scan stops almost immediately.
  for (std::size_t f = lru_tail_; f != kInvalid; f = frames_[f].lru_prev) {
    if (frames_[f].pin_count == 0) {
      HashRemove(f);
      LruRemove(f);
      frames_[f].occupied = false;
      --num_cached_;
      ++stats_.evictions;
      return f;
    }
  }
  throw Error(
      "buffer pool exhausted: every frame is pinned. Increase "
      "Options::buffer_pool_capacity_pages (it must exceed the number of live "
      "iterators plus the height of the tallest SST).");
}

PageGuard BufferPool::Fetch(std::uint32_t file_id, std::uint64_t page_index,
                            File& file) {
  std::size_t frame = Lookup(file_id, page_index);
  if (frame != kInvalid) {
    ++stats_.hits;
    LruRemove(frame);        // refresh the recency order
    LruPushFront(frame);
    ++frames_[frame].pin_count;
    return PageGuard(this, frame, pages_.page(frame));
  }

  ++stats_.misses;
  frame = AcquireFrame();
  // Read before publishing the frame, so a failed read leaves no half-filled
  // page visible in the hash table.
  file.ReadPage(page_index, pages_.page(frame));
  ++stats_.page_reads;

  frames_[frame].file_id = file_id;
  frames_[frame].page_index = page_index;
  frames_[frame].occupied = true;
  frames_[frame].pin_count = 1;
  HashInsert(frame);
  LruPushFront(frame);
  ++num_cached_;
  return PageGuard(this, frame, pages_.page(frame));
}

void BufferPool::Unpin(std::size_t frame) {
  if (frames_[frame].pin_count == 0) throw Error("unpinning an unpinned frame");
  --frames_[frame].pin_count;
}

void BufferPool::EvictFile(std::uint32_t file_id) {
  for (std::size_t f = 0; f < frames_.size(); ++f) {
    if (!frames_[f].occupied || frames_[f].file_id != file_id) continue;
    if (frames_[f].pin_count != 0) {
      throw Error("cannot evict pages of a file that is still in use");
    }
    HashRemove(f);
    LruRemove(f);
    frames_[f].occupied = false;
    --num_cached_;
    free_frames_.push_back(f);
  }
}

}  // namespace kvstore
