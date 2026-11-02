#pragma once
// Parsed Trino type signature, e.g. "decimal(10,2)", "timestamp(6) with time zone",
// "array(row(x integer, \"my col\" varchar(3)))".
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trino_odbc {

enum class TypeCategory {
  Boolean, TinyInt, SmallInt, Integer, BigInt, Real, Double, Decimal,
  Varchar, Char, Varbinary, Json,
  Date, Time, TimeWithTimeZone, Timestamp, TimestampWithTimeZone,
  IntervalYearToMonth, IntervalDayToSecond,
  Array, Map, Row,
  Uuid, IpAddress,
  Unknown  // any other type (HyperLogLog, Geometry, ...) exposed as text
};

struct TrinoType;

struct TrinoTypeArg {
  enum class Kind { Long, Type, NamedType };
  Kind kind = Kind::Long;
  int64_t longValue = 0;
  std::string name;                   // NamedType only (unquoted field name, may be empty)
  std::shared_ptr<TrinoType> type;    // Type / NamedType
};

struct TrinoType {
  std::string rawType;                // lowercase base name as Trino prints it ("varchar", "timestamp with time zone")
  TypeCategory category = TypeCategory::Unknown;
  std::vector<TrinoTypeArg> args;

  /// Parses a Trino type name. Never throws: unparsable input yields category Unknown with rawType = input.
  static TrinoType parse(std::string_view typeName);
  /// Canonical Trino syntax ("varchar(10)", "timestamp(3) with time zone", "row(a integer)").
  std::string toString() const;
  /// Base name without parameters ("varchar", "decimal", "timestamp with time zone").
  std::string baseName() const { return rawType; }

  // ---- helpers ------------------------------------------------------------------------------
  /// varchar/char length or nullopt when unbounded (plain `varchar`).
  std::optional<int64_t> length() const;
  /// decimal precision (default 38 when absent).
  int decimalPrecision() const;
  /// decimal scale (default 0 when absent).
  int decimalScale() const;
  /// Fractional-second digits for time/timestamp (Trino default 3; up to 12).
  int temporalPrecision() const;
  bool isCharacter() const { return category == TypeCategory::Varchar || category == TypeCategory::Char; }
  bool isNumeric() const;
  bool isTemporal() const;
  bool isComplex() const { return category == TypeCategory::Array || category == TypeCategory::Map || category == TypeCategory::Row; }
  bool isUnboundedVarchar() const { return category == TypeCategory::Varchar && !length().has_value(); }

  static TypeCategory categoryFor(std::string_view baseName);
  static const char* categoryName(TypeCategory c);
};

}  // namespace trino_odbc
