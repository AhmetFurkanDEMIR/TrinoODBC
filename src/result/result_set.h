#pragma once
// Forward-only cursor over a RowSource with block (rowset) support. Rows of the current rowset may
// span several pages; every page referenced by the rowset is kept alive until the next fetch.
#include "result/row_source.h"

#include <memory>
#include <vector>

namespace trino_odbc {

/// A run of consecutive rows inside one page.
struct RowsetRun {
  std::shared_ptr<Page> page;
  size_t firstRow = 0;
  size_t count = 0;
};

/// Reference to one row (page + index) valid while the rowset that produced it is alive.
struct RowRef {
  const Page* page = nullptr;
  size_t row = 0;
  bool valid() const { return page != nullptr; }
};

class ResultSet {
 public:
  explicit ResultSet(std::unique_ptr<RowSource> source);
  ~ResultSet();

  const std::vector<ResultColumn>& columns() const { return source_->columns(); }
  size_t columnCount() const { return source_->columns().size(); }

  /// Fetches the next rowset of up to `maxRows` rows. Returns the number of rows fetched (0 at end).
  /// Throws DriverError subclasses on server / transport failures.
  size_t fetchRowset(size_t maxRows);
  /// Rows in the current rowset.
  size_t rowsetSize() const { return rowsetRows_; }
  /// Row `i` (0-based) of the current rowset.
  RowRef rowsetRow(size_t i) const;
  const std::vector<RowsetRun>& rowsetRuns() const { return runs_; }
  /// 1-based number of the first row of the current rowset within the whole result (0 before first fetch).
  int64_t rowsetStartRowNumber() const { return rowsetStart_; }
  /// Total rows delivered so far.
  int64_t rowsFetched() const { return rowsFetched_; }
  bool atEnd() const { return atEnd_; }
  /// Stop after this many rows in total (SQL_ATTR_MAX_ROWS); 0 = unlimited.
  void setMaxRows(int64_t maxRows) { maxRows_ = maxRows; }

  void cancel();
  void close();
  RowSource& source() { return *source_; }
  const RowSource& source() const { return *source_; }

 private:
  bool loadNextPage();

  std::unique_ptr<RowSource> source_;
  std::shared_ptr<Page> current_;
  size_t currentPos_ = 0;         // next unread row in current_
  std::vector<RowsetRun> runs_;
  size_t rowsetRows_ = 0;
  int64_t rowsetStart_ = 0;
  int64_t rowsFetched_ = 0;
  int64_t maxRows_ = 0;
  bool atEnd_ = false;
  bool closed_ = false;
};

}  // namespace trino_odbc
