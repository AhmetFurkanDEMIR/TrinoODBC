#pragma once
// Abstract source of pages plus the in-memory implementation used for catalog functions and small
// internal queries.
#include "result/page.h"
#include "types/odbc_types.h"
#include "types/trino_type.h"

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace trino_odbc {

/// Metadata of one result column (from Trino `columns[]` or DESCRIBE OUTPUT or a catalog function).
struct ResultColumn {
  std::string name;
  TrinoType type;
  OdbcTypeInfo odbc;
  // Base-table information when known (DESCRIBE OUTPUT); empty otherwise.
  std::string catalog, schema, table;
  bool aliased = false;
  /// Builds a column with mapping applied.
  static ResultColumn make(std::string name, const TrinoType& type, const TypeMappingOptions& opts);
  static ResultColumn make(std::string name, std::string_view trinoTypeName, const TypeMappingOptions& opts);
};

struct QueryWarning {
  int code = 0;
  std::string name;
  std::string message;
};

class RowSource {
 public:
  virtual ~RowSource() = default;
  virtual const std::vector<ResultColumn>& columns() const = 0;
  /// Blocks until a page is available. Returns nullptr at end of data. Throws DriverError subclasses.
  virtual std::unique_ptr<Page> nextPage() = 0;
  /// Best-effort asynchronous cancel (thread-safe, may be called while nextPage() blocks).
  virtual void cancel() {}
  /// Releases resources; further nextPage() calls return nullptr.
  virtual void close() {}
  virtual std::optional<int64_t> updateCount() const { return std::nullopt; }
  virtual std::string updateType() const { return {}; }
  virtual const std::vector<QueryWarning>& warnings() const { static const std::vector<QueryWarning> none; return none; }
  /// Query id (Trino) or empty.
  virtual std::string queryId() const { return {}; }
};

/// Pages held in memory; used for catalog functions, SQLGetTypeInfo, DESCRIBE, empty result sets.
class MemoryRowSource : public RowSource {
 public:
  explicit MemoryRowSource(std::vector<ResultColumn> columns);
  MemoryRowSource(std::vector<ResultColumn> columns, std::unique_ptr<Page> page);
  MemoryRowSource(std::vector<ResultColumn> columns, std::vector<std::unique_ptr<Page>> pages);
  const std::vector<ResultColumn>& columns() const override { return columns_; }
  std::unique_ptr<Page> nextPage() override;
  void addPage(std::unique_ptr<Page> page);
  size_t totalRows() const { return totalRows_; }
  void setUpdateCount(int64_t n) { updateCount_ = n; }
  std::optional<int64_t> updateCount() const override { return updateCount_; }
  void setWarnings(std::vector<QueryWarning> w) { warnings_ = std::move(w); }
  const std::vector<QueryWarning>& warnings() const override { return warnings_; }
  /// StorageKinds of the columns (for PageBuilder).
  std::vector<StorageKind> storageKinds() const;

 private:
  std::vector<ResultColumn> columns_;
  std::deque<std::unique_ptr<Page>> pages_;
  size_t totalRows_ = 0;
  std::optional<int64_t> updateCount_;
  std::vector<QueryWarning> warnings_;
};

/// Drains `source` completely into a MemoryRowSource (metadata queries only — never for user data).
std::unique_ptr<MemoryRowSource> materialize(RowSource& source);

/// Reads a whole page into string cells (nullopt for NULL) — convenient for tests and catalog code.
std::vector<std::vector<std::optional<std::string>>> pageToStrings(const Page& page);

}  // namespace trino_odbc
