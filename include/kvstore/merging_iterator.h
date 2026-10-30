// Merging of several ordered sources into one ordered stream.
#ifndef KVSTORE_MERGING_ITERATOR_H_
#define KVSTORE_MERGING_ITERATOR_H_

#include <cstddef>
#include <memory>
#include <vector>

#include "kvstore/iterator.h"
#include "kvstore/types.h"

namespace kvstore {

// Merges N ordered child iterators using a binary min-heap of child indices, so
// each step costs O(log N) comparisons and no data is copied.
//
// Children must be supplied newest first: index 0 is the memtable, then the SSTs
// from youngest to oldest.  Entries are produced in ascending key order and, for
// equal keys, in ascending child index -- that is, newest version first.  The
// merge itself does not discard anything: duplicates and tombstones are still
// visible, which is what compaction needs in order to decide what may be dropped.
// DBIterator applies the client-visible filtering on top.
class MergingIterator : public InternalIterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<InternalIterator>> children);

  void Seek(KeyType key) override;
  void SeekToFirst() override;
  bool Valid() const override { return !heap_.empty(); }
  void Next() override;
  KeyType Key() const override;
  ValueType Value() const override;
  bool IsTombstone() const override;

 private:
  // Orders the heap: smaller key first, and on ties the newer (lower index)
  // child first.  Expressed as "a must come after b" so that std::*_heap, which
  // builds a max-heap, yields a min-heap.
  struct Compare {
    const std::vector<std::unique_ptr<InternalIterator>>* children;
    bool operator()(std::size_t a, std::size_t b) const {
      const KeyType ka = (*children)[a]->Key();
      const KeyType kb = (*children)[b]->Key();
      if (ka != kb) return ka > kb;
      return a > b;
    }
  };

  const InternalIterator& Top() const { return *children_[heap_.front()]; }
  void RebuildHeap();

  std::vector<std::unique_ptr<InternalIterator>> children_;
  std::vector<std::size_t> heap_;  // indices of the children that are Valid()
  Compare compare_;
};

}  // namespace kvstore

#endif  // KVSTORE_MERGING_ITERATOR_H_
