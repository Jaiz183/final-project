// Test runner: implements the framework and provides main().
#include <algorithm>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "test_framework.h"
#include "test_util.h"

namespace testing {
namespace {

int g_failure_count = 0;

}  // namespace

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

void ReportFailure(const char* file, int line, const std::string& message) {
  ++g_failure_count;
  std::printf("    %s:%d: %s\n", file, line, message.c_str());
}

int RunAllTests(const std::string& filter) {
  auto& tests = Registry();
  std::sort(tests.begin(), tests.end(), [](const TestCase& a, const TestCase& b) {
    return a.full_name() < b.full_name();
  });

  int run = 0;
  int failed = 0;
  for (const TestCase& test : tests) {
    if (!filter.empty() && test.full_name().find(filter) == std::string::npos) {
      continue;
    }
    ++run;
    std::printf("[ RUN      ] %s\n", test.full_name().c_str());
    std::fflush(stdout);

    const int before = g_failure_count;
    bool test_failed = false;
    try {
      test.body(&test_failed);
    } catch (const std::exception& e) {
      ReportFailure(__FILE__, __LINE__,
                    std::string("uncaught exception: ") + e.what());
      test_failed = true;
    } catch (...) {
      ReportFailure(__FILE__, __LINE__, "uncaught non-standard exception");
      test_failed = true;
    }

    if (test_failed || g_failure_count != before) {
      ++failed;
      std::printf("[     FAIL ] %s\n", test.full_name().c_str());
    } else {
      std::printf("[       OK ] %s\n", test.full_name().c_str());
    }
  }

  std::printf("\n%d test(s) run, %d passed, %d failed\n", run, run - failed, failed);
  return failed;
}

}  // namespace testing

int main(int argc, char** argv) {
  const std::string filter = argc > 1 ? argv[1] : "";
  if (!filter.empty()) std::printf("filter: %s\n\n", filter.c_str());
  const int failed = testing::RunAllTests(filter);
  kvstore::test::CleanupTempRoot();
  return failed == 0 ? 0 : 1;
}
