#include "kvstore/bloom_filter.h"

#include <algorithm>
#include <cmath>

namespace kvstore {
namespace bloom {
namespace {

// SplitMix64.  A single multiply-xor-shift round with good avalanche behaviour;
// more than enough for filter probes and far cheaper than a general-purpose hash
// on an 8-byte key.
std::uint64_t Mix(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

}  // namespace

std::uint32_t NumHashesFor(std::uint32_t bits_per_entry) {
  if (bits_per_entry == 0) return 0;
  const auto k = static_cast<std::uint32_t>(
      std::lround(0.693147 * static_cast<double>(bits_per_entry)));
  return std::clamp<std::uint32_t>(k, 1, 12);
}

std::uint64_t BitIndex(KeyType key, std::uint32_t probe,
                       std::uint64_t num_bits) {
  const std::uint64_t h1 = Mix(static_cast<std::uint64_t>(key));
  // Forcing h2 odd keeps it coprime with any power of two and stops a zero delta
  // from making every probe hit the same bit.
  const std::uint64_t h2 = Mix(h1) | 1ULL;
  return (h1 + static_cast<std::uint64_t>(probe) * h2) % num_bits;
}

}  // namespace bloom

BloomFilterBuilder::BloomFilterBuilder(std::uint64_t expected_entries,
                                       std::uint32_t bits_per_entry) {
  if (bits_per_entry == 0 || expected_entries == 0) return;  // filter disabled

  num_hashes_ = bloom::NumHashesFor(bits_per_entry);
  const std::uint64_t requested_bits = expected_entries * bits_per_entry;
  // Round the bit array up to a whole number of pages: the filter is stored as
  // pages inside the SST and fetched one page at a time through the buffer pool,
  // and the spare bits only lower the false-positive rate.
  const std::uint64_t pages = (requested_bits + kPageSize * 8 - 1) / (kPageSize * 8);
  bits_.assign(static_cast<std::size_t>(pages * kPageSize), 0);
  num_bits_ = pages * kPageSize * 8;
}

void BloomFilterBuilder::Add(KeyType key) {
  if (!enabled()) return;
  for (std::uint32_t i = 0; i < num_hashes_; ++i) {
    const std::uint64_t bit = bloom::BitIndex(key, i, num_bits_);
    bits_[static_cast<std::size_t>(bit / 8)] |=
        static_cast<std::uint8_t>(1u << (bit % 8));
  }
}

}  // namespace kvstore
