// Column-major page storage (Column / Page) and the row-at-a-time PageBuilder used by catalog
// functions, DESCRIBE results and tests. The JSON / spool decoders append to Column directly.
#include "result/page.h"

#include "util/errors.h"
#include "util/strings.h"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <variant>

namespace trino_odbc {

namespace result_internal {

/// Shortest round-trip text for a double ("1.5", "1e+300"); NaN / infinities use Trino's spelling.
/// std::to_chars is used because printf-family formatting honours LC_NUMERIC, and host applications
/// (Power BI, Excel) may run with a locale whose decimal separator is a comma.
std::string formatDoubleShortest(double v) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
  char buf[64];
  const std::to_chars_result res = std::to_chars(buf, buf + sizeof(buf), v);
  if (res.ec != std::errc()) {
    // Cannot happen with a 64-byte buffer (shortest form is at most 24 characters); keep a
    // deterministic fallback rather than returning garbage.
    const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string("0");
  }
  return std::string(buf, res.ptr);
}

}  // namespace result_internal

// ---- Column -----------------------------------------------------------------------------------

void Column::reserve(size_t rows, size_t bytes) {
  nulls_.reserve(rows);
  switch (kind_) {
    case StorageKind::Bool: bools_.reserve(rows); break;
    case StorageKind::Int64: i64_.reserve(rows); break;
    case StorageKind::Double: f64_.reserve(rows); break;
    case StorageKind::String:
    case StorageKind::Binary:
      offsets_.reserve(rows + 1);
      arena_.reserve(bytes);
      break;
  }
}

void Column::appendNull() {
  nulls_.push_back(1);
  // Every typed vector stays index-aligned with nulls_, so getX(row) on a NULL row is well-defined
  // (it yields a zero / empty value) instead of reading past the end of the vector.
  switch (kind_) {
    case StorageKind::Bool: bools_.push_back(0); break;
    case StorageKind::Int64: i64_.push_back(0); break;
    case StorageKind::Double: f64_.push_back(0.0); break;
    case StorageKind::String:
    case StorageKind::Binary: offsets_.push_back(offsets_.back()); break;
  }
}

void Column::appendBool(bool v) {
  assert(kind_ == StorageKind::Bool);
  nulls_.push_back(0);
  bools_.push_back(v ? 1 : 0);
}

void Column::appendInt64(int64_t v) {
  assert(kind_ == StorageKind::Int64);
  nulls_.push_back(0);
  i64_.push_back(v);
}

void Column::appendDouble(double v) {
  assert(kind_ == StorageKind::Double);
  nulls_.push_back(0);
  f64_.push_back(v);
}

void Column::appendBytes(std::string_view bytes) {
  assert(kind_ == StorageKind::String || kind_ == StorageKind::Binary);
  nulls_.push_back(0);
  if (!bytes.empty()) arena_.append(bytes.data(), bytes.size());
  offsets_.push_back(static_cast<uint64_t>(arena_.size()));
}

size_t Column::byteSize() const {
  // Capacities rather than sizes: the prefetch queue bounds real heap usage, and the decoders
  // reserve ahead of filling.
  return nulls_.capacity() * sizeof(uint8_t) + bools_.capacity() * sizeof(uint8_t) +
         i64_.capacity() * sizeof(int64_t) + f64_.capacity() * sizeof(double) + arena_.capacity() +
         offsets_.capacity() * sizeof(uint64_t);
}

void Column::clear() {
  nulls_.clear();
  bools_.clear();
  i64_.clear();
  f64_.clear();
  arena_.clear();
  offsets_.clear();
  offsets_.push_back(0);
}

// ---- Page -------------------------------------------------------------------------------------

size_t Page::byteSize() const {
  size_t total = sizeof(Page) + columns.size() * sizeof(Column);
  for (const Column& c : columns) total += c.byteSize();
  return total;
}

// ---- PageBuilder ------------------------------------------------------------------------------

namespace {

constexpr size_t kMaxQuotedValueLength = 64;

const char* storageKindName(StorageKind kind) {
  switch (kind) {
    case StorageKind::Bool: return "boolean";
    case StorageKind::Int64: return "bigint";
    case StorageKind::Double: return "double";
    case StorageKind::String: return "varchar";
    case StorageKind::Binary: return "varbinary";
  }
  return "unknown";
}

/// Human-readable rendering of a cell for error messages (long strings are cut).
std::string describeCell(const Cell& cell) {
  struct Visitor {
    std::string operator()(std::monostate) const { return "NULL"; }
    std::string operator()(bool v) const { return v ? "true" : "false"; }
    std::string operator()(int64_t v) const { return std::to_string(v); }
    std::string operator()(double v) const { return result_internal::formatDoubleShortest(v); }
    std::string operator()(const std::string& v) const {
      if (v.size() <= kMaxQuotedValueLength) return "'" + v + "'";
      return "'" + v.substr(0, kMaxQuotedValueLength) + "...'";
    }
  };
  return std::visit(Visitor{}, cell);
}

[[noreturn]] void throwCoercion(const Cell& cell, StorageKind kind, size_t column) {
  throw DriverError(sqlstate::kGeneralError,
                    "PageBuilder: cannot store " + describeCell(cell) + " in " + storageKindName(kind) +
                        " column " + std::to_string(column + 1));
}

std::string toText(const Cell& cell) {
  struct Visitor {
    std::string operator()(std::monostate) const { return std::string(); }
    std::string operator()(bool v) const { return v ? "true" : "false"; }
    std::string operator()(int64_t v) const { return std::to_string(v); }
    std::string operator()(double v) const { return result_internal::formatDoubleShortest(v); }
    std::string operator()(const std::string& v) const { return v; }
  };
  return std::visit(Visitor{}, cell);
}

int64_t toInt64(const Cell& cell, size_t column) {
  if (const bool* b = std::get_if<bool>(&cell)) return *b ? 1 : 0;
  if (const int64_t* i = std::get_if<int64_t>(&cell)) return *i;
  if (const double* d = std::get_if<double>(&cell)) {
    // Only exact integers are accepted: silently truncating a fraction would hide caller bugs.
    constexpr double kTwo63 = 9223372036854775808.0;
    if (std::isfinite(*d) && std::trunc(*d) == *d && *d >= -kTwo63 && *d < kTwo63) return static_cast<int64_t>(*d);
    throwCoercion(cell, StorageKind::Int64, column);
  }
  int64_t parsed = 0;
  if (parseInt64(std::get<std::string>(cell), &parsed)) return parsed;
  throwCoercion(cell, StorageKind::Int64, column);
}

double toDouble(const Cell& cell, size_t column) {
  if (const bool* b = std::get_if<bool>(&cell)) return *b ? 1.0 : 0.0;
  if (const int64_t* i = std::get_if<int64_t>(&cell)) return static_cast<double>(*i);
  if (const double* d = std::get_if<double>(&cell)) return *d;
  const std::string& text = std::get<std::string>(cell);
  // Trino spells the non-finite doubles this way in JSON and in DESCRIBE output.
  if (text == "NaN") return std::numeric_limits<double>::quiet_NaN();
  if (text == "Infinity" || text == "+Infinity") return std::numeric_limits<double>::infinity();
  if (text == "-Infinity") return -std::numeric_limits<double>::infinity();
  double parsed = 0.0;
  if (parseDouble(text, &parsed)) return parsed;
  throwCoercion(cell, StorageKind::Double, column);
}

bool toBool(const Cell& cell, size_t column) {
  if (const bool* b = std::get_if<bool>(&cell)) return *b;
  if (const int64_t* i = std::get_if<int64_t>(&cell)) return *i != 0;
  if (const double* d = std::get_if<double>(&cell)) {
    if (std::isnan(*d)) throwCoercion(cell, StorageKind::Bool, column);
    return *d != 0.0;
  }
  const std::string& text = std::get<std::string>(cell);
  if (iequals(text, "true") || text == "1") return true;
  if (iequals(text, "false") || text == "0") return false;
  throwCoercion(cell, StorageKind::Bool, column);
}

/// Converts `cell` into a Cell that holds exactly the alternative matching `kind` (or NULL).
Cell coerceCell(const Cell& cell, StorageKind kind, size_t column) {
  if (std::holds_alternative<std::monostate>(cell)) return Cell{};  // NULL is NULL in every kind
  switch (kind) {
    case StorageKind::String: return Cell{toText(cell)};
    case StorageKind::Binary:
      // Binary columns take raw bytes only; formatting a number as text would be a silent bug.
      if (const std::string* s = std::get_if<std::string>(&cell)) return Cell{*s};
      throwCoercion(cell, kind, column);
    case StorageKind::Int64: return Cell{toInt64(cell, column)};
    case StorageKind::Double: return Cell{toDouble(cell, column)};
    case StorageKind::Bool: return Cell{toBool(cell, column)};
  }
  throwCoercion(cell, kind, column);
}

void appendCoerced(Column& col, const Cell& cell) {
  switch (cell.index()) {
    case 0: col.appendNull(); break;
    case 1: col.appendBool(std::get<bool>(cell)); break;
    case 2: col.appendInt64(std::get<int64_t>(cell)); break;
    case 3: col.appendDouble(std::get<double>(cell)); break;
    default: col.appendBytes(std::get<std::string>(cell)); break;
  }
}

}  // namespace

PageBuilder::PageBuilder(std::vector<StorageKind> kinds) : page_(std::make_unique<Page>()) {
  page_->columns.reserve(kinds.size());
  for (StorageKind k : kinds) page_->columns.emplace_back(k);
}

void PageBuilder::addRow(const std::vector<Cell>& cells) {
  const size_t n = page_->columns.size();
  if (cells.size() != n) {
    throw DriverError(sqlstate::kGeneralError, "PageBuilder: row has " + std::to_string(cells.size()) +
                                                   " cells but the page has " + std::to_string(n) + " columns");
  }
  // Coerce every cell before appending anything: a conversion failure must not leave the columns
  // with different lengths.
  std::vector<Cell> coerced;
  coerced.reserve(n);
  for (size_t i = 0; i < n; ++i) coerced.push_back(coerceCell(cells[i], page_->columns[i].kind(), i));
  for (size_t i = 0; i < n; ++i) appendCoerced(page_->columns[i], coerced[i]);
  ++rows_;
}

void PageBuilder::addRow(std::initializer_list<Cell> cells) { addRow(std::vector<Cell>(cells)); }

std::unique_ptr<Page> PageBuilder::finish() {
  page_->rowCount = rows_;
  std::unique_ptr<Page> done = std::move(page_);
  // Leave the builder usable for another page with the same layout so columnCount() and addRow()
  // never dereference a moved-from page.
  page_ = std::make_unique<Page>();
  page_->columns.reserve(done->columns.size());
  for (const Column& c : done->columns) page_->columns.emplace_back(c.kind());
  rows_ = 0;
  return done;
}

}  // namespace trino_odbc
