#include "test_util.h"

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace kvstore {
namespace test {
namespace {

// One root per process, so concurrent `make test` runs do not collide.
const std::string& TempRoot() {
  static const std::string root =
      (fs::temp_directory_path() /
       ("kvstore_tests_" + std::to_string(::getpid())))
          .string();
  return root;
}

std::atomic<int> g_counter{0};

}  // namespace

TempDir::TempDir(const std::string& label) {
  path_ = (fs::path(TempRoot()) /
           (label + "_" + std::to_string(g_counter.fetch_add(1))))
              .string();
  fs::remove_all(path_);
  fs::create_directories(path_);
}

TempDir::~TempDir() {
  std::error_code ec;  // a destructor must not throw
  fs::remove_all(path_, ec);
}

void CleanupTempRoot() {
  std::error_code ec;
  fs::remove_all(TempRoot(), ec);
}

}  // namespace test
}  // namespace kvstore
