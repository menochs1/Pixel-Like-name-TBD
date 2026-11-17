#pragma once
// Minimal dependency-free test harness: TEST(name) { CHECK(cond); CHECK_EQ(a, b); }

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace pxtest {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& failures() {
  static int n = 0;
  return n;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

template <typename T>
auto printable(const T& v) {
  if constexpr (std::is_integral_v<T>) return +v;  // print uint8/uint16 as numbers
  else return v;
}

inline void fail(const char* file, int line, const std::string& msg) {
  std::printf("    FAIL %s:%d: %s\n", file, line, msg.c_str());
  ++failures();
}

}  // namespace pxtest

#define PX_CONCAT_INNER(a, b) a##b
#define PX_CONCAT(a, b) PX_CONCAT_INNER(a, b)
#define TEST(name)                                                              \
  static void name();                                                           \
  static const pxtest::Registrar PX_CONCAT(px_reg_, name){#name, &name};       \
  static void name()

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) pxtest::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                             \
  do {                                                                             \
    const auto px_a = (a);                                                         \
    const auto px_b = (b);                                                         \
    if (!(px_a == px_b)) {                                                         \
      std::ostringstream px_os;                                                    \
      px_os << "CHECK_EQ(" #a ", " #b "): " << pxtest::printable(px_a) << " != " << pxtest::printable(px_b);           \
      pxtest::fail(__FILE__, __LINE__, px_os.str());                               \
    }                                                                              \
  } while (0)
