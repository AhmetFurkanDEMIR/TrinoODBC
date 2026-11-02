// Integration-test runner.
//
//   trino_odbc_integration_tests [--mock] [--connstr "<connection string>"] [--filter <substring>]
//                                [--list] [--loglevel N]
//
// --mock (the default when no connection string is given) launches the Python mock Trino servers
// on free ports (plain + spooling, Basic auth, Bearer auth, TLS) and kills them at exit. --connstr
// (or TRINO_ODBC_TEST_CONNSTR) runs against a real Trino; tests that need mock-only tables skip.
// The driver comes from TRINO_ODBC_DRIVER (env) or the TRINO_ODBC_DRIVER_PATH compile definition,
// the mock script from TRINO_ODBC_MOCK_SERVER (env or compile definition).
#include "integration/odbc_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <direct.h>
#  include <process.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#ifndef TRINO_ODBC_DRIVER_PATH
#  define TRINO_ODBC_DRIVER_PATH ""
#endif
#ifndef TRINO_ODBC_MOCK_SERVER
#  define TRINO_ODBC_MOCK_SERVER ""
#endif

namespace {

struct Options {
  bool mock = false;
  bool list = false;
  std::string connStr;
  std::string filter;
  int logLevel = -1;
};

void usage() {
  std::printf(
      "usage: trino_odbc_integration_tests [--mock] [--connstr <string>] [--filter <substring>] [--list]\n"
      "                                     [--loglevel N]\n"
      "  --mock             start the Python mock servers (default when no connection string is given)\n"
      "  --connstr <s>      run against a real server (or set TRINO_ODBC_TEST_CONNSTR)\n"
      "  --filter <s>       run only tests whose name contains <s>\n"
      "  --list             print the test names and exit\n"
      "  --loglevel N       driver LogLevel appended to the connection string (env TRINO_ODBC_TEST_LOGLEVEL)\n"
      "environment: TRINO_ODBC_DRIVER (driver library), TRINO_ODBC_MOCK_SERVER (mock_trino.py),\n"
      "             TRINO_ODBC_PYTHON (interpreter, default python3), TRINO_ODBC_TEST_LOGPATH\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (a == "--mock") {
      o.mock = true;
    } else if (a == "--connstr") {
      if (!value(o.connStr)) return false;
    } else if (a == "--filter" || a == "-f") {
      if (!value(o.filter)) return false;
    } else if (a == "--list") {
      o.list = true;
    } else if (a == "--loglevel") {
      std::string v;
      if (!value(v)) return false;
      o.logLevel = std::atoi(v.c_str());
    } else if (a == "--help" || a == "-h") {
      usage();
      std::exit(0);
    } else if (!a.empty() && a[0] != '-' && o.filter.empty()) {
      o.filter = a;  // bare argument = filter, like the unit-test runner
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      return false;
    }
  }
  return true;
}

std::string tempDirectory() {
  std::string base = harness::envOr("TMPDIR", harness::envOr("TEMP", harness::envOr("TMP", "")));
  if (base.empty()) {
#ifdef _WIN32
    base = "C:\\Temp";
#else
    base = "/tmp";
#endif
  }
  if (base.back() == '/' || base.back() == '\\') base.pop_back();
#ifdef _WIN32
  const std::string dir = base + "\\trino_odbc_it_" + std::to_string(_getpid());
  _mkdir(dir.c_str());
#else
  const std::string dir = base + "/trino_odbc_it_" + std::to_string(getpid());
  mkdir(dir.c_str(), 0700);
#endif
  return dir;
}

bool fileExists(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

/// Owns the mock server processes for the duration of the run.
struct MockServers {
  harness::ChildProcess plain, basic, bearer, tls;
  std::string python, script;

  bool launch(harness::ChildProcess& proc, int port, const std::vector<std::string>& extra, const char* label) {
    std::vector<std::string> argv = {python, script, "--port", std::to_string(port)};
    argv.insert(argv.end(), extra.begin(), extra.end());
    std::string error;
    if (!proc.start(argv, &error)) {
      std::fprintf(stderr, "cannot start %s mock: %s\n", label, error.c_str());
      return false;
    }
    if (!harness::waitForPort("127.0.0.1", port, 15000) || !proc.running()) {
      std::fprintf(stderr, "%s mock did not come up on port %d\n", label, port);
      proc.kill();
      return false;
    }
    std::printf("mock server (%s) on port %d\n", label, port);
    return true;
  }
};

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parseArgs(argc, argv, opt)) {
    usage();
    return 2;
  }

  // Deterministic order regardless of static-initialisation order across translation units.
  std::vector<testfw::TestCase> tests = testfw::Registry::get().tests;
  std::stable_sort(tests.begin(), tests.end(),
                   [](const testfw::TestCase& a, const testfw::TestCase& b) { return std::strcmp(a.name, b.name) < 0; });
  if (opt.list) {
    for (const testfw::TestCase& t : tests) std::printf("%s\n", t.name);
    return 0;
  }

  harness::Context& ctx = harness::ctx();
  ctx.logLevel = opt.logLevel >= 0 ? opt.logLevel : std::atoi(harness::envOr("TRINO_ODBC_TEST_LOGLEVEL", "0").c_str());
  ctx.logPath = harness::envOr("TRINO_ODBC_TEST_LOGPATH", "");

  const std::string driver = harness::envOr("TRINO_ODBC_DRIVER", TRINO_ODBC_DRIVER_PATH);
  if (driver.empty()) {
    std::fprintf(stderr, "no driver path: set TRINO_ODBC_DRIVER or build with TRINO_ODBC_DRIVER_PATH\n");
    return 2;
  }
  try {
    const std::vector<std::string> missing = harness::loadDriver(driver);
    std::printf("driver: %s (%zu missing exports)\n", driver.c_str(), missing.size());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 2;
  }

  if (opt.connStr.empty()) opt.connStr = harness::envOr("TRINO_ODBC_TEST_CONNSTR", "");
  if (opt.connStr.empty()) opt.mock = true;

  MockServers mocks;
  if (opt.mock) {
    mocks.python = harness::envOr("TRINO_ODBC_PYTHON",
#ifdef _WIN32
                                  "python"
#else
                                  "python3"
#endif
    );
    mocks.script = harness::envOr("TRINO_ODBC_MOCK_SERVER", TRINO_ODBC_MOCK_SERVER);
    if (mocks.script.empty() || !fileExists(mocks.script)) {
      std::fprintf(stderr, "mock server script not found (TRINO_ODBC_MOCK_SERVER='%s')\n", mocks.script.c_str());
      return 2;
    }
    ctx.mock = true;
    ctx.host = "127.0.0.1";
    ctx.port = harness::findFreePort();
    if (ctx.port == 0 || !mocks.launch(mocks.plain, ctx.port, {"--spooling"}, "plain+spooling")) return 2;
    ctx.connStr = ctx.mockConnStr(ctx.port);

    // Secondary instances are optional: their tests skip when an instance is unavailable.
    const int basicPort = harness::findFreePort();
    if (basicPort && mocks.launch(mocks.basic, basicPort, {"--auth", "basic:alice:secret"}, "basic auth")) {
      ctx.basicAuthPort = basicPort;
    }
    const int bearerPort = harness::findFreePort();
    if (bearerPort && mocks.launch(mocks.bearer, bearerPort, {"--auth", "bearer:" + ctx.bearerToken}, "bearer auth")) {
      ctx.bearerPort = bearerPort;
    }
    // TLS instance: certificate from gen_cert.py next to the mock script (needs openssl).
    const std::string scriptDir = mocks.script.substr(0, mocks.script.find_last_of("/\\") + 1);
    const std::string genCert = scriptDir + "gen_cert.py";
    const std::string certDir = tempDirectory();
    if (fileExists(genCert)) {
      harness::ChildProcess gen;
      std::string error;
      if (gen.start({mocks.python, genCert, certDir}, &error)) {
        for (int i = 0; i < 300 && gen.running(); ++i) harness::sleepMs(100);
        const std::string cert = certDir + "/cert.pem";
        const std::string key = certDir + "/key.pem";
        if (fileExists(cert) && fileExists(key)) {
          const int tlsPort = harness::findFreePort();
          if (tlsPort && mocks.launch(mocks.tls, tlsPort, {"--tls", cert, key}, "tls")) {
            ctx.tlsPort = tlsPort;
            ctx.tlsCertFile = cert;
          }
        } else {
          std::printf("TLS mock skipped: gen_cert.py produced no certificate (openssl missing?)\n");
        }
      }
    }
  } else {
    ctx.mock = false;
    ctx.connStr = opt.connStr;
    if (ctx.logLevel > 0 && ctx.connStr.find("LogLevel=") == std::string::npos) {
      ctx.connStr = ctx.connStrWith("LogLevel=" + std::to_string(ctx.logLevel));
    }
    std::printf("connection string: %s\n", ctx.connStr.c_str());
  }

  int run = 0, failed = 0;
  const double started = harness::nowSeconds();
  for (const testfw::TestCase& t : tests) {
    if (!opt.filter.empty() && std::string(t.name).find(opt.filter) == std::string::npos) continue;
    ++run;
    testfw::State& st = testfw::State::get();
    st.current = t.name;
    const int before = st.failures;
    const int skippedBefore = harness::skippedCount();
    std::printf("RUN  %s\n", t.name);
    std::fflush(stdout);
    const double t0 = harness::nowSeconds();
    try {
      t.fn();
    } catch (const testfw::Failure&) {
      // already reported by REQUIRE
    } catch (const std::exception& e) {
      testfw::fail(t.file, 0, std::string("unexpected exception: ") + e.what(), false);
    } catch (...) {
      testfw::fail(t.file, 0, "unexpected non-std exception", false);
    }
    const double ms = (harness::nowSeconds() - t0) * 1000.0;
    if (st.failures != before) {
      ++failed;
      std::printf("FAIL %s (%.0f ms)\n", t.name, ms);
    } else if (harness::skippedCount() != skippedBefore) {
      std::printf("SKIP %s\n", t.name);
    } else {
      std::printf("OK   %s (%.0f ms)\n", t.name, ms);
    }
    std::fflush(stdout);
  }
  std::printf("%d tests run, %d failed, %d skipped, %d checks, %.1f s\n", run, failed, harness::skippedCount(),
              testfw::State::get().checks, harness::nowSeconds() - started);
  std::fflush(stdout);
  // MockServers' destructors kill the child processes.
  return failed == 0 ? 0 : 1;
}
