// Tunable parameters of the store.  Everything a benchmark might want to vary
// lives here, so experiments never have to edit source code.
#ifndef KVSTORE_OPTIONS_H_
#define KVSTORE_OPTIONS_H_

#include <cstddef>
#include <cstdint>

#include "kvstore/types.h"

namespace kvstore {

struct Options {
  // How much data may accumulate in the memtable before it is flushed to a new
  // SST.  Measured as the *on-disk* footprint of the buffered entries
  // (entry_count * sizeof(DiskEntry)), so it directly controls the size of the
  // SSTs that flushing produces.  The in-memory AVL nodes themselves are
  // larger, because of the child pointers and balance metadata.
  std::size_t memtable_size_bytes = 1 << 20;  // 1 MiB ~ 43k entries

  // Capacity of the buffer pool, in 4 KiB pages.  Must be large enough to hold
  // one page per live iterator plus the height of the tallest SST; see
  // docs/DESIGN.md.
  std::size_t buffer_pool_capacity_pages = 1024;  // 4 MiB

  // Number of buckets in the buffer pool's hash table.  Rounded up to a power
  // of two.  0 means "derive it from the capacity".
  std::size_t buffer_pool_hash_buckets = 0;

  // Bits of Bloom filter per entry.  0 disables the filters entirely, which is
  // useful as an experimental baseline.
  std::uint32_t bloom_bits_per_entry = 10;

  // LSM-tree size ratio T.  A level is compacted into the next one as soon as
  // it holds T runs (SSTs).  The specification's simple starting policy is
  // T = 2.
  std::uint32_t size_ratio = 2;

  // Bypass the OS page cache with O_DIRECT so that the buffer pool is the only
  // cache in the system.  If the filesystem refuses O_DIRECT the store falls
  // back to buffered I/O; see Database::uses_direct_io().
  bool use_direct_io = true;

  // Create the database directory if it does not exist yet.
  bool create_if_missing = true;
};

}  // namespace kvstore

#endif  // KVSTORE_OPTIONS_H_
