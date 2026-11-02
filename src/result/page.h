#pragma once
// Column-major page storage. A Page holds N rows of every column in typed vectors so the fetch
// path never re-parses or allocates per cell.
#include "types/odbc_types.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace trino_odbc {

class Column {
 public:
  explicit Column(StorageKind kind) : kind_(kind) {}
  StorageKind kind() const { return kind_; }
  size_t size() const { return nulls_.size(); }
  bool isNull(size_t row) const { return nulls_[row] != 0; }
  bool getBool(size_t row) const { return bools_[row] != 0; }
  int64_t getInt64(size_t row) const { return i64_[row]; }
  double getDouble(size_t row) const { return f64_[row]; }
  /// String / Binary payload (UTF-8 text for String, raw bytes for Binary).
  std::string_view getBytes(size_t row) const {
    return std::string_view(arena_.data() + offsets_[row], offsets_[row + 1] - offsets_[row]);
  }

  // ---- building -----------------------------------------------------------------------------
  void reserve(size_t rows, size_t bytes);
  void appendNull();
  void appendBool(bool v);
  void appendInt64(int64_t v);
  void appendDouble(double v);
  void appendBytes(std::string_view bytes);
  /// Approximate heap bytes used (for queue accounting).
  size_t byteSize() const;
  void clear();

 private:
  StorageKind kind_;
  std::vector<uint8_t> nulls_;
  std::vector<uint8_t> bools_;
  std::vector<int64_t> i64_;
  std::vector<double> f64_;
  std::string arena_;
  std::vector<uint64_t> offsets_{0};
};

struct Page {
  std::vector<Column> columns;
  size_t rowCount = 0;
  size_t byteSize() const;
};

/// Convenience cell used by PageBuilder (catalog functions, tests, DESCRIBE results).
using Cell = std::variant<std::monostate, bool, int64_t, double, std::string>;

/// Builds a Page row by row from Cells; each cell is coerced to the column's StorageKind
/// (e.g. an int64_t into a String column is formatted, a string into an Int64 column is parsed).
class PageBuilder {
 public:
  explicit PageBuilder(std::vector<StorageKind> kinds);
  void addRow(const std::vector<Cell>& cells);
  void addRow(std::initializer_list<Cell> cells);
  size_t rowCount() const { return rows_; }
  size_t columnCount() const { return page_->columns.size(); }
  std::unique_ptr<Page> finish();

 private:
  std::unique_ptr<Page> page_;
  size_t rows_ = 0;
};

}  // namespace trino_odbc
