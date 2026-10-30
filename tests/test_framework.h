// A ~100-line test framework, so that the project has no external test
// dependency to install.
//
// Write a test like this:
//
//   TEST(memtable, put_then_get) {
//     Memtable t;
//     t.Put(1, 10);
//     ...
//     EXPECT_EQ(value, 10);
//   }
//
// EXPECT_* records a failure and keeps going; ASSERT_* records it and returns
// from the test, for when continuing would crash.  Every test runs; the binary
// exits non-zero if any of them failed.
#ifndef KVSTORE_TEST_FRAMEWORK_H_
#define KVSTORE_TEST_FRAMEWORK_H_

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void(bool*)> body;
  std::string full_name() const { return suite + "." + name; }
};

// Global registry, filled in before main() by the TEST macro.
std::vector<TestCase>& Registry();

// Records a failure for the test that is currently running.
void ReportFailure(const char* file, int line, const std::string& message);

// Runs every registered test whose name contains `filter` ("" matches all).
// Returns the number of failed tests.
int RunAllTests(const std::string& filter);

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void(bool*)> body) {
    Registry().push_back({suite, name, std::move(body)});
  }
};

}  // namespace testing

// `kvstore_test_failed` is the per-test flag the ASSERT_* macros return through.
#define TEST(suite, name)                                                     \
  static void kvstore_test_##suite##_##name(bool* kvstore_test_failed);       \
  static ::testing::Registrar kvstore_registrar_##suite##_##name(             \
      #suite, #name, kvstore_test_##suite##_##name);                          \
  static void kvstore_test_##suite##_##name([[maybe_unused]] bool* kvstore_test_failed)

#define KVSTORE_FAIL(message)                       \
  do {                                              \
    ::testing::ReportFailure(__FILE__, __LINE__, (message)); \
    *kvstore_test_failed = true;                    \
  } while (false)

#define KVSTORE_CHECK(condition, description, fatal)                    \
  do {                                                                  \
    if (!(condition)) {                                                 \
      KVSTORE_FAIL(description);                                        \
      if (fatal) return;                                                \
    }                                                                   \
  } while (false)

#define EXPECT_TRUE(cond) KVSTORE_CHECK((cond), "expected true: " #cond, false)
#define EXPECT_FALSE(cond) KVSTORE_CHECK(!(cond), "expected false: " #cond, false)
#define ASSERT_TRUE(cond) KVSTORE_CHECK((cond), "expected true: " #cond, true)
#define ASSERT_FALSE(cond) KVSTORE_CHECK(!(cond), "expected false: " #cond, true)

// Comparison macros evaluate each side once and print both values on failure.
#define KVSTORE_CMP(a, b, op, fatal)                                        \
  do {                                                                      \
    const auto kvstore_a = (a);                                             \
    const auto kvstore_b = (b);                                             \
    if (!(kvstore_a op kvstore_b)) {                                        \
      std::ostringstream kvstore_os;                                        \
      kvstore_os << "expected " #a " " #op " " #b " (" << kvstore_a << " vs " \
                 << kvstore_b << ")";                                       \
      KVSTORE_FAIL(kvstore_os.str());                                       \
      if (fatal) return;                                                    \
    }                                                                       \
  } while (false)

#define EXPECT_EQ(a, b) KVSTORE_CMP(a, b, ==, false)
#define EXPECT_NE(a, b) KVSTORE_CMP(a, b, !=, false)
#define EXPECT_LT(a, b) KVSTORE_CMP(a, b, <, false)
#define EXPECT_LE(a, b) KVSTORE_CMP(a, b, <=, false)
#define EXPECT_GT(a, b) KVSTORE_CMP(a, b, >, false)
#define EXPECT_GE(a, b) KVSTORE_CMP(a, b, >=, false)
#define ASSERT_EQ(a, b) KVSTORE_CMP(a, b, ==, true)

// Expects that `statement` throws kvstore::Error.
#define EXPECT_THROWS(statement)                                    \
  do {                                                              \
    bool kvstore_threw = false;                                     \
    try {                                                           \
      statement;                                                    \
    } catch (const ::kvstore::Error&) {                             \
      kvstore_threw = true;                                         \
    }                                                               \
    if (!kvstore_threw) KVSTORE_FAIL("expected " #statement " to throw"); \
  } while (false)

#endif  // KVSTORE_TEST_FRAMEWORK_H_
