// Tests for the Bloom filter hashing and bit array.
#include <cstdint>
#include <vector>

#include "kvstore/bloom_filter.h"
#include "test_framework.h"

using kvstore::BloomFilterBuilder;
using kvstore::KeyType;

namespace {

// Queries a builder's bit array directly, the same way SSTable::MayContain
// queries the filter pages it reads back from disk.
bool MayContain(const BloomFilterBuilder& filter, KeyType key) {
  if (!filter.enabled()) return true;
  for (std::uint32_t probe = 0; probe < filter.num_hashes(); ++probe) {
    const std::uint64_t bit =
        kvstore::bloom::BitIndex(key, probe, filter.num_bits());
    const std::uint8_t byte = filter.bits()[static_cast<std::size_t>(bit / 8)];
    if ((byte & (1u << (bit % 8))) == 0) return false;
  }
  return true;
}

}  // namespace

TEST(bloom, no_false_negatives) {
  // The defining property: a key that was inserted must always test positive.
  BloomFilterBuilder filter(10000, 10);
  for (KeyType key = 0; key < 10000; ++key) filter.Add(key * 7 + 3);
  for (KeyType key = 0; key < 10000; ++key) EXPECT_TRUE(MayContain(filter, key * 7 + 3));
}

TEST(bloom, false_positive_rate_is_near_theory) {
  // With k = round(ln2 * b) probes the expected rate is about 2^-b*ln2, i.e.
  // roughly 1% at 10 bits per entry.  Allow generous slack: the test is here to
  // catch a broken hash (rate near 0 or near 1), not to pin down a constant.
  const int n = 20000;
  BloomFilterBuilder filter(n, 10);
  for (KeyType key = 0; key < n; ++key) filter.Add(key);

  int positives = 0;
  const int probes = 20000;
  for (KeyType key = n; key < n + probes; ++key) {
    if (MayContain(filter, key)) ++positives;
  }
  const double rate = static_cast<double>(positives) / probes;
  EXPECT_LT(rate, 0.05);
}

TEST(bloom, more_bits_per_entry_means_fewer_false_positives) {
  const int n = 20000;
  auto false_positive_rate = [&](std::uint32_t bits_per_entry) {
    BloomFilterBuilder filter(n, bits_per_entry);
    for (KeyType key = 0; key < n; ++key) filter.Add(key);
    int positives = 0;
    for (KeyType key = n; key < 2 * n; ++key) {
      if (MayContain(filter, key)) ++positives;
    }
    return static_cast<double>(positives) / n;
  };

  EXPECT_GT(false_positive_rate(4), false_positive_rate(10));
  EXPECT_GT(false_positive_rate(10), false_positive_rate(16));
}

TEST(bloom, zero_bits_per_entry_disables_the_filter) {
  // A disabled filter must answer "maybe" to everything, so that the read path
  // still finds the keys and only loses the skipping optimisation.
  BloomFilterBuilder filter(1000, 0);
  EXPECT_FALSE(filter.enabled());
  EXPECT_EQ(filter.num_bits(), 0u);
  EXPECT_EQ(filter.num_pages(), 0u);
  filter.Add(1);
  EXPECT_TRUE(MayContain(filter, 12345));
}

TEST(bloom, bit_array_is_page_aligned_and_large_enough) {
  BloomFilterBuilder filter(1000, 10);  // 10000 bits requested
  EXPECT_GE(filter.num_bits(), 10000u);
  EXPECT_EQ(filter.bits().size() % kvstore::kPageSize, 0u);
  EXPECT_EQ(filter.num_bits(), filter.bits().size() * 8);
  EXPECT_EQ(filter.num_pages(), filter.bits().size() / kvstore::kPageSize);
}

TEST(bloom, num_hashes_follows_the_budget) {
  EXPECT_EQ(kvstore::bloom::NumHashesFor(0), 0u);
  EXPECT_EQ(kvstore::bloom::NumHashesFor(1), 1u);
  EXPECT_EQ(kvstore::bloom::NumHashesFor(10), 7u);  // round(0.693 * 10)
  EXPECT_LE(kvstore::bloom::NumHashesFor(1000), 12u);
}

TEST(bloom, probes_of_one_key_are_spread_out) {
  // A broken derivation of the second hash would make every probe land on the
  // same bit, which would silently turn the filter into a 1-bit filter.
  const std::uint64_t num_bits = 1 << 16;
  std::vector<std::uint64_t> seen;
  for (std::uint32_t probe = 0; probe < 7; ++probe) {
    seen.push_back(kvstore::bloom::BitIndex(42, probe, num_bits));
  }
  int distinct = 0;
  for (std::size_t i = 0; i < seen.size(); ++i) {
    bool duplicate = false;
    for (std::size_t j = 0; j < i; ++j) {
      if (seen[i] == seen[j]) duplicate = true;
    }
    if (!duplicate) ++distinct;
  }
  EXPECT_EQ(distinct, 7);
}
