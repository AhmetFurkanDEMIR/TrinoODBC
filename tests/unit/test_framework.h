#pragma once
// Minimal self-contained unit test framework (no external dependencies).
//   TEST_CASE("name") { CHECK(x == 1); CHECK_EQ(a, b); REQUIRE(ptr != nullptr); CHECK_THROWS_AS(expr, T); }
#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
  const char* name;
  const char* file;
  std::function<void()> fn;
};

struct Registry {
  std::vector<TestCase> tests;
  static Registry& get() { static Registry r; return r; }
};

struct Failure : std::exception {
  std::string msg;
  explicit Failure(std::string m) : msg(std::move(m)) {}
  const char* what() const noexcept override { return msg.c_str(); }
};

struct State {
  int checks = 0;
  int failures = 0;
  std::string current;
  static State& get() { static State s; return s; }
};

struct Registrar {
  Registrar(const char* name, const char* file, std::function<void()> fn) {
    Registry::get().tests.push_back({name, file, std::move(fn)});
  }
};

template <typename T>
std::string repr(const T& v) {
  std::ostringstream os;
  os << v;
  return os.str();
}
inline std::string repr(const std::string& v) { return "\"" + v + "\""; }
inline std::string repr(const char* v) { return v ? "\"" + std::string(v) + "\"" : "nullptr"; }
inline std::string repr(bool v) { return v ? "true" : "false"; }
inline std::string repr(std::nullptr_t) { return "nullptr"; }

inline void fail(const char* file, int line, const std::string& msg, bool fatal) {
  State::get().failures++;
  std::fprintf(stderr, "  FAIL %s:%d [%s] %s\n", file, line, State::get().current.c_str(), msg.c_str());
  if (fatal) throw Failure(msg);
}

/// Runs every registered test; optional substring filter. Returns process exit code.
inline int runAll(int argc, char** argv) {
  std::string filter = argc > 1 ? argv[1] : "";
  int run = 0, failedTests = 0;
  for (auto& t : Registry::get().tests) {
    if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
    run++;
    auto& st = State::get();
    st.current = t.name;
    const int before = st.failures;
    try {
      t.fn();
    } catch (const Failure&) {
    } catch (const std::exception& e) {
      fail(t.file, 0, std::string("unexpected exception: ") + e.what(), false);
    } catch (...) {
      fail(t.file, 0, "unexpected non-std exception", false);
    }
    if (st.failures != before) {
      failedTests++;
      std::fprintf(stderr, "FAILED: %s\n", t.name);
    }
  }
  std::printf("%d tests run, %d failed, %d checks\n", run, failedTests, State::get().checks);
  return failedTests == 0 ? 0 : 1;
}

}  // namespace testfw

#define TESTFW_CAT2(a, b) a##b
#define TESTFW_CAT(a, b) TESTFW_CAT2(a, b)
#define TEST_CASE(name)                                                                     \
  static void TESTFW_CAT(testfw_fn_, __LINE__)();                                          \
  static ::testfw::Registrar TESTFW_CAT(testfw_reg_, __LINE__)(name, __FILE__, TESTFW_CAT(testfw_fn_, __LINE__)); \
  static void TESTFW_CAT(testfw_fn_, __LINE__)()

#define CHECK(expr)                                                                         \
  do {                                                                                      \
    ::testfw::State::get().checks++;                                                        \
    if (!(expr)) ::testfw::fail(__FILE__, __LINE__, "CHECK(" #expr ")", false);             \
  } while (0)
#define REQUIRE(expr)                                                                       \
  do {                                                                                      \
    ::testfw::State::get().checks++;                                                        \
    if (!(expr)) ::testfw::fail(__FILE__, __LINE__, "REQUIRE(" #expr ")", true);            \
  } while (0)
#define CHECK_EQ(a, b)                                                                      \
  do {                                                                                      \
    ::testfw::State::get().checks++;                                                        \
    const auto& testfw_a = (a);                                                             \
    const auto& testfw_b = (b);                                                             \
    if (!(testfw_a == testfw_b))                                                            \
      ::testfw::fail(__FILE__, __LINE__, std::string("CHECK_EQ(" #a ", " #b ") -> ") + ::testfw::repr(testfw_a) + " != " + ::testfw::repr(testfw_b), false); \
  } while (0)
#define REQUIRE_EQ(a, b)                                                                    \
  do {                                                                                      \
    ::testfw::State::get().checks++;                                                        \
    const auto& testfw_a = (a);                                                             \
    const auto& testfw_b = (b);                                                             \
    if (!(testfw_a == testfw_b))                                                            \
      ::testfw::fail(__FILE__, __LINE__, std::string("REQUIRE_EQ(" #a ", " #b ") -> ") + ::testfw::repr(testfw_a) + " != " + ::testfw::repr(testfw_b), true); \
  } while (0)
#define CHECK_THROWS_AS(expr, ExType)                                                       \
  do {                                                                                      \
    ::testfw::State::get().checks++;                                                        \
    bool testfw_thrown = false;                                                             \
    try { (void)(expr); } catch (const ExType&) { testfw_thrown = true; } catch (...) {}    \
    if (!testfw_thrown) ::testfw::fail(__FILE__, __LINE__, "CHECK_THROWS_AS(" #expr ", " #ExType ") did not throw", false); \
  } while (0)
#define CHECK_NOTHROW(expr)                                                                 \
  do {                                                                                      \
    ::testfw::State::get().checks++;                                                        \
    try { (void)(expr); } catch (const std::exception& e) { ::testfw::fail(__FILE__, __LINE__, std::string("CHECK_NOTHROW(" #expr ") threw: ") + e.what(), false); } \
  } while (0)
