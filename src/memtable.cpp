#include "kvstore/memtable.h"

#include <algorithm>

namespace kvstore {

Memtable::~Memtable() { Destroy(root_); }

void Memtable::Destroy(Node* n) {
  // Iterative post-order teardown would avoid deep recursion, but an AVL tree of
  // n nodes is only ~1.44*log2(n) deep, so plain recursion is safe here.
  if (n == nullptr) return;
  Destroy(n->left);
  Destroy(n->right);
  delete n;
}

void Memtable::Clear() {
  Destroy(root_);
  root_ = nullptr;
  num_entries_ = 0;
}

void Memtable::UpdateHeight(Node* n) {
  n->height = 1 + std::max(Height(n->left), Height(n->right));
}

int Memtable::BalanceFactor(const Node* n) {
  return Height(n->left) - Height(n->right);
}

/* Right rotation, used when the left subtree is too tall.  Before and after:
 *
 *         y                 x
 *        / \               / \
 *       x   c    ==>      a   y
 *      / \                   / \
 *     a   b                 b   c
 *
 * The in-order sequence a x b y c is the same on both sides, so the rotation
 * preserves the search-tree property and only changes the heights.
 */
Memtable::Node* Memtable::RotateRight(Node* y) {
  Node* x = y->left;
  y->left = x->right;
  x->right = y;
  UpdateHeight(y);
  UpdateHeight(x);
  return x;
}

// Mirror image of RotateRight.
Memtable::Node* Memtable::RotateLeft(Node* x) {
  Node* y = x->right;
  x->right = y->left;
  y->left = x;
  UpdateHeight(x);
  UpdateHeight(y);
  return y;
}

Memtable::Node* Memtable::Rebalance(Node* n) {
  UpdateHeight(n);
  const int balance = BalanceFactor(n);
  if (balance > 1) {
    // Left-heavy.  If the left child leans right this is the left-right case and
    // needs a left rotation there first to reduce it to the left-left case.
    if (BalanceFactor(n->left) < 0) n->left = RotateLeft(n->left);
    return RotateRight(n);
  }
  if (balance < -1) {
    if (BalanceFactor(n->right) > 0) n->right = RotateRight(n->right);
    return RotateLeft(n);
  }
  return n;
}

Memtable::Node* Memtable::Insert(Node* node, KeyType key, ValueType value,
                                 bool tombstone, bool* inserted) {
  if (node == nullptr) {
    *inserted = true;
    return new Node{key, value, tombstone, 1, nullptr, nullptr};
  }
  if (key < node->key) {
    node->left = Insert(node->left, key, value, tombstone, inserted);
  } else if (key > node->key) {
    node->right = Insert(node->right, key, value, tombstone, inserted);
  } else {
    // Same key: the newer write simply replaces the older one.  No rebalancing
    // is needed because the shape of the tree did not change.
    node->value = value;
    node->tombstone = tombstone;
    return node;
  }
  return Rebalance(node);
}

void Memtable::Put(KeyType key, ValueType value, bool tombstone) {
  bool inserted = false;
  root_ = Insert(root_, key, value, tombstone, &inserted);
  if (inserted) ++num_entries_;
}

bool Memtable::Get(KeyType key, ValueType* value, bool* tombstone) const {
  const Node* n = root_;
  while (n != nullptr) {
    if (key < n->key) {
      n = n->left;
    } else if (key > n->key) {
      n = n->right;
    } else {
      *value = n->value;
      *tombstone = n->tombstone;
      return true;
    }
  }
  return false;
}

std::unique_ptr<InternalIterator> Memtable::NewIterator() const {
  return std::make_unique<MemtableIterator>(this);
}

void Memtable::MemtableIterator::SeekToFirst() {
  stack_.clear();
  for (const Node* n = table_->root_; n != nullptr; n = n->left) {
    stack_.push_back(n);
  }
}

void Memtable::MemtableIterator::Seek(KeyType key) {
  // Descend from the root, keeping on the stack exactly those ancestors that are
  // still candidates, i.e. the ones whose key is >= `key`.  Whatever ends up on
  // top of the stack is the smallest such key.
  stack_.clear();
  const Node* n = table_->root_;
  while (n != nullptr) {
    if (n->key < key) {
      n = n->right;  // n and its left subtree are all too small.
    } else {
      stack_.push_back(n);
      n = n->left;  // n is a candidate, but something smaller may still qualify.
    }
  }
}

void Memtable::MemtableIterator::Next() {
  const Node* n = stack_.back();
  stack_.pop_back();
  // The in-order successor is the leftmost node of the right subtree, if any;
  // otherwise it is the nearest ancestor we have not visited, already on top of
  // the stack.
  for (const Node* m = n->right; m != nullptr; m = m->left) {
    stack_.push_back(m);
  }
}

KeyType Memtable::MemtableIterator::Key() const { return Current()->key; }
ValueType Memtable::MemtableIterator::Value() const { return Current()->value; }
bool Memtable::MemtableIterator::IsTombstone() const {
  return Current()->tombstone;
}

}  // namespace kvstore
