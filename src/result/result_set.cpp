// Forward-only cursor over a RowSource with block (rowset) support. A rowset is a list of runs,
// each a slice of one page; the runs' shared_ptr keep every referenced page alive until the next
// fetch, so bound-column conversion and SQLGetData never copy cell data.
#include "result/result_set.h"

#include "util/errors.h"
#include "util/log.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace trino_odbc {

namespace {
constexpr const char* kLog = "result";
}

ResultSet::ResultSet(std::unique_ptr<RowSource> source) : source_(std::move(source)) {
  if (!source_) throw DriverError(sqlstate::kGeneralError, "ResultSet created without a row source");
}

ResultSet::~ResultSet() {
  try {
    close();
  } catch (...) {
    // close() already swallows source failures; nothing may escape a destructor.
  }
}

bool ResultSet::loadNextPage() {
  while (!atEnd_) {
    std::unique_ptr<Page> page = source_->nextPage();
    if (!page) {
      atEnd_ = true;
      break;
    }
    // Pages without rows (keep-alive responses while the query is queued) carry nothing to fetch.
    if (page->rowCount == 0) continue;
    current_ = std::shared_ptr<Page>(std::move(page));
    currentPos_ = 0;
    return true;
  }
  // The previous tail page stays alive through runs_ if the rowset being built still references it.
  current_.reset();
  currentPos_ = 0;
  return false;
}

size_t ResultSet::fetchRowset(size_t maxRows) {
  // Dropping the previous rowset's runs releases the pages it pinned; current_ keeps the page the
  // cursor is positioned in.
  runs_.clear();
  rowsetRows_ = 0;
  rowsetStart_ = 0;
  if (closed_ || atEnd_ || maxRows == 0) return 0;

  // SQL_ATTR_MAX_ROWS: the cursor ends here and the server-side query is cancelled so it stops
  // producing rows nobody will read. The rowset already built stays valid (runs_ pin its page).
  const auto stopAtMaxRows = [this]() {
    atEnd_ = true;
    current_.reset();
    currentPos_ = 0;
    TRINO_LOG_DEBUG(kLog, "SQL_ATTR_MAX_ROWS=%lld reached after %lld rows; cancelling the rest of the query",
                    static_cast<long long>(maxRows_), static_cast<long long>(rowsFetched_));
    try {
      source_->cancel();
    } catch (const std::exception& e) {
      // Best effort: the rows already fetched are valid whether or not the server got the cancel.
      TRINO_LOG_WARN(kLog, "cancel after SQL_ATTR_MAX_ROWS failed: %s", e.what());
    }
  };

  size_t want = maxRows;
  if (maxRows_ > 0) {
    const int64_t remaining = maxRows_ - rowsFetched_;
    if (remaining <= 0) {
      // SQL_ATTR_MAX_ROWS was lowered below what has already been delivered.
      stopAtMaxRows();
      return 0;
    }
    if (static_cast<uint64_t>(remaining) < want) want = static_cast<size_t>(remaining);
  }

  const int64_t firstRowNumber = rowsFetched_ + 1;
  try {
    while (rowsetRows_ < want) {
      if (!current_ || currentPos_ >= current_->rowCount) {
        if (!loadNextPage()) break;
      }
      const size_t take = std::min(current_->rowCount - currentPos_, want - rowsetRows_);
      runs_.push_back(RowsetRun{current_, currentPos_, take});
      currentPos_ += take;
      rowsetRows_ += take;
    }
  } catch (...) {
    // A failed page load (server error, transport failure, cancel) yields no rowset at all: the
    // caller reports the error and the row counters stay as they were before this call.
    runs_.clear();
    rowsetRows_ = 0;
    throw;
  }

  if (rowsetRows_ == 0) return 0;  // atEnd_ was set by loadNextPage()
  rowsetStart_ = firstRowNumber;
  rowsFetched_ += static_cast<int64_t>(rowsetRows_);
  if (maxRows_ > 0 && rowsFetched_ >= maxRows_) stopAtMaxRows();
  TRINO_LOG_TRACE(kLog, "fetched rowset rows=%llu start=%lld runs=%llu total=%lld",
                  static_cast<unsigned long long>(rowsetRows_), static_cast<long long>(rowsetStart_),
                  static_cast<unsigned long long>(runs_.size()), static_cast<long long>(rowsFetched_));
  return rowsetRows_;
}

RowRef ResultSet::rowsetRow(size_t i) const {
  for (const RowsetRun& run : runs_) {
    if (i < run.count) return RowRef{run.page.get(), run.firstRow + i};
    i -= run.count;
  }
  return RowRef{};
}

void ResultSet::cancel() {
  // Only touches the source (never reassigned after construction), so this is safe from SQLCancel
  // on another thread while fetchRowset() blocks inside nextPage().
  source_->cancel();
}

void ResultSet::close() {
  if (closed_) return;
  closed_ = true;
  const bool unfinished = !atEnd_;
  runs_.clear();
  rowsetRows_ = 0;
  rowsetStart_ = 0;
  current_.reset();
  currentPos_ = 0;
  atEnd_ = true;
  if (unfinished) {
    // The application stopped reading early (SQLCloseCursor / SQLFreeStmt): stop the server-side
    // query and unblock the prefetch thread before releasing the source.
    try {
      source_->cancel();
    } catch (const std::exception& e) {
      TRINO_LOG_WARN(kLog, "cancel while closing cursor failed: %s", e.what());
    }
  }
  try {
    source_->close();
  } catch (const std::exception& e) {
    // Closing a cursor must not fail the application call; the resources are released on destruction.
    TRINO_LOG_WARN(kLog, "closing row source failed: %s", e.what());
  }
}

}  // namespace trino_odbc
