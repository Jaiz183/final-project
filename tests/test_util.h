// Helpers shared by the test files.
#ifndef KVSTORE_TEST_UTIL_H_
#define KVSTORE_TEST_UTIL_H_

#include <string>

namespace kvstore {
namespace test {

// A fresh, empty directory for one test to use, deleted again by TempDir's
// destructor.  All of them live under a single temporary root so that a crashed
// run leaves at most one directory behind.
class TempDir {
 public:
  explicit TempDir(const std::string& label);
  ~TempDir();

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

// Removes the shared temporary root.  Called once at the end of main().
void CleanupTempRoot();

}  // namespace test
}  // namespace kvstore

#endif  // KVSTORE_TEST_UTIL_H_
