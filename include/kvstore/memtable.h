// In-memory write buffer, implemented as an AVL tree.
#ifndef KVSTORE_MEMTABLE_H_
#define KVSTORE_MEMTABLE_H_

#include <cstddef>
#include <memory>
#include <vector>

#include "kvstore/format.h"
#include "kvstore/iterator.h"
#include "kvstore/types.h"

namespace kvstore {

// A balanced binary search tree holding the most recent version of every key
// written since the last flush.  AVL was chosen over a red-black tree because
// its invariant (|height(left) - height(right)| <= 1) is the easiest to state,
// test and reason about, and the baseline does not care about the slightly
// higher rebalancing cost on writes.
//
// All three operations the specification asks for are O(log n) or better:
//   Put  - O(log n), overwriting in place when the key already exists, so the
//          memtable never holds two versions of the same key.
//   Get  - O(log n).
//   ordered iteration - O(1) amortised per step via NewIterator().
class Memtable {
 public:
  Memtable() = default;
  ~Memtable();

  Memtable(const Memtable&) = delete;
  Memtable& operator=(const Memtable&) = delete;

  // Inserts or overwrites `key`.  `tombstone` marks the entry as a delete.
  void Put(KeyType key, ValueType value, bool tombstone = false);

  // Looks `key` up.  Returns false if the key is absent from this memtable.  If
  // it is present, `*value` and `*tombstone` describe the entry; a tombstone
  // means the key has been deleted and older versions must not be consulted.
  bool Get(KeyType key, ValueType* value, bool* tombstone) const;

  std::size_t num_entries() const { return num_entries_; }
  bool empty() const { return root_ == nullptr; }

  // On-disk footprint of the buffered entries.  This is what Options::
  // memtable_size_bytes is compared against.
  std::size_t ApproximateSizeBytes() const {
    return num_entries_ * sizeof(DiskEntry);
  }

  // Height of the tree; exposed for tests that check the AVL invariant.
  int height() const { return Height(root_); }

  void Clear();

  std::unique_ptr<InternalIterator> NewIterator() const;

 private:
  struct Node {
    KeyType key;
    ValueType value;
    bool tombstone;
    int height;
    Node* left = nullptr;
    Node* right = nullptr;
  };

  // Iterates the tree in order using an explicit stack of the ancestors whose
  // subtrees have not been exhausted yet.  The stack depth is bounded by the
  // tree height, which AVL keeps at O(log n).
  class MemtableIterator : public InternalIterator {
   public:
    explicit MemtableIterator(const Memtable* table) : table_(table) {}

    void Seek(KeyType key) override;
    void SeekToFirst() override;
    bool Valid() const override { return !stack_.empty(); }
    void Next() override;
    KeyType Key() const override;
    ValueType Value() const override;
    bool IsTombstone() const override;

   private:
    const Node* Current() const { return stack_.back(); }

    const Memtable* table_;
    std::vector<const Node*> stack_;
  };

  static int Height(const Node* n) { return n == nullptr ? 0 : n->height; }
  static void UpdateHeight(Node* n);
  static int BalanceFactor(const Node* n);
  static Node* RotateRight(Node* y);
  static Node* RotateLeft(Node* x);
  static Node* Rebalance(Node* n);
  // Returns the new subtree root; sets *inserted when a node was added.
  Node* Insert(Node* node, KeyType key, ValueType value, bool tombstone,
               bool* inserted);
  static void Destroy(Node* n);

  Node* root_ = nullptr;
  std::size_t num_entries_ = 0;
};

}  // namespace kvstore

#endif  // KVSTORE_MEMTABLE_H_
