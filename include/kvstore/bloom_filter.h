// Bloom filter used to skip SSTs that cannot contain a key.
#ifndef KVSTORE_BLOOM_FILTER_H_
#define KVSTORE_BLOOM_FILTER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kvstore/types.h"

namespace kvstore {

// Hashing shared by the builder and by the read path.  Both must agree
// bit-for-bit, so the probe positions are computed in exactly one place.
namespace bloom {

// Optimal number of probes for a given budget is ln(2) * bits_per_entry.  The
// result is clamped to [1, 12]: below 1 the filter would store nothing, and
// beyond 12 the extra probes cost more CPU and I/O than the false-positive
// reduction is worth.
std::uint32_t NumHashesFor(std::uint32_t bits_per_entry);

// Position of the `probe`-th bit for `key` in a filter of `num_bits` bits.
//
// Only one 64-bit hash of the key is computed; the two independent values the
// double-hashing scheme needs are derived from it, following Kirsch and
// Mitzenmacher's result that h1 + i*h2 behaves as well as i independent hashes.
std::uint64_t BitIndex(KeyType key, std::uint32_t probe, std::uint64_t num_bits);

}  // namespace bloom

// Accumulates the bit array of one SST's filter while the SST is being built.
// The filter is sized from an *upper bound* on the entry count, because during a
// compaction the exact output size is not known until the merge has finished.
// Over-estimating only ever gives the filter more bits per entry than requested,
// never fewer, so the configured budget is a lower bound on what is delivered.
class BloomFilterBuilder {
 public:
  BloomFilterBuilder(std::uint64_t expected_entries,
                     std::uint32_t bits_per_entry);

  void Add(KeyType key);

  bool enabled() const { return num_bits_ > 0; }
  std::uint64_t num_bits() const { return num_bits_; }
  std::uint32_t num_hashes() const { return num_hashes_; }
  // Bit array, zero-padded to a whole number of pages so that it can be written
  // out with page-aligned direct I/O.
  const std::vector<std::uint8_t>& bits() const { return bits_; }
  std::size_t num_pages() const { return bits_.size() / kPageSize; }

 private:
  std::uint64_t num_bits_ = 0;
  std::uint32_t num_hashes_ = 0;
  std::vector<std::uint8_t> bits_;
};

}  // namespace kvstore

#endif  // KVSTORE_BLOOM_FILTER_H_
