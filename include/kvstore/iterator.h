// Iterator interfaces.
#ifndef KVSTORE_ITERATOR_H_
#define KVSTORE_ITERATOR_H_

#include "kvstore/types.h"

namespace kvstore {

// An ordered cursor over one source of entries (a memtable or a single SST).
// Unlike the public iterator it exposes *every* version it stores, including
// tombstones; resolving duplicate keys across sources is the job of DBIterator.
class InternalIterator {
 public:
  virtual ~InternalIterator() = default;

  // Positions the cursor at the first entry with key >= `key`.
  virtual void Seek(KeyType key) = 0;
  // Positions the cursor at the smallest key in the source.
  virtual void SeekToFirst() = 0;
  virtual bool Valid() const = 0;
  virtual void Next() = 0;

  // Only valid while Valid() is true.
  virtual KeyType Key() const = 0;
  virtual ValueType Value() const = 0;
  virtual bool IsTombstone() const = 0;
};

// The public iterator handed to clients by Database::NewIterator().  It presents
// the logical contents of the database in ascending key order: each live key
// appears exactly once, with its newest value, and deleted keys do not appear at
// all.  A range query is a Seek() followed by Next() calls until Key() passes the
// end of the range.
class Iterator {
 public:
  virtual ~Iterator() = default;

  virtual void Seek(KeyType key) = 0;
  virtual void SeekToFirst() = 0;
  virtual bool Valid() const = 0;
  virtual void Next() = 0;
  virtual KeyType Key() const = 0;
  virtual ValueType Value() const = 0;
};

}  // namespace kvstore

#endif  // KVSTORE_ITERATOR_H_
