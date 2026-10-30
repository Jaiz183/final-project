#include "kvstore/merging_iterator.h"

#include <algorithm>
#include <utility>

namespace kvstore {

MergingIterator::MergingIterator(
    std::vector<std::unique_ptr<InternalIterator>> children)
    : children_(std::move(children)) {
  compare_.children = &children_;
  heap_.reserve(children_.size());
}

void MergingIterator::RebuildHeap() {
  heap_.clear();
  for (std::size_t i = 0; i < children_.size(); ++i) {
    if (children_[i]->Valid()) heap_.push_back(i);
  }
  std::make_heap(heap_.begin(), heap_.end(), compare_);
}

void MergingIterator::SeekToFirst() {
  for (auto& child : children_) child->SeekToFirst();
  RebuildHeap();
}

void MergingIterator::Seek(KeyType key) {
  for (auto& child : children_) child->Seek(key);
  RebuildHeap();
}

void MergingIterator::Next() {
  if (heap_.empty()) return;
  // Move the current smallest to the back of the vector, advance it, and either
  // re-insert it or drop it if it is now exhausted.
  std::pop_heap(heap_.begin(), heap_.end(), compare_);
  const std::size_t child = heap_.back();
  children_[child]->Next();
  if (children_[child]->Valid()) {
    std::push_heap(heap_.begin(), heap_.end(), compare_);
  } else {
    heap_.pop_back();
  }
}

KeyType MergingIterator::Key() const { return Top().Key(); }
ValueType MergingIterator::Value() const { return Top().Value(); }
bool MergingIterator::IsTombstone() const { return Top().IsTombstone(); }

}  // namespace kvstore
