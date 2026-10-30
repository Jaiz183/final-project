// kv_bench: a small, fully parameterised workload driver for the baseline store.
//
// Its job is to make experiments reproducible without editing source code: every
// knob is a command-line flag, and the result is one line of CSV on stdout.  See
// docs/EXPERIMENTS.md for the intended usage and for the meaning of each column.
//
// Run `kv_bench --help` for the flag list.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "kvstore/db.h"

namespace fs = std::filesystem;
using kvstore::Database;
using kvstore::KeyType;
using kvstore::Options;
using kvstore::ValueType;

namespace {

struct Config {
  std::string db_path = "bench_db";
  std::uint64_t num_keys = 200000;      // distinct keys written in the load phase
  std::uint64_t num_queries = 20000;    // point lookups in the query phase
  std::uint64_t scan_length = 0;        // entries per scan; 0 disables scans
  std::uint64_t num_scans = 100;
  double zipf_skew = 0.0;               // 0 => uniform query keys
  double miss_fraction = 0.0;           // fraction of lookups for absent keys
  std::size_t memtable_size_bytes = 1 << 20;
  std::size_t buffer_pool_pages = 1024;
  std::uint32_t bloom_bits_per_entry = 10;
  std::uint32_t size_ratio = 2;
  bool direct_io = true;
  bool keep_db = false;
  bool csv_header = false;
  std::uint64_t seed = 42;
  std::string label = "baseline";
};

[[noreturn]] void Usage(int exit_code) {
  std::fprintf(stderr, R"(usage: kv_bench [options]

Workload
  --db PATH                database directory (default bench_db)
  --num-keys N             distinct keys to write (default 200000)
  --num-queries N          point lookups to run (default 20000)
  --miss-fraction F        fraction of lookups aimed at absent keys (default 0)
  --zipf-skew S            0 = uniform query keys, >0 = skewed (try 0.99)
  --scan-length N          entries per range scan; 0 disables scans (default 0)
  --num-scans N            number of range scans (default 100)
  --seed N                 RNG seed (default 42)

System under test
  --memtable-bytes N       memtable capacity in bytes (default 1048576)
  --buffer-pool-pages N    buffer pool capacity in 4 KiB pages (default 1024)
  --bloom-bits N           Bloom filter bits per entry, 0 disables (default 10)
  --size-ratio N           LSM size ratio T (default 2)
  --no-direct-io           use buffered I/O instead of O_DIRECT

Output
  --label NAME             value of the 'label' CSV column (default baseline)
  --csv-header             print the CSV header line before the result
  --keep-db                do not delete the database directory afterwards
  -h, --help               this message
)");
  std::exit(exit_code);
}

// Small hand-rolled flag parser: no dependency, and every flag is explicit.
Config ParseArgs(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", name);
        Usage(2);
      }
      return argv[++i];
    };
    if (arg == "-h" || arg == "--help") Usage(0);
    else if (arg == "--db") config.db_path = next("--db");
    else if (arg == "--num-keys") config.num_keys = std::stoull(next("--num-keys"));
    else if (arg == "--num-queries") config.num_queries = std::stoull(next("--num-queries"));
    else if (arg == "--miss-fraction") config.miss_fraction = std::stod(next("--miss-fraction"));
    else if (arg == "--zipf-skew") config.zipf_skew = std::stod(next("--zipf-skew"));
    else if (arg == "--scan-length") config.scan_length = std::stoull(next("--scan-length"));
    else if (arg == "--num-scans") config.num_scans = std::stoull(next("--num-scans"));
    else if (arg == "--seed") config.seed = std::stoull(next("--seed"));
    else if (arg == "--memtable-bytes") config.memtable_size_bytes = std::stoull(next("--memtable-bytes"));
    else if (arg == "--buffer-pool-pages") config.buffer_pool_pages = std::stoull(next("--buffer-pool-pages"));
    else if (arg == "--bloom-bits") config.bloom_bits_per_entry = static_cast<std::uint32_t>(std::stoul(next("--bloom-bits")));
    else if (arg == "--size-ratio") config.size_ratio = static_cast<std::uint32_t>(std::stoul(next("--size-ratio")));
    else if (arg == "--no-direct-io") config.direct_io = false;
    else if (arg == "--label") config.label = next("--label");
    else if (arg == "--csv-header") config.csv_header = true;
    else if (arg == "--keep-db") config.keep_db = true;
    else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      Usage(2);
    }
  }
  return config;
}

// Draws keys from a Zipf distribution over [0, n) by inverting the CDF of the
// discrete harmonic distribution.  The table is built once, so sampling is a
// binary search.  With skew 0 the caller uses a uniform distribution instead.
class ZipfSampler {
 public:
  ZipfSampler(std::uint64_t n, double skew) : cumulative_(n) {
    double sum = 0.0;
    for (std::uint64_t i = 0; i < n; ++i) {
      sum += 1.0 / std::pow(static_cast<double>(i + 1), skew);
      cumulative_[i] = sum;
    }
    for (double& value : cumulative_) value /= sum;
  }

  std::uint64_t operator()(std::mt19937_64& rng) const {
    const double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
    const auto it = std::lower_bound(cumulative_.begin(), cumulative_.end(), u);
    return static_cast<std::uint64_t>(it - cumulative_.begin());
  }

 private:
  std::vector<double> cumulative_;
};

double SecondsSince(std::chrono::steady_clock::time_point start) {
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(end - start).count();
}

}  // namespace

int main(int argc, char** argv) try {
  const Config config = ParseArgs(argc, argv);

  Options options;
  options.memtable_size_bytes = config.memtable_size_bytes;
  options.buffer_pool_capacity_pages = config.buffer_pool_pages;
  options.bloom_bits_per_entry = config.bloom_bits_per_entry;
  options.size_ratio = config.size_ratio;
  options.use_direct_io = config.direct_io;

  fs::remove_all(config.db_path);
  Database db(options);
  db.Open(config.db_path);

  std::mt19937_64 rng(config.seed);

  // --- Load phase: write num_keys distinct keys in a random order, so the
  // memtable sees no favourable insertion pattern. --------------------------
  std::vector<KeyType> keys(config.num_keys);
  for (std::uint64_t i = 0; i < config.num_keys; ++i) {
    keys[i] = static_cast<KeyType>(i);
  }
  std::shuffle(keys.begin(), keys.end(), rng);

  const auto load_start = std::chrono::steady_clock::now();
  for (KeyType key : keys) db.Put(key, key * 2 + 1);
  const double load_seconds = SecondsSince(load_start);

  const std::uint64_t flushes = db.num_flushes();
  const std::uint64_t compactions = db.num_compactions();
  const std::uint64_t sst_entries = db.TotalSSTEntries();
  const std::size_t num_ssts = db.NumSSTs();
  const std::size_t num_levels = db.LevelSizes().size();

  // --- Query phase: point lookups. ------------------------------------------
  db.ResetBufferPoolStats();
  std::unique_ptr<ZipfSampler> zipf;
  if (config.zipf_skew > 0.0 && config.num_keys > 0) {
    zipf = std::make_unique<ZipfSampler>(config.num_keys, config.zipf_skew);
  }
  std::uniform_int_distribution<std::uint64_t> uniform(0, config.num_keys ? config.num_keys - 1 : 0);
  std::uniform_real_distribution<double> unit(0.0, 1.0);

  std::uint64_t found = 0;
  const auto query_start = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < config.num_queries; ++i) {
    KeyType key;
    if (unit(rng) < config.miss_fraction) {
      // Absent keys live above the loaded range, so they exercise the filters.
      key = static_cast<KeyType>(config.num_keys + uniform(rng));
    } else {
      key = static_cast<KeyType>(zipf ? (*zipf)(rng) : uniform(rng));
    }
    ValueType value = 0;
    if (db.Get(key, &value)) ++found;
  }
  const double query_seconds = SecondsSince(query_start);
  const auto point_stats = db.buffer_pool_stats();

  // --- Scan phase: range queries expressed as Seek + Next. ------------------
  db.ResetBufferPoolStats();
  std::uint64_t scanned = 0;
  double scan_seconds = 0.0;
  if (config.scan_length > 0 && config.num_scans > 0) {
    const auto scan_start = std::chrono::steady_clock::now();
    for (std::uint64_t s = 0; s < config.num_scans; ++s) {
      const auto start_key = static_cast<KeyType>(uniform(rng));
      auto it = db.NewIterator();
      std::uint64_t steps = 0;
      for (it->Seek(start_key); it->Valid() && steps < config.scan_length;
           it->Next(), ++steps) {
        scanned += static_cast<std::uint64_t>(it->Value() != 0);
      }
    }
    scan_seconds = SecondsSince(scan_start);
  }
  const auto scan_stats = db.buffer_pool_stats();

  const bool direct = db.uses_direct_io();
  db.Close();
  if (!config.keep_db) fs::remove_all(config.db_path);

  if (config.csv_header) {
    std::printf(
        "label,num_keys,num_queries,miss_fraction,zipf_skew,scan_length,"
        "num_scans,memtable_bytes,buffer_pool_pages,bloom_bits,size_ratio,"
        "direct_io,load_seconds,put_per_second,query_seconds,get_per_second,"
        "queries_found,scan_seconds,entries_scanned,flushes,compactions,"
        "num_ssts,num_levels,sst_entries,point_hits,point_misses,"
        "point_evictions,scan_hits,scan_misses\n");
  }
  std::printf(
      "%s,%llu,%llu,%g,%g,%llu,%llu,%zu,%zu,%u,%u,%d,"
      "%.6f,%.1f,%.6f,%.1f,%llu,%.6f,%llu,%llu,%llu,%zu,%zu,%llu,"
      "%llu,%llu,%llu,%llu,%llu\n",
      config.label.c_str(),
      static_cast<unsigned long long>(config.num_keys),
      static_cast<unsigned long long>(config.num_queries),
      config.miss_fraction, config.zipf_skew,
      static_cast<unsigned long long>(config.scan_length),
      static_cast<unsigned long long>(config.num_scans),
      config.memtable_size_bytes, config.buffer_pool_pages,
      config.bloom_bits_per_entry, config.size_ratio, direct ? 1 : 0,
      load_seconds,
      load_seconds > 0 ? static_cast<double>(config.num_keys) / load_seconds : 0.0,
      query_seconds,
      query_seconds > 0 ? static_cast<double>(config.num_queries) / query_seconds : 0.0,
      static_cast<unsigned long long>(found), scan_seconds,
      static_cast<unsigned long long>(scanned),
      static_cast<unsigned long long>(flushes),
      static_cast<unsigned long long>(compactions), num_ssts, num_levels,
      static_cast<unsigned long long>(sst_entries),
      static_cast<unsigned long long>(point_stats.hits),
      static_cast<unsigned long long>(point_stats.misses),
      static_cast<unsigned long long>(point_stats.evictions),
      static_cast<unsigned long long>(scan_stats.hits),
      static_cast<unsigned long long>(scan_stats.misses));
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "kv_bench failed: %s\n", e.what());
  return 1;
}
