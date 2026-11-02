// ResultColumn construction, the in-memory RowSource used by catalog functions / DESCRIBE / empty
// results, and the materialize() / pageToStrings() helpers.
#include "result/row_source.h"

#include "util/errors.h"
#include "util/log.h"

#include <string>
#include <utility>

namespace trino_odbc {

namespace result_internal {
// Defined in page.cpp; shared inside the result module without a public header.
std::string formatDoubleShortest(double v);
}  // namespace result_internal

namespace {

constexpr const char* kLog = "result";

/// Throws HY000 when `page` does not fit a source with `columnCount` columns.
void validatePageShape(const Page& page, size_t columnCount) {
  if (page.columns.size() != columnCount) {
    throw DriverError(sqlstate::kGeneralError, "page has " + std::to_string(page.columns.size()) +
                                                   " columns but the row source has " +
                                                   std::to_string(columnCount));
  }
  for (size_t i = 0; i < page.columns.size(); ++i) {
    if (page.columns[i].size() != page.rowCount) {
      throw DriverError(sqlstate::kGeneralError, "page column " + std::to_string(i + 1) + " holds " +
                                                     std::to_string(page.columns[i].size()) +
                                                     " values for " + std::to_string(page.rowCount) + " rows");
    }
  }
}

/// Appends every row of `src` to `dst` (same column layout). Used only for metadata results.
void appendPageRows(Page& dst, const Page& src) {
  for (size_t c = 0; c < dst.columns.size(); ++c) {
    Column& d = dst.columns[c];
    const Column& s = src.columns[c];
    if (d.kind() != s.kind()) {
      throw DriverError(sqlstate::kGeneralError, "cannot concatenate pages: column " + std::to_string(c + 1) +
                                                     " changed storage kind between pages");
    }
    for (size_t r = 0; r < src.rowCount; ++r) {
      if (s.isNull(r)) {
        d.appendNull();
        continue;
      }
      switch (s.kind()) {
        case StorageKind::Bool: d.appendBool(s.getBool(r)); break;
        case StorageKind::Int64: d.appendInt64(s.getInt64(r)); break;
        case StorageKind::Double: d.appendDouble(s.getDouble(r)); break;
        case StorageKind::String:
        case StorageKind::Binary: d.appendBytes(s.getBytes(r)); break;
      }
    }
  }
  dst.rowCount += src.rowCount;
}

}  // namespace

// ---- ResultColumn -----------------------------------------------------------------------------

ResultColumn ResultColumn::make(std::string name, const TrinoType& type, const TypeMappingOptions& opts) {
  ResultColumn column;
  column.name = std::move(name);
  column.type = type;
  column.odbc = mapTrinoType(type, opts);
  return column;
}

ResultColumn ResultColumn::make(std::string name, std::string_view trinoTypeName, const TypeMappingOptions& opts) {
  return make(std::move(name), TrinoType::parse(trinoTypeName), opts);
}

// ---- MemoryRowSource --------------------------------------------------------------------------

MemoryRowSource::MemoryRowSource(std::vector<ResultColumn> columns) : columns_(std::move(columns)) {}

MemoryRowSource::MemoryRowSource(std::vector<ResultColumn> columns, std::unique_ptr<Page> page)
    : columns_(std::move(columns)) {
  addPage(std::move(page));
}

MemoryRowSource::MemoryRowSource(std::vector<ResultColumn> columns, std::vector<std::unique_ptr<Page>> pages)
    : columns_(std::move(columns)) {
  for (std::unique_ptr<Page>& page : pages) addPage(std::move(page));
}

std::unique_ptr<Page> MemoryRowSource::nextPage() {
  if (pages_.empty()) return nullptr;
  std::unique_ptr<Page> page = std::move(pages_.front());
  pages_.pop_front();
  return page;
}

void MemoryRowSource::addPage(std::unique_ptr<Page> page) {
  if (!page) return;
  validatePageShape(*page, columns_.size());
  totalRows_ += page->rowCount;
  pages_.push_back(std::move(page));
}

std::vector<StorageKind> MemoryRowSource::storageKinds() const {
  std::vector<StorageKind> kinds;
  kinds.reserve(columns_.size());
  for (const ResultColumn& c : columns_) kinds.push_back(c.odbc.storage);
  return kinds;
}

// ---- helpers ----------------------------------------------------------------------------------

std::unique_ptr<MemoryRowSource> materialize(RowSource& source) {
  auto out = std::make_unique<MemoryRowSource>(source.columns());
  // Everything is folded into one page so consumers of metadata results can treat the whole
  // result as a single table; the first page is moved, later ones are copied row by row.
  std::unique_ptr<Page> merged;
  size_t pagesSeen = 0;
  while (std::unique_ptr<Page> page = source.nextPage()) {
    ++pagesSeen;
    validatePageShape(*page, out->columns().size());
    if (page->rowCount == 0) continue;
    if (!merged) {
      merged = std::move(page);
    } else {
      appendPageRows(*merged, *page);
    }
  }
  if (merged) out->addPage(std::move(merged));
  if (std::optional<int64_t> count = source.updateCount()) out->setUpdateCount(*count);
  out->setWarnings(source.warnings());
  TRINO_LOG_TRACE(kLog, "materialized %llu rows from %llu pages", static_cast<unsigned long long>(out->totalRows()),
                  static_cast<unsigned long long>(pagesSeen));
  return out;
}

std::vector<std::vector<std::optional<std::string>>> pageToStrings(const Page& page) {
  std::vector<std::vector<std::optional<std::string>>> rows;
  rows.reserve(page.rowCount);
  for (size_t r = 0; r < page.rowCount; ++r) {
    std::vector<std::optional<std::string>> row;
    row.reserve(page.columns.size());
    for (const Column& c : page.columns) {
      if (c.isNull(r)) {
        row.emplace_back(std::nullopt);
        continue;
      }
      switch (c.kind()) {
        case StorageKind::Bool: row.emplace_back(c.getBool(r) ? "true" : "false"); break;
        case StorageKind::Int64: row.emplace_back(std::to_string(c.getInt64(r))); break;
        case StorageKind::Double: row.emplace_back(result_internal::formatDoubleShortest(c.getDouble(r))); break;
        case StorageKind::String:
        case StorageKind::Binary: row.emplace_back(std::string(c.getBytes(r))); break;  // binary = raw bytes
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace trino_odbc
