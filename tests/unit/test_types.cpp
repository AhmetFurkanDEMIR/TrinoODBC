// Unit tests for the types package: Trino type parsing/printing, Trino -> ODBC mapping, ODBC type
// identifier helpers and the SQLGetTypeInfo result set.
#include "unit/test_framework.h"

#include "catalog/catalog.h"
#include "core/handles.h"
#include "result/row_source.h"
#include "types/odbc_types.h"
#include "types/trino_type.h"
#include "util/errors.h"

#include <optional>
#include <string>
#include <vector>

using namespace trino_odbc;

namespace {

TypeMappingOptions defaultOpts() { return TypeMappingOptions{}; }

TypeMappingOptions narrowOpts() {
  TypeMappingOptions o;
  o.unicode = false;
  return o;
}

OdbcTypeInfo map(const char* trinoType, const TypeMappingOptions& opts = defaultOpts()) {
  return mapTrinoType(TrinoType::parse(trinoType), opts);
}

// SQLLEN / SQLULEN may be macros on some driver managers, so brace-init spelled as a type is avoided.
SQLULEN ulen(long long v) { return static_cast<SQLULEN>(v); }
SQLLEN slen(long long v) { return static_cast<SQLLEN>(v); }

SQLLEN wideOctets(int64_t chars) { return static_cast<SQLLEN>(chars * static_cast<int64_t>(sizeof(SQLWCHAR))); }

/// Parses, prints, re-parses and checks the printed form is stable and matches `canonical`.
void checkRoundTrip(const char* input, const char* canonical, TypeCategory category) {
  const TrinoType t = TrinoType::parse(input);
  CHECK_EQ(t.toString(), std::string(canonical));
  CHECK(t.category == category);
  const TrinoType again = TrinoType::parse(t.toString());
  CHECK_EQ(again.toString(), std::string(canonical));
  CHECK(again.category == category);
  CHECK_EQ(again.args.size(), t.args.size());
}

std::vector<SQLSMALLINT> dataTypes(const std::vector<OdbcTypeInfo>& rows) {
  std::vector<SQLSMALLINT> out;
  for (const auto& r : rows) out.push_back(r.conciseType);
  return out;
}

std::vector<std::string> typeNames(const std::vector<OdbcTypeInfo>& rows) {
  std::vector<std::string> out;
  for (const auto& r : rows) out.push_back(r.typeName);
  return out;
}

/// Drains a MemoryRowSource into string cells ([row][column], nullopt = NULL).
std::vector<std::vector<std::optional<std::string>>> drain(MemoryRowSource& src) {
  std::vector<std::vector<std::optional<std::string>>> rows;
  while (auto page = src.nextPage()) {
    auto strings = pageToStrings(*page);
    rows.insert(rows.end(), strings.begin(), strings.end());
  }
  return rows;
}

}  // namespace

// ---- TrinoType parsing --------------------------------------------------------------------------

TEST_CASE("types: parse scalar names") {
  struct Row {
    const char* name;
    TypeCategory category;
  };
  const Row rows[] = {
      {"boolean", TypeCategory::Boolean},
      {"tinyint", TypeCategory::TinyInt},
      {"smallint", TypeCategory::SmallInt},
      {"integer", TypeCategory::Integer},
      {"bigint", TypeCategory::BigInt},
      {"real", TypeCategory::Real},
      {"double", TypeCategory::Double},
      {"decimal", TypeCategory::Decimal},
      {"varchar", TypeCategory::Varchar},
      {"char", TypeCategory::Char},
      {"varbinary", TypeCategory::Varbinary},
      {"json", TypeCategory::Json},
      {"date", TypeCategory::Date},
      {"time", TypeCategory::Time},
      {"time with time zone", TypeCategory::TimeWithTimeZone},
      {"timestamp", TypeCategory::Timestamp},
      {"timestamp with time zone", TypeCategory::TimestampWithTimeZone},
      {"interval year to month", TypeCategory::IntervalYearToMonth},
      {"interval day to second", TypeCategory::IntervalDayToSecond},
      {"uuid", TypeCategory::Uuid},
      {"ipaddress", TypeCategory::IpAddress},
  };
  for (const Row& r : rows) {
    const TrinoType t = TrinoType::parse(r.name);
    CHECK(t.category == r.category);
    CHECK_EQ(t.rawType, std::string(r.name));
    CHECK_EQ(t.baseName(), std::string(r.name));
    CHECK(t.args.empty());
    CHECK_EQ(t.toString(), std::string(r.name));
  }
}

TEST_CASE("types: parse parametrized scalars") {
  TrinoType t = TrinoType::parse("varchar(10)");
  CHECK(t.category == TypeCategory::Varchar);
  REQUIRE_EQ(t.args.size(), size_t{1});
  CHECK(t.args[0].kind == TrinoTypeArg::Kind::Long);
  CHECK_EQ(t.args[0].longValue, int64_t{10});
  CHECK(t.length().has_value());
  CHECK_EQ(t.length().value_or(-1), int64_t{10});
  CHECK(!t.isUnboundedVarchar());
  CHECK_EQ(t.toString(), std::string("varchar(10)"));

  t = TrinoType::parse("varchar");
  CHECK(t.isUnboundedVarchar());
  CHECK(!t.length().has_value());

  t = TrinoType::parse("char(5)");
  CHECK(t.category == TypeCategory::Char);
  CHECK_EQ(t.length().value_or(-1), int64_t{5});
  CHECK(t.isCharacter());
  CHECK_EQ(TrinoType::parse("char").length().value_or(-1), int64_t{1});

  t = TrinoType::parse("decimal(10,2)");
  CHECK(t.category == TypeCategory::Decimal);
  CHECK_EQ(t.decimalPrecision(), 10);
  CHECK_EQ(t.decimalScale(), 2);
  CHECK_EQ(t.toString(), std::string("decimal(10,2)"));
  CHECK_EQ(TrinoType::parse("decimal").decimalPrecision(), 38);
  CHECK_EQ(TrinoType::parse("decimal").decimalScale(), 0);
  CHECK_EQ(TrinoType::parse("decimal(7)").decimalScale(), 0);

  t = TrinoType::parse("timestamp(6) with time zone");
  CHECK(t.category == TypeCategory::TimestampWithTimeZone);
  CHECK_EQ(t.rawType, std::string("timestamp with time zone"));
  CHECK_EQ(t.temporalPrecision(), 6);
  CHECK_EQ(t.toString(), std::string("timestamp(6) with time zone"));

  t = TrinoType::parse("time(9) with time zone");
  CHECK(t.category == TypeCategory::TimeWithTimeZone);
  CHECK_EQ(t.temporalPrecision(), 9);
  CHECK_EQ(t.toString(), std::string("time(9) with time zone"));

  CHECK_EQ(TrinoType::parse("timestamp").temporalPrecision(), 3);
  CHECK_EQ(TrinoType::parse("timestamp with time zone").temporalPrecision(), 3);
  CHECK_EQ(TrinoType::parse("time").temporalPrecision(), 3);
  CHECK_EQ(TrinoType::parse("timestamp(0)").temporalPrecision(), 0);
  CHECK_EQ(TrinoType::parse("timestamp(12)").temporalPrecision(), 12);
  CHECK_EQ(TrinoType::parse("timestamp(99)").temporalPrecision(), 12);
  CHECK_EQ(TrinoType::parse("integer").temporalPrecision(), 0);
}

TEST_CASE("types: parse is case and whitespace tolerant") {
  TrinoType t = TrinoType::parse("  Decimal ( 10 , 2 )  ");
  CHECK(t.category == TypeCategory::Decimal);
  CHECK_EQ(t.toString(), std::string("decimal(10,2)"));
  t = TrinoType::parse("VARCHAR(255)");
  CHECK(t.category == TypeCategory::Varchar);
  CHECK_EQ(t.toString(), std::string("varchar(255)"));
  t = TrinoType::parse("TIMESTAMP(3)   WITH  TIME   ZONE");
  CHECK(t.category == TypeCategory::TimestampWithTimeZone);
  CHECK_EQ(t.toString(), std::string("timestamp(3) with time zone"));
  t = TrinoType::parse("Map(Varchar,Integer)");
  CHECK(t.category == TypeCategory::Map);
  CHECK_EQ(t.toString(), std::string("map(varchar, integer)"));
  CHECK(TrinoType::parse("double precision").category == TypeCategory::Double);
  CHECK(TrinoType::parse("int").category == TypeCategory::Integer);
}

TEST_CASE("types: parse nested complex types") {
  TrinoType t = TrinoType::parse("array(integer)");
  CHECK(t.category == TypeCategory::Array);
  CHECK(t.isComplex());
  REQUIRE_EQ(t.args.size(), size_t{1});
  CHECK(t.args[0].kind == TrinoTypeArg::Kind::Type);
  REQUIRE(t.args[0].type != nullptr);
  CHECK(t.args[0].type->category == TypeCategory::Integer);

  t = TrinoType::parse("map(varchar, array(row(a integer, b timestamp(3) with time zone)))");
  CHECK(t.category == TypeCategory::Map);
  REQUIRE_EQ(t.args.size(), size_t{2});
  CHECK(t.args[0].type->category == TypeCategory::Varchar);
  REQUIRE(t.args[1].type->category == TypeCategory::Array);
  const TrinoType& row = *t.args[1].type->args[0].type;
  CHECK(row.category == TypeCategory::Row);
  REQUIRE_EQ(row.args.size(), size_t{2});
  CHECK(row.args[0].kind == TrinoTypeArg::Kind::NamedType);
  CHECK_EQ(row.args[0].name, std::string("a"));
  CHECK(row.args[0].type->category == TypeCategory::Integer);
  CHECK_EQ(row.args[1].name, std::string("b"));
  CHECK(row.args[1].type->category == TypeCategory::TimestampWithTimeZone);
  CHECK_EQ(row.args[1].type->temporalPrecision(), 3);
  CHECK_EQ(t.toString(), std::string("map(varchar, array(row(a integer, b timestamp(3) with time zone)))"));
}

TEST_CASE("types: row field names quoted, escaped and unnamed") {
  TrinoType t = TrinoType::parse("row(\"my col\" varchar(3), \"a\"\"b\" integer, x double)");
  REQUIRE_EQ(t.args.size(), size_t{3});
  CHECK(t.args[0].kind == TrinoTypeArg::Kind::NamedType);
  CHECK_EQ(t.args[0].name, std::string("my col"));
  CHECK_EQ(t.args[0].type->length().value_or(-1), int64_t{3});
  CHECK_EQ(t.args[1].name, std::string("a\"b"));
  CHECK(t.args[1].type->category == TypeCategory::Integer);
  CHECK_EQ(t.args[2].name, std::string("x"));
  CHECK_EQ(t.toString(), std::string("row(\"my col\" varchar(3), \"a\"\"b\" integer, x double)"));

  // Quoted simple names print unquoted; names needing quotes keep them.
  CHECK_EQ(TrinoType::parse("row(\"x\" integer)").toString(), std::string("row(x integer)"));
  CHECK_EQ(TrinoType::parse("row(\"Mixed\" integer)").toString(), std::string("row(\"Mixed\" integer)"));
  CHECK_EQ(TrinoType::parse("row(\"1st\" integer)").toString(), std::string("row(\"1st\" integer)"));

  // Unnamed fields, including multi-word types that could be mistaken for "name type".
  t = TrinoType::parse("row(integer, timestamp with time zone, interval day to second, varchar(2))");
  REQUIRE_EQ(t.args.size(), size_t{4});
  for (const auto& a : t.args) CHECK(a.kind == TrinoTypeArg::Kind::Type);
  CHECK(t.args[1].type->category == TypeCategory::TimestampWithTimeZone);
  CHECK(t.args[2].type->category == TypeCategory::IntervalDayToSecond);
  CHECK_EQ(t.toString(), std::string("row(integer, timestamp with time zone, interval day to second, varchar(2))"));

  // Named fields whose type is multi-word / unknown.
  t = TrinoType::parse("row(ts timestamp(6) with time zone, iv interval year to month, hll HyperLogLog)");
  REQUIRE_EQ(t.args.size(), size_t{3});
  CHECK_EQ(t.args[0].name, std::string("ts"));
  CHECK(t.args[0].type->category == TypeCategory::TimestampWithTimeZone);
  CHECK_EQ(t.args[0].type->temporalPrecision(), 6);
  CHECK_EQ(t.args[1].name, std::string("iv"));
  CHECK(t.args[1].type->category == TypeCategory::IntervalYearToMonth);
  CHECK_EQ(t.args[2].name, std::string("hll"));
  CHECK(t.args[2].type->category == TypeCategory::Unknown);
  CHECK_EQ(t.args[2].type->rawType, std::string("hyperloglog"));

  // A field named like a type.
  t = TrinoType::parse("row(timestamp timestamp, integer integer)");
  REQUIRE_EQ(t.args.size(), size_t{2});
  CHECK_EQ(t.args[0].name, std::string("timestamp"));
  CHECK(t.args[0].type->category == TypeCategory::Timestamp);
  CHECK_EQ(t.args[1].name, std::string("integer"));
  CHECK(t.args[1].type->category == TypeCategory::Integer);
}

TEST_CASE("types: unknown type names are kept as text types") {
  TrinoType t = TrinoType::parse("HyperLogLog");
  CHECK(t.category == TypeCategory::Unknown);
  CHECK_EQ(t.rawType, std::string("hyperloglog"));
  CHECK_EQ(t.toString(), std::string("hyperloglog"));

  t = TrinoType::parse("qdigest(double)");
  CHECK(t.category == TypeCategory::Unknown);
  REQUIRE_EQ(t.args.size(), size_t{1});
  CHECK(t.args[0].type->category == TypeCategory::Double);
  CHECK_EQ(t.toString(), std::string("qdigest(double)"));

  t = TrinoType::parse("array(Geometry)");
  CHECK(t.category == TypeCategory::Array);
  REQUIRE_EQ(t.args.size(), size_t{1});
  CHECK(t.args[0].kind == TrinoTypeArg::Kind::Type);
  CHECK_EQ(t.args[0].type->rawType, std::string("geometry"));

  t = TrinoType::parse("unknown");
  CHECK(t.category == TypeCategory::Unknown);
  CHECK_EQ(t.rawType, std::string("unknown"));
}

TEST_CASE("types: unparsable input never throws and keeps the raw text") {
  const char* bad[] = {"", "   ", "varchar(", "row(a integer", "varchar()", ")", "(integer)", "decimal(10,)",
                       "decimal(99999999999999999999)", "row(\"unterminated integer)", "varchar(10)x(", "a,b",
                       "decimal(1 0)", "map(varchar, )", "row(a integer,)", "array(,integer)"};
  for (const char* input : bad) {
    TrinoType t;
    CHECK_NOTHROW(t = TrinoType::parse(input));
    CHECK(t.category == TypeCategory::Unknown);
    CHECK_EQ(t.rawType, std::string(input));
    CHECK(t.args.empty());
    CHECK_EQ(t.toString(), std::string(input));
  }
}

TEST_CASE("types: deep nesting is rejected instead of overflowing") {
  std::string deep;
  for (int i = 0; i < 500; ++i) deep += "array(";
  deep += "integer";
  for (int i = 0; i < 500; ++i) deep += ")";
  TrinoType t;
  CHECK_NOTHROW(t = TrinoType::parse(deep));
  CHECK(t.category == TypeCategory::Unknown);
  // Moderate nesting still works.
  std::string ok;
  for (int i = 0; i < 20; ++i) ok += "array(";
  ok += "integer";
  for (int i = 0; i < 20; ++i) ok += ")";
  t = TrinoType::parse(ok);
  CHECK(t.category == TypeCategory::Array);
  CHECK_EQ(t.toString(), ok);
}

TEST_CASE("types: toString round trips") {
  checkRoundTrip("varchar", "varchar", TypeCategory::Varchar);
  checkRoundTrip("varchar(42)", "varchar(42)", TypeCategory::Varchar);
  checkRoundTrip("char(1)", "char(1)", TypeCategory::Char);
  checkRoundTrip("decimal(38,10)", "decimal(38,10)", TypeCategory::Decimal);
  checkRoundTrip("decimal(38, 10)", "decimal(38,10)", TypeCategory::Decimal);
  checkRoundTrip("time(6)", "time(6)", TypeCategory::Time);
  checkRoundTrip("timestamp(3)", "timestamp(3)", TypeCategory::Timestamp);
  checkRoundTrip("timestamp(3) with time zone", "timestamp(3) with time zone", TypeCategory::TimestampWithTimeZone);
  checkRoundTrip("time with time zone", "time with time zone", TypeCategory::TimeWithTimeZone);
  checkRoundTrip("interval day to second", "interval day to second", TypeCategory::IntervalDayToSecond);
  checkRoundTrip("interval year to month", "interval year to month", TypeCategory::IntervalYearToMonth);
  checkRoundTrip("array(varchar(3))", "array(varchar(3))", TypeCategory::Array);
  checkRoundTrip("map(varchar,integer)", "map(varchar, integer)", TypeCategory::Map);
  checkRoundTrip("row(a integer, \"b c\" array(row(d date)))", "row(a integer, \"b c\" array(row(d date)))",
                 TypeCategory::Row);
  checkRoundTrip("row(integer, varchar)", "row(integer, varchar)", TypeCategory::Row);
  checkRoundTrip("Geometry", "geometry", TypeCategory::Unknown);
  checkRoundTrip("uuid", "uuid", TypeCategory::Uuid);
  checkRoundTrip("ipaddress", "ipaddress", TypeCategory::IpAddress);
  checkRoundTrip("json", "json", TypeCategory::Json);
  checkRoundTrip("varbinary", "varbinary", TypeCategory::Varbinary);
}

TEST_CASE("types: category helpers") {
  CHECK(TrinoType::categoryFor("VARCHAR") == TypeCategory::Varchar);
  CHECK(TrinoType::categoryFor("Timestamp  With Time Zone") == TypeCategory::TimestampWithTimeZone);
  CHECK(TrinoType::categoryFor("nope") == TypeCategory::Unknown);
  CHECK(TrinoType::categoryFor("") == TypeCategory::Unknown);
  CHECK_EQ(std::string(TrinoType::categoryName(TypeCategory::Decimal)), std::string("Decimal"));
  CHECK_EQ(std::string(TrinoType::categoryName(TypeCategory::Unknown)), std::string("Unknown"));
  CHECK_EQ(std::string(TrinoType::categoryName(TypeCategory::TimestampWithTimeZone)),
           std::string("TimestampWithTimeZone"));

  CHECK(TrinoType::parse("tinyint").isNumeric());
  CHECK(TrinoType::parse("decimal(3,1)").isNumeric());
  CHECK(TrinoType::parse("real").isNumeric());
  CHECK(!TrinoType::parse("varchar").isNumeric());
  CHECK(!TrinoType::parse("boolean").isNumeric());
  CHECK(TrinoType::parse("date").isTemporal());
  CHECK(TrinoType::parse("time(3) with time zone").isTemporal());
  CHECK(!TrinoType::parse("interval day to second").isTemporal());
  CHECK(!TrinoType::parse("bigint").isTemporal());
  CHECK(TrinoType::parse("row(a integer)").isComplex());
  CHECK(TrinoType::parse("map(integer, integer)").isComplex());
  CHECK(!TrinoType::parse("json").isComplex());
  CHECK(!TrinoType::parse("integer").isCharacter());
  CHECK(TrinoType::parse("char(2)").isCharacter());
}

// ---- Trino -> ODBC mapping ------------------------------------------------------------------------

TEST_CASE("mapping: common attributes") {
  const OdbcTypeInfo v = map("varchar(10)");
  CHECK_EQ(v.nullable, SQL_NULLABLE);
  CHECK(!v.fixedPrecScale);
  CHECK(!v.autoIncrement);
  CHECK_EQ(v.typeName, std::string("varchar(10)"));
  CHECK_EQ(v.localTypeName, std::string("varchar(10)"));
  CHECK(v.category == TypeCategory::Varchar);
  CHECK(v.storage == StorageKind::String);
  CHECK(v.unsignedAttr);
  CHECK(!map("integer").unsignedAttr);
  CHECK(!map("decimal(5,2)").unsignedAttr);
  CHECK(map("boolean").unsignedAttr);
}

TEST_CASE("mapping: boolean and integers") {
  OdbcTypeInfo t = map("boolean");
  CHECK_EQ(t.conciseType, SQL_BIT);
  CHECK_EQ(t.verboseType, SQL_BIT);
  CHECK_EQ(t.datetimeSubcode, 0);
  CHECK_EQ(t.columnSize, ulen(1));
  CHECK_EQ(t.octetLength, slen(1));
  CHECK_EQ(t.displaySize, slen(1));
  CHECK_EQ(t.decimalDigits, 0);
  CHECK_EQ(t.numPrecRadix, 0);
  CHECK_EQ(t.defaultCType, SQL_C_BIT);
  CHECK(t.storage == StorageKind::Bool);
  CHECK_EQ(t.searchable, SQL_PRED_BASIC);
  CHECK(!t.caseSensitive);
  CHECK(t.literalPrefix.empty());

  struct Row {
    const char* name;
    SQLSMALLINT sqlType;
    SQLULEN size;
    SQLLEN octet;
    SQLLEN display;
    SQLSMALLINT defaultC;
  };
  const Row rows[] = {
      {"tinyint", SQL_TINYINT, 3, 1, 4, SQL_C_STINYINT},
      {"smallint", SQL_SMALLINT, 5, 2, 6, SQL_C_SSHORT},
      {"integer", SQL_INTEGER, 10, 4, 11, SQL_C_SLONG},
      {"bigint", SQL_BIGINT, 19, 8, 20, SQL_C_SBIGINT},
  };
  for (const Row& r : rows) {
    t = map(r.name);
    CHECK_EQ(t.conciseType, r.sqlType);
    CHECK_EQ(t.verboseType, r.sqlType);
    CHECK_EQ(t.columnSize, r.size);
    CHECK_EQ(t.octetLength, r.octet);
    CHECK_EQ(t.displaySize, r.display);
    CHECK_EQ(t.decimalDigits, 0);
    CHECK_EQ(t.precision, static_cast<SQLSMALLINT>(r.size));
    CHECK_EQ(t.numPrecRadix, 10);
    CHECK_EQ(t.defaultCType, r.defaultC);
    CHECK(t.storage == StorageKind::Int64);
    CHECK(!t.unsignedAttr);
    CHECK_EQ(t.searchable, SQL_PRED_BASIC);
    CHECK(t.literalPrefix.empty());
    CHECK(t.createParams.empty());
  }
}

TEST_CASE("mapping: approximate numerics") {
  OdbcTypeInfo t = map("real");
  CHECK_EQ(t.conciseType, SQL_REAL);
  CHECK_EQ(t.columnSize, ulen(7));
  CHECK_EQ(t.octetLength, slen(4));
  CHECK_EQ(t.displaySize, slen(14));
  CHECK_EQ(t.numPrecRadix, 10);  // 7 / 15 are decimal digits, so the radix is 10 (review 71)
  CHECK_EQ(t.defaultCType, SQL_C_FLOAT);
  CHECK(t.storage == StorageKind::Double);
  t = map("double");
  CHECK_EQ(t.conciseType, SQL_DOUBLE);
  CHECK_EQ(t.columnSize, ulen(15));
  CHECK_EQ(t.octetLength, slen(8));
  CHECK_EQ(t.displaySize, slen(24));
  CHECK_EQ(t.numPrecRadix, 10);
  CHECK_EQ(t.defaultCType, SQL_C_DOUBLE);
  CHECK(t.storage == StorageKind::Double);
}

TEST_CASE("mapping: decimal") {
  OdbcTypeInfo t = map("decimal(12,4)");
  CHECK_EQ(t.conciseType, SQL_DECIMAL);
  CHECK_EQ(t.verboseType, SQL_DECIMAL);
  CHECK_EQ(t.columnSize, ulen(12));
  CHECK_EQ(t.octetLength, slen(14));
  CHECK_EQ(t.displaySize, slen(14));
  CHECK_EQ(t.decimalDigits, 4);
  CHECK_EQ(t.precision, 12);
  CHECK_EQ(t.numPrecRadix, 10);
  CHECK_EQ(t.defaultCType, SQL_C_CHAR);
  CHECK(t.storage == StorageKind::String);
  CHECK_EQ(t.createParams, std::string("precision,scale"));
  CHECK_EQ(t.minScale, 0);
  CHECK_EQ(t.maxScale, 38);
  CHECK_EQ(t.typeName, std::string("decimal(12,4)"));

  t = map("decimal");
  CHECK_EQ(t.columnSize, ulen(38));
  CHECK_EQ(t.decimalDigits, 0);

  TypeMappingOptions o;
  o.decimalAsDouble = true;
  t = map("decimal(12,4)", o);
  CHECK_EQ(t.conciseType, SQL_DOUBLE);
  CHECK_EQ(t.defaultCType, SQL_C_DOUBLE);
  CHECK_EQ(t.numPrecRadix, 10);
  CHECK(t.storage == StorageKind::String);
  CHECK(t.category == TypeCategory::Decimal);
  CHECK_EQ(t.typeName, std::string("decimal(12,4)"));
}

TEST_CASE("mapping: varchar and char") {
  OdbcTypeInfo t = map("varchar(100)");
  CHECK_EQ(t.conciseType, SQL_WVARCHAR);
  CHECK_EQ(t.verboseType, SQL_WVARCHAR);
  CHECK_EQ(t.columnSize, ulen(100));
  CHECK_EQ(t.octetLength, wideOctets(100));
  CHECK_EQ(t.displaySize, slen(100));
  CHECK_EQ(t.decimalDigits, 0);
  CHECK_EQ(t.numPrecRadix, 0);
  CHECK_EQ(t.defaultCType, SQL_C_WCHAR);
  CHECK(t.caseSensitive);
  CHECK_EQ(t.searchable, SQL_SEARCHABLE);
  CHECK_EQ(t.literalPrefix, std::string("'"));
  CHECK_EQ(t.literalSuffix, std::string("'"));
  CHECK_EQ(t.createParams, std::string("length"));

  t = map("varchar(100)", narrowOpts());
  CHECK_EQ(t.conciseType, SQL_VARCHAR);
  CHECK_EQ(t.octetLength, slen(100));
  CHECK_EQ(t.defaultCType, SQL_C_CHAR);

  // Unbounded varchar is described with the configured default length.
  t = map("varchar");
  CHECK_EQ(t.conciseType, SQL_WVARCHAR);
  CHECK_EQ(t.columnSize, ulen(65535));
  CHECK_EQ(t.octetLength, wideOctets(65535));
  CHECK_EQ(t.displaySize, slen(65535));
  CHECK_EQ(t.typeName, std::string("varchar"));

  TypeMappingOptions o;
  o.defaultStringLength = 4000;
  t = map("varchar", o);
  CHECK_EQ(t.columnSize, ulen(4000));
  o.unboundedAsLong = true;
  t = map("varchar", o);
  CHECK_EQ(t.conciseType, SQL_WLONGVARCHAR);
  CHECK_EQ(t.columnSize, ulen(4000));
  o.unicode = false;
  t = map("varchar", o);
  CHECK_EQ(t.conciseType, SQL_LONGVARCHAR);
  CHECK_EQ(t.octetLength, slen(4000));
  // Bounded varchar is never reported long.
  t = map("varchar(7)", o);
  CHECK_EQ(t.conciseType, SQL_VARCHAR);

  t = map("char(8)");
  CHECK_EQ(t.conciseType, SQL_WCHAR);
  CHECK_EQ(t.columnSize, ulen(8));
  CHECK_EQ(t.octetLength, wideOctets(8));
  CHECK_EQ(t.displaySize, slen(8));
  CHECK(t.caseSensitive);
  CHECK_EQ(t.searchable, SQL_SEARCHABLE);
  CHECK_EQ(t.createParams, std::string("length"));
  t = map("char(8)", narrowOpts());
  CHECK_EQ(t.conciseType, SQL_CHAR);
  CHECK_EQ(t.octetLength, slen(8));

  // Huge lengths never overflow the descriptor fields.
  t = map("varchar(2147483647)");
  CHECK_EQ(t.columnSize, ulen(2147483647));
  CHECK(t.octetLength > 0);
  CHECK(t.precision > 0);
}

TEST_CASE("mapping: varbinary and json") {
  OdbcTypeInfo t = map("varbinary");
  CHECK_EQ(t.conciseType, SQL_VARBINARY);
  CHECK_EQ(t.columnSize, ulen(65535));
  CHECK_EQ(t.octetLength, slen(65535));
  CHECK_EQ(t.displaySize, slen(2 * 65535));
  CHECK_EQ(t.defaultCType, SQL_C_BINARY);
  CHECK(t.storage == StorageKind::Binary);
  CHECK_EQ(t.literalPrefix, std::string("X'"));
  CHECK_EQ(t.literalSuffix, std::string("'"));
  CHECK(!t.caseSensitive);
  TypeMappingOptions o;
  o.unboundedAsLong = true;
  CHECK_EQ(map("varbinary", o).conciseType, SQL_LONGVARBINARY);

  t = map("json");
  CHECK_EQ(t.conciseType, SQL_WLONGVARCHAR);
  CHECK_EQ(t.columnSize, ulen(65535));
  CHECK_EQ(t.octetLength, wideOctets(65535));
  CHECK_EQ(t.displaySize, slen(65535));
  CHECK(t.caseSensitive);
  CHECK_EQ(t.searchable, SQL_SEARCHABLE);
  CHECK(t.storage == StorageKind::String);
  CHECK_EQ(map("json", narrowOpts()).conciseType, SQL_LONGVARCHAR);
  CHECK_EQ(map("json", narrowOpts()).octetLength, slen(65535));
}

TEST_CASE("mapping: date, time, timestamp") {
  OdbcTypeInfo t = map("date");
  CHECK_EQ(t.conciseType, SQL_TYPE_DATE);
  CHECK_EQ(t.verboseType, SQL_DATETIME);
  CHECK_EQ(t.datetimeSubcode, SQL_CODE_DATE);
  CHECK_EQ(t.columnSize, ulen(10));
  CHECK_EQ(t.octetLength, slen(6));
  CHECK_EQ(t.displaySize, slen(10));
  CHECK_EQ(t.decimalDigits, 0);
  CHECK_EQ(t.numPrecRadix, 0);
  CHECK_EQ(t.defaultCType, SQL_C_TYPE_DATE);
  CHECK(t.storage == StorageKind::String);
  CHECK_EQ(t.literalPrefix, std::string("DATE '"));
  CHECK_EQ(t.literalSuffix, std::string("'"));
  CHECK(t.unsignedAttr);
  CHECK_EQ(t.searchable, SQL_PRED_BASIC);

  t = map("time(3)");
  CHECK_EQ(t.conciseType, SQL_TYPE_TIME);
  CHECK_EQ(t.verboseType, SQL_DATETIME);
  CHECK_EQ(t.datetimeSubcode, SQL_CODE_TIME);
  CHECK_EQ(t.columnSize, ulen(12));
  CHECK_EQ(t.octetLength, slen(6));
  CHECK_EQ(t.displaySize, slen(12));
  CHECK_EQ(t.decimalDigits, 3);
  CHECK_EQ(t.precision, 3);
  CHECK_EQ(t.temporalPrecision, 3);
  CHECK_EQ(t.defaultCType, SQL_C_TYPE_TIME);
  CHECK_EQ(t.literalPrefix, std::string("TIME '"));
  CHECK_EQ(t.createParams, std::string("precision"));
  CHECK_EQ(t.minScale, 0);
  CHECK_EQ(t.maxScale, 9);
  t = map("time(0)");
  CHECK_EQ(t.columnSize, ulen(8));
  CHECK_EQ(t.decimalDigits, 0);
  t = map("time(12)");
  CHECK_EQ(t.columnSize, ulen(21));
  CHECK_EQ(t.decimalDigits, 9);  // ODBC structs stop at nanoseconds
  CHECK_EQ(t.temporalPrecision, 12);

  t = map("timestamp(6)");
  CHECK_EQ(t.conciseType, SQL_TYPE_TIMESTAMP);
  CHECK_EQ(t.verboseType, SQL_DATETIME);
  CHECK_EQ(t.datetimeSubcode, SQL_CODE_TIMESTAMP);
  CHECK_EQ(t.columnSize, ulen(26));
  CHECK_EQ(t.octetLength, slen(16));
  CHECK_EQ(t.displaySize, slen(26));
  CHECK_EQ(t.decimalDigits, 6);
  CHECK_EQ(t.defaultCType, SQL_C_TYPE_TIMESTAMP);
  CHECK_EQ(t.literalPrefix, std::string("TIMESTAMP '"));
  CHECK_EQ(t.createParams, std::string("precision"));
  CHECK_EQ(t.maxScale, 9);
  t = map("timestamp(0)");
  CHECK_EQ(t.columnSize, ulen(19));
  t = map("timestamp");
  CHECK_EQ(t.columnSize, ulen(23));
  CHECK_EQ(t.decimalDigits, 3);
}

TEST_CASE("mapping: timestamp with time zone modes") {
  TypeMappingOptions o;  // default Utc
  OdbcTypeInfo t = map("timestamp(3) with time zone", o);
  CHECK_EQ(t.conciseType, SQL_TYPE_TIMESTAMP);
  CHECK_EQ(t.verboseType, SQL_DATETIME);
  CHECK_EQ(t.datetimeSubcode, SQL_CODE_TIMESTAMP);
  CHECK_EQ(t.columnSize, ulen(23));
  CHECK_EQ(t.octetLength, slen(16));
  CHECK_EQ(t.decimalDigits, 3);
  CHECK_EQ(t.defaultCType, SQL_C_TYPE_TIMESTAMP);
  CHECK(t.category == TypeCategory::TimestampWithTimeZone);
  CHECK_EQ(t.typeName, std::string("timestamp(3) with time zone"));
  CHECK_EQ(t.temporalPrecision, 3);

  o.timestampTz = TimestampTzMode::WallClock;
  t = map("timestamp(3) with time zone", o);
  CHECK_EQ(t.conciseType, SQL_TYPE_TIMESTAMP);

  o.timestampTz = TimestampTzMode::String;
  t = map("timestamp(3) with time zone", o);
  CHECK_EQ(t.conciseType, SQL_WVARCHAR);
  CHECK_EQ(t.verboseType, SQL_WVARCHAR);
  CHECK_EQ(t.datetimeSubcode, 0);
  CHECK_EQ(t.columnSize, ulen(19 + 4 + 1 + 40));
  CHECK_EQ(t.octetLength, wideOctets(64));
  CHECK_EQ(t.displaySize, slen(64));
  CHECK_EQ(t.defaultCType, SQL_C_WCHAR);
  CHECK(t.category == TypeCategory::TimestampWithTimeZone);
  CHECK(t.storage == StorageKind::String);
  t = map("timestamp(0) with time zone", o);
  CHECK_EQ(t.columnSize, ulen(60));
  o.unicode = false;
  t = map("timestamp(6) with time zone", o);
  CHECK_EQ(t.conciseType, SQL_VARCHAR);
  CHECK_EQ(t.columnSize, ulen(67));
  CHECK_EQ(t.octetLength, slen(67));
  CHECK_EQ(t.defaultCType, SQL_C_CHAR);
}

TEST_CASE("mapping: time with time zone modes") {
  TypeMappingOptions o;  // default String
  OdbcTypeInfo t = map("time(3) with time zone", o);
  CHECK_EQ(t.conciseType, SQL_WVARCHAR);
  CHECK_EQ(t.columnSize, ulen(8 + 4 + 6));
  CHECK_EQ(t.octetLength, wideOctets(18));
  CHECK_EQ(t.displaySize, slen(18));
  CHECK_EQ(t.defaultCType, SQL_C_WCHAR);
  CHECK(t.category == TypeCategory::TimeWithTimeZone);
  t = map("time(0) with time zone", o);
  CHECK_EQ(t.columnSize, ulen(14));

  o.timeTz = TimeTzMode::Utc;
  t = map("time(3) with time zone", o);
  CHECK_EQ(t.conciseType, SQL_TYPE_TIME);
  CHECK_EQ(t.verboseType, SQL_DATETIME);
  CHECK_EQ(t.datetimeSubcode, SQL_CODE_TIME);
  CHECK_EQ(t.columnSize, ulen(12));
  CHECK_EQ(t.octetLength, slen(6));
  CHECK_EQ(t.decimalDigits, 3);
  CHECK_EQ(t.defaultCType, SQL_C_TYPE_TIME);
  o.timeTz = TimeTzMode::WallClock;
  CHECK_EQ(map("time(3) with time zone", o).conciseType, SQL_TYPE_TIME);
}

TEST_CASE("mapping: intervals") {
  TypeMappingOptions o;  // default String
  OdbcTypeInfo t = map("interval year to month", o);
  CHECK_EQ(t.conciseType, SQL_WVARCHAR);
  CHECK_EQ(t.columnSize, ulen(20));
  CHECK_EQ(t.octetLength, wideOctets(20));
  CHECK_EQ(t.displaySize, slen(20));
  CHECK_EQ(t.defaultCType, SQL_C_WCHAR);
  // Text exposure carries no interval code (SQL_DATETIME_SUB NULL, review 38).
  CHECK(!t.intervalType);
  CHECK_EQ(t.intervalCode, 0);
  CHECK_EQ(t.datetimeSubcode, 0);
  CHECK(t.category == TypeCategory::IntervalYearToMonth);
  CHECK(t.storage == StorageKind::String);
  t = map("interval day to second", o);
  CHECK_EQ(t.conciseType, SQL_WVARCHAR);
  CHECK_EQ(t.columnSize, ulen(30));
  CHECK(!t.intervalType);
  CHECK_EQ(t.intervalCode, 0);
  o.unicode = false;
  CHECK_EQ(map("interval day to second", o).conciseType, SQL_VARCHAR);
  CHECK_EQ(map("interval day to second", o).octetLength, slen(30));

  o.unicode = true;
  o.interval = IntervalMode::Interval;
  t = map("interval year to month", o);
  CHECK_EQ(t.conciseType, SQL_INTERVAL_YEAR_TO_MONTH);
  CHECK_EQ(t.verboseType, SQL_INTERVAL);
  CHECK_EQ(t.datetimeSubcode, SQL_CODE_YEAR_TO_MONTH);
  CHECK_EQ(t.columnSize, ulen(20));
  CHECK_EQ(t.octetLength, static_cast<SQLLEN>(sizeof(SQL_INTERVAL_STRUCT)));
  CHECK_EQ(t.displaySize, slen(20));
  CHECK_EQ(t.decimalDigits, 0);
  CHECK_EQ(t.defaultCType, SQL_C_INTERVAL_YEAR_TO_MONTH);
  CHECK(t.intervalType);
  CHECK_EQ(t.intervalCode, SQL_CODE_YEAR_TO_MONTH);
  CHECK_EQ(t.literalPrefix, std::string("INTERVAL '"));
  CHECK_EQ(t.literalSuffix, std::string("' YEAR TO MONTH"));
  t = map("interval day to second", o);
  CHECK_EQ(t.conciseType, SQL_INTERVAL_DAY_TO_SECOND);
  CHECK_EQ(t.verboseType, SQL_INTERVAL);
  CHECK_EQ(t.datetimeSubcode, SQL_CODE_DAY_TO_SECOND);
  CHECK_EQ(t.columnSize, ulen(30));
  CHECK_EQ(t.octetLength, static_cast<SQLLEN>(sizeof(SQL_INTERVAL_STRUCT)));
  CHECK_EQ(t.decimalDigits, 3);
  CHECK_EQ(t.defaultCType, SQL_C_INTERVAL_DAY_TO_SECOND);
  CHECK_EQ(t.literalSuffix, std::string("' DAY TO SECOND"));
}

TEST_CASE("mapping: complex types are long text") {
  for (const char* name : {"array(integer)", "map(varchar, integer)", "row(a integer, b varchar)"}) {
    OdbcTypeInfo t = map(name);
    CHECK_EQ(t.conciseType, SQL_WLONGVARCHAR);
    CHECK_EQ(t.verboseType, SQL_WLONGVARCHAR);
    CHECK_EQ(t.columnSize, ulen(65535));
    CHECK_EQ(t.octetLength, wideOctets(65535));
    CHECK_EQ(t.displaySize, slen(65535));
    CHECK_EQ(t.defaultCType, SQL_C_WCHAR);
    CHECK(t.caseSensitive);
    CHECK_EQ(t.searchable, SQL_SEARCHABLE);
    CHECK(t.storage == StorageKind::String);
    CHECK_EQ(t.typeName, std::string(name));
    CHECK(t.literalPrefix.empty());
    t = map(name, narrowOpts());
    CHECK_EQ(t.conciseType, SQL_LONGVARCHAR);
    CHECK_EQ(t.octetLength, slen(65535));
    CHECK_EQ(t.defaultCType, SQL_C_CHAR);
  }
}

TEST_CASE("mapping: uuid, ipaddress and unknown") {
  OdbcTypeInfo t = map("uuid");
  CHECK_EQ(t.conciseType, SQL_WVARCHAR);
  CHECK_EQ(t.columnSize, ulen(36));
  CHECK_EQ(t.octetLength, wideOctets(36));
  CHECK_EQ(t.displaySize, slen(36));
  CHECK_EQ(t.defaultCType, SQL_C_WCHAR);
  CHECK(t.category == TypeCategory::Uuid);
  TypeMappingOptions o;
  o.uuidAsGuid = true;
  t = map("uuid", o);
  CHECK_EQ(t.conciseType, SQL_GUID);
  CHECK_EQ(t.verboseType, SQL_GUID);
  CHECK_EQ(t.columnSize, ulen(36));
  CHECK_EQ(t.octetLength, slen(16));
  CHECK_EQ(t.displaySize, slen(36));
  CHECK_EQ(t.defaultCType, SQL_C_GUID);
  CHECK(t.storage == StorageKind::String);

  t = map("ipaddress");
  CHECK_EQ(t.conciseType, SQL_WVARCHAR);
  CHECK_EQ(t.columnSize, ulen(45));
  CHECK_EQ(t.octetLength, wideOctets(45));
  CHECK_EQ(map("ipaddress", narrowOpts()).conciseType, SQL_VARCHAR);

  t = map("HyperLogLog");
  CHECK_EQ(t.conciseType, SQL_WVARCHAR);
  CHECK_EQ(t.columnSize, ulen(65535));
  CHECK_EQ(t.typeName, std::string("hyperloglog"));
  CHECK(t.caseSensitive);
  CHECK_EQ(t.searchable, SQL_SEARCHABLE);
  CHECK(t.category == TypeCategory::Unknown);
  CHECK(t.storage == StorageKind::String);
  t = map("not a (type", narrowOpts());
  CHECK_EQ(t.conciseType, SQL_VARCHAR);
  CHECK_EQ(t.typeName, std::string("not a (type"));
}

TEST_CASE("mapping: storageKindFor") {
  CHECK(storageKindFor(TypeCategory::Boolean) == StorageKind::Bool);
  CHECK(storageKindFor(TypeCategory::TinyInt) == StorageKind::Int64);
  CHECK(storageKindFor(TypeCategory::SmallInt) == StorageKind::Int64);
  CHECK(storageKindFor(TypeCategory::Integer) == StorageKind::Int64);
  CHECK(storageKindFor(TypeCategory::BigInt) == StorageKind::Int64);
  CHECK(storageKindFor(TypeCategory::Real) == StorageKind::Double);
  CHECK(storageKindFor(TypeCategory::Double) == StorageKind::Double);
  CHECK(storageKindFor(TypeCategory::Varbinary) == StorageKind::Binary);
  CHECK(storageKindFor(TypeCategory::Decimal) == StorageKind::String);
  CHECK(storageKindFor(TypeCategory::Varchar) == StorageKind::String);
  CHECK(storageKindFor(TypeCategory::Timestamp) == StorageKind::String);
  CHECK(storageKindFor(TypeCategory::Row) == StorageKind::String);
  CHECK(storageKindFor(TypeCategory::Unknown) == StorageKind::String);
}

// ---- ODBC identifier helpers ----------------------------------------------------------------------

TEST_CASE("odbc types: defaultCTypeFor follows Appendix D") {
  CHECK_EQ(defaultCTypeFor(SQL_CHAR), SQL_C_CHAR);
  CHECK_EQ(defaultCTypeFor(SQL_VARCHAR), SQL_C_CHAR);
  CHECK_EQ(defaultCTypeFor(SQL_LONGVARCHAR), SQL_C_CHAR);
  CHECK_EQ(defaultCTypeFor(SQL_WCHAR), SQL_C_WCHAR);
  CHECK_EQ(defaultCTypeFor(SQL_WVARCHAR), SQL_C_WCHAR);
  CHECK_EQ(defaultCTypeFor(SQL_WLONGVARCHAR), SQL_C_WCHAR);
  CHECK_EQ(defaultCTypeFor(SQL_DECIMAL), SQL_C_CHAR);
  CHECK_EQ(defaultCTypeFor(SQL_NUMERIC), SQL_C_CHAR);
  CHECK_EQ(defaultCTypeFor(SQL_BIT), SQL_C_BIT);
  CHECK_EQ(defaultCTypeFor(SQL_TINYINT), SQL_C_STINYINT);
  CHECK_EQ(defaultCTypeFor(SQL_SMALLINT), SQL_C_SSHORT);
  CHECK_EQ(defaultCTypeFor(SQL_INTEGER), SQL_C_SLONG);
  CHECK_EQ(defaultCTypeFor(SQL_BIGINT), SQL_C_SBIGINT);
  CHECK_EQ(defaultCTypeFor(SQL_REAL), SQL_C_FLOAT);
  CHECK_EQ(defaultCTypeFor(SQL_FLOAT), SQL_C_DOUBLE);
  CHECK_EQ(defaultCTypeFor(SQL_DOUBLE), SQL_C_DOUBLE);
  CHECK_EQ(defaultCTypeFor(SQL_BINARY), SQL_C_BINARY);
  CHECK_EQ(defaultCTypeFor(SQL_VARBINARY), SQL_C_BINARY);
  CHECK_EQ(defaultCTypeFor(SQL_LONGVARBINARY), SQL_C_BINARY);
  CHECK_EQ(defaultCTypeFor(SQL_TYPE_DATE), SQL_C_TYPE_DATE);
  CHECK_EQ(defaultCTypeFor(SQL_TYPE_TIME), SQL_C_TYPE_TIME);
  CHECK_EQ(defaultCTypeFor(SQL_TYPE_TIMESTAMP), SQL_C_TYPE_TIMESTAMP);
  CHECK_EQ(defaultCTypeFor(SQL_DATE), SQL_C_TYPE_DATE);
  CHECK_EQ(defaultCTypeFor(SQL_TIME), SQL_C_TYPE_TIME);
  CHECK_EQ(defaultCTypeFor(SQL_TIMESTAMP), SQL_C_TYPE_TIMESTAMP);
  CHECK_EQ(defaultCTypeFor(SQL_GUID), SQL_C_GUID);
  for (SQLSMALLINT iv = SQL_INTERVAL_YEAR; iv <= SQL_INTERVAL_MINUTE_TO_SECOND; ++iv) {
    CHECK_EQ(defaultCTypeFor(iv), iv);
  }
  CHECK_EQ(defaultCTypeFor(SQL_INTERVAL_DAY_TO_SECOND), SQL_C_INTERVAL_DAY_TO_SECOND);
  CHECK_EQ(defaultCTypeFor(static_cast<SQLSMALLINT>(9999)), SQL_C_CHAR);
}

TEST_CASE("odbc types: verbose type, subcode and concise round trip") {
  CHECK_EQ(verboseTypeOf(SQL_TYPE_DATE), SQL_DATETIME);
  CHECK_EQ(verboseTypeOf(SQL_TYPE_TIME), SQL_DATETIME);
  CHECK_EQ(verboseTypeOf(SQL_TYPE_TIMESTAMP), SQL_DATETIME);
  CHECK_EQ(verboseTypeOf(SQL_TIMESTAMP), SQL_DATETIME);
  CHECK_EQ(verboseTypeOf(SQL_INTERVAL_YEAR), SQL_INTERVAL);
  CHECK_EQ(verboseTypeOf(SQL_INTERVAL_MINUTE_TO_SECOND), SQL_INTERVAL);
  CHECK_EQ(verboseTypeOf(SQL_INTEGER), SQL_INTEGER);
  CHECK_EQ(verboseTypeOf(SQL_WVARCHAR), SQL_WVARCHAR);
  CHECK_EQ(verboseTypeOf(SQL_C_TYPE_TIMESTAMP), SQL_DATETIME);

  CHECK_EQ(datetimeSubcodeOf(SQL_TYPE_DATE), SQL_CODE_DATE);
  CHECK_EQ(datetimeSubcodeOf(SQL_TYPE_TIME), SQL_CODE_TIME);
  CHECK_EQ(datetimeSubcodeOf(SQL_TYPE_TIMESTAMP), SQL_CODE_TIMESTAMP);
  CHECK_EQ(datetimeSubcodeOf(SQL_TIMESTAMP), SQL_CODE_TIMESTAMP);
  CHECK_EQ(datetimeSubcodeOf(SQL_INTERVAL_YEAR), SQL_CODE_YEAR);
  CHECK_EQ(datetimeSubcodeOf(SQL_INTERVAL_YEAR_TO_MONTH), SQL_CODE_YEAR_TO_MONTH);
  CHECK_EQ(datetimeSubcodeOf(SQL_INTERVAL_DAY_TO_SECOND), SQL_CODE_DAY_TO_SECOND);
  CHECK_EQ(datetimeSubcodeOf(SQL_INTERVAL_MINUTE_TO_SECOND), SQL_CODE_MINUTE_TO_SECOND);
  CHECK_EQ(datetimeSubcodeOf(SQL_VARCHAR), 0);
  CHECK_EQ(datetimeSubcodeOf(SQL_INTEGER), 0);

  CHECK_EQ(conciseTypeOf(SQL_DATETIME, SQL_CODE_DATE), SQL_TYPE_DATE);
  CHECK_EQ(conciseTypeOf(SQL_DATETIME, SQL_CODE_TIME), SQL_TYPE_TIME);
  CHECK_EQ(conciseTypeOf(SQL_DATETIME, SQL_CODE_TIMESTAMP), SQL_TYPE_TIMESTAMP);
  CHECK_EQ(conciseTypeOf(SQL_INTERVAL, SQL_CODE_DAY_TO_SECOND), SQL_INTERVAL_DAY_TO_SECOND);
  CHECK_EQ(conciseTypeOf(SQL_INTERVAL, SQL_CODE_YEAR), SQL_INTERVAL_YEAR);
  CHECK_EQ(conciseTypeOf(SQL_INTERVAL, 99), SQL_INTERVAL);
  CHECK_EQ(conciseTypeOf(SQL_DATETIME, 0), SQL_DATETIME);
  CHECK_EQ(conciseTypeOf(SQL_INTEGER, 0), SQL_INTEGER);
  CHECK_EQ(conciseTypeOf(SQL_WCHAR, 5), SQL_WCHAR);

  const SQLSMALLINT concise[] = {SQL_TYPE_DATE, SQL_TYPE_TIME, SQL_TYPE_TIMESTAMP, SQL_INTERVAL_YEAR,
                                 SQL_INTERVAL_MONTH, SQL_INTERVAL_DAY, SQL_INTERVAL_HOUR, SQL_INTERVAL_MINUTE,
                                 SQL_INTERVAL_SECOND, SQL_INTERVAL_YEAR_TO_MONTH, SQL_INTERVAL_DAY_TO_HOUR,
                                 SQL_INTERVAL_DAY_TO_MINUTE, SQL_INTERVAL_DAY_TO_SECOND, SQL_INTERVAL_HOUR_TO_MINUTE,
                                 SQL_INTERVAL_HOUR_TO_SECOND, SQL_INTERVAL_MINUTE_TO_SECOND, SQL_INTEGER, SQL_WVARCHAR,
                                 SQL_GUID, SQL_BIT};
  for (SQLSMALLINT c : concise) CHECK_EQ(conciseTypeOf(verboseTypeOf(c), datetimeSubcodeOf(c)), c);
}

TEST_CASE("odbc types: isValidSqlType / isValidCType") {
  const SQLSMALLINT validSql[] = {SQL_CHAR, SQL_VARCHAR, SQL_LONGVARCHAR, SQL_WCHAR, SQL_WVARCHAR, SQL_WLONGVARCHAR,
                                  SQL_DECIMAL, SQL_NUMERIC, SQL_SMALLINT, SQL_INTEGER, SQL_REAL, SQL_FLOAT,
                                  SQL_DOUBLE, SQL_BIT, SQL_TINYINT, SQL_BIGINT, SQL_BINARY, SQL_VARBINARY,
                                  SQL_LONGVARBINARY, SQL_TYPE_DATE, SQL_TYPE_TIME, SQL_TYPE_TIMESTAMP, SQL_DATE,
                                  SQL_TIME, SQL_TIMESTAMP, SQL_GUID, SQL_INTERVAL_YEAR, SQL_INTERVAL_MONTH,
                                  SQL_INTERVAL_DAY, SQL_INTERVAL_HOUR, SQL_INTERVAL_MINUTE, SQL_INTERVAL_SECOND,
                                  SQL_INTERVAL_YEAR_TO_MONTH, SQL_INTERVAL_DAY_TO_HOUR, SQL_INTERVAL_DAY_TO_MINUTE,
                                  SQL_INTERVAL_DAY_TO_SECOND, SQL_INTERVAL_HOUR_TO_MINUTE,
                                  SQL_INTERVAL_HOUR_TO_SECOND, SQL_INTERVAL_MINUTE_TO_SECOND};
  for (SQLSMALLINT s : validSql) CHECK(isValidSqlType(s));
  CHECK(!isValidSqlType(SQL_UNKNOWN_TYPE));
  CHECK(!isValidSqlType(SQL_C_SLONG));
  CHECK(!isValidSqlType(SQL_C_SBIGINT));
  CHECK(!isValidSqlType(SQL_C_DEFAULT));
  CHECK(!isValidSqlType(static_cast<SQLSMALLINT>(114)));
  CHECK(!isValidSqlType(static_cast<SQLSMALLINT>(100)));
  CHECK(!isValidSqlType(static_cast<SQLSMALLINT>(-12)));
  CHECK(!isValidSqlType(static_cast<SQLSMALLINT>(9999)));

  const SQLSMALLINT validC[] = {SQL_C_CHAR, SQL_C_WCHAR, SQL_C_SSHORT, SQL_C_USHORT, SQL_C_SHORT, SQL_C_SLONG,
                                SQL_C_ULONG, SQL_C_LONG, SQL_C_FLOAT, SQL_C_DOUBLE, SQL_C_BIT, SQL_C_STINYINT,
                                SQL_C_UTINYINT, SQL_C_TINYINT, SQL_C_SBIGINT, SQL_C_UBIGINT, SQL_C_BINARY,
                                SQL_C_BOOKMARK, SQL_C_VARBOOKMARK, SQL_C_TYPE_DATE, SQL_C_TYPE_TIME,
                                SQL_C_TYPE_TIMESTAMP, SQL_C_DATE, SQL_C_TIME, SQL_C_TIMESTAMP, SQL_C_NUMERIC,
                                SQL_C_GUID, SQL_C_DEFAULT, SQL_C_INTERVAL_YEAR, SQL_C_INTERVAL_MONTH,
                                SQL_C_INTERVAL_DAY, SQL_C_INTERVAL_HOUR, SQL_C_INTERVAL_MINUTE,
                                SQL_C_INTERVAL_SECOND, SQL_C_INTERVAL_YEAR_TO_MONTH, SQL_C_INTERVAL_DAY_TO_HOUR,
                                SQL_C_INTERVAL_DAY_TO_MINUTE, SQL_C_INTERVAL_DAY_TO_SECOND,
                                SQL_C_INTERVAL_HOUR_TO_MINUTE, SQL_C_INTERVAL_HOUR_TO_SECOND,
                                SQL_C_INTERVAL_MINUTE_TO_SECOND};
  for (SQLSMALLINT c : validC) CHECK(isValidCType(c));
  CHECK(!isValidCType(SQL_WVARCHAR));
  CHECK(!isValidCType(SQL_DECIMAL));
  CHECK(!isValidCType(SQL_VARCHAR));
  CHECK(!isValidCType(static_cast<SQLSMALLINT>(0)));
  CHECK(!isValidCType(static_cast<SQLSMALLINT>(114)));
  CHECK(!isValidCType(static_cast<SQLSMALLINT>(-100)));
}

TEST_CASE("odbc types: trinoTypeForSqlType") {
  CHECK_EQ(trinoTypeForSqlType(SQL_VARCHAR, 0, 0), std::string("varchar"));
  CHECK_EQ(trinoTypeForSqlType(SQL_WVARCHAR, 20, 0), std::string("varchar(20)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_CHAR, 3, 0), std::string("varchar(3)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_WLONGVARCHAR, 2147483647, 0), std::string("varchar"));
  CHECK_EQ(trinoTypeForSqlType(SQL_LONGVARCHAR, 0, 0), std::string("varchar"));
  CHECK_EQ(trinoTypeForSqlType(SQL_BIT, 0, 0), std::string("boolean"));
  CHECK_EQ(trinoTypeForSqlType(SQL_TINYINT, 0, 0), std::string("tinyint"));
  CHECK_EQ(trinoTypeForSqlType(SQL_SMALLINT, 0, 0), std::string("smallint"));
  CHECK_EQ(trinoTypeForSqlType(SQL_INTEGER, 0, 0), std::string("integer"));
  CHECK_EQ(trinoTypeForSqlType(SQL_BIGINT, 0, 0), std::string("bigint"));
  CHECK_EQ(trinoTypeForSqlType(SQL_REAL, 0, 0), std::string("real"));
  CHECK_EQ(trinoTypeForSqlType(SQL_FLOAT, 0, 0), std::string("double"));
  CHECK_EQ(trinoTypeForSqlType(SQL_DOUBLE, 0, 0), std::string("double"));
  CHECK_EQ(trinoTypeForSqlType(SQL_DECIMAL, 10, 2), std::string("decimal(10,2)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_NUMERIC, 0, 0), std::string("decimal(38,0)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_NUMERIC, 0, 5), std::string("decimal(38,5)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_DECIMAL, 5, 9), std::string("decimal(5,5)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_DECIMAL, 99, 2), std::string("decimal(38,2)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_BINARY, 0, 0), std::string("varbinary"));
  CHECK_EQ(trinoTypeForSqlType(SQL_VARBINARY, 10, 0), std::string("varbinary"));
  CHECK_EQ(trinoTypeForSqlType(SQL_LONGVARBINARY, 0, 0), std::string("varbinary"));
  CHECK_EQ(trinoTypeForSqlType(SQL_TYPE_DATE, 0, 0), std::string("date"));
  CHECK_EQ(trinoTypeForSqlType(SQL_DATE, 0, 0), std::string("date"));
  CHECK_EQ(trinoTypeForSqlType(SQL_TYPE_TIME, 0, 0), std::string("time(0)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_TIME, 0, 6), std::string("time(6)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_TYPE_TIMESTAMP, 0, 0), std::string("timestamp(3)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_TYPE_TIMESTAMP, 0, 6), std::string("timestamp(6)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_TIMESTAMP, 0, 9), std::string("timestamp(9)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_TYPE_TIMESTAMP, 0, 20), std::string("timestamp(12)"));
  CHECK_EQ(trinoTypeForSqlType(SQL_GUID, 0, 0), std::string("uuid"));
  CHECK_EQ(trinoTypeForSqlType(SQL_INTERVAL_YEAR_TO_MONTH, 0, 0), std::string("interval year to month"));
  CHECK_EQ(trinoTypeForSqlType(SQL_INTERVAL_YEAR, 0, 0), std::string("interval year to month"));
  CHECK_EQ(trinoTypeForSqlType(SQL_INTERVAL_MONTH, 0, 0), std::string("interval year to month"));
  CHECK_EQ(trinoTypeForSqlType(SQL_INTERVAL_DAY_TO_SECOND, 0, 0), std::string("interval day to second"));
  CHECK_EQ(trinoTypeForSqlType(SQL_INTERVAL_HOUR, 0, 0), std::string("interval day to second"));
  CHECK_EQ(trinoTypeForSqlType(SQL_INTERVAL_MINUTE_TO_SECOND, 0, 0), std::string("interval day to second"));
  CHECK_EQ(trinoTypeForSqlType(static_cast<SQLSMALLINT>(9999), 0, 0), std::string("varchar"));
}

TEST_CASE("odbc types: names and small helpers") {
  CHECK_EQ(std::string(sqlTypeName(SQL_WVARCHAR)), std::string("SQL_WVARCHAR"));
  CHECK_EQ(std::string(sqlTypeName(SQL_TYPE_TIMESTAMP)), std::string("SQL_TYPE_TIMESTAMP"));
  CHECK_EQ(std::string(sqlTypeName(SQL_INTERVAL_DAY_TO_SECOND)), std::string("SQL_INTERVAL_DAY_TO_SECOND"));
  CHECK_EQ(std::string(sqlTypeName(SQL_GUID)), std::string("SQL_GUID"));
  CHECK(std::string(sqlTypeName(static_cast<SQLSMALLINT>(9999))).find("unknown") != std::string::npos);
  CHECK_EQ(std::string(cTypeName(SQL_C_SBIGINT)), std::string("SQL_C_SBIGINT"));
  CHECK_EQ(std::string(cTypeName(SQL_C_TYPE_DATE)), std::string("SQL_C_TYPE_DATE"));
  CHECK_EQ(std::string(cTypeName(SQL_C_DATE)), std::string("SQL_C_DATE"));
  CHECK_EQ(std::string(cTypeName(SQL_C_INTERVAL_YEAR_TO_MONTH)), std::string("SQL_C_INTERVAL_YEAR_TO_MONTH"));
  CHECK_EQ(std::string(cTypeName(SQL_C_DEFAULT)), std::string("SQL_C_DEFAULT"));
  CHECK(std::string(cTypeName(static_cast<SQLSMALLINT>(9999))).find("unknown") != std::string::npos);

  CHECK(isChunkedCType(SQL_C_CHAR));
  CHECK(isChunkedCType(SQL_C_WCHAR));
  CHECK(isChunkedCType(SQL_C_BINARY));
  CHECK(!isChunkedCType(SQL_C_SLONG));
  CHECK(!isChunkedCType(SQL_C_TYPE_TIMESTAMP));
  CHECK(!isChunkedCType(SQL_C_DEFAULT));

  CHECK_EQ(normalizeCType(SQL_C_DATE), SQL_C_TYPE_DATE);
  CHECK_EQ(normalizeCType(SQL_C_TIME), SQL_C_TYPE_TIME);
  CHECK_EQ(normalizeCType(SQL_C_TIMESTAMP), SQL_C_TYPE_TIMESTAMP);
  CHECK_EQ(normalizeCType(SQL_C_SHORT), SQL_C_SSHORT);
  CHECK_EQ(normalizeCType(SQL_C_LONG), SQL_C_SLONG);
  CHECK_EQ(normalizeCType(SQL_C_TINYINT), SQL_C_STINYINT);
  CHECK_EQ(normalizeCType(SQL_C_TYPE_TIMESTAMP), SQL_C_TYPE_TIMESTAMP);
  CHECK_EQ(normalizeCType(SQL_C_WCHAR), SQL_C_WCHAR);
  CHECK_EQ(normalizeCType(SQL_C_DEFAULT), SQL_C_DEFAULT);
}

// ---- SQLGetTypeInfo table ---------------------------------------------------------------------------

TEST_CASE("type info: default (unicode) table order and names") {
  const auto rows = odbcTypeInfoTable(defaultOpts());
  // The long text row is the unbounded varchar, json follows it (review 72).
  const std::vector<SQLSMALLINT> expected = {SQL_WLONGVARCHAR, SQL_WLONGVARCHAR, SQL_WVARCHAR, SQL_WCHAR,  SQL_BIT,
                                             SQL_TINYINT,      SQL_BIGINT,   SQL_VARBINARY, SQL_DECIMAL,
                                             SQL_INTEGER,      SQL_SMALLINT, SQL_REAL,   SQL_DOUBLE,
                                             SQL_TYPE_DATE,    SQL_TYPE_TIME, SQL_TYPE_TIMESTAMP};
  CHECK(dataTypes(rows) == expected);
  const std::vector<std::string> names = {"varchar", "json",    "varchar",  "char", "boolean", "tinyint", "bigint",
                                          "varbinary", "decimal", "integer", "smallint", "real", "double",
                                          "date",    "time",     "timestamp"};
  CHECK(typeNames(rows) == names);
  for (size_t i = 1; i < rows.size(); ++i) CHECK(rows[i - 1].conciseType <= rows[i].conciseType);
  for (const auto& r : rows) {
    CHECK_EQ(r.localTypeName, r.typeName);
    CHECK_EQ(r.nullable, SQL_NULLABLE);
  }
  // Base type names never carry parameters, and the catalog sizes are the maxima.
  CHECK_EQ(rows[0].columnSize, ulen(65535));
  CHECK(rows[0].createParams.empty());
  CHECK_EQ(rows[0].literalPrefix, std::string("'"));
  CHECK_EQ(rows[2].columnSize, ulen(65535));
  CHECK_EQ(rows[2].createParams, std::string("length"));
  CHECK_EQ(rows[8].columnSize, ulen(38));
  CHECK_EQ(rows[8].createParams, std::string("precision,scale"));
  CHECK_EQ(rows[14].columnSize, ulen(18));
  CHECK_EQ(rows[15].columnSize, ulen(29));
  CHECK_EQ(rows[15].createParams, std::string("precision"));
}

TEST_CASE("type info: narrow table uses the ANSI character types") {
  const auto rows = odbcTypeInfoTable(narrowOpts());
  const std::vector<SQLSMALLINT> expected = {SQL_BIT,     SQL_TINYINT,  SQL_BIGINT, SQL_VARBINARY, SQL_LONGVARCHAR,
                                             SQL_LONGVARCHAR, SQL_CHAR,    SQL_DECIMAL,  SQL_INTEGER, SQL_SMALLINT, SQL_REAL,
                                             SQL_DOUBLE,  SQL_VARCHAR,  SQL_TYPE_DATE, SQL_TYPE_TIME,
                                             SQL_TYPE_TIMESTAMP};
  CHECK(dataTypes(rows) == expected);
  CHECK_EQ(rows[4].typeName, std::string("varchar"));
  CHECK_EQ(rows[5].typeName, std::string("json"));
  CHECK_EQ(rows[12].typeName, std::string("varchar"));
}

TEST_CASE("type info: options add or replace rows") {
  TypeMappingOptions o;
  o.uuidAsGuid = true;
  o.unboundedAsLong = true;
  o.interval = IntervalMode::Interval;
  const auto rows = odbcTypeInfoTable(o);
  const std::vector<SQLSMALLINT> expected = {SQL_GUID,       SQL_WLONGVARCHAR, SQL_WLONGVARCHAR,  SQL_WVARCHAR,      SQL_WCHAR,
                                             SQL_BIT,        SQL_TINYINT,      SQL_BIGINT,        SQL_LONGVARBINARY,
                                             SQL_DECIMAL,    SQL_INTEGER,      SQL_SMALLINT,      SQL_REAL,
                                             SQL_DOUBLE,     SQL_TYPE_DATE,    SQL_TYPE_TIME,     SQL_TYPE_TIMESTAMP,
                                             SQL_INTERVAL_YEAR_TO_MONTH, SQL_INTERVAL_DAY_TO_SECOND};
  CHECK(dataTypes(rows) == expected);
  CHECK_EQ(rows[0].typeName, std::string("uuid"));
  CHECK_EQ(rows[1].typeName, std::string("varchar"));  // the long row is the unbounded varchar now
  CHECK(rows[1].createParams.empty());
  CHECK_EQ(rows[2].typeName, std::string("json"));
  CHECK_EQ(rows[3].typeName, std::string("varchar"));
  CHECK_EQ(rows[3].createParams, std::string("length"));
  CHECK_EQ(rows[8].typeName, std::string("varbinary"));
  CHECK_EQ(rows[17].typeName, std::string("interval year to month"));
  CHECK_EQ(rows[17].verboseType, SQL_INTERVAL);
  CHECK_EQ(rows[18].typeName, std::string("interval day to second"));

  // Interval columns in String mode produce no interval rows.
  o.interval = IntervalMode::String;
  for (const auto& r : odbcTypeInfoTable(o)) CHECK(r.verboseType != SQL_INTERVAL);

  // decimal-as-double keeps a decimal row after the double row (closest match first).
  TypeMappingOptions d;
  d.decimalAsDouble = true;
  const auto dr = odbcTypeInfoTable(d);
  std::vector<std::string> doubles;
  for (const auto& r : dr) {
    if (r.conciseType == SQL_DOUBLE) doubles.push_back(r.typeName);
  }
  CHECK(doubles == std::vector<std::string>({"double", "decimal"}));
  for (const auto& r : dr) CHECK(r.conciseType != SQL_DECIMAL);
  for (size_t i = 1; i < dr.size(); ++i) CHECK(dr[i - 1].conciseType <= dr[i].conciseType);

  TypeMappingOptions l;
  l.defaultStringLength = 1000;
  const auto lr = odbcTypeInfoTable(l);
  CHECK_EQ(lr[0].columnSize, ulen(1000));  // long varchar
  CHECK_EQ(lr[1].columnSize, ulen(1000));  // json
  CHECK_EQ(lr[2].columnSize, ulen(1000));  // varchar
}

// ---- catalogTypeInfo result set -----------------------------------------------------------------------

TEST_CASE("catalogTypeInfo: 19 columns with the mandated names and types") {
  Connection conn;
  auto src = catalogTypeInfo(conn, SQL_ALL_TYPES);
  REQUIRE(src != nullptr);
  const auto& cols = src->columns();
  REQUIRE_EQ(cols.size(), size_t{19});
  const char* names[] = {"TYPE_NAME",       "DATA_TYPE",        "COLUMN_SIZE",       "LITERAL_PREFIX",
                         "LITERAL_SUFFIX",  "CREATE_PARAMS",    "NULLABLE",          "CASE_SENSITIVE",
                         "SEARCHABLE",      "UNSIGNED_ATTRIBUTE", "FIXED_PREC_SCALE", "AUTO_UNIQUE_VALUE",
                         "LOCAL_TYPE_NAME", "MINIMUM_SCALE",    "MAXIMUM_SCALE",     "SQL_DATA_TYPE",
                         "SQL_DATETIME_SUB", "NUM_PREC_RADIX",  "INTERVAL_PRECISION"};
  const TypeCategory cats[] = {TypeCategory::Varchar,  TypeCategory::SmallInt, TypeCategory::Integer,
                               TypeCategory::Varchar,  TypeCategory::Varchar,  TypeCategory::Varchar,
                               TypeCategory::SmallInt, TypeCategory::SmallInt, TypeCategory::SmallInt,
                               TypeCategory::SmallInt, TypeCategory::SmallInt, TypeCategory::SmallInt,
                               TypeCategory::Varchar,  TypeCategory::SmallInt, TypeCategory::SmallInt,
                               TypeCategory::SmallInt, TypeCategory::SmallInt, TypeCategory::Integer,
                               TypeCategory::SmallInt};
  for (size_t i = 0; i < 19; ++i) {
    CHECK_EQ(cols[i].name, std::string(names[i]));
    CHECK(cols[i].type.category == cats[i]);
    CHECK(cols[i].odbc.category == cats[i]);
  }
  CHECK_EQ(cols[1].odbc.conciseType, SQL_SMALLINT);
  CHECK_EQ(cols[2].odbc.conciseType, SQL_INTEGER);
  CHECK_EQ(cols[0].odbc.conciseType, SQL_WVARCHAR);
}

TEST_CASE("catalogTypeInfo: SQL_ALL_TYPES returns the whole table in order") {
  Connection conn;
  const auto table = odbcTypeInfoTable(conn.config.typeMapping());
  auto src = catalogTypeInfo(conn, SQL_ALL_TYPES);
  const auto rows = drain(*src);
  REQUIRE_EQ(rows.size(), table.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    REQUIRE_EQ(rows[i].size(), size_t{19});
    REQUIRE(rows[i][0].has_value());
    CHECK_EQ(*rows[i][0], table[i].typeName);
    REQUIRE(rows[i][1].has_value());
    CHECK_EQ(*rows[i][1], std::to_string(table[i].conciseType));
    REQUIRE(rows[i][2].has_value());
    CHECK_EQ(*rows[i][2], std::to_string(static_cast<long long>(table[i].columnSize)));
    CHECK_EQ(*rows[i][6], std::to_string(SQL_NULLABLE));
    CHECK_EQ(*rows[i][12], table[i].typeName);
    CHECK_EQ(*rows[i][15], std::to_string(table[i].verboseType));
  }
}

TEST_CASE("catalogTypeInfo: NULL cells where the spec says NULL") {
  Connection conn;
  auto src = catalogTypeInfo(conn, SQL_ALL_TYPES);
  const auto rows = drain(*src);
  auto find = [&](const std::string& name) -> const std::vector<std::optional<std::string>>* {
    for (const auto& r : rows) {
      // "varchar" also names the WLONGVARCHAR row; the checks below are about the WVARCHAR one.
      if (name == "varchar" && r[1].has_value() && *r[1] != std::to_string(SQL_WVARCHAR)) continue;
      if (r[0].has_value() && *r[0] == name) return &r;
    }
    return nullptr;
  };
  const auto* integer = find("integer");
  REQUIRE(integer != nullptr);
  CHECK(!(*integer)[3].has_value());   // LITERAL_PREFIX
  CHECK(!(*integer)[4].has_value());   // LITERAL_SUFFIX
  CHECK(!(*integer)[5].has_value());   // CREATE_PARAMS
  CHECK(!(*integer)[13].has_value());  // MINIMUM_SCALE
  CHECK(!(*integer)[14].has_value());  // MAXIMUM_SCALE
  CHECK(!(*integer)[16].has_value());  // SQL_DATETIME_SUB
  CHECK_EQ(*(*integer)[17], std::string("10"));  // NUM_PREC_RADIX
  CHECK(!(*integer)[18].has_value());  // INTERVAL_PRECISION
  CHECK_EQ(*(*integer)[9], std::to_string(SQL_FALSE));  // UNSIGNED_ATTRIBUTE
  CHECK_EQ(*(*integer)[7], std::to_string(SQL_FALSE));  // CASE_SENSITIVE
  CHECK_EQ(*(*integer)[8], std::to_string(SQL_PRED_BASIC));

  const auto* varchar = find("varchar");
  REQUIRE(varchar != nullptr);
  CHECK_EQ(*(*varchar)[3], std::string("'"));
  CHECK_EQ(*(*varchar)[4], std::string("'"));
  CHECK_EQ(*(*varchar)[5], std::string("length"));
  CHECK_EQ(*(*varchar)[7], std::to_string(SQL_TRUE));
  CHECK_EQ(*(*varchar)[8], std::to_string(SQL_SEARCHABLE));
  CHECK_EQ(*(*varchar)[9], std::to_string(SQL_TRUE));
  CHECK(!(*varchar)[17].has_value());

  const auto* decimal = find("decimal");
  REQUIRE(decimal != nullptr);
  CHECK_EQ(*(*decimal)[5], std::string("precision,scale"));
  CHECK_EQ(*(*decimal)[13], std::string("0"));
  CHECK_EQ(*(*decimal)[14], std::string("38"));
  CHECK_EQ(*(*decimal)[17], std::string("10"));

  const auto* real = find("real");
  REQUIRE(real != nullptr);
  CHECK_EQ(*(*real)[17], std::string("10"));

  const auto* timestamp = find("timestamp");
  REQUIRE(timestamp != nullptr);
  CHECK_EQ(*(*timestamp)[3], std::string("TIMESTAMP '"));
  CHECK_EQ(*(*timestamp)[5], std::string("precision"));
  CHECK_EQ(*(*timestamp)[13], std::string("0"));
  CHECK_EQ(*(*timestamp)[14], std::string("9"));
  CHECK_EQ(*(*timestamp)[15], std::to_string(SQL_DATETIME));
  CHECK_EQ(*(*timestamp)[16], std::to_string(SQL_CODE_TIMESTAMP));
  CHECK(!(*timestamp)[17].has_value());

  const auto* date = find("date");
  REQUIRE(date != nullptr);
  CHECK_EQ(*(*date)[16], std::to_string(SQL_CODE_DATE));
  CHECK(!(*date)[13].has_value());
}

TEST_CASE("catalogTypeInfo: filtering by data type and legacy identifiers") {
  Connection conn;
  auto rows = drain(*catalogTypeInfo(conn, SQL_WVARCHAR));
  REQUIRE_EQ(rows.size(), size_t{1});
  CHECK_EQ(*rows[0][0], std::string("varchar"));
  CHECK_EQ(*rows[0][1], std::to_string(SQL_WVARCHAR));

  // Unicode is on by default, so the narrow identifier matches nothing.
  rows = drain(*catalogTypeInfo(conn, SQL_VARCHAR));
  CHECK_EQ(rows.size(), size_t{0});

  rows = drain(*catalogTypeInfo(conn, SQL_DATE));
  REQUIRE_EQ(rows.size(), size_t{1});
  CHECK_EQ(*rows[0][0], std::string("date"));
  CHECK_EQ(*rows[0][1], std::to_string(SQL_TYPE_DATE));
  rows = drain(*catalogTypeInfo(conn, SQL_TIME));
  REQUIRE_EQ(rows.size(), size_t{1});
  CHECK_EQ(*rows[0][0], std::string("time"));
  rows = drain(*catalogTypeInfo(conn, SQL_TIMESTAMP));
  REQUIRE_EQ(rows.size(), size_t{1});
  CHECK_EQ(*rows[0][0], std::string("timestamp"));
  CHECK_EQ(*rows[0][1], std::to_string(SQL_TYPE_TIMESTAMP));
  rows = drain(*catalogTypeInfo(conn, SQL_TYPE_TIMESTAMP));
  REQUIRE_EQ(rows.size(), size_t{1});
  rows = drain(*catalogTypeInfo(conn, SQL_NUMERIC));
  REQUIRE_EQ(rows.size(), size_t{1});
  CHECK_EQ(*rows[0][0], std::string("decimal"));
  rows = drain(*catalogTypeInfo(conn, SQL_FLOAT));
  REQUIRE_EQ(rows.size(), size_t{1});
  CHECK_EQ(*rows[0][0], std::string("double"));
  rows = drain(*catalogTypeInfo(conn, SQL_INTERVAL_DAY_TO_SECOND));
  CHECK_EQ(rows.size(), size_t{0});
  rows = drain(*catalogTypeInfo(conn, SQL_GUID));
  CHECK_EQ(rows.size(), size_t{0});

  CHECK_THROWS_AS(catalogTypeInfo(conn, static_cast<SQLSMALLINT>(9999)), DriverError);
  try {
    catalogTypeInfo(conn, static_cast<SQLSMALLINT>(9999));
  } catch (const DriverError& e) {
    CHECK_EQ(e.sqlstate(), std::string("HY004"));
  }
}

TEST_CASE("catalogTypeInfo: honours the connection's type mapping options") {
  Connection conn;
  conn.config.unicodeTypes = false;
  conn.config.uuidAsGuid = true;
  conn.config.intervalMode = IntervalMode::Interval;
  conn.config.unboundedVarcharAsLong = true;
  const auto table = odbcTypeInfoTable(conn.config.typeMapping());
  const auto rows = drain(*catalogTypeInfo(conn, SQL_ALL_TYPES));
  REQUIRE_EQ(rows.size(), table.size());
  REQUIRE(!rows.empty());
  CHECK_EQ(*rows[0][0], std::string("uuid"));
  CHECK_EQ(*rows[0][1], std::to_string(SQL_GUID));
  bool sawLongVarbinary = false, sawInterval = false, sawVarchar = false;
  for (const auto& r : rows) {
    if (*r[1] == std::to_string(SQL_LONGVARBINARY)) sawLongVarbinary = true;
    if (*r[1] == std::to_string(SQL_VARCHAR)) sawVarchar = true;
    if (*r[1] == std::to_string(SQL_INTERVAL_DAY_TO_SECOND)) {
      sawInterval = true;
      CHECK_EQ(*r[18], std::string("9"));  // INTERVAL_PRECISION
      CHECK_EQ(*r[15], std::to_string(SQL_INTERVAL));
      CHECK_EQ(*r[16], std::to_string(SQL_CODE_DAY_TO_SECOND));
    }
    if (*r[1] == std::to_string(SQL_INTERVAL_YEAR_TO_MONTH)) CHECK_EQ(*r[18], std::string("2"));
    CHECK(*r[1] != std::to_string(SQL_WVARCHAR));
  }
  CHECK(sawLongVarbinary);
  CHECK(sawInterval);
  CHECK(sawVarchar);
  const auto filtered = drain(*catalogTypeInfo(conn, SQL_VARCHAR));
  REQUIRE_EQ(filtered.size(), size_t{1});
  CHECK_EQ(*filtered[0][0], std::string("varchar"));
}

TEST_CASE("catalogTypeInfo: text columns stay (W)VARCHAR with UnboundedVarcharAsLongVarchar (review 39)") {
  for (bool unicode : {true, false}) {
    Connection conn;
    conn.config.unicodeTypes = unicode;
    conn.config.unboundedVarcharAsLong = true;
    auto src = catalogTypeInfo(conn, SQL_ALL_TYPES);
    const auto& cols = src->columns();
    REQUIRE_EQ(cols.size(), size_t{19});
    const SQLSMALLINT textType = unicode ? SQL_WVARCHAR : SQL_VARCHAR;
    // TYPE_NAME and LOCAL_TYPE_NAME are Varchar(128); prefix, suffix and create params are short.
    for (size_t i : {size_t{0}, size_t{12}}) {
      CHECK_EQ(cols[i].odbc.conciseType, textType);
      CHECK_EQ(cols[i].odbc.columnSize, static_cast<SQLULEN>(128));
    }
    for (size_t i : {size_t{3}, size_t{4}, size_t{5}}) {
      CHECK_EQ(cols[i].odbc.conciseType, textType);
      CHECK(cols[i].odbc.columnSize > 0 && cols[i].odbc.columnSize < 128);
    }
    // The spec's NOT NULL columns.
    for (size_t i : {size_t{0}, size_t{1}, size_t{6}, size_t{8}, size_t{15}}) {
      CHECK_EQ(cols[i].odbc.nullable, static_cast<SQLSMALLINT>(SQL_NO_NULLS));
    }
    CHECK_EQ(cols[2].odbc.nullable, static_cast<SQLSMALLINT>(SQL_NULLABLE));
    // Every value fits the declared column lengths.
    for (const auto& r : drain(*src)) {
      REQUIRE(r[0].has_value());
      CHECK(r[0]->size() <= 128);
      for (size_t i : {size_t{3}, size_t{4}, size_t{5}}) {
        if (r[i].has_value()) CHECK(r[i]->size() <= cols[i].odbc.columnSize);
      }
    }
  }
}

TEST_CASE("catalogTypeInfo: ODBC 2.x applications see the 2.x date/time identifiers") {
  Environment env;
  env.odbcVersion = SQL_OV_ODBC2;
  Connection conn;
  conn.env = &env;
  auto rows = drain(*catalogTypeInfo(conn, SQL_DATE));
  REQUIRE_EQ(rows.size(), size_t{1});
  CHECK_EQ(*rows[0][0], std::string("date"));
  CHECK_EQ(*rows[0][1], std::to_string(SQL_DATE));
  rows = drain(*catalogTypeInfo(conn, SQL_TIME));
  REQUIRE_EQ(rows.size(), size_t{1});
  CHECK_EQ(*rows[0][1], std::to_string(SQL_TIME));
  rows = drain(*catalogTypeInfo(conn, SQL_TIMESTAMP));
  REQUIRE_EQ(rows.size(), size_t{1});
  CHECK_EQ(*rows[0][1], std::to_string(SQL_TIMESTAMP));
  // The whole table: no ODBC 3 datetime identifier leaks through; other types are unchanged.
  bool sawBigint = false;
  for (const auto& r : drain(*catalogTypeInfo(conn, SQL_ALL_TYPES))) {
    CHECK(*r[1] != std::to_string(SQL_TYPE_DATE));
    CHECK(*r[1] != std::to_string(SQL_TYPE_TIME));
    CHECK(*r[1] != std::to_string(SQL_TYPE_TIMESTAMP));
    if (*r[0] == "bigint") sawBigint = *r[1] == std::to_string(SQL_BIGINT);
  }
  CHECK(sawBigint);

  // An ODBC 3 environment keeps the 3.x identifiers.
  env.odbcVersion = SQL_OV_ODBC3;
  rows = drain(*catalogTypeInfo(conn, SQL_TYPE_DATE));
  REQUIRE_EQ(rows.size(), size_t{1});
  CHECK_EQ(*rows[0][1], std::to_string(SQL_TYPE_DATE));
  conn.env = nullptr;
}
