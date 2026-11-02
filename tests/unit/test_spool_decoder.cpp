// Unit tests for the spooled protocol codecs (client/spool_decoder.h), the ordered segment fetcher
// driven with inline segments (and, on POSIX, a loopback-only HTTP stub), the OAuth2 challenge
// parser and token refresh (client/oauth2.h), the session-state response header decoding and the
// transport policy helpers (client/http_client.h), and catalog cancellation (core/client_cancel.h).
// Compression round trips are guarded by TRINO_ODBC_HAVE_ZSTD / TRINO_ODBC_HAVE_LZ4 so the file
// builds with any codec set.
#include "unit/test_framework.h"

#include "client/http_client.h"
#include "client/oauth2.h"
#include "client/spool_decoder.h"
#include "client/trino_client.h"
#include "catalog/catalog.h"
#include "core/client_cancel.h"
#include "core/config.h"
#include "core/handles.h"
#include "result/json_decoder.h"
#include "result/row_source.h"
#include "util/errors.h"
#include "util/strings.h"

#if defined(TRINO_ODBC_HAVE_ZSTD)
#  include <zstd.h>
#endif
#if defined(TRINO_ODBC_HAVE_LZ4)
#  include <lz4.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(TRINO_ODBC_UNIX)
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using namespace trino_odbc;

namespace {

const TypeMappingOptions& opts() {
  static const TypeMappingOptions o;
  return o;
}

/// SQLSTATE of the DriverError thrown by `fn`, "" when nothing was thrown.
template <typename F>
std::string sqlstateOf(F&& fn) {
  try {
    fn();
  } catch (const DriverError& e) {
    return e.sqlstate();
  }
  return "";
}

/// Rows JSON for ids [from, to).
std::string rowsJson(int64_t from, int64_t to) {
  std::string s = "[";
  for (int64_t i = from; i < to; ++i) {
    if (i != from) s += ',';
    s += "[" + std::to_string(i) + ",\"r" + std::to_string(i) + "\"]";
  }
  return s + "]";
}

SpooledSegment inlineSegment(int64_t rowOffset, int64_t rowsCount, std::string bytes, int64_t uncompressed = 0) {
  SpooledSegment seg;
  seg.type = SpooledSegment::Type::Inline;
  seg.segmentSize = static_cast<int64_t>(bytes.size());
  seg.inlineData = std::move(bytes);
  seg.rowOffset = rowOffset;
  seg.rowsCount = rowsCount;
  seg.uncompressedSize = uncompressed;
  return seg;
}

std::vector<ResultColumn> idNameColumns() {
  return {ResultColumn::make("id", "bigint", opts()), ResultColumn::make("name", "varchar", opts())};
}

std::shared_ptr<const ConnectionConfig> testConfig(int maxParallel = 2) {
  ConnectionConfig c;
  c.host = "127.0.0.1";
  c.port = 8080;
  c.maxRetries = 0;
  c.maxParallelSegments = maxParallel;
  return std::make_shared<const ConnectionConfig>(c);
}

/// Everything a fetcher test needs, built once per test.
struct FetcherFixture {
  std::shared_ptr<const ConnectionConfig> config = testConfig();
  std::shared_ptr<CurlShare> share = std::make_shared<CurlShare>();
  std::atomic<bool> cancel{false};
  std::vector<ResultColumn> columns = idNameColumns();
  std::vector<std::unique_ptr<Page>> pages;

  SpoolFetcher::PageSink collector() {
    return [this](std::unique_ptr<Page> p) {
      pages.push_back(std::move(p));
      return true;
    };
  }
  /// Ids of the first column of every delivered page, in delivery order.
  std::vector<int64_t> ids() const {
    std::vector<int64_t> out;
    for (const auto& p : pages)
      for (size_t r = 0; r < p->rowCount; ++r) out.push_back(p->columns[0].getInt64(r));
    return out;
  }
};

HttpResponse responseWithHeaders(std::initializer_list<std::pair<const char*, const char*>> headers) {
  HttpResponse r;
  r.status = 200;
  for (const auto& h : headers) r.headers.emplace(h.first, h.second);
  return r;
}

}  // namespace

// ---- codecs -----------------------------------------------------------------------------------

TEST_CASE("spool_decoder: json encoding passes bytes through") {
  const std::string rows = R"([[1,"a"],[2,"b"]])";
  CHECK_EQ(decodeSegmentBytes("json", std::string_view(rows), 0), rows);
  CHECK_EQ(decodeSegmentBytes("JSON", std::string_view(rows), rows.size()), rows);  // case-insensitive
  std::string movable = rows;
  CHECK_EQ(decodeSegmentBytes("json", std::move(movable), 0), rows);
  CHECK_EQ(decodeSegmentBytes("json", std::string_view(""), 0), std::string());
}

TEST_CASE("spool_decoder: unknown encodings are HYC00") {
  CHECK_EQ(sqlstateOf([] { decodeSegmentBytes("json+snappy", std::string_view("x"), 1); }), std::string("HYC00"));
  CHECK_EQ(sqlstateOf([] { decodeSegmentBytes("gzip", std::string_view("x"), 1); }), std::string("HYC00"));
  CHECK_EQ(sqlstateOf([] { decodeSegmentBytes("", std::string_view("x"), 1); }), std::string("HYC00"));
  std::string owned = "x";
  CHECK_EQ(sqlstateOf([&] { decodeSegmentBytes("json+brotli", std::move(owned), 1); }), std::string("HYC00"));
  CHECK(!isEncodingSupported("json+snappy"));
  CHECK(!isEncodingSupported(""));
}

TEST_CASE("spool_decoder: isEncodingSupported and filterSupportedEncodings") {
  CHECK(isEncodingSupported("json"));
  CHECK(isEncodingSupported("JSON"));
  CHECK_EQ(filterSupportedEncodings("json"), std::string("json"));
  CHECK_EQ(filterSupportedEncodings(" JSON , json,bogus,json "), std::string("json"));
  CHECK_EQ(filterSupportedEncodings(""), std::string(""));
  CHECK_EQ(filterSupportedEncodings("bogus,also-bogus"), std::string(""));
#if defined(TRINO_ODBC_HAVE_ZSTD)
  CHECK(isEncodingSupported("json+zstd"));
  CHECK(isEncodingSupported("JSON+ZSTD"));
  CHECK_EQ(filterSupportedEncodings("json+zstd,json"), std::string("json+zstd,json"));
  CHECK_EQ(filterSupportedEncodings("json,JSON+zstd,json+zstd"), std::string("json,json+zstd"));
#else
  CHECK(!isEncodingSupported("json+zstd"));
  CHECK_EQ(filterSupportedEncodings("json+zstd,json"), std::string("json"));
#endif
#if defined(TRINO_ODBC_HAVE_LZ4)
  CHECK(isEncodingSupported("json+lz4"));
  CHECK_EQ(filterSupportedEncodings("json+lz4"), std::string("json+lz4"));
#else
  CHECK(!isEncodingSupported("json+lz4"));
  CHECK_EQ(filterSupportedEncodings("json+lz4,json"), std::string("json"));
#endif
  // The default config list always keeps "json" last, whatever is compiled in.
  const std::string filtered = filterSupportedEncodings("json+zstd,json+lz4,json");
  CHECK(endsWith(filtered, "json"));
  CHECK(filtered.find("bogus") == std::string::npos);
}

#if defined(TRINO_ODBC_HAVE_ZSTD)
TEST_CASE("spool_decoder: json+zstd round trip") {
  const std::string rows = rowsJson(0, 500);
  std::string compressed(ZSTD_compressBound(rows.size()), '\0');
  const size_t n = ZSTD_compress(compressed.data(), compressed.size(), rows.data(), rows.size(), 3);
  REQUIRE(!ZSTD_isError(n));
  compressed.resize(n);
  CHECK(compressed.size() < rows.size());

  CHECK_EQ(decodeSegmentBytes("json+zstd", std::string_view(compressed), rows.size()), rows);
  CHECK_EQ(decodeSegmentBytes("JSON+ZSTD", std::string_view(compressed), rows.size()), rows);
  // The frame written by ZSTD_compress carries the content size, so 0 (unknown) works too.
  CHECK_EQ(decodeSegmentBytes("json+zstd", std::string_view(compressed), 0), rows);
  std::string owned = compressed;
  CHECK_EQ(decodeSegmentBytes("json+zstd", std::move(owned), rows.size()), rows);

  // Wrong announced size, corrupt payload, truncated payload, garbage.
  CHECK_EQ(sqlstateOf([&] { decodeSegmentBytes("json+zstd", std::string_view(compressed), rows.size() + 1); }),
           std::string("HY000"));
  CHECK_EQ(sqlstateOf([&] { decodeSegmentBytes("json+zstd", std::string_view(compressed), rows.size() - 1); }),
           std::string("HY000"));
  std::string corrupt = compressed;
  for (size_t i = 12; i < corrupt.size(); i += 7) corrupt[i] = static_cast<char>(corrupt[i] ^ 0x5A);
  CHECK_EQ(sqlstateOf([&] { decodeSegmentBytes("json+zstd", std::string_view(corrupt), rows.size()); }),
           std::string("HY000"));
  const std::string truncated = compressed.substr(0, compressed.size() / 2);
  CHECK_EQ(sqlstateOf([&] { decodeSegmentBytes("json+zstd", std::string_view(truncated), rows.size()); }),
           std::string("HY000"));
  CHECK_EQ(sqlstateOf([] { decodeSegmentBytes("json+zstd", std::string_view("this is not zstd"), 10); }),
           std::string("HY000"));
  CHECK_EQ(sqlstateOf([] { decodeSegmentBytes("json+zstd", std::string_view("this is not zstd"), 0); }),
           std::string("HY000"));
  // An implausible size is refused before any allocation.
  CHECK_EQ(sqlstateOf([&] { decodeSegmentBytes("json+zstd", std::string_view(compressed), size_t(1) << 40); }),
           std::string("HY000"));

  // Decoded segment bytes are ordinary rows JSON.
  std::string json = decodeSegmentBytes("json+zstd", std::string_view(compressed), rows.size());
  std::unique_ptr<Page> page = decodeRowsJson(json, idNameColumns());
  REQUIRE(page != nullptr);
  CHECK_EQ(page->rowCount, size_t(500));
  CHECK_EQ(page->columns[0].getInt64(499), int64_t(499));
}
#else
TEST_CASE("spool_decoder: json+zstd not compiled in is HYC00") {
  CHECK_EQ(sqlstateOf([] { decodeSegmentBytes("json+zstd", std::string_view("abc"), 3); }), std::string("HYC00"));
}
#endif

#if defined(TRINO_ODBC_HAVE_LZ4)
TEST_CASE("spool_decoder: json+lz4 round trip") {
  const std::string rows = rowsJson(1000, 1600);
  std::string compressed(static_cast<size_t>(LZ4_compressBound(static_cast<int>(rows.size()))), '\0');
  const int n = LZ4_compress_default(rows.data(), compressed.data(), static_cast<int>(rows.size()),
                                     static_cast<int>(compressed.size()));
  REQUIRE(n > 0);
  compressed.resize(static_cast<size_t>(n));
  CHECK(compressed.size() < rows.size());

  CHECK_EQ(decodeSegmentBytes("json+lz4", std::string_view(compressed), rows.size()), rows);
  CHECK_EQ(decodeSegmentBytes("JSON+LZ4", std::string_view(compressed), rows.size()), rows);
  std::string owned = compressed;
  CHECK_EQ(decodeSegmentBytes("json+lz4", std::move(owned), rows.size()), rows);

  // lz4 block format has no header: the size is mandatory and must be exact.
  CHECK_EQ(sqlstateOf([&] { decodeSegmentBytes("json+lz4", std::string_view(compressed), 0); }), std::string("HY000"));
  CHECK_EQ(sqlstateOf([&] { decodeSegmentBytes("json+lz4", std::string_view(compressed), rows.size() + 5); }),
           std::string("HY000"));
  CHECK_EQ(sqlstateOf([&] { decodeSegmentBytes("json+lz4", std::string_view(compressed), rows.size() / 2); }),
           std::string("HY000"));
  const std::string truncated = compressed.substr(0, compressed.size() / 2);
  CHECK_EQ(sqlstateOf([&] { decodeSegmentBytes("json+lz4", std::string_view(truncated), rows.size()); }),
           std::string("HY000"));
  std::string corrupt = compressed;
  for (size_t i = 0; i < corrupt.size(); i += 5) corrupt[i] = static_cast<char>(corrupt[i] ^ 0xA5);
  CHECK_EQ(sqlstateOf([&] { decodeSegmentBytes("json+lz4", std::string_view(corrupt), rows.size()); }),
           std::string("HY000"));
  CHECK_EQ(sqlstateOf([&] { decodeSegmentBytes("json+lz4", std::string_view(compressed), size_t(1) << 40); }),
           std::string("HY000"));

  std::string json = decodeSegmentBytes("json+lz4", std::string_view(compressed), rows.size());
  std::unique_ptr<Page> page = decodeRowsJson(json, idNameColumns());
  REQUIRE(page != nullptr);
  CHECK_EQ(page->rowCount, size_t(600));
  CHECK_EQ(std::string(page->columns[1].getBytes(0)), std::string("r1000"));
}
#else
TEST_CASE("spool_decoder: json+lz4 not compiled in is HYC00") {
  CHECK_EQ(sqlstateOf([] { decodeSegmentBytes("json+lz4", std::string_view("abc"), 3); }), std::string("HYC00"));
}
#endif

// ---- SpoolFetcher (inline segments only: no network) ------------------------------------------

TEST_CASE("spool_fetcher: inline segments are delivered in rowOffset order") {
  FetcherFixture f;
  SpoolFetcher fetcher(f.config, f.share, &f.cancel, std::nullopt, 3);
  SpooledData data;
  data.encoding = "json";
  // Deliberately shuffled: offsets 20, 0, 10, 30.
  data.segments.push_back(inlineSegment(20, 10, rowsJson(20, 30)));
  data.segments.push_back(inlineSegment(0, 10, rowsJson(0, 10)));
  data.segments.push_back(inlineSegment(10, 10, rowsJson(10, 20)));
  data.segments.push_back(inlineSegment(30, 5, rowsJson(30, 35)));
  CHECK(fetcher.process(std::move(data), f.columns, f.collector()));
  REQUIRE_EQ(f.pages.size(), size_t(4));
  const std::vector<int64_t> ids = f.ids();
  REQUIRE_EQ(ids.size(), size_t(35));
  for (size_t i = 0; i < ids.size(); ++i) CHECK_EQ(ids[i], static_cast<int64_t>(i));
  CHECK_EQ(f.pages[0]->rowCount, size_t(10));
  CHECK_EQ(f.pages[3]->rowCount, size_t(5));
  CHECK_EQ(std::string(f.pages[3]->columns[1].getBytes(4)), std::string("r34"));

  // A second batch on the same fetcher reuses the workers.
  SpooledData more;
  more.encoding = "json";
  more.segments.push_back(inlineSegment(40, 2, rowsJson(40, 42)));
  more.segments.push_back(inlineSegment(35, 5, rowsJson(35, 40)));
  CHECK(fetcher.process(std::move(more), f.columns, f.collector()));
  REQUIRE_EQ(f.pages.size(), size_t(6));
  CHECK_EQ(f.pages[4]->columns[0].getInt64(0), int64_t(35));
  CHECK_EQ(f.pages[5]->columns[0].getInt64(1), int64_t(41));
  fetcher.shutdown();
  fetcher.shutdown();  // idempotent
}

TEST_CASE("spool_fetcher: many small segments with one worker keep order and count") {
  FetcherFixture f;
  SpoolFetcher fetcher(f.config, f.share, &f.cancel, std::nullopt, 1);
  SpooledData data;
  data.encoding = "json";
  for (int64_t i = 49; i >= 0; --i) data.segments.push_back(inlineSegment(i * 3, 3, rowsJson(i * 3, i * 3 + 3)));
  CHECK(fetcher.process(std::move(data), f.columns, f.collector()));
  const std::vector<int64_t> ids = f.ids();
  REQUIRE_EQ(ids.size(), size_t(150));
  for (size_t i = 0; i < ids.size(); ++i) CHECK_EQ(ids[i], static_cast<int64_t>(i));
}

TEST_CASE("spool_fetcher: empty segment list is a no-op") {
  FetcherFixture f;
  SpoolFetcher fetcher(f.config, f.share, &f.cancel, std::nullopt, 2);
  SpooledData data;
  data.encoding = "json";
  CHECK(fetcher.process(std::move(data), f.columns, f.collector()));
  CHECK(f.pages.empty());
}

TEST_CASE("spool_fetcher: sink returning false stops delivery") {
  FetcherFixture f;
  SpoolFetcher fetcher(f.config, f.share, &f.cancel, std::nullopt, 2);
  SpooledData data;
  data.encoding = "json";
  for (int64_t i = 0; i < 8; ++i) data.segments.push_back(inlineSegment(i * 4, 4, rowsJson(i * 4, i * 4 + 4)));
  int delivered = 0;
  const bool completed = fetcher.process(std::move(data), f.columns, [&](std::unique_ptr<Page> p) {
    ++delivered;
    CHECK_EQ(p->rowCount, size_t(4));
    return delivered < 2;
  });
  CHECK(!completed);
  CHECK_EQ(delivered, 2);
  // The fetcher is still usable afterwards.
  SpooledData again;
  again.encoding = "json";
  again.segments.push_back(inlineSegment(0, 1, rowsJson(0, 1)));
  CHECK(fetcher.process(std::move(again), f.columns, f.collector()));
  REQUIRE_EQ(f.pages.size(), size_t(1));
}

TEST_CASE("spool_fetcher: segment metadata mismatches are errors") {
  FetcherFixture f;
  SpoolFetcher fetcher(f.config, f.share, &f.cancel, std::nullopt, 2);
  {
    SpooledData data;
    data.encoding = "json";
    data.segments.push_back(inlineSegment(0, 2, rowsJson(0, 2)));
    data.segments.push_back(inlineSegment(2, 99, rowsJson(2, 4)));  // rowsCount lies
    CHECK_EQ(sqlstateOf([&] { fetcher.process(std::move(data), f.columns, f.collector()); }), std::string("HY000"));
  }
  {
    SpooledData data;
    data.encoding = "json";
    SpooledSegment bad = inlineSegment(0, 2, rowsJson(0, 2));
    bad.segmentSize = 5;  // segmentSize lies
    data.segments.push_back(std::move(bad));
    CHECK_EQ(sqlstateOf([&] { fetcher.process(std::move(data), f.columns, f.collector()); }), std::string("HY000"));
  }
  {
    SpooledData data;
    data.encoding = "json";
    data.segments.push_back(inlineSegment(0, 1, "[[1,\"a\",\"extra\"]]"));  // wrong column count
    CHECK_EQ(sqlstateOf([&] { fetcher.process(std::move(data), f.columns, f.collector()); }), std::string("HY000"));
  }
  {
    SpooledData data;
    data.encoding = "json";
    data.segments.push_back(inlineSegment(0, 1, "[[1,"));  // truncated JSON
    CHECK_EQ(sqlstateOf([&] { fetcher.process(std::move(data), f.columns, f.collector()); }), std::string("HY000"));
  }
  // Still healthy after the failures. Pages of valid segments that preceded a corrupt one were
  // already delivered (streaming delivery); drop them before checking the healthy batch.
  f.pages.clear();
  SpooledData ok;
  ok.encoding = "json";
  ok.segments.push_back(inlineSegment(0, 3, rowsJson(0, 3)));
  CHECK(fetcher.process(std::move(ok), f.columns, f.collector()));
  REQUIRE_EQ(f.pages.size(), size_t(1));
  CHECK_EQ(f.pages[0]->rowCount, size_t(3));
}

TEST_CASE("spool_fetcher: encoding the driver did not offer is HYC00") {
  FetcherFixture f;
  SpoolFetcher fetcher(f.config, f.share, &f.cancel, std::nullopt, 2);
  SpooledData data;
  data.encoding = "json+snappy";
  data.segments.push_back(inlineSegment(0, 1, "x"));
  CHECK_EQ(sqlstateOf([&] { fetcher.process(std::move(data), f.columns, f.collector()); }), std::string("HYC00"));
  CHECK(f.pages.empty());
}

TEST_CASE("spool_fetcher: process after shutdown is cancelled") {
  FetcherFixture f;
  SpoolFetcher fetcher(f.config, f.share, &f.cancel, std::nullopt, 2);
  SpooledData warmup;
  warmup.encoding = "json";
  warmup.segments.push_back(inlineSegment(0, 1, rowsJson(0, 1)));
  CHECK(fetcher.process(std::move(warmup), f.columns, f.collector()));
  fetcher.shutdown();
  SpooledData data;
  data.encoding = "json";
  data.segments.push_back(inlineSegment(0, 1, rowsJson(0, 1)));
  CHECK_THROWS_AS(fetcher.process(std::move(data), f.columns, f.collector()), CancelledError);
  CHECK_EQ(f.pages.size(), size_t(1));

  // A fetcher that never started workers can be shut down and destroyed safely too.
  SpoolFetcher idle(f.config, f.share, &f.cancel, std::nullopt, 4);
  idle.shutdown();
}

#if defined(TRINO_ODBC_HAVE_ZSTD)
TEST_CASE("spool_fetcher: compressed inline segments decode through the codec") {
  FetcherFixture f;
  SpoolFetcher fetcher(f.config, f.share, &f.cancel, std::nullopt, 2);
  SpooledData data;
  data.encoding = "json+zstd";
  for (int64_t i = 2; i >= 0; --i) {
    const std::string rows = rowsJson(i * 7, i * 7 + 7);
    std::string compressed(ZSTD_compressBound(rows.size()), '\0');
    const size_t n = ZSTD_compress(compressed.data(), compressed.size(), rows.data(), rows.size(), 1);
    REQUIRE(!ZSTD_isError(n));
    compressed.resize(n);
    data.segments.push_back(inlineSegment(i * 7, 7, std::move(compressed), static_cast<int64_t>(rows.size())));
  }
  CHECK(fetcher.process(std::move(data), f.columns, f.collector()));
  const std::vector<int64_t> ids = f.ids();
  REQUIRE_EQ(ids.size(), size_t(21));
  for (size_t i = 0; i < ids.size(); ++i) CHECK_EQ(ids[i], static_cast<int64_t>(i));
}
#endif

// ---- OAuth2 challenge parsing -------------------------------------------------------------------

TEST_CASE("oauth2: parseBearerChallenge accepts Trino's challenge in every spelling") {
  std::string redirect, token;
  CHECK(parseBearerChallenge(
      R"(Bearer x_redirect_server="https://trino.example.com/oauth2/token/initiate/abc", x_token_server="https://trino.example.com/oauth2/token/abc")",
      &redirect, &token));
  CHECK_EQ(redirect, std::string("https://trino.example.com/oauth2/token/initiate/abc"));
  CHECK_EQ(token, std::string("https://trino.example.com/oauth2/token/abc"));

  // Parameters in the other order, bare (unquoted) values, extra whitespace, lowercase scheme.
  redirect.clear();
  token.clear();
  CHECK(parseBearerChallenge("bearer   x_token_server=https://t/token/1 ,x_redirect_server = https://t/init/1",
                             &redirect, &token));
  CHECK_EQ(redirect, std::string("https://t/init/1"));
  CHECK_EQ(token, std::string("https://t/token/1"));

  // Other challenges on the same line and unrelated parameters are ignored.
  redirect.clear();
  token.clear();
  CHECK(parseBearerChallenge(
      R"(Basic realm="Trino", Bearer realm="Trino", x_redirect_server="https://t/init/2", x_token_server="https://t/token/2", error="invalid_token")",
      &redirect, &token));
  CHECK_EQ(redirect, std::string("https://t/init/2"));
  CHECK_EQ(token, std::string("https://t/token/2"));

  // Escaped quotes inside a quoted value.
  redirect.clear();
  CHECK(parseBearerChallenge(R"(Bearer x_redirect_server="https://t/init?a=\"b\"", x_token_server="https://t/tok")",
                             &redirect, &token));
  CHECK_EQ(redirect, std::string("https://t/init?a=\"b\""));

  // Null outputs just report the presence of both parameters.
  CHECK(parseBearerChallenge(R"(Bearer x_redirect_server="https://a", x_token_server="https://b")", nullptr, nullptr));
}

TEST_CASE("oauth2: parseBearerChallenge rejects incomplete challenges") {
  std::string redirect = "keep", token = "keep";
  CHECK(!parseBearerChallenge(R"(Bearer x_redirect_server="https://t/init")", &redirect, &token));
  CHECK(!parseBearerChallenge(R"(Bearer x_token_server="https://t/token")", &redirect, &token));
  CHECK(!parseBearerChallenge(R"(Basic realm="Trino")", &redirect, &token));
  CHECK(!parseBearerChallenge(R"(Bearer realm="Trino")", &redirect, &token));
  CHECK(!parseBearerChallenge("", &redirect, &token));
  // The scheme must be the word Bearer, not part of another token.
  CHECK(!parseBearerChallenge(R"(NotBearer x_redirect_server="https://a", x_token_server="https://b")", &redirect,
                              &token));
  // Empty values do not count.
  CHECK(!parseBearerChallenge(R"(Bearer x_redirect_server="", x_token_server="https://b")", &redirect, &token));
  // Parameter names must match whole tokens (my_x_token_server is not x_token_server).
  CHECK(!parseBearerChallenge(R"(Bearer x_redirect_server="https://a", my_x_token_server="https://b")", &redirect,
                              &token));
  CHECK_EQ(redirect, std::string("keep"));
  CHECK_EQ(token, std::string("keep"));
}

// ---- session state from response headers -------------------------------------------------------

TEST_CASE("session: response headers update catalog, schema, path and session properties") {
  SessionState s;
  s.sessionProperties["query_max_run_time"] = "1h";
  s.sessionProperties["hive.x"] = "old";
  applySessionResponseHeaders(
      s, responseWithHeaders({{"X-Trino-Set-Catalog", "tpch"},
                              {"x-trino-set-schema", "tiny"},
                              {"X-Trino-Set-Path", "tpch.tiny, hive.default"},
                              {"X-Trino-Set-Session", "hive.x=a+b%2Fc%3Dd"},
                              {"X-Trino-Set-Session", "join_distribution_type=PARTITIONED"},
                              {"X-Trino-Clear-Session", "query_max_run_time"},
                              {"Content-Type", "application/json"},
                              {"X-Trino-Something-Else", "ignored"}}));
  CHECK_EQ(s.catalog, std::string("tpch"));
  CHECK_EQ(s.schema, std::string("tiny"));
  CHECK_EQ(s.path, std::string("tpch.tiny, hive.default"));
  CHECK_EQ(s.sessionProperties.size(), size_t(2));
  CHECK_EQ(s.sessionProperties["hive.x"], std::string("a b/c=d"));
  CHECK_EQ(s.sessionProperties["join_distribution_type"], std::string("PARTITIONED"));
  CHECK(s.sessionProperties.count("query_max_run_time") == 0);
  CHECK(s.transactionId.empty());
  CHECK(s.roles.empty());
  CHECK(s.preparedStatements.empty());
}

TEST_CASE("session: roles, prepared statements, transactions and authorization user") {
  SessionState s;
  applySessionResponseHeaders(
      s, responseWithHeaders({{"X-Trino-Set-Role", "hive=ROLE%7Badmin%7D"},
                              {"X-Trino-Set-Role", "system=ALL"},
                              {"X-Trino-Added-Prepare", "odbc_stmt_1=SELECT+%2A+FROM+t+WHERE+x+%3D+%3F"},
                              {"X-Trino-Added-Prepare", "odbc_stmt_2=SELECT%201"},
                              {"X-Trino-Started-Transaction-Id", "tx-123"},
                              {"X-Trino-Set-Authorization-User", "alice"}}));
  CHECK_EQ(s.roles["hive"], std::string("ROLE{admin}"));
  CHECK_EQ(s.roles["system"], std::string("ALL"));
  CHECK_EQ(s.preparedStatements["odbc_stmt_1"], std::string("SELECT * FROM t WHERE x = ?"));
  CHECK_EQ(s.preparedStatements["odbc_stmt_2"], std::string("SELECT 1"));
  CHECK_EQ(s.transactionId, std::string("tx-123"));
  CHECK_EQ(s.authorizationUser, std::string("alice"));

  applySessionResponseHeaders(s, responseWithHeaders({{"X-Trino-Deallocated-Prepare", "odbc_stmt_1"},
                                                      {"X-Trino-Clear-Transaction-Id", "true"},
                                                      {"X-Trino-Reset-Authorization-User", "true"}}));
  CHECK(s.preparedStatements.count("odbc_stmt_1") == 0);
  CHECK_EQ(s.preparedStatements.size(), size_t(1));
  CHECK(s.transactionId.empty());
  CHECK(s.authorizationUser.empty());
  CHECK_EQ(s.roles.size(), size_t(2));  // untouched

  // Malformed pairs (no '=') are ignored rather than corrupting the state.
  applySessionResponseHeaders(s, responseWithHeaders({{"X-Trino-Set-Session", "novalue"}, {"X-Trino-Set-Role", "x"}}));
  CHECK(s.sessionProperties.empty());
  CHECK_EQ(s.roles.size(), size_t(2));
  // A response without any Trino headers leaves everything alone.
  applySessionResponseHeaders(s, responseWithHeaders({{"Content-Length", "12"}}));
  CHECK_EQ(s.roles.size(), size_t(2));
}

TEST_CASE("spool_fetcher: inline segments sharing one concatenated payload are sliced by segmentSize") {
  FetcherFixture f;
  SpoolFetcher fetcher(f.config, f.share, &f.cancel, std::nullopt, 2);
  const std::string first = rowsJson(0, 3);
  const std::string second = rowsJson(3, 5);
  SpooledData data;
  data.encoding = "json";
  SpooledSegment a = inlineSegment(0, 3, first + second);
  a.segmentSize = static_cast<int64_t>(first.size());
  SpooledSegment b = inlineSegment(3, 2, first + second);
  b.segmentSize = static_cast<int64_t>(second.size());
  data.segments.push_back(std::move(a));
  data.segments.push_back(std::move(b));
  CHECK(fetcher.process(std::move(data), f.columns, f.collector()));
  REQUIRE_EQ(f.pages.size(), size_t(2));
  CHECK_EQ(f.pages[0]->rowCount, size_t(3));
  CHECK_EQ(f.pages[1]->rowCount, size_t(2));
  CHECK_EQ(f.pages[1]->columns[0].getInt64(0), int64_t(3));
}

// ==================================================================================================
// Transport policy of client/http_client.h: credential scope, log redaction, TLS backend checks,
// host canonicalisation and pinned helper threads.
// ==================================================================================================

namespace {

std::shared_ptr<const ConnectionConfig> authConfig(AuthType auth) {
  ConnectionConfig c;
  c.host = "trino.example";
  c.port = 443;
  c.ssl = true;
  c.auth = auth;
  c.user = "alice";
  c.password = "secret";
  c.maxRetries = 0;
  return std::make_shared<const ConnectionConfig>(c);
}

}  // namespace

TEST_CASE("http: urlOrigin normalises scheme, host and default ports") {
  CHECK_EQ(urlOrigin("https://Trino.Example/v1/statement"), std::string("https://trino.example:443"));
  CHECK_EQ(urlOrigin("http://trino.example/v1"), std::string("http://trino.example:80"));
  CHECK_EQ(urlOrigin("https://trino.example:8443/x?y=1"), std::string("https://trino.example:8443"));
  CHECK_EQ(urlOrigin("https://user:pw@trino.example:443/x"), std::string("https://trino.example:443"));
  CHECK_EQ(urlOrigin("http://[::1]:8080/v1"), std::string("http://[::1]:8080"));
  CHECK_EQ(urlOrigin("not a url"), std::string());
  CHECK_EQ(replaceUrlHost("https://alias.example:8443/v1/statement?x=1", "coord01.example"),
           std::string("https://coord01.example:8443/v1/statement?x=1"));
  CHECK_EQ(replaceUrlHost("http://[::1]:8080/v1", "::2"), std::string("http://[::2]:8080/v1"));
}

TEST_CASE("http: Basic and Bearer credentials are never sent to another origin") {
  // Every refusal happens before any network I/O, so the (unreachable) hosts are never contacted.
  auto share = std::make_shared<CurlShare>();
  auto auth = std::make_shared<AuthContext>();
  auth->setOrigin("https://trino.example:443");
  HttpClient basic(authConfig(AuthType::Ldap), share, auth);
  HttpRequest req;
  req.url = "http://trino.example:8080/v1/statement/queued/q1/t/1";  // scheme downgrade behind a proxy
  CHECK_EQ(sqlstateOf([&] { basic.perform(req); }), std::string("08S01"));
  req.url = "https://evil.example/v1/statement/queued/q1/t/1";        // other host
  CHECK_EQ(sqlstateOf([&] { basic.perform(req); }), std::string("08S01"));
  req.url = "https://trino.example:8443/v1/statement/queued/q1/t/1";  // other port
  CHECK_EQ(sqlstateOf([&] { basic.perform(req); }), std::string("08S01"));

  auto bearerAuth = std::make_shared<AuthContext>();
  bearerAuth->setBearer("token-1");
  HttpClient bearer(authConfig(AuthType::Jwt), share, bearerAuth);  // origin derived from the config
  req.url = "http://trino.example/v1/statement/executing/q1/t/2";
  CHECK_EQ(sqlstateOf([&] { bearer.perform(req); }), std::string("08S01"));
}

TEST_CASE("http: URLs are redacted before they are logged") {
  CHECK_EQ(redactUrlForLog("https://s3.example/bucket/seg-1?X-Amz-Signature=abc&X-Amz-Credential=k", false),
           std::string("https://s3.example/bucket/seg-1?..."));
  CHECK_EQ(redactUrlForLog("https://trino.example/oauth2/token/secret-auth-id", true),
           std::string("https://trino.example/<redacted>"));
  CHECK_EQ(redactUrlForLog("https://u:p@trino.example:8443/v1/statement", false),
           std::string("https://trino.example:8443/v1/statement"));
  CHECK_EQ(redactUrlForLog("garbage", false), std::string("<unparsable URL>"));
  const std::string logged = redactUrlForLog("https://gcs.example/o?token=SECRET", true);
  CHECK(logged.find("SECRET") == std::string::npos);
}

TEST_CASE("http: Schannel rejects PEM client certificates and a separate key up front") {
  ConnectionConfig c;
  c.host = "trino.example";
  c.ssl = true;
  c.auth = AuthType::Certificate;
  c.clientCert = "C:\\certs\\client.pem";
  c.clientKey = "C:\\certs\\client.key";
  CHECK_EQ(sqlstateOf([&] { checkClientCertificateSupport(c, "Schannel"); }), std::string("28000"));
  c.auth = AuthType::Ldap;  // mTLS on top of another authentication: general error
  CHECK_EQ(sqlstateOf([&] { checkClientCertificateSupport(c, "Schannel"); }), std::string("HY000"));
  // PKCS#12 files and certificate-store paths are what Schannel understands.
  c.clientCert = "C:\\certs\\client.PFX";
  CHECK_EQ(sqlstateOf([&] { checkClientCertificateSupport(c, "Schannel"); }), std::string());
  c.clientCert = "client.p12";
  CHECK_EQ(sqlstateOf([&] { checkClientCertificateSupport(c, "Schannel"); }), std::string());
  c.clientCert = "CurrentUser\\MY\\0123456789abcdef0123456789abcdef01234567";
  CHECK_EQ(sqlstateOf([&] { checkClientCertificateSupport(c, "Schannel"); }), std::string());
  // OpenSSL / SecureTransport keep accepting PEM certificates with a separate key.
  c.clientCert = "/etc/certs/client.pem";
  CHECK_EQ(sqlstateOf([&] { checkClientCertificateSupport(c, "OpenSSL/3.3.1"); }), std::string());
  CHECK_EQ(sqlstateOf([&] { checkClientCertificateSupport(c, "SecureTransport"); }), std::string());
}

TEST_CASE("http: Schannel revocation checking defaults to best effort and is configurable") {
  ConnectionConfig c;
  c.host = "trino.example";
  CHECK_EQ(revocationSslOptions(c, "OpenSSL/3.3.1"), 0L);  // no revocation check to relax
  const long relaxed = revocationSslOptions(c, "Schannel");
  CHECK(relaxed != 0L);  // CURLSSLOPT_REVOKE_BEST_EFFORT
  c.unknown["checkcertrevocation"] = "1";
  CHECK_EQ(revocationSslOptions(c, "Schannel"), 0L);  // strict: Schannel's own default
  c.unknown["CheckCertRevocation"] = "0";
  const long off = revocationSslOptions(c, "Schannel");
  CHECK(off != 0L);  // CURLSSLOPT_NO_REVOKE
  CHECK(off != relaxed);
  c.unknown["CheckCertRevocation"] = "maybe";  // invalid: warning, default kept
  CHECK_EQ(revocationSslOptions(c, "Schannel"), relaxed);
}

TEST_CASE("http: canonicalHostName leaves IP literals alone") {
  CHECK_EQ(canonicalHostName("127.0.0.1"), std::string());
  CHECK_EQ(canonicalHostName("::1"), std::string());
  CHECK_EQ(canonicalHostName(""), std::string());
}

TEST_CASE("http: startDetachedHelper runs the helper to completion") {
  auto done = std::make_shared<std::atomic<int>>(0);
  for (int i = 0; i < 3; ++i) CHECK(startDetachedHelper([done] { done->fetch_add(1); }));
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (done->load() < 3 && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK_EQ(done->load(), 3);
}

// ==================================================================================================
// Loopback HTTP tests (POSIX only; everything stays on 127.0.0.1): sync-phase cancel of spooled
// segment downloads, the query-timeout hand-over, OAuth2 token refresh and catalog cancellation.
// ==================================================================================================

#if defined(TRINO_ODBC_UNIX)
namespace {

/// Minimal HTTP/1.1 server on 127.0.0.1 answering every request with `body` (200, JSON), or never
/// answering at all when `silent` (the connection is held open until the server is destroyed).
class LoopbackServer {
 public:
  LoopbackServer(std::string body, bool silent) : body_(std::move(body)), silent_(silent) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd_ >= 0);
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(::listen(fd_, 16) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port = ntohs(addr.sin_port);
    thread_ = std::thread([this] { serve(); });
  }
  ~LoopbackServer() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    for (int c : held_) ::close(c);
    ::close(fd_);
  }
  std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }

  int port = 0;
  std::atomic<int> requests{0};

 private:
  void serve() {
    while (!stop_.load()) {
      pollfd p{fd_, POLLIN, 0};
      if (::poll(&p, 1, 20) <= 0) continue;
      const int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0) continue;
      // Read the request head (the tests only send GET / DELETE without a body).
      std::string head;
      char buf[1024];
      while (head.find("\r\n\r\n") == std::string::npos) {
        pollfd q{c, POLLIN, 0};
        if (::poll(&q, 1, 1000) <= 0) break;
        const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        head.append(buf, static_cast<size_t>(n));
      }
      requests.fetch_add(1);
      if (silent_) {
        held_.push_back(c);
        continue;
      }
      const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                               std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_;
      (void)::send(c, resp.data(), resp.size(), 0);
      ::close(c);
    }
  }

  std::string body_;
  bool silent_;
  int fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::vector<int> held_;
};

SpooledSegment spooledSegment(const std::string& uri, int64_t rowOffset, int64_t rowsCount) {
  SpooledSegment seg;
  seg.type = SpooledSegment::Type::Spooled;
  seg.uri = uri;
  seg.rowOffset = rowOffset;
  seg.rowsCount = rowsCount;
  return seg;
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

TEST_CASE("spool_fetcher: the external cancel flag aborts segment downloads in flight") {
  LoopbackServer server("", /*silent=*/true);  // the segment download never completes
  FetcherFixture f;
  SpoolFetcher fetcher(f.config, f.share, &f.cancel, std::nullopt, 2);
  SpooledData data;
  data.encoding = "json";
  data.segments.push_back(spooledSegment(server.url("/seg/0"), 0, 1));
  data.segments.push_back(spooledSegment(server.url("/seg/1"), 1, 1));
  std::atomic<bool> external{false};
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    external.store(true);  // SQLCancel during the synchronous phase of SQLExecute
  });
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_THROWS_AS(fetcher.process(std::move(data), f.columns, f.collector(), &external), CancelledError);
  canceller.join();
  CHECK(secondsSince(t0) < 5.0);  // RequestTimeout is 60 s: only the cancel can end it this early
  CHECK(f.pages.empty());
  fetcher.shutdown();
}

TEST_CASE("spool_fetcher: a lifted deadline no longer fails later segment downloads") {
  LoopbackServer server("[[7,\"seven\"]]", false);
  FetcherFixture f;
  const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  {
    SpoolFetcher fetcher(f.config, f.share, &f.cancel, past, 1);
    SpooledData data;
    data.encoding = "json";
    data.segments.push_back(spooledSegment(server.url("/seg/0"), 0, 1));
    CHECK_EQ(sqlstateOf([&] { fetcher.process(std::move(data), f.columns, f.collector()); }), std::string("HYT00"));
  }
  // The query timeout covers only SQLExecute: the owner lifts it before the prefetch phase.
  SpoolFetcher fetcher(f.config, f.share, &f.cancel, past, 1);
  fetcher.setDeadline(std::nullopt);
  SpooledData data;
  data.encoding = "json";
  data.segments.push_back(spooledSegment(server.url("/seg/1"), 0, 1));
  CHECK(fetcher.process(std::move(data), f.columns, f.collector()));
  REQUIRE_EQ(f.pages.size(), size_t(1));
  CHECK_EQ(f.pages[0]->columns[0].getInt64(0), int64_t(7));
}

TEST_CASE("oauth2: refreshToken reuses a token another request obtained instead of a new login") {
  LoopbackServer server("{\"token\":\"tok-fresh\"}", false);
  const std::string challenge = "Bearer x_redirect_server=\"" + server.url("/oauth2/redirect") +
                                "\", x_token_server=\"" + server.url("/oauth2/token/abc") + "\"";
  auto share = std::make_shared<CurlShare>();
  HttpClient http(testConfig(), share, nullptr);
  auto auth = std::make_shared<AuthContext>();

  // The request carried T1, but another statement has meanwhile published T2: retry with T2.
  auth->setBearer("T2");
  OAuth2Authenticator a("refresh-test|alice", 5, RedirectHandler::Print, nullptr);
  CHECK_EQ(a.refreshToken(http, challenge, "T1", auth.get()), std::string("T2"));
  CHECK_EQ(server.requests.load(), 0);
  CHECK_EQ(auth->bearer(), std::string("T2"));

  // The rejected token is the current one: exactly one login, even for concurrent 401s.
  std::string r1, r2;
  std::thread t1([&] {
    OAuth2Authenticator o("refresh-test|alice", 5, RedirectHandler::Print, nullptr);
    HttpClient h(testConfig(), share, nullptr);
    r1 = o.refreshToken(h, challenge, "T2", auth.get());
  });
  std::thread t2([&] {
    OAuth2Authenticator o("refresh-test|alice", 5, RedirectHandler::Print, nullptr);
    HttpClient h(testConfig(), share, nullptr);
    r2 = o.refreshToken(h, challenge, "T2", auth.get());
  });
  t1.join();
  t2.join();
  CHECK_EQ(r1, std::string("tok-fresh"));
  CHECK_EQ(r2, std::string("tok-fresh"));
  CHECK_EQ(server.requests.load(), 1);
  CHECK_EQ(auth->bearer(), std::string("tok-fresh"));
  a.dropCachedToken("tok-fresh");
}

TEST_CASE("oauth2: tokens of connection-private cache keys stay out of the process cache") {
  LoopbackServer server("{\"token\":\"tok-private\"}", false);
  const std::string challenge = "Bearer x_redirect_server=\"" + server.url("/r") + "\", x_token_server=\"" +
                                server.url("/t") + "\"";
  HttpClient http(testConfig(), std::make_shared<CurlShare>(), nullptr);
  OAuth2Authenticator priv("private-test|connection#1", 5, RedirectHandler::Print, nullptr);
  priv.setUseProcessCache(false);
  CHECK_EQ(priv.obtainToken(http, challenge), std::string("tok-private"));
  CHECK(!priv.cachedToken().has_value());
  OAuth2Authenticator sameKey("private-test|connection#1", 5, RedirectHandler::Print, nullptr);
  CHECK(!sameKey.cachedToken().has_value());  // nothing accumulated process-wide

  // Keys shared between connections (UID given) keep using the cache.
  OAuth2Authenticator shared("private-test|bob", 5, RedirectHandler::Print, nullptr);
  CHECK_EQ(shared.obtainToken(http, challenge), std::string("tok-private"));
  const std::optional<std::string> cached = shared.cachedToken();
  REQUIRE(cached.has_value());
  CHECK_EQ(*cached, std::string("tok-private"));
  shared.dropCachedToken("tok-private");
  CHECK(!shared.cachedToken().has_value());
}

TEST_CASE("catalog: SQLCancel reaches metadata queries through MetadataCancelScope") {
  LoopbackServer server("", /*silent=*/true);  // system.jdbc query that never answers
  Connection conn;
  conn.config.host = "127.0.0.1";
  conn.config.port = server.port;
  conn.config.validateConnection = false;
  conn.config.maxRetries = 0;
  conn.client = std::make_unique<TrinoClient>(conn.config, nullptr);
  conn.client->connect();  // no validation, no authentication: no request yet
  conn.connected = true;
  CatalogArgs args;
  args.catalog = "tpch";
  args.schema = "tiny";
  args.table = "%";

  // A flag already set fails the call before any request is made.
  std::atomic<bool> preset{true};
  {
    MetadataCancelScope scope(&preset);
    CHECK_EQ(sqlstateOf([&] { catalogTables(conn, args); }), std::string("HY008"));
  }
  CHECK(MetadataCancelScope::current() == nullptr);
  CHECK_EQ(server.requests.load(), 0);

  // A cancel arriving while the query waits for the server aborts it with HY008.
  std::atomic<bool> flag{false};
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    flag.store(true);
  });
  const auto t0 = std::chrono::steady_clock::now();
  {
    MetadataCancelScope scope(&flag);
    CHECK_EQ(sqlstateOf([&] { catalogColumns(conn, args); }), std::string("HY008"));
  }
  canceller.join();
  CHECK(secondsSince(t0) < 5.0);
  conn.client->disconnect();
}
#endif  // TRINO_ODBC_UNIX
