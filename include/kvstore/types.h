// Basic scalar types and constants shared by every component of the store.
#ifndef KVSTORE_TYPES_H_
#define KVSTORE_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace kvstore {

// The baseline uses fixed-size 8-byte integer keys and values, as allowed by
// the project scope.  Everything on disk is therefore fixed width, which keeps
// page layouts and binary search trivial.
using KeyType = std::int64_t;
using ValueType = std::int64_t;

// All I/O in the system happens in units of this many bytes.  4 KiB matches the
// typical page size and satisfies the alignment requirements of O_DIRECT on all
// common Linux filesystems.
constexpr std::size_t kPageSize = 4096;

// Exception type used for every unrecoverable error (bad arguments, corrupt
// files, failed syscalls).  The baseline favours exceptions over status codes so
// that the happy path stays readable.
class Error : public std::runtime_error {
 public:
  explicit Error(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace kvstore

#endif  // KVSTORE_TYPES_H_
