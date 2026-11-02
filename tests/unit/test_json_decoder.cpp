// Unit tests for result/json_decoder.h: statement responses as Trino really sends them, typed cell
// decoding for every storage kind, the spooled `data` object, errors, warnings and the OAuth2
// token-server answer. No network, no server.
#include "unit/test_framework.h"

#include "core/config.h"
#include "result/json_decoder.h"
#include "result/page.h"
#include "result/row_source.h"
#include "util/errors.h"
#include "util/strings.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace trino_odbc;

namespace {

const TypeMappingOptions& opts() {
  static const TypeMappingOptions o;
  return o;
}

/// A JSON document in a buffer that carries the padding simdjson needs.
std::string padded(const std::string& json) {
  std::string s = json;
  ensureJsonPadding(s);
  return s;
}

StatementResponse parse(const std::string& json, const std::vector<ResultColumn>* known = nullptr) {
  std::string buf = padded(json);
  return parseStatementResponse(buf, known, opts());
}

std::vector<ResultColumn> makeColumns(std::initializer_list<std::pair<const char*, const char*>> defs) {
  std::vector<ResultColumn> out;
  for (const auto& d : defs) out.push_back(ResultColumn::make(d.first, d.second, opts()));
  return out;
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

const char* kQueued =
    R"J({"id":"20240101_000000_00001_abcde","infoUri":"http://h:8080/ui/query.html?20240101_000000_00001_abcde",)J"
    R"J("nextUri":"http://h:8080/v1/statement/queued/20240101_000000_00001_abcde/y/1",)J"
    R"J("stats":{"state":"QUEUED","queued":true,"scheduled":false,"nodes":0,"totalSplits":0,"queuedSplits":0,)J"
    R"J("runningSplits":0,"completedSplits":0,"cpuTimeMillis":0,"wallTimeMillis":0,"queuedTimeMillis":0,)J"
    R"J("elapsedTimeMillis":3,"processedRows":0,"processedBytes":0,"physicalInputBytes":0,"peakMemoryBytes":0,)J"
    R"J("spilledBytes":0},"warnings":[]})J";

}  // namespace

TEST_CASE("json_decoder: queued response carries no columns and no data") {
  StatementResponse r = parse(kQueued);
  CHECK_EQ(r.id, std::string("20240101_000000_00001_abcde"));
  CHECK(startsWith(r.infoUri, "http://h:8080/ui/query.html"));
  CHECK_EQ(r.nextUri, std::string("http://h:8080/v1/statement/queued/20240101_000000_00001_abcde/y/1"));
  CHECK(!r.columnsPresent);
  CHECK(r.columns.empty());
  CHECK(r.page == nullptr);
  CHECK(!r.dataIsSpooled);
  CHECK(r.statsPresent);
  CHECK_EQ(r.stats.state, std::string("QUEUED"));
  CHECK(r.stats.queued);
  CHECK(!r.stats.scheduled);
  CHECK_EQ(r.stats.elapsedTimeMillis, int64_t(3));
  CHECK(!r.error.has_value());
  CHECK(r.warnings.empty());
  CHECK(r.updateType.empty());
  CHECK(!r.updateCount.has_value());
}

TEST_CASE("json_decoder: running response decodes every storage kind") {
  const std::string bin = base64Encode(std::string("\x00\x01\xff\x7f", 4));
  const std::string json =
      R"J({"id":"q1","infoUri":"http://h/ui","nextUri":"http://h/v1/statement/executing/q1/x/2",)J"
      R"J("columns":[)J"
      R"J({"name":"b","type":"boolean","typeSignature":{"rawType":"boolean","arguments":[]}},)J"
      R"J({"name":"i","type":"integer","typeSignature":{"rawType":"integer","arguments":[]}},)J"
      R"J({"name":"l","type":"bigint","typeSignature":{"rawType":"bigint","arguments":[]}},)J"
      R"J({"name":"d","type":"double","typeSignature":{"rawType":"double","arguments":[]}},)J"
      R"J({"name":"r","type":"real","typeSignature":{"rawType":"real","arguments":[]}},)J"
      R"J({"name":"v","type":"varchar(10)","typeSignature":{"rawType":"varchar","arguments":[{"kind":"LONG","value":10}]}},)J"
      R"J({"name":"dec","type":"decimal(10,2)"},)J"
      R"J({"name":"ts","type":"timestamp(3) with time zone"},)J"
      R"J({"name":"dt","type":"date"},)J"
      R"J({"name":"bin","type":"varbinary"},)J"
      R"J({"name":"arr","type":"array(integer)"},)J"
      R"J({"name":"m","type":"map(varchar, integer)"},)J"
      R"J({"name":"row","type":"row(x integer, y varchar)"},)J"
      R"J({"name":"u","type":"uuid"},)J"
      R"J({"name":"j","type":"json"}],)J"
      R"J("data":[)J"
      R"J([true,42,9007199254740993,1.5,2.25,"hello","12.34","2024-01-02 03:04:05.678 America/New_York","2024-01-02",")J" +
      bin +
      R"J(",[1,2,3],{"a":1,"b":2},[7,"z"],"12345678-1234-1234-1234-123456789abc","{\"k\":[1,2]}"],)J"
      R"J([false,-1,-9223372036854775808,-0.5,0.0,"","0.00","1970-01-01 00:00:00.000 UTC","1970-01-01","",[],{},[null,null],"00000000-0000-0000-0000-000000000000","null"],)J"
      R"J([null,null,null,null,null,null,null,null,null,null,null,null,null,null,null]],)J"
      R"J("stats":{"state":"RUNNING","queued":false,"scheduled":true,"completedSplits":3,"totalSplits":10,)J"
      R"J("processedRows":1234,"processedBytes":5678,"elapsedTimeMillis":250,"progressPercentage":30.5},)J"
      R"J("warnings":[]})J";
  StatementResponse r = parse(json);
  CHECK(r.columnsPresent);
  REQUIRE_EQ(r.columns.size(), size_t(15));
  CHECK_EQ(r.columns[0].name, std::string("b"));
  CHECK(r.columns[0].odbc.storage == StorageKind::Bool);
  CHECK(r.columns[1].odbc.storage == StorageKind::Int64);
  CHECK(r.columns[2].odbc.storage == StorageKind::Int64);
  CHECK(r.columns[3].odbc.storage == StorageKind::Double);
  CHECK(r.columns[4].odbc.storage == StorageKind::Double);
  CHECK(r.columns[5].odbc.storage == StorageKind::String);
  CHECK(r.columns[9].odbc.storage == StorageKind::Binary);
  CHECK(r.columns[5].type.category == TypeCategory::Varchar);
  CHECK(r.columns[7].type.category == TypeCategory::TimestampWithTimeZone);
  CHECK(r.columns[10].type.category == TypeCategory::Array);
  CHECK(r.columns[12].type.category == TypeCategory::Row);

  REQUIRE(r.page != nullptr);
  REQUIRE_EQ(r.page->rowCount, size_t(3));
  REQUIRE_EQ(r.page->columns.size(), size_t(15));
  const Page& p = *r.page;
  CHECK(p.columns[0].getBool(0));
  CHECK(!p.columns[0].getBool(1));
  CHECK_EQ(p.columns[1].getInt64(0), int64_t(42));
  CHECK_EQ(p.columns[1].getInt64(1), int64_t(-1));
  CHECK_EQ(p.columns[2].getInt64(0), int64_t(9007199254740993LL));
  CHECK_EQ(p.columns[2].getInt64(1), std::numeric_limits<int64_t>::min());
  CHECK_EQ(p.columns[3].getDouble(0), 1.5);
  CHECK_EQ(p.columns[3].getDouble(1), -0.5);
  CHECK_EQ(p.columns[4].getDouble(0), 2.25);
  CHECK_EQ(std::string(p.columns[5].getBytes(0)), std::string("hello"));
  CHECK_EQ(std::string(p.columns[5].getBytes(1)), std::string(""));
  CHECK(!p.columns[5].isNull(1));  // empty string is not NULL
  CHECK_EQ(std::string(p.columns[7].getBytes(0)), std::string("2024-01-02 03:04:05.678 America/New_York"));
  CHECK_EQ(std::string(p.columns[8].getBytes(0)), std::string("2024-01-02"));
  CHECK_EQ(std::string(p.columns[9].getBytes(0)), std::string("\x00\x01\xff\x7f", 4));
  CHECK_EQ(p.columns[9].getBytes(1).size(), size_t(0));
  CHECK_EQ(std::string(p.columns[10].getBytes(0)), std::string("[1,2,3]"));
  CHECK_EQ(std::string(p.columns[10].getBytes(1)), std::string("[]"));
  CHECK_EQ(std::string(p.columns[11].getBytes(0)), std::string(R"J({"a":1,"b":2})J"));
  CHECK_EQ(std::string(p.columns[11].getBytes(1)), std::string("{}"));
  CHECK_EQ(std::string(p.columns[12].getBytes(0)), std::string(R"J([7,"z"])J"));
  CHECK_EQ(std::string(p.columns[12].getBytes(1)), std::string("[null,null]"));
  CHECK_EQ(std::string(p.columns[13].getBytes(0)), std::string("12345678-1234-1234-1234-123456789abc"));
  CHECK_EQ(std::string(p.columns[14].getBytes(0)), std::string(R"J({"k":[1,2]})J"));
  CHECK_EQ(std::string(p.columns[14].getBytes(1)), std::string("null"));  // JSON string "null", not SQL NULL
  for (size_t c = 0; c < 15; ++c) {
    CHECK(!p.columns[c].isNull(0));
    CHECK(p.columns[c].isNull(2));
    CHECK_EQ(p.columns[c].size(), size_t(3));
  }
  const auto rows = pageToStrings(p);
  REQUIRE_EQ(rows.size(), size_t(3));
  CHECK_EQ(*rows[0][6], std::string("12.34"));
  CHECK_EQ(*rows[0][1], std::string("42"));
  CHECK(!rows[2][0].has_value());

  CHECK(r.statsPresent);
  CHECK_EQ(r.stats.state, std::string("RUNNING"));
  CHECK(r.stats.scheduled);
  CHECK_EQ(r.stats.completedSplits, int64_t(3));
  CHECK_EQ(r.stats.totalSplits, int64_t(10));
  CHECK_EQ(r.stats.processedRows, int64_t(1234));
  CHECK_EQ(r.stats.processedBytes, int64_t(5678));
  CHECK_EQ(r.stats.elapsedTimeMillis, int64_t(250));
  CHECK_EQ(r.stats.progressPercentage, 30.5);
  CHECK_EQ(r.nextUri, std::string("http://h/v1/statement/executing/q1/x/2"));
}

TEST_CASE("json_decoder: known columns type the data when the response omits them") {
  const std::vector<ResultColumn> known = makeColumns({{"id", "bigint"}, {"name", "varchar"}});
  StatementResponse r = parse(R"J({"id":"q","data":[[1,"a"],[2,"b"]],"stats":{"state":"FINISHED"}})J", &known);
  CHECK(!r.columnsPresent);
  CHECK(r.columns.empty());
  REQUIRE(r.page != nullptr);
  CHECK_EQ(r.page->rowCount, size_t(2));
  CHECK_EQ(r.page->columns[0].getInt64(1), int64_t(2));
  CHECK_EQ(std::string(r.page->columns[1].getBytes(0)), std::string("a"));
  CHECK(r.nextUri.empty());
  CHECK_EQ(r.stats.state, std::string("FINISHED"));
}

TEST_CASE("json_decoder: response columns win over known columns") {
  const std::vector<ResultColumn> known = makeColumns({{"x", "varchar"}});
  StatementResponse r =
      parse(R"J({"id":"q","columns":[{"name":"n","type":"integer"}],"data":[[5]]})J", &known);
  CHECK(r.columnsPresent);
  REQUIRE_EQ(r.columns.size(), size_t(1));
  CHECK_EQ(r.columns[0].name, std::string("n"));
  REQUIRE(r.page != nullptr);
  CHECK(r.page->columns[0].kind() == StorageKind::Int64);
  CHECK_EQ(r.page->columns[0].getInt64(0), int64_t(5));
}

TEST_CASE("json_decoder: data without any column metadata is rejected") {
  CHECK_EQ(sqlstateOf([] { parse(R"J({"id":"q","data":[[1]]})J"); }), std::string("HY000"));
}

TEST_CASE("json_decoder: data placed before columns in the document still decodes") {
  StatementResponse r = parse(R"J({"id":"q","data":[[1,"a"],[2,"b"]],"columns":[{"name":"i","type":"bigint"},{"name":"s","type":"varchar"}],"nextUri":"http://h/next"})J");
  CHECK(r.columnsPresent);
  REQUIRE(r.page != nullptr);
  CHECK_EQ(r.page->rowCount, size_t(2));
  CHECK_EQ(r.page->columns[0].getInt64(0), int64_t(1));
  CHECK_EQ(std::string(r.page->columns[1].getBytes(1)), std::string("b"));
  CHECK_EQ(r.nextUri, std::string("http://h/next"));
}

TEST_CASE("json_decoder: empty and null data yield no page") {
  StatementResponse a = parse(R"J({"id":"q","columns":[{"name":"i","type":"bigint"}],"data":[]})J");
  CHECK(a.columnsPresent);
  CHECK(a.page == nullptr);
  StatementResponse b = parse(R"J({"id":"q","columns":[{"name":"i","type":"bigint"}],"data":null})J");
  CHECK(b.page == nullptr);
  StatementResponse c = parse(R"J({"id":"q","columns":null})J");
  CHECK(!c.columnsPresent);
  CHECK(c.page == nullptr);
}

TEST_CASE("json_decoder: error object maps every field") {
  const std::string json =
      R"J({"id":"q9","infoUri":"http://h/ui","error":{"message":"line 1:8: Column 'x' cannot be resolved",)J"
      R"J("errorCode":47,"errorName":"COLUMN_NOT_FOUND","errorType":"USER_ERROR",)J"
      R"J("errorLocation":{"lineNumber":1,"columnNumber":8},)J"
      R"J("failureInfo":{"type":"io.trino.spi.TrinoException","message":"line 1:8: Column 'x' cannot be resolved",)J"
      R"J("suppressed":[],"stack":["a","b"]}},"stats":{"state":"FAILED"},"warnings":[]})J";
  StatementResponse r = parse(json);
  REQUIRE(r.error.has_value());
  CHECK_EQ(r.error->message, std::string("line 1:8: Column 'x' cannot be resolved"));
  CHECK_EQ(r.error->errorCode, 47);
  CHECK_EQ(r.error->errorName, std::string("COLUMN_NOT_FOUND"));
  CHECK_EQ(r.error->errorType, std::string("USER_ERROR"));
  CHECK(r.error->sqlState.empty());
  CHECK_EQ(r.error->line, 1);
  CHECK_EQ(r.error->column, 8);
  CHECK_EQ(r.error->failureType, std::string("io.trino.spi.TrinoException"));
  CHECK_EQ(r.error->failureMessage, std::string("line 1:8: Column 'x' cannot be resolved"));
  CHECK(r.nextUri.empty());
  CHECK_EQ(r.stats.state, std::string("FAILED"));

  // Trino-supplied sqlState and a message that only lives in failureInfo.
  StatementResponse s = parse(
      R"J({"id":"q","error":{"errorCode":1,"errorName":"DIVISION_BY_ZERO","errorType":"USER_ERROR","sqlState":"22012",)J"
      R"J("failureInfo":{"type":"ArithmeticException","message":"Division by zero"}}})J");
  REQUIRE(s.error.has_value());
  CHECK_EQ(s.error->sqlState, std::string("22012"));
  CHECK_EQ(s.error->message, std::string("Division by zero"));
  CHECK_EQ(s.error->line, 0);
  StatementResponse n = parse(R"J({"id":"q","error":null})J");
  CHECK(!n.error.has_value());
}

TEST_CASE("json_decoder: warnings are collected") {
  StatementResponse r = parse(
      R"J({"id":"q","warnings":[{"warningCode":{"code":1,"name":"PARSER_WARNING"},"message":"first"},)J"
      R"J({"warningCode":{"code":2,"name":"OTHER"},"message":"second"}],"stats":{"state":"RUNNING"}})J");
  REQUIRE_EQ(r.warnings.size(), size_t(2));
  CHECK_EQ(r.warnings[0].code, 1);
  CHECK_EQ(r.warnings[0].name, std::string("PARSER_WARNING"));
  CHECK_EQ(r.warnings[0].message, std::string("first"));
  CHECK_EQ(r.warnings[1].code, 2);
  CHECK_EQ(r.warnings[1].message, std::string("second"));
  StatementResponse none = parse(R"J({"id":"q","warnings":null})J");
  CHECK(none.warnings.empty());
}

TEST_CASE("json_decoder: DML response carries updateType and updateCount") {
  StatementResponse r = parse(
      R"J({"id":"q","columns":[{"name":"rows","type":"bigint"}],"data":[[5]],"stats":{"state":"FINISHED"},)J"
      R"J("updateType":"INSERT","updateCount":5})J");
  CHECK_EQ(r.updateType, std::string("INSERT"));
  REQUIRE(r.updateCount.has_value());
  CHECK_EQ(*r.updateCount, int64_t(5));
  REQUIRE(r.page != nullptr);
  CHECK_EQ(r.page->columns[0].getInt64(0), int64_t(5));

  StatementResponse ddl = parse(R"J({"id":"q","columns":[{"name":"result","type":"boolean"}],"data":[[true]],"updateType":"CREATE TABLE"})J");
  CHECK_EQ(ddl.updateType, std::string("CREATE TABLE"));
  CHECK(!ddl.updateCount.has_value());
  StatementResponse nul = parse(R"J({"id":"q","updateType":null,"updateCount":null})J");
  CHECK(nul.updateType.empty());
  CHECK(!nul.updateCount.has_value());
}

TEST_CASE("json_decoder: spooled data object with inline and spooled segments") {
  const std::string rowsJson = R"J([[1,"a"],[2,"b"]])J";
  const std::string json =
      R"J({"id":"q","nextUri":"http://h/next","columns":[{"name":"i","type":"bigint"},{"name":"s","type":"varchar"}],)J"
      R"J("data":{"encoding":"json+zstd","metadata":{"rowsCount":5},"segments":[)J"
      R"J({"type":"inline","data":")J" + base64Encode(rowsJson) + R"J(","metadata":{"segmentSize":17,"rowOffset":0,"rowsCount":2,"uncompressedSize":17}},)J"
      R"J({"type":"spooled","uri":"http://spool/seg/1","ackUri":"http://spool/ack/1",)J"
      R"J("headers":{"x-trino-mock-secret":["s3cr3t"],"X-Multi":["a","b"],"Single":"v"},)J"
      R"J("metadata":{"segmentSize":99,"rowOffset":2,"rowsCount":3,"uncompressedSize":300}}]},)J"
      R"J("stats":{"state":"RUNNING"}})J";
  StatementResponse r = parse(json);
  CHECK(r.columnsPresent);
  CHECK(r.page == nullptr);
  CHECK(r.dataIsSpooled);
  CHECK_EQ(r.spooled.encoding, std::string("json+zstd"));
  REQUIRE_EQ(r.spooled.segments.size(), size_t(2));
  const SpooledSegment& a = r.spooled.segments[0];
  CHECK(a.type == SpooledSegment::Type::Inline);
  CHECK_EQ(a.inlineData, rowsJson);
  CHECK_EQ(a.segmentSize, int64_t(17));
  CHECK_EQ(a.rowOffset, int64_t(0));
  CHECK_EQ(a.rowsCount, int64_t(2));
  CHECK_EQ(a.uncompressedSize, int64_t(17));
  const SpooledSegment& b = r.spooled.segments[1];
  CHECK(b.type == SpooledSegment::Type::Spooled);
  CHECK_EQ(b.uri, std::string("http://spool/seg/1"));
  CHECK_EQ(b.ackUri, std::string("http://spool/ack/1"));
  REQUIRE_EQ(b.headers.size(), size_t(4));
  CHECK_EQ(b.headers[0].first, std::string("x-trino-mock-secret"));
  CHECK_EQ(b.headers[0].second, std::string("s3cr3t"));
  CHECK_EQ(b.headers[1].second, std::string("a"));
  CHECK_EQ(b.headers[2].second, std::string("b"));
  CHECK_EQ(b.headers[3].first, std::string("Single"));
  CHECK_EQ(b.headers[3].second, std::string("v"));
  CHECK_EQ(b.rowOffset, int64_t(2));
  CHECK_EQ(b.rowsCount, int64_t(3));
  CHECK_EQ(b.uncompressedSize, int64_t(300));
  CHECK_EQ(r.nextUri, std::string("http://h/next"));

  // The inline segment decodes with decodeRowsJson exactly like a classic `data` array.
  std::string seg = r.spooled.segments[0].inlineData;
  std::unique_ptr<Page> page = decodeRowsJson(seg, r.columns);
  REQUIRE(page != nullptr);
  CHECK_EQ(page->rowCount, size_t(2));
  CHECK_EQ(page->columns[0].getInt64(1), int64_t(2));

  // Encoding defaults to json when absent.
  StatementResponse d = parse(R"J({"id":"q","columns":[{"name":"i","type":"bigint"}],"data":{"segments":[]}})J");
  CHECK(d.dataIsSpooled);
  CHECK_EQ(d.spooled.encoding, std::string("json"));
  CHECK(d.spooled.segments.empty());
}

TEST_CASE("json_decoder: malformed spooled segments are rejected") {
  CHECK_EQ(sqlstateOf([] {
             parse(R"J({"id":"q","columns":[{"name":"i","type":"bigint"}],"data":{"encoding":"json","segments":[{"type":"weird"}]}})J");
           }),
           std::string("HY000"));
  CHECK_EQ(sqlstateOf([] {
             parse(R"J({"id":"q","columns":[{"name":"i","type":"bigint"}],"data":{"encoding":"json","segments":[{"type":"inline","data":"!!not base64!!","metadata":{}}]}})J");
           }),
           std::string("HY000"));
  CHECK_EQ(sqlstateOf([] {
             parse(R"J({"id":"q","columns":[{"name":"i","type":"bigint"}],"data":{"encoding":"json","segments":[{"type":"spooled","ackUri":"http://a"}]}})J");
           }),
           std::string("HY000"));
}

TEST_CASE("json_decoder: malformed documents raise HY000") {
  CHECK_EQ(sqlstateOf([] { parse(R"J({"id":"q",)J"); }), std::string("HY000"));
  CHECK_EQ(sqlstateOf([] { parse("[1,2,3]"); }), std::string("HY000"));
  CHECK_EQ(sqlstateOf([] { parse(""); }), std::string("HY000"));
  CHECK_EQ(sqlstateOf([] { parse("<html>Service Unavailable</html>"); }), std::string("HY000"));
  CHECK_EQ(sqlstateOf([] { parse(R"J({"id":"q","columns":"nope","data":[[1]]})J"); }), std::string("HY000"));
  // Wrong cell types for the declared column.
  CHECK_EQ(sqlstateOf([] { parse(R"J({"id":"q","columns":[{"name":"i","type":"bigint"}],"data":[["abc"]]})J"); }),
           std::string("HY000"));
  CHECK_EQ(sqlstateOf([] { parse(R"J({"id":"q","columns":[{"name":"b","type":"varbinary"}],"data":[[12]]})J"); }),
           std::string("HY000"));
  CHECK_EQ(sqlstateOf([] { parse(R"J({"id":"q","columns":[{"name":"b","type":"varbinary"}],"data":[["%%%"]]})J"); }),
           std::string("HY000"));
}

TEST_CASE("json_decoder: numeric strings and non-finite doubles") {
  StatementResponse r = parse(
      R"J({"id":"q","columns":[{"name":"l","type":"bigint"},{"name":"d","type":"double"},{"name":"r","type":"real"}],)J"
      R"J("data":[["123","NaN","Infinity"],[456,"-Infinity",1e300],[-7,2.5,-0.0]]})J");
  REQUIRE(r.page != nullptr);
  const Page& p = *r.page;
  CHECK_EQ(p.columns[0].getInt64(0), int64_t(123));
  CHECK_EQ(p.columns[0].getInt64(1), int64_t(456));
  CHECK_EQ(p.columns[0].getInt64(2), int64_t(-7));
  CHECK(std::isnan(p.columns[1].getDouble(0)));
  CHECK(std::isinf(p.columns[1].getDouble(1)) && p.columns[1].getDouble(1) < 0);
  CHECK_EQ(p.columns[1].getDouble(2), 2.5);
  CHECK(std::isinf(p.columns[2].getDouble(0)) && p.columns[2].getDouble(0) > 0);
  CHECK_EQ(p.columns[2].getDouble(1), 1e300);
  CHECK_EQ(p.columns[2].getDouble(2), 0.0);
}

TEST_CASE("json_decoder: decodeRowsJson validates the row shape") {
  const std::vector<ResultColumn> cols = makeColumns({{"i", "bigint"}, {"s", "varchar"}});
  std::string ok = padded(R"J([[1,"a"],[2,null]])J");
  std::unique_ptr<Page> page = decodeRowsJson(ok, cols);
  REQUIRE(page != nullptr);
  CHECK_EQ(page->rowCount, size_t(2));
  CHECK(page->columns[1].isNull(1));

  std::string tooMany = padded(R"J([[1,"a",3]])J");
  CHECK_EQ(sqlstateOf([&] { decodeRowsJson(tooMany, cols); }), std::string("HY000"));
  std::string tooFew = padded(R"J([[1]])J");
  CHECK_EQ(sqlstateOf([&] { decodeRowsJson(tooFew, cols); }), std::string("HY000"));
  std::string notRows = padded(R"J([1,2])J");
  CHECK_EQ(sqlstateOf([&] { decodeRowsJson(notRows, cols); }), std::string("HY000"));
  std::string notArray = padded(R"J({"a":1})J");
  CHECK_EQ(sqlstateOf([&] { decodeRowsJson(notArray, cols); }), std::string("HY000"));
  std::string truncated = padded(R"J([[1,"a"],[2,)J");
  CHECK_EQ(sqlstateOf([&] { decodeRowsJson(truncated, cols); }), std::string("HY000"));
  std::string empty = padded("[]");
  std::unique_ptr<Page> none = decodeRowsJson(empty, cols);
  REQUIRE(none != nullptr);
  CHECK_EQ(none->rowCount, size_t(0));
  CHECK_EQ(none->columns.size(), size_t(2));
  // An unpadded buffer is padded on the way in rather than rejected.
  std::string unpadded = R"J([[3,"c"]])J";
  unpadded.shrink_to_fit();
  std::unique_ptr<Page> p2 = decodeRowsJson(unpadded, cols);
  REQUIRE(p2 != nullptr);
  CHECK_EQ(p2->columns[0].getInt64(0), int64_t(3));
}

TEST_CASE("json_decoder: many rows decode with the right count and values") {
  std::string json = R"J({"id":"q","columns":[{"name":"i","type":"bigint"},{"name":"s","type":"varchar"},{"name":"d","type":"double"}],"data":[)J";
  for (int i = 0; i < 2000; ++i) {
    if (i) json += ',';
    json += "[" + std::to_string(i) + ",\"row" + std::to_string(i) + "\"," + std::to_string(i) + ".5]";
  }
  json += R"J(],"stats":{"state":"RUNNING"}})J";
  StatementResponse r = parse(json);
  REQUIRE(r.page != nullptr);
  REQUIRE_EQ(r.page->rowCount, size_t(2000));
  int64_t sum = 0;
  for (size_t i = 0; i < 2000; ++i) sum += r.page->columns[0].getInt64(i);
  CHECK_EQ(sum, int64_t(2000 * 1999 / 2));
  CHECK_EQ(std::string(r.page->columns[1].getBytes(1999)), std::string("row1999"));
  CHECK_EQ(r.page->columns[2].getDouble(7), 7.5);
  CHECK(r.page->byteSize() > 0);
}

TEST_CASE("json_decoder: unicode escapes decode to UTF-8 and raw JSON keeps its text") {
  StatementResponse r = parse(
      R"J({"id":"q","columns":[{"name":"s","type":"varchar"},{"name":"a","type":"array(varchar)"}],)J"
      "\"data\":[[\"caf\\u00e9 \\ud83d\\ude00 tab\\tnl\\n\\\"q\\\"\",[\"x\\u00e9\", 1 ]]]}");
  REQUIRE(r.page != nullptr);
  CHECK_EQ(std::string(r.page->columns[0].getBytes(0)), std::string("caf\xC3\xA9 \xF0\x9F\x98\x80 tab\tnl\n\"q\""));
  // Complex values are the raw document text (escapes untouched, inner whitespace preserved).
  CHECK_EQ(std::string(r.page->columns[1].getBytes(0)), std::string(R"J(["x\u00e9", 1 ])J"));
}

TEST_CASE("json_decoder: unknown members and nested stats are skipped") {
  StatementResponse r = parse(
      R"J({"id":"q","futureField":{"deep":[1,2,{"x":null}]},"stats":{"state":"RUNNING","rootStage":{"stageId":"0",)J"
      R"J("subStages":[{"stageId":"1"}]},"progressPercentage":12.5,"peakMemoryBytes":4096},)J"
      R"J("columns":[{"name":"i","type":"integer","extra":true}],"data":[[1]],"nextUri":"http://h/n"})J");
  CHECK_EQ(r.stats.state, std::string("RUNNING"));
  CHECK_EQ(r.stats.progressPercentage, 12.5);
  CHECK_EQ(r.stats.peakMemoryBytes, int64_t(4096));
  REQUIRE(r.page != nullptr);
  CHECK_EQ(r.page->columns[0].getInt64(0), int64_t(1));
  CHECK_EQ(r.nextUri, std::string("http://h/n"));
}

TEST_CASE("json_decoder: textual types keep the exact server text") {
  StatementResponse r = parse(
      R"J({"id":"q","columns":[{"name":"dec","type":"decimal(38,10)"},{"name":"t","type":"time(6)"},)J"
      R"J({"name":"ts","type":"timestamp(9)"},{"name":"iv","type":"interval day to second"},{"name":"ip","type":"ipaddress"},)J"
      R"J({"name":"hll","type":"HyperLogLog"},{"name":"ch","type":"char(3)"}],)J"
      R"J("data":[["-1234567890123456789012345678.1234567890","23:59:59.123456","2024-02-29 12:00:00.123456789","1 02:03:04.005","10.0.0.1","AQID","ab "]]})J");
  REQUIRE(r.page != nullptr);
  REQUIRE_EQ(r.columns.size(), size_t(7));
  const Page& p = *r.page;
  if (r.columns[0].odbc.storage == StorageKind::String)
    CHECK_EQ(std::string(p.columns[0].getBytes(0)), std::string("-1234567890123456789012345678.1234567890"));
  CHECK_EQ(std::string(p.columns[1].getBytes(0)), std::string("23:59:59.123456"));
  CHECK_EQ(std::string(p.columns[2].getBytes(0)), std::string("2024-02-29 12:00:00.123456789"));
  CHECK_EQ(std::string(p.columns[3].getBytes(0)), std::string("1 02:03:04.005"));
  CHECK_EQ(std::string(p.columns[4].getBytes(0)), std::string("10.0.0.1"));
  CHECK_EQ(std::string(p.columns[5].getBytes(0)), std::string("AQID"));
  CHECK_EQ(std::string(p.columns[6].getBytes(0)), std::string("ab "));
  CHECK(r.columns[5].type.category == TypeCategory::Unknown);
}

TEST_CASE("json_decoder: partialCancelUri and final response without nextUri") {
  StatementResponse r = parse(
      R"J({"id":"q","infoUri":"http://h/ui","partialCancelUri":"http://h/v1/statement/executing/partialCancel/q/1/0",)J"
      R"J("nextUri":"http://h/v1/statement/executing/q/1/1","columns":[{"name":"i","type":"bigint"}],"data":[[1]],"stats":{"state":"RUNNING"}})J");
  CHECK_EQ(r.partialCancelUri, std::string("http://h/v1/statement/executing/partialCancel/q/1/0"));
  StatementResponse last = parse(R"J({"id":"q","infoUri":"http://h/ui","stats":{"state":"FINISHED","completedSplits":10,"totalSplits":10}})J");
  CHECK(last.nextUri.empty());
  CHECK(last.partialCancelUri.empty());
  CHECK(last.page == nullptr);
  CHECK(!last.columnsPresent);
  CHECK_EQ(last.stats.state, std::string("FINISHED"));
}

TEST_CASE("json_decoder: parseOAuthTokenResponse") {
  std::string token, next, error;
  std::string a = padded(R"J({"token":"eyJhbGciOi.abc.def"})J");
  CHECK(parseOAuthTokenResponse(a, &token, &next, &error));
  CHECK_EQ(token, std::string("eyJhbGciOi.abc.def"));
  CHECK(next.empty());
  CHECK(error.empty());

  token.clear();
  std::string b = padded(R"J({"nextUri":"https://h/oauth2/token/poll/xyz"})J");
  CHECK(parseOAuthTokenResponse(b, &token, &next, &error));
  CHECK(token.empty());
  CHECK_EQ(next, std::string("https://h/oauth2/token/poll/xyz"));

  next.clear();
  std::string c = padded(R"J({"error":"access_denied","extra":1})J");
  CHECK(parseOAuthTokenResponse(c, &token, &next, &error));
  CHECK_EQ(error, std::string("access_denied"));

  std::string d = padded("[1]");
  CHECK(!parseOAuthTokenResponse(d, &token, &next, &error));
  std::string e = padded("not json");
  CHECK(!parseOAuthTokenResponse(e, &token, &next, &error));
  std::string f = padded(R"J({"token":"t"})J");
  CHECK(parseOAuthTokenResponse(f, nullptr, nullptr, nullptr));  // null outputs are allowed
}

TEST_CASE("json_decoder: ensureJsonPadding leaves spare capacity") {
  std::string s = "{}";
  s.shrink_to_fit();
  ensureJsonPadding(s);
  CHECK(s.capacity() >= s.size() + kJsonPaddingBytes);
  CHECK_EQ(s, std::string("{}"));
  const size_t before = s.capacity();
  ensureJsonPadding(s);
  CHECK_EQ(s.capacity(), before);  // no-op when already padded
  std::string big(100000, 'x');
  ensureJsonPadding(big);
  CHECK(big.capacity() >= big.size() + kJsonPaddingBytes);
}
