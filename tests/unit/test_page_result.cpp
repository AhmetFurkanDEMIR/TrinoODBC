// Unit tests for result/page.h, result/row_source.h and result/result_set.h.
#include "unit/test_framework.h"

#include "core/config.h"
#include "result/page.h"
#include "result/result_set.h"
#include "result/row_source.h"
#include "util/errors.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace trino_odbc;

namespace {

const TypeMappingOptions& defaultOpts() {
  static const TypeMappingOptions opts;
  return opts;
}

/// Columns used by the cursor tests: (id bigint, name varchar).
std::vector<ResultColumn> idNameColumns() {
  return {ResultColumn::make("id", "bigint", defaultOpts()), ResultColumn::make("name", "varchar", defaultOpts())};
}

/// Page with rows id = from .. to-1 and name = "r<id>".
std::unique_ptr<Page> idPage(int64_t from, int64_t to) {
  PageBuilder b({StorageKind::Int64, StorageKind::String});
  for (int64_t i = from; i < to; ++i) b.addRow({i, std::string("r") + std::to_string(i)});
  return b.finish();
}

/// Pages of the given sizes with consecutive ids starting at 0.
std::vector<std::unique_ptr<Page>> idPages(std::initializer_list<int64_t> sizes) {
  std::vector<std::unique_ptr<Page>> pages;
  int64_t next = 0;
  for (int64_t n : sizes) {
    pages.push_back(idPage(next, next + n));
    next += n;
  }
  return pages;
}

int64_t idAt(const ResultSet& rs, size_t i) {
  RowRef ref = rs.rowsetRow(i);
  REQUIRE(ref.valid());
  return ref.page->columns[0].getInt64(ref.row);
}

/// Call counters of a ScriptedSource, shared so a test can inspect them after the ResultSet that
/// owns the source has been destroyed.
struct SourceStats {
  int cancelCalls = 0;
  int closeCalls = 0;
  int nextPageCalls = 0;
};

/// RowSource that records cancel()/close() and can fail on a chosen page (simulates a server error).
class ScriptedSource : public RowSource {
 public:
  ScriptedSource(std::vector<ResultColumn> columns, std::vector<std::unique_ptr<Page>> pages)
      : stats(std::make_shared<SourceStats>()), columns_(std::move(columns)), pages_(std::move(pages)) {}

  const std::vector<ResultColumn>& columns() const override { return columns_; }
  std::unique_ptr<Page> nextPage() override {
    ++stats->nextPageCalls;
    if (closed_) return nullptr;
    if (next_ == failAtPage) throw TrinoQueryError("22012", "Division by zero", 65545, "DIVISION_BY_ZERO", "USER_ERROR", "q1");
    if (next_ >= pages_.size()) return nullptr;
    return std::move(pages_[next_++]);
  }
  void cancel() override { ++stats->cancelCalls; }
  void close() override {
    ++stats->closeCalls;
    closed_ = true;
  }
  std::optional<int64_t> updateCount() const override { return updateCount_; }
  const std::vector<QueryWarning>& warnings() const override { return warnings_; }

  void setUpdateCount(int64_t n) { updateCount_ = n; }
  void setWarnings(std::vector<QueryWarning> w) { warnings_ = std::move(w); }

  size_t failAtPage = static_cast<size_t>(-1);
  std::shared_ptr<SourceStats> stats;

 private:
  std::vector<ResultColumn> columns_;
  std::vector<std::unique_ptr<Page>> pages_;
  size_t next_ = 0;
  bool closed_ = false;
  std::optional<int64_t> updateCount_;
  std::vector<QueryWarning> warnings_;
};

}  // namespace

// ---- Column -----------------------------------------------------------------------------------

TEST_CASE("column: typed appends keep null bitmap and values aligned") {
  Column ints(StorageKind::Int64);
  ints.reserve(4, 0);
  ints.appendInt64(7);
  ints.appendNull();
  ints.appendInt64(-9);
  CHECK_EQ(ints.size(), size_t{3});
  CHECK(!ints.isNull(0));
  CHECK(ints.isNull(1));
  CHECK(!ints.isNull(2));
  CHECK_EQ(ints.getInt64(0), int64_t{7});
  CHECK_EQ(ints.getInt64(2), int64_t{-9});
  CHECK(ints.kind() == StorageKind::Int64);

  Column bools(StorageKind::Bool);
  bools.appendBool(true);
  bools.appendNull();
  bools.appendBool(false);
  CHECK(bools.getBool(0));
  CHECK(bools.isNull(1));
  CHECK(!bools.getBool(2));

  Column doubles(StorageKind::Double);
  doubles.appendDouble(1.5);
  doubles.appendNull();
  CHECK_EQ(doubles.getDouble(0), 1.5);
  CHECK(doubles.isNull(1));

  Column text(StorageKind::String);
  text.reserve(3, 16);
  text.appendBytes("abc");
  text.appendNull();
  text.appendBytes("");
  text.appendBytes("d\0e");  // embedded NUL from a literal: only "d" (string_view from const char*)
  CHECK_EQ(text.size(), size_t{4});
  CHECK(text.getBytes(0) == "abc");
  CHECK(text.isNull(1));
  CHECK(text.getBytes(1).empty());  // NULL rows read as empty, never out of bounds
  CHECK(text.getBytes(2).empty());
  CHECK(text.getBytes(3) == "d");

  Column bin(StorageKind::Binary);
  const std::string raw("\x00\x01\xff", 3);
  bin.appendBytes(raw);
  CHECK(bin.getBytes(0) == std::string_view(raw));
  CHECK_EQ(bin.getBytes(0).size(), size_t{3});
  CHECK(bin.byteSize() >= 3);

  text.clear();
  CHECK_EQ(text.size(), size_t{0});
  text.appendBytes("after clear");
  CHECK(text.getBytes(0) == "after clear");
}

TEST_CASE("page: byteSize sums the columns") {
  Page page;
  page.columns.emplace_back(StorageKind::Int64);
  page.columns.emplace_back(StorageKind::String);
  for (int i = 0; i < 100; ++i) {
    page.columns[0].appendInt64(i);
    page.columns[1].appendBytes("0123456789");
  }
  page.rowCount = 100;
  CHECK(page.byteSize() >= 100 * sizeof(int64_t) + 1000);
}

// ---- PageBuilder ------------------------------------------------------------------------------

TEST_CASE("page builder: NULL stays NULL in every storage kind") {
  PageBuilder b({StorageKind::Bool, StorageKind::Int64, StorageKind::Double, StorageKind::String, StorageKind::Binary});
  b.addRow({Cell{}, Cell{}, Cell{}, Cell{}, Cell{}});
  CHECK_EQ(b.rowCount(), size_t{1});
  CHECK_EQ(b.columnCount(), size_t{5});
  std::unique_ptr<Page> page = b.finish();
  REQUIRE(page != nullptr);
  CHECK_EQ(page->rowCount, size_t{1});
  for (const Column& c : page->columns) {
    CHECK_EQ(c.size(), size_t{1});
    CHECK(c.isNull(0));
  }
}

TEST_CASE("page builder: coercion into String columns") {
  PageBuilder b({StorageKind::String, StorageKind::String, StorageKind::String, StorageKind::String});
  b.addRow({true, int64_t{-42}, 1.5, std::string("as is")});
  b.addRow({false, int64_t{9223372036854775807LL}, 1e300, std::string()});
  b.addRow({Cell{}, int64_t{0}, 100.0, std::string("x")});
  std::unique_ptr<Page> page = b.finish();
  auto rows = pageToStrings(*page);
  REQUIRE_EQ(rows.size(), size_t{3});
  CHECK_EQ(rows[0][0].value(), std::string("true"));
  CHECK_EQ(rows[0][1].value(), std::string("-42"));
  CHECK_EQ(rows[0][2].value(), std::string("1.5"));
  CHECK_EQ(rows[0][3].value(), std::string("as is"));
  CHECK_EQ(rows[1][0].value(), std::string("false"));
  CHECK_EQ(rows[1][1].value(), std::string("9223372036854775807"));
  CHECK_EQ(rows[1][2].value(), std::string("1e+300"));
  CHECK_EQ(rows[1][3].value(), std::string(""));
  CHECK(!rows[2][0].has_value());
  CHECK_EQ(rows[2][1].value(), std::string("0"));
  CHECK_EQ(rows[2][2].value(), std::string("100"));
}

TEST_CASE("page builder: coercion into Int64 columns") {
  PageBuilder b({StorageKind::Int64});
  b.addRow({std::string("123")});
  b.addRow({std::string("-9223372036854775808")});
  b.addRow({true});
  b.addRow({int64_t{5}});
  b.addRow({4.0});
  std::unique_ptr<Page> page = b.finish();
  REQUIRE_EQ(page->rowCount, size_t{5});
  const Column& c = page->columns[0];
  CHECK_EQ(c.getInt64(0), int64_t{123});
  CHECK_EQ(c.getInt64(1), std::numeric_limits<int64_t>::min());
  CHECK_EQ(c.getInt64(2), int64_t{1});
  CHECK_EQ(c.getInt64(3), int64_t{5});
  CHECK_EQ(c.getInt64(4), int64_t{4});

  // Strict parsing: no fraction, no trailing garbage, no surrounding whitespace, no overflow.
  CHECK_THROWS_AS(b.addRow({std::string("abc")}), DriverError);
  CHECK_THROWS_AS(b.addRow({std::string("1.5")}), DriverError);
  CHECK_THROWS_AS(b.addRow({std::string(" 1")}), DriverError);
  CHECK_THROWS_AS(b.addRow({std::string("")}), DriverError);
  CHECK_THROWS_AS(b.addRow({std::string("9223372036854775808")}), DriverError);
  CHECK_THROWS_AS(b.addRow({2.5}), DriverError);
  CHECK_THROWS_AS(b.addRow({std::numeric_limits<double>::infinity()}), DriverError);
  try {
    b.addRow({std::string("abc")});
    CHECK(false);
  } catch (const DriverError& e) {
    CHECK_EQ(e.sqlstate(), std::string("HY000"));
    CHECK(e.message().find("abc") != std::string::npos);
  }
  // A failed row leaves nothing behind: the builder is still consistent.
  CHECK_EQ(b.rowCount(), size_t{0});
  b.addRow({int64_t{1}});
  std::unique_ptr<Page> page2 = b.finish();
  CHECK_EQ(page2->rowCount, size_t{1});
  CHECK_EQ(page2->columns[0].size(), size_t{1});
}

TEST_CASE("page builder: coercion into Double columns") {
  PageBuilder b({StorageKind::Double});
  b.addRow({std::string("1.25")});
  b.addRow({std::string("-2")});
  b.addRow({std::string("NaN")});
  b.addRow({std::string("Infinity")});
  b.addRow({std::string("-Infinity")});
  b.addRow({int64_t{3}});
  b.addRow({true});
  b.addRow({0.5});
  std::unique_ptr<Page> page = b.finish();
  const Column& c = page->columns[0];
  REQUIRE_EQ(c.size(), size_t{8});
  CHECK_EQ(c.getDouble(0), 1.25);
  CHECK_EQ(c.getDouble(1), -2.0);
  CHECK(std::isnan(c.getDouble(2)));
  CHECK(std::isinf(c.getDouble(3)) && c.getDouble(3) > 0);
  CHECK(std::isinf(c.getDouble(4)) && c.getDouble(4) < 0);
  CHECK_EQ(c.getDouble(5), 3.0);
  CHECK_EQ(c.getDouble(6), 1.0);
  CHECK_EQ(c.getDouble(7), 0.5);
  CHECK_THROWS_AS(b.addRow({std::string("1.5x")}), DriverError);
  CHECK_THROWS_AS(b.addRow({std::string("")}), DriverError);
}

TEST_CASE("page builder: coercion into Bool columns") {
  PageBuilder b({StorageKind::Bool});
  b.addRow({std::string("true")});
  b.addRow({std::string("FALSE")});
  b.addRow({std::string("1")});
  b.addRow({std::string("0")});
  b.addRow({int64_t{0}});
  b.addRow({int64_t{-3}});
  b.addRow({false});
  b.addRow({1.0});
  std::unique_ptr<Page> page = b.finish();
  const Column& c = page->columns[0];
  REQUIRE_EQ(c.size(), size_t{8});
  CHECK(c.getBool(0));
  CHECK(!c.getBool(1));
  CHECK(c.getBool(2));
  CHECK(!c.getBool(3));
  CHECK(!c.getBool(4));
  CHECK(c.getBool(5));
  CHECK(!c.getBool(6));
  CHECK(c.getBool(7));
  CHECK_THROWS_AS(b.addRow({std::string("yes")}), DriverError);
  CHECK_THROWS_AS(b.addRow({std::string("")}), DriverError);
  CHECK_THROWS_AS(b.addRow({std::numeric_limits<double>::quiet_NaN()}), DriverError);
}

TEST_CASE("page builder: Binary columns take raw strings only") {
  PageBuilder b({StorageKind::Binary});
  const std::string raw("\x01\x00\x02", 3);
  b.addRow({raw});
  CHECK_THROWS_AS(b.addRow({int64_t{1}}), DriverError);
  CHECK_THROWS_AS(b.addRow({true}), DriverError);
  std::unique_ptr<Page> page = b.finish();
  REQUIRE_EQ(page->rowCount, size_t{1});
  CHECK(page->columns[0].getBytes(0) == std::string_view(raw));
}

TEST_CASE("page builder: wrong cell count is rejected and the builder is reusable after finish") {
  PageBuilder b({StorageKind::Int64, StorageKind::String});
  CHECK_THROWS_AS(b.addRow({int64_t{1}}), DriverError);
  CHECK_THROWS_AS(b.addRow(std::vector<Cell>{}), DriverError);
  CHECK_EQ(b.rowCount(), size_t{0});
  b.addRow(std::vector<Cell>{int64_t{1}, std::string("a")});
  std::unique_ptr<Page> first = b.finish();
  CHECK_EQ(first->rowCount, size_t{1});
  CHECK_EQ(b.rowCount(), size_t{0});
  CHECK_EQ(b.columnCount(), size_t{2});
  b.addRow({int64_t{2}, std::string("b")});
  b.addRow({int64_t{3}, std::string("c")});
  std::unique_ptr<Page> second = b.finish();
  CHECK_EQ(second->rowCount, size_t{2});
  CHECK_EQ(second->columns[0].getInt64(1), int64_t{3});
  CHECK(second->columns[1].getBytes(0) == "b");
  CHECK_EQ(first->rowCount, size_t{1});  // the first page is untouched
}

TEST_CASE("page builder: zero columns") {
  PageBuilder b({});
  b.addRow(std::vector<Cell>{});
  std::unique_ptr<Page> page = b.finish();
  CHECK_EQ(page->rowCount, size_t{1});
  CHECK(page->columns.empty());
}

// ---- pageToStrings ----------------------------------------------------------------------------

TEST_CASE("pageToStrings renders every kind and NULL as nullopt") {
  PageBuilder b({StorageKind::Bool, StorageKind::Int64, StorageKind::Double, StorageKind::String, StorageKind::Binary});
  b.addRow({true, int64_t{-1}, 2.5, std::string("text"), std::string("\x00\x7f", 2)});
  b.addRow({Cell{}, Cell{}, Cell{}, Cell{}, Cell{}});
  b.addRow({false, int64_t{10}, std::string("NaN"), std::string(""), std::string("")});
  std::unique_ptr<Page> page = b.finish();
  auto rows = pageToStrings(*page);
  REQUIRE_EQ(rows.size(), size_t{3});
  REQUIRE_EQ(rows[0].size(), size_t{5});
  CHECK_EQ(rows[0][0].value(), std::string("true"));
  CHECK_EQ(rows[0][1].value(), std::string("-1"));
  CHECK_EQ(rows[0][2].value(), std::string("2.5"));
  CHECK_EQ(rows[0][3].value(), std::string("text"));
  CHECK_EQ(rows[0][4].value(), std::string("\x00\x7f", 2));
  for (const auto& cell : rows[1]) CHECK(!cell.has_value());
  CHECK_EQ(rows[2][0].value(), std::string("false"));
  CHECK_EQ(rows[2][1].value(), std::string("10"));
  CHECK_EQ(rows[2][2].value(), std::string("NaN"));
  CHECK_EQ(rows[2][3].value(), std::string(""));
  CHECK_EQ(rows[2][4].value(), std::string(""));

  Page empty;
  empty.columns.emplace_back(StorageKind::Int64);
  CHECK(pageToStrings(empty).empty());
}

// ---- ResultColumn -----------------------------------------------------------------------------

TEST_CASE("ResultColumn::make applies the ODBC type mapping") {
  const TypeMappingOptions& opts = defaultOpts();
  ResultColumn id = ResultColumn::make("id", "bigint", opts);
  CHECK_EQ(id.name, std::string("id"));
  CHECK(id.type.category == TypeCategory::BigInt);
  CHECK_EQ(id.odbc.conciseType, SQL_BIGINT);
  CHECK(id.odbc.storage == StorageKind::Int64);
  CHECK(id.catalog.empty() && id.schema.empty() && id.table.empty());
  CHECK(!id.aliased);

  ResultColumn name = ResultColumn::make("name", "varchar(10)", opts);
  CHECK_EQ(name.odbc.conciseType, SQL_WVARCHAR);
  CHECK_EQ(name.odbc.columnSize, static_cast<SQLULEN>(10));
  CHECK(name.odbc.storage == StorageKind::String);

  TypeMappingOptions ansi = opts;
  ansi.unicode = false;
  CHECK_EQ(ResultColumn::make("name", "varchar(10)", ansi).odbc.conciseType, SQL_VARCHAR);

  ResultColumn flag = ResultColumn::make("flag", TrinoType::parse("boolean"), opts);
  CHECK_EQ(flag.odbc.conciseType, SQL_BIT);
  CHECK(flag.odbc.storage == StorageKind::Bool);

  ResultColumn value = ResultColumn::make("value", "double", opts);
  CHECK_EQ(value.odbc.conciseType, SQL_DOUBLE);
  CHECK(value.odbc.storage == StorageKind::Double);

  ResultColumn amount = ResultColumn::make("amount", "decimal(18,4)", opts);
  CHECK_EQ(amount.odbc.conciseType, SQL_DECIMAL);
  CHECK_EQ(amount.odbc.decimalDigits, static_cast<SQLSMALLINT>(4));
  CHECK(amount.odbc.storage == StorageKind::String);

  ResultColumn blob = ResultColumn::make("blob", "varbinary", opts);
  CHECK(blob.odbc.storage == StorageKind::Binary);

  ResultColumn weird = ResultColumn::make("w", "HyperLogLog", opts);
  CHECK(weird.type.category == TypeCategory::Unknown);
  CHECK(weird.odbc.storage == StorageKind::String);
}

// ---- MemoryRowSource --------------------------------------------------------------------------

TEST_CASE("memory row source keeps pages in order and ends with nullptr") {
  MemoryRowSource src(idNameColumns(), idPages({2, 3, 1}));
  CHECK_EQ(src.totalRows(), size_t{6});
  CHECK_EQ(src.columns().size(), size_t{2});
  std::vector<StorageKind> kinds = src.storageKinds();
  REQUIRE_EQ(kinds.size(), size_t{2});
  CHECK(kinds[0] == StorageKind::Int64);
  CHECK(kinds[1] == StorageKind::String);
  CHECK(!src.updateCount().has_value());
  CHECK(src.warnings().empty());
  CHECK(src.updateType().empty());
  CHECK(src.queryId().empty());

  std::unique_ptr<Page> p = src.nextPage();
  REQUIRE(p != nullptr);
  CHECK_EQ(p->rowCount, size_t{2});
  CHECK_EQ(p->columns[0].getInt64(0), int64_t{0});
  p = src.nextPage();
  REQUIRE(p != nullptr);
  CHECK_EQ(p->rowCount, size_t{3});
  CHECK_EQ(p->columns[0].getInt64(0), int64_t{2});
  p = src.nextPage();
  REQUIRE(p != nullptr);
  CHECK_EQ(p->rowCount, size_t{1});
  CHECK_EQ(p->columns[0].getInt64(0), int64_t{5});
  CHECK(src.nextPage() == nullptr);
  CHECK(src.nextPage() == nullptr);
  CHECK_EQ(src.totalRows(), size_t{6});  // cumulative, not "remaining"

  src.addPage(idPage(6, 8));
  CHECK_EQ(src.totalRows(), size_t{8});
  p = src.nextPage();
  REQUIRE(p != nullptr);
  CHECK_EQ(p->columns[0].getInt64(1), int64_t{7});
  src.addPage(nullptr);  // ignored
  CHECK(src.nextPage() == nullptr);
}

TEST_CASE("memory row source: single page constructor, update count and warnings, shape checks") {
  MemoryRowSource empty(idNameColumns());
  CHECK(empty.nextPage() == nullptr);
  CHECK_EQ(empty.totalRows(), size_t{0});

  MemoryRowSource one(idNameColumns(), idPage(0, 4));
  CHECK_EQ(one.totalRows(), size_t{4});
  one.setUpdateCount(3);
  one.setWarnings({QueryWarning{1, "MOCK_WARNING", "mock warning"}});
  REQUIRE(one.updateCount().has_value());
  CHECK_EQ(one.updateCount().value_or(-1), int64_t{3});
  REQUIRE_EQ(one.warnings().size(), size_t{1});
  CHECK_EQ(one.warnings()[0].message, std::string("mock warning"));

  // A page whose column count does not match the metadata is a programming error.
  PageBuilder wrong({StorageKind::Int64});
  wrong.addRow({int64_t{1}});
  CHECK_THROWS_AS(one.addPage(wrong.finish()), DriverError);
  CHECK_EQ(one.totalRows(), size_t{4});

  // A page whose columns are not all rowCount long is rejected too.
  auto inconsistent = std::make_unique<Page>();
  inconsistent->columns.emplace_back(StorageKind::Int64);
  inconsistent->columns.emplace_back(StorageKind::String);
  inconsistent->columns[0].appendInt64(1);
  inconsistent->rowCount = 1;
  CHECK_THROWS_AS(one.addPage(std::move(inconsistent)), DriverError);
}

TEST_CASE("materialize concatenates pages and preserves update count and warnings") {
  ScriptedSource src(idNameColumns(), idPages({3, 0, 4, 2}));
  src.setUpdateCount(9);
  src.setWarnings({QueryWarning{7, "W", "first"}, QueryWarning{8, "W2", "second"}});
  std::unique_ptr<MemoryRowSource> mem = materialize(src);
  REQUIRE(mem != nullptr);
  CHECK_EQ(mem->totalRows(), size_t{9});
  CHECK_EQ(mem->columns().size(), size_t{2});
  REQUIRE(mem->updateCount().has_value());
  CHECK_EQ(mem->updateCount().value_or(-1), int64_t{9});
  REQUIRE_EQ(mem->warnings().size(), size_t{2});
  CHECK_EQ(mem->warnings()[1].code, 8);
  CHECK_EQ(mem->warnings()[1].message, std::string("second"));

  std::unique_ptr<Page> page = mem->nextPage();
  REQUIRE(page != nullptr);
  CHECK_EQ(page->rowCount, size_t{9});
  auto rows = pageToStrings(*page);
  REQUIRE_EQ(rows.size(), size_t{9});
  for (size_t i = 0; i < rows.size(); ++i) {
    CHECK_EQ(rows[i][0].value(), std::to_string(i));
    CHECK_EQ(rows[i][1].value(), "r" + std::to_string(i));
  }
  CHECK(mem->nextPage() == nullptr);  // one merged page, nothing else
  CHECK_EQ(src.stats->nextPageCalls, 5);  // 4 pages + the terminating nullptr

  // NULLs survive the copy across pages.
  PageBuilder b({StorageKind::Int64, StorageKind::String});
  b.addRow({Cell{}, std::string("x")});
  std::vector<std::unique_ptr<Page>> pages;
  pages.push_back(idPage(0, 1));
  pages.push_back(b.finish());
  ScriptedSource withNulls(idNameColumns(), std::move(pages));
  std::unique_ptr<MemoryRowSource> mem2 = materialize(withNulls);
  std::unique_ptr<Page> merged = mem2->nextPage();
  REQUIRE(merged != nullptr);
  REQUIRE_EQ(merged->rowCount, size_t{2});
  CHECK(!merged->columns[0].isNull(0));
  CHECK(merged->columns[0].isNull(1));
  CHECK(merged->columns[1].getBytes(1) == "x");

  // An empty source materializes to no pages and no update count.
  ScriptedSource nothing(idNameColumns(), {});
  std::unique_ptr<MemoryRowSource> mem3 = materialize(nothing);
  CHECK_EQ(mem3->totalRows(), size_t{0});
  CHECK(mem3->nextPage() == nullptr);
  CHECK(!mem3->updateCount().has_value());

  // Server errors during draining propagate.
  ScriptedSource failing(idNameColumns(), idPages({1, 1}));
  failing.failAtPage = 1;
  CHECK_THROWS_AS(materialize(failing), TrinoQueryError);
}

// ---- ResultSet --------------------------------------------------------------------------------

TEST_CASE("result set: rowsets span pages and rows are addressable across runs") {
  ResultSet rs(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({3, 4, 2})));
  CHECK_EQ(rs.columnCount(), size_t{2});
  CHECK_EQ(rs.columns()[1].name, std::string("name"));
  CHECK_EQ(rs.rowsetSize(), size_t{0});
  CHECK_EQ(rs.rowsetStartRowNumber(), int64_t{0});
  CHECK_EQ(rs.rowsFetched(), int64_t{0});
  CHECK(!rs.atEnd());
  CHECK(!rs.rowsetRow(0).valid());

  // Rowset 1: rows 0..3 = all of page 1 (3 rows) + 1 row of page 2 -> two runs.
  CHECK_EQ(rs.fetchRowset(4), size_t{4});
  CHECK_EQ(rs.rowsetSize(), size_t{4});
  CHECK_EQ(rs.rowsetStartRowNumber(), int64_t{1});
  CHECK_EQ(rs.rowsFetched(), int64_t{4});
  REQUIRE_EQ(rs.rowsetRuns().size(), size_t{2});
  CHECK_EQ(rs.rowsetRuns()[0].firstRow, size_t{0});
  CHECK_EQ(rs.rowsetRuns()[0].count, size_t{3});
  CHECK_EQ(rs.rowsetRuns()[1].firstRow, size_t{0});
  CHECK_EQ(rs.rowsetRuns()[1].count, size_t{1});
  CHECK(rs.rowsetRuns()[0].page.get() != rs.rowsetRuns()[1].page.get());
  for (size_t i = 0; i < 4; ++i) CHECK_EQ(idAt(rs, i), static_cast<int64_t>(i));
  RowRef last = rs.rowsetRow(3);
  CHECK(last.page == rs.rowsetRuns()[1].page.get());
  CHECK_EQ(last.row, size_t{0});
  CHECK(last.page->columns[1].getBytes(last.row) == "r3");
  CHECK(!rs.rowsetRow(4).valid());
  CHECK(!rs.rowsetRow(1000).valid());

  // Rowset 2: rows 4..7 = rest of page 2 (3 rows) + 1 row of page 3.
  CHECK_EQ(rs.fetchRowset(4), size_t{4});
  CHECK_EQ(rs.rowsetStartRowNumber(), int64_t{5});
  CHECK_EQ(rs.rowsFetched(), int64_t{8});
  REQUIRE_EQ(rs.rowsetRuns().size(), size_t{2});
  CHECK_EQ(rs.rowsetRuns()[0].firstRow, size_t{1});
  CHECK_EQ(rs.rowsetRuns()[0].count, size_t{3});
  CHECK_EQ(rs.rowsetRuns()[1].count, size_t{1});
  for (size_t i = 0; i < 4; ++i) CHECK_EQ(idAt(rs, i), static_cast<int64_t>(4 + i));
  CHECK(!rs.atEnd());

  // Rowset 3: the last row.
  CHECK_EQ(rs.fetchRowset(4), size_t{1});
  CHECK_EQ(rs.rowsetSize(), size_t{1});
  CHECK_EQ(rs.rowsetStartRowNumber(), int64_t{9});
  CHECK_EQ(idAt(rs, 0), int64_t{8});
  CHECK_EQ(rs.rowsFetched(), int64_t{9});

  // End of data: no rows, no current row number, and it stays that way.
  CHECK_EQ(rs.fetchRowset(4), size_t{0});
  CHECK(rs.atEnd());
  CHECK_EQ(rs.rowsetSize(), size_t{0});
  CHECK_EQ(rs.rowsetStartRowNumber(), int64_t{0});
  CHECK(rs.rowsetRuns().empty());
  CHECK(!rs.rowsetRow(0).valid());
  CHECK_EQ(rs.fetchRowset(4), size_t{0});
  CHECK_EQ(rs.rowsFetched(), int64_t{9});
}

TEST_CASE("result set: one big rowset takes everything, single-row rowsets walk page boundaries") {
  ResultSet all(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({3, 4, 2})));
  CHECK_EQ(all.fetchRowset(100), size_t{9});
  CHECK_EQ(all.rowsetRuns().size(), size_t{3});
  for (size_t i = 0; i < 9; ++i) CHECK_EQ(idAt(all, i), static_cast<int64_t>(i));
  CHECK_EQ(all.fetchRowset(100), size_t{0});
  CHECK(all.atEnd());

  ResultSet single(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({2, 1, 2})));
  for (int64_t expected = 0; expected < 5; ++expected) {
    CHECK_EQ(single.fetchRowset(1), size_t{1});
    CHECK_EQ(single.rowsetRuns().size(), size_t{1});
    CHECK_EQ(idAt(single, 0), expected);
    CHECK_EQ(single.rowsetStartRowNumber(), expected + 1);
  }
  CHECK_EQ(single.fetchRowset(1), size_t{0});
  CHECK(single.atEnd());
}

TEST_CASE("result set: empty pages are skipped and an empty source ends immediately") {
  ResultSet rs(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({0, 2, 0, 0, 1, 0})));
  CHECK_EQ(rs.fetchRowset(10), size_t{3});
  CHECK_EQ(rs.rowsetRuns().size(), size_t{2});
  CHECK_EQ(idAt(rs, 2), int64_t{2});
  CHECK_EQ(rs.fetchRowset(10), size_t{0});

  ResultSet none(std::make_unique<MemoryRowSource>(idNameColumns()));
  CHECK(!none.atEnd());
  CHECK_EQ(none.fetchRowset(10), size_t{0});
  CHECK(none.atEnd());
  CHECK_EQ(none.rowsFetched(), int64_t{0});

  ResultSet zero(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({2})));
  CHECK_EQ(zero.fetchRowset(0), size_t{0});  // a zero-sized rowset consumes nothing
  CHECK(!zero.atEnd());
  CHECK_EQ(zero.fetchRowset(5), size_t{2});
}

TEST_CASE("result set: pages stay alive while referenced by the rowset and are released afterwards") {
  ResultSet rs(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({3, 4, 2})));
  CHECK_EQ(rs.fetchRowset(4), size_t{4});
  REQUIRE_EQ(rs.rowsetRuns().size(), size_t{2});
  std::weak_ptr<Page> firstPage = rs.rowsetRuns()[0].page;
  std::weak_ptr<Page> secondPage = rs.rowsetRuns()[1].page;
  CHECK_EQ(rs.rowsetRuns()[0].page.use_count(), 1L);  // page 1 is fully consumed: only the run holds it
  CHECK_EQ(rs.rowsetRuns()[1].page.use_count(), 2L);  // page 2 is also the cursor's current page
  CHECK(!firstPage.expired());

  CHECK_EQ(rs.fetchRowset(4), size_t{4});
  CHECK(firstPage.expired());   // previous rowset released page 1
  CHECK(!secondPage.expired()); // page 2 still referenced by the new rowset
  CHECK_EQ(rs.fetchRowset(4), size_t{1});
  CHECK(secondPage.expired());
  std::weak_ptr<Page> thirdPage = rs.rowsetRuns()[0].page;
  CHECK_EQ(rs.fetchRowset(4), size_t{0});
  CHECK(thirdPage.expired());
}

TEST_CASE("result set: SQL_ATTR_MAX_ROWS boundaries") {
  // Limit in the middle of a rowset and of a page.
  ResultSet mid(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({3, 4, 2})));
  mid.setMaxRows(5);
  CHECK_EQ(mid.fetchRowset(4), size_t{4});
  CHECK(!mid.atEnd());
  CHECK_EQ(mid.fetchRowset(4), size_t{1});
  CHECK_EQ(idAt(mid, 0), int64_t{4});
  CHECK_EQ(mid.rowsFetched(), int64_t{5});
  CHECK(mid.atEnd());
  CHECK_EQ(mid.fetchRowset(4), size_t{0});
  CHECK_EQ(mid.rowsFetched(), int64_t{5});

  // Limit exactly equal to the rowset size: the first fetch fills the rowset, the second is empty.
  ResultSet exact(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({3, 4, 2})));
  exact.setMaxRows(4);
  CHECK_EQ(exact.fetchRowset(4), size_t{4});
  CHECK(exact.atEnd());
  CHECK_EQ(exact.rowsetSize(), size_t{4});  // the rowset is intact after the limit is reached
  CHECK_EQ(idAt(exact, 3), int64_t{3});
  CHECK_EQ(exact.fetchRowset(4), size_t{0});
  CHECK_EQ(exact.rowsetSize(), size_t{0});

  // Limit equal to the total row count.
  ResultSet total(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({3, 4, 2})));
  total.setMaxRows(9);
  CHECK_EQ(total.fetchRowset(100), size_t{9});
  CHECK_EQ(total.fetchRowset(100), size_t{0});
  CHECK_EQ(total.rowsFetched(), int64_t{9});

  // Limit larger than the result is a no-op.
  ResultSet large(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({3, 4, 2})));
  large.setMaxRows(1000);
  CHECK_EQ(large.fetchRowset(100), size_t{9});
  CHECK_EQ(large.fetchRowset(100), size_t{0});

  // Limit of 1.
  ResultSet one(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({3, 4, 2})));
  one.setMaxRows(1);
  CHECK_EQ(one.fetchRowset(100), size_t{1});
  CHECK_EQ(idAt(one, 0), int64_t{0});
  CHECK_EQ(one.fetchRowset(100), size_t{0});

  // 0 (and negative values) mean unlimited.
  ResultSet unlimited(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({3, 4, 2})));
  unlimited.setMaxRows(0);
  CHECK_EQ(unlimited.fetchRowset(100), size_t{9});
  ResultSet negative(std::make_unique<MemoryRowSource>(idNameColumns(), idPages({3, 4, 2})));
  negative.setMaxRows(-5);
  CHECK_EQ(negative.fetchRowset(100), size_t{9});
}

TEST_CASE("result set: reaching SQL_ATTR_MAX_ROWS cancels the source once and close() does not cancel again") {
  auto owned = std::make_unique<ScriptedSource>(idNameColumns(), idPages({3, 4, 2}));
  std::shared_ptr<SourceStats> stats = owned->stats;
  ResultSet rs(std::move(owned));
  rs.setMaxRows(5);
  CHECK_EQ(rs.fetchRowset(3), size_t{3});
  CHECK_EQ(stats->cancelCalls, 0);
  CHECK_EQ(rs.fetchRowset(3), size_t{2});
  CHECK_EQ(stats->cancelCalls, 1);
  CHECK(rs.atEnd());
  CHECK_EQ(rs.fetchRowset(3), size_t{0});
  CHECK_EQ(stats->cancelCalls, 1);
  rs.close();
  CHECK_EQ(stats->cancelCalls, 1);  // already at end: nothing to cancel
  CHECK_EQ(stats->closeCalls, 1);
}

TEST_CASE("result set: close() cancels an unfinished source, is idempotent and ends the cursor") {
  auto owned = std::make_unique<ScriptedSource>(idNameColumns(), idPages({3, 4, 2}));
  ScriptedSource* src = owned.get();  // valid only while the ResultSet below owns it
  std::shared_ptr<SourceStats> stats = owned->stats;
  {
    ResultSet rs(std::move(owned));
    CHECK_EQ(rs.fetchRowset(2), size_t{2});
    CHECK(!rs.atEnd());
    rs.close();
    CHECK_EQ(stats->cancelCalls, 1);
    CHECK_EQ(stats->closeCalls, 1);
    CHECK(rs.atEnd());
    CHECK_EQ(rs.rowsetSize(), size_t{0});
    CHECK(rs.rowsetRuns().empty());
    CHECK(!rs.rowsetRow(0).valid());
    CHECK_EQ(rs.fetchRowset(5), size_t{0});
    CHECK_EQ(rs.rowsFetched(), int64_t{2});
    rs.close();
    CHECK_EQ(stats->cancelCalls, 1);
    CHECK_EQ(stats->closeCalls, 1);
    CHECK(&rs.source() == src);
    // The destructor must not call close() a second time.
  }
  CHECK_EQ(stats->closeCalls, 1);
}

TEST_CASE("result set: destructor closes and cancels an unread source; a drained source is not cancelled") {
  auto owned = std::make_unique<ScriptedSource>(idNameColumns(), idPages({3}));
  std::shared_ptr<SourceStats> stats = owned->stats;
  {
    ResultSet rs(std::move(owned));
    CHECK_EQ(rs.fetchRowset(1), size_t{1});
  }
  CHECK_EQ(stats->cancelCalls, 1);
  CHECK_EQ(stats->closeCalls, 1);

  auto owned2 = std::make_unique<ScriptedSource>(idNameColumns(), idPages({3}));
  std::shared_ptr<SourceStats> stats2 = owned2->stats;
  {
    ResultSet rs(std::move(owned2));
    CHECK_EQ(rs.fetchRowset(10), size_t{3});
    CHECK_EQ(rs.fetchRowset(10), size_t{0});
    CHECK(rs.atEnd());
  }
  CHECK_EQ(stats2->cancelCalls, 0);
  CHECK_EQ(stats2->closeCalls, 1);
}

TEST_CASE("result set: cancel() forwards to the source without changing cursor state") {
  auto owned = std::make_unique<ScriptedSource>(idNameColumns(), idPages({2, 2}));
  std::shared_ptr<SourceStats> stats = owned->stats;
  ResultSet rs(std::move(owned));
  CHECK_EQ(rs.fetchRowset(1), size_t{1});
  rs.cancel();
  CHECK_EQ(stats->cancelCalls, 1);
  CHECK(!rs.atEnd());
  CHECK_EQ(rs.rowsetSize(), size_t{1});
  CHECK_EQ(idAt(rs, 0), int64_t{0});
}

TEST_CASE("result set: a server error while loading a page propagates and leaves an empty rowset") {
  auto owned = std::make_unique<ScriptedSource>(idNameColumns(), idPages({2, 2}));
  owned->failAtPage = 1;
  ResultSet rs(std::move(owned));
  CHECK_EQ(rs.fetchRowset(2), size_t{2});
  CHECK_EQ(rs.rowsFetched(), int64_t{2});
  CHECK_THROWS_AS(rs.fetchRowset(2), TrinoQueryError);
  CHECK_EQ(rs.rowsetSize(), size_t{0});
  CHECK(rs.rowsetRuns().empty());
  CHECK_EQ(rs.rowsFetched(), int64_t{2});
  CHECK(!rs.atEnd());

  // Failing in the middle of a rowset (page 1 consumed, page 2 fails) also yields no rowset.
  auto owned2 = std::make_unique<ScriptedSource>(idNameColumns(), idPages({2, 2}));
  owned2->failAtPage = 1;
  ResultSet rs2(std::move(owned2));
  CHECK_THROWS_AS(rs2.fetchRowset(10), DriverError);
  CHECK_EQ(rs2.rowsetSize(), size_t{0});
  CHECK_EQ(rs2.rowsFetched(), int64_t{0});
  CHECK_EQ(rs2.rowsetStartRowNumber(), int64_t{0});
}

TEST_CASE("result set: a null source is rejected") {
  CHECK_THROWS_AS(ResultSet(std::unique_ptr<RowSource>()), DriverError);
}
