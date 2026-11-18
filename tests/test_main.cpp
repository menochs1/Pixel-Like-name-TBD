#include <cstdio>

#include "test_framework.hpp"

int main() {
  int failed_tests = 0;
  for (const auto& t : pxtest::registry()) {
    const int before = pxtest::failures();
    t.fn();
    const bool ok = pxtest::failures() == before;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
    failed_tests += ok ? 0 : 1;
  }
  std::printf("\n%zu tests, %d failed\n", pxtest::registry().size(), failed_tests);
  return failed_tests == 0 ? 0 : 1;
}
