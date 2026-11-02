// Statement-response JSON decoding with simdjson on-demand. One pass over the document: fields are
// dispatched in document order (order-independent), and `data` rows are decoded straight into the
// typed Page while the iterator walks them — no DOM, no intermediate strings, no per-cell heap
// allocation beyond the column arenas.
// simdjson must be included before any ODBC header: unixODBC's sqltypes.h defines BOOL as a macro,
// which would corrupt simdjson's element_type enum.
#include <simdjson.h>

#include "result/json_decoder.h"

#include "util/log.h"
#include "util/strings.h"


#include <cmath>
#include <limits>
#include <utility>

namespace trino_odbc {

static_assert(kJsonPaddingBytes >= simdjson::SIMDJSON_PADDING,
              "kJsonPaddingBytes must cover SIMDJSON_PADDING");

namespace {

namespace sj = simdjson;
using sj::ondemand::json_type;

constexpr const char* kLog = "json";

[[noreturn]] void throwJson(const char* what, sj::error_code err) {
  throw DriverError(sqlstate::kGeneralError,
                    formatString("Malformed JSON in server response (%s): %s", what, sj::error_message(err)));
}

inline void check(sj::error_code err, const char* what) {
  if (err != sj::SUCCESS) throwJson(what, err);
}

/// One parser per thread: the prefetch thread, each spooled-segment worker and the calling thread
/// each keep their own buffers, so decoding never contends on a lock.
sj::ondemand::parser& threadParser() {
  thread_local sj::ondemand::parser parser;
  return parser;
}

/// How a JSON cell is turned into column storage. Derived once per column, not per cell.
enum class CellMode : uint8_t { Bool, Int64, Double, Text, Binary, RawJson };

CellMode cellModeFor(const ResultColumn& rc) {
  switch (rc.odbc.storage) {
    case StorageKind::Bool: return CellMode::Bool;
    case StorageKind::Int64: return CellMode::Int64;
    case StorageKind::Double: return CellMode::Double;
    case StorageKind::Binary: return CellMode::Binary;
    case StorageKind::String: break;
  }
  // Complex values arrive as JSON arrays/objects and are exposed as their JSON text; everything
  // else textual (decimal, temporal, uuid, json, ...) arrives as a JSON string.
  const TypeCategory c = rc.type.category;
  if (c == TypeCategory::Array || c == TypeCategory::Map || c == TypeCategory::Row) return CellMode::RawJson;
  return CellMode::Text;
}

/// Reads any JSON number (or numeric string) as int64. Doubles are accepted when integral.
bool readInt64(sj::ondemand::value& v, json_type t, int64_t* out) {
  if (t == json_type::number) {
    if (v.get_int64().get(*out) == sj::SUCCESS) return true;
    uint64_t u = 0;
    if (v.get_uint64().get(u) == sj::SUCCESS) {
      if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
      *out = static_cast<int64_t>(u);
      return true;
    }
    double d = 0;
    if (v.get_double().get(d) != sj::SUCCESS) return false;
    if (!std::isfinite(d) || d != std::floor(d) || d < -9223372036854775808.0 || d >= 9223372036854775808.0)
      return false;
    *out = static_cast<int64_t>(d);
    return true;
  }
  if (t == json_type::string) {
    std::string_view s;
    if (v.get_string().get(s) != sj::SUCCESS) return false;
    return parseInt64(trim(s), out);
  }
  if (t == json_type::boolean) {
    bool b = false;
    if (v.get_bool().get(b) != sj::SUCCESS) return false;
    *out = b ? 1 : 0;
    return true;
  }
  return false;
}

/// Reads a JSON number or one of Trino's non-finite spellings ("NaN", "Infinity", "-Infinity").
bool readDouble(sj::ondemand::value& v, json_type t, double* out) {
  if (t == json_type::number) return v.get_double().get(*out) == sj::SUCCESS;
  if (t == json_type::string) {
    std::string_view s;
    if (v.get_string().get(s) != sj::SUCCESS) return false;
    s = trim(s);
    if (s == "NaN" || s == "nan") { *out = std::numeric_limits<double>::quiet_NaN(); return true; }
    if (s == "Infinity" || s == "+Infinity" || s == "inf" || s == "+inf") { *out = std::numeric_limits<double>::infinity(); return true; }
    if (s == "-Infinity" || s == "-inf") { *out = -std::numeric_limits<double>::infinity(); return true; }
    return parseDouble(s, out);
  }
  if (t == json_type::boolean) {
    bool b = false;
    if (v.get_bool().get(b) != sj::SUCCESS) return false;
    *out = b ? 1.0 : 0.0;
    return true;
  }
  return false;
}

std::string readString(sj::ondemand::value& v, const char* what) {
  std::string_view s;
  check(v.get_string().get(s), what);
  return std::string(s);
}

/// Reads a string member, tolerating null / non-string (returns empty).
std::string readOptionalString(sj::ondemand::value& v) {
  json_type t;
  if (v.type().get(t) != sj::SUCCESS || t != json_type::string) return {};
  std::string_view s;
  if (v.get_string().get(s) != sj::SUCCESS) return {};
  return std::string(s);
}

int64_t readOptionalInt64(sj::ondemand::value& v) {
  json_type t;
  if (v.type().get(t) != sj::SUCCESS) return 0;
  int64_t n = 0;
  return readInt64(v, t, &n) ? n : 0;
}

double readOptionalDouble(sj::ondemand::value& v) {
  json_type t;
  if (v.type().get(t) != sj::SUCCESS) return 0;
  double d = 0;
  return readDouble(v, t, &d) ? d : 0;
}

bool readOptionalBool(sj::ondemand::value& v) {
  json_type t;
  if (v.type().get(t) != sj::SUCCESS) return false;
  if (t == json_type::boolean) {
    bool b = false;
    return v.get_bool().get(b) == sj::SUCCESS && b;
  }
  int64_t n = 0;
  return readInt64(v, t, &n) && n != 0;
}

/// Decodes one cell into its column. `scratch` is reused across cells (base64 output).
void decodeCell(sj::ondemand::value& v, Column& col, CellMode mode, size_t colIndex, std::string& scratch) {
  json_type t;
  check(v.type().get(t), "cell");
  if (t == json_type::null) {
    col.appendNull();
    return;
  }
  switch (mode) {
    case CellMode::Bool: {
      bool b = false;
      if (t == json_type::boolean) {
        check(v.get_bool().get(b), "boolean cell");
      } else if (t == json_type::string) {
        std::string_view s;
        check(v.get_string().get(s), "boolean cell");
        if (!parseBool(trim(s), &b))
          throw DriverError(sqlstate::kGeneralError,
                            formatString("Column %zu: cannot read '%.*s' as boolean", colIndex + 1,
                                         static_cast<int>(s.size()), s.data()));
      } else {
        int64_t n = 0;
        if (!readInt64(v, t, &n))
          throw DriverError(sqlstate::kGeneralError, formatString("Column %zu: unexpected JSON value for boolean", colIndex + 1));
        b = n != 0;
      }
      col.appendBool(b);
      return;
    }
    case CellMode::Int64: {
      int64_t n = 0;
      if (!readInt64(v, t, &n))
        throw DriverError(sqlstate::kGeneralError, formatString("Column %zu: unexpected JSON value for integer", colIndex + 1));
      col.appendInt64(n);
      return;
    }
    case CellMode::Double: {
      double d = 0;
      if (!readDouble(v, t, &d))
        throw DriverError(sqlstate::kGeneralError, formatString("Column %zu: unexpected JSON value for double", colIndex + 1));
      col.appendDouble(d);
      return;
    }
    case CellMode::Binary: {
      if (t != json_type::string)
        throw DriverError(sqlstate::kGeneralError, formatString("Column %zu: varbinary value is not a base64 string", colIndex + 1));
      std::string_view s;
      check(v.get_string().get(s), "varbinary cell");
      scratch.clear();
      if (!base64Decode(s, scratch))
        throw DriverError(sqlstate::kGeneralError, formatString("Column %zu: invalid base64 in varbinary value", colIndex + 1));
      col.appendBytes(scratch);
      return;
    }
    case CellMode::Text: {
      if (t == json_type::string) {
        std::string_view s;
        check(v.get_string().get(s), "text cell");
        col.appendBytes(s);
        return;
      }
      // A number/boolean where text was expected (e.g. an unknown type rendered natively): keep
      // the literal token so nothing is lost.
      std::string_view raw;
      check(v.raw_json().get(raw), "text cell");
      col.appendBytes(trim(raw));
      return;
    }
    case CellMode::RawJson: {
      std::string_view raw;
      check(v.raw_json().get(raw), "complex cell");
      col.appendBytes(trim(raw));
      return;
    }
  }
}

/// Walks a JSON array of rows into a fresh Page.
std::unique_ptr<Page> decodeRowsArray(sj::ondemand::array rows, const std::vector<ResultColumn>& columns) {
  const size_t ncols = columns.size();
  auto page = std::make_unique<Page>();
  page->columns.reserve(ncols);
  std::vector<CellMode> modes;
  modes.reserve(ncols);
  for (const ResultColumn& rc : columns) {
    page->columns.emplace_back(rc.odbc.storage);
    modes.push_back(cellModeFor(rc));
  }
  std::string scratch;
  size_t rowCount = 0;
  for (auto rowResult : rows) {
    sj::ondemand::array row;
    check(rowResult.get_array().get(row), "data row");
    size_t c = 0;
    for (auto cellResult : row) {
      sj::ondemand::value cell;
      check(std::move(cellResult).get(cell), "data cell");
      if (c >= ncols)
        throw DriverError(sqlstate::kGeneralError,
                          formatString("Row %zu has more than %zu columns", rowCount + 1, ncols));
      decodeCell(cell, page->columns[c], modes[c], c, scratch);
      ++c;
    }
    if (c != ncols)
      throw DriverError(sqlstate::kGeneralError,
                        formatString("Row %zu has %zu columns, expected %zu", rowCount + 1, c, ncols));
    ++rowCount;
  }
  page->rowCount = rowCount;
  return page;
}

std::vector<ResultColumn> parseColumns(sj::ondemand::value& v, const TypeMappingOptions& opts) {
  std::vector<ResultColumn> out;
  sj::ondemand::array arr;
  check(v.get_array().get(arr), "columns");
  for (auto colResult : arr) {
    sj::ondemand::object obj;
    check(colResult.get_object().get(obj), "columns[]");
    std::string name, type;
    for (auto fieldResult : obj) {
      sj::ondemand::field field;
      check(std::move(fieldResult).get(field), "columns[] field");
      std::string_view key;
      check(field.unescaped_key(false).get(key), "columns[] key");
      sj::ondemand::value fv = field.value();
      if (key == "name") name = readString(fv, "column name");
      else if (key == "type") type = readString(fv, "column type");
      // typeSignature is redundant with `type`; skipped by the iterator.
    }
    if (type.empty()) type = "unknown";
    out.push_back(ResultColumn::make(std::move(name), type, opts));
  }
  return out;
}

QueryStats parseStats(sj::ondemand::value& v) {
  QueryStats s;
  sj::ondemand::object obj;
  check(v.get_object().get(obj), "stats");
  for (auto fieldResult : obj) {
    sj::ondemand::field field;
    check(std::move(fieldResult).get(field), "stats field");
    std::string_view key;
    check(field.unescaped_key(false).get(key), "stats key");
    sj::ondemand::value fv = field.value();
    if (key == "state") s.state = readOptionalString(fv);
    else if (key == "queued") s.queued = readOptionalBool(fv);
    else if (key == "scheduled") s.scheduled = readOptionalBool(fv);
    else if (key == "nodes") s.nodes = readOptionalInt64(fv);
    else if (key == "totalSplits") s.totalSplits = readOptionalInt64(fv);
    else if (key == "queuedSplits") s.queuedSplits = readOptionalInt64(fv);
    else if (key == "runningSplits") s.runningSplits = readOptionalInt64(fv);
    else if (key == "completedSplits") s.completedSplits = readOptionalInt64(fv);
    else if (key == "cpuTimeMillis") s.cpuTimeMillis = readOptionalInt64(fv);
    else if (key == "wallTimeMillis") s.wallTimeMillis = readOptionalInt64(fv);
    else if (key == "queuedTimeMillis") s.queuedTimeMillis = readOptionalInt64(fv);
    else if (key == "elapsedTimeMillis") s.elapsedTimeMillis = readOptionalInt64(fv);
    else if (key == "processedRows") s.processedRows = readOptionalInt64(fv);
    else if (key == "processedBytes") s.processedBytes = readOptionalInt64(fv);
    else if (key == "physicalInputBytes") s.physicalInputBytes = readOptionalInt64(fv);
    else if (key == "peakMemoryBytes") s.peakMemoryBytes = readOptionalInt64(fv);
    else if (key == "spilledBytes") s.spilledBytes = readOptionalInt64(fv);
    else if (key == "progressPercentage") s.progressPercentage = readOptionalDouble(fv);
    // rootStage and friends are skipped structurally.
  }
  return s;
}

TrinoError parseError(sj::ondemand::value& v) {
  TrinoError e;
  sj::ondemand::object obj;
  check(v.get_object().get(obj), "error");
  for (auto fieldResult : obj) {
    sj::ondemand::field field;
    check(std::move(fieldResult).get(field), "error field");
    std::string_view key;
    check(field.unescaped_key(false).get(key), "error key");
    sj::ondemand::value fv = field.value();
    if (key == "message") e.message = readOptionalString(fv);
    else if (key == "errorCode") e.errorCode = static_cast<int>(readOptionalInt64(fv));
    else if (key == "errorName") e.errorName = readOptionalString(fv);
    else if (key == "errorType") e.errorType = readOptionalString(fv);
    else if (key == "sqlState") e.sqlState = readOptionalString(fv);
    else if (key == "errorLocation") {
      sj::ondemand::object loc;
      if (fv.get_object().get(loc) == sj::SUCCESS) {
        for (auto lr : loc) {
          sj::ondemand::field lf;
          check(std::move(lr).get(lf), "errorLocation field");
          std::string_view lk;
          check(lf.unescaped_key(false).get(lk), "errorLocation key");
          sj::ondemand::value lv = lf.value();
          if (lk == "lineNumber") e.line = static_cast<int>(readOptionalInt64(lv));
          else if (lk == "columnNumber") e.column = static_cast<int>(readOptionalInt64(lv));
        }
      }
    } else if (key == "failureInfo") {
      sj::ondemand::object fi;
      if (fv.get_object().get(fi) == sj::SUCCESS) {
        for (auto fr : fi) {
          sj::ondemand::field ff;
          check(std::move(fr).get(ff), "failureInfo field");
          std::string_view fk;
          check(ff.unescaped_key(false).get(fk), "failureInfo key");
          sj::ondemand::value fvv = ff.value();
          if (fk == "type") e.failureType = readOptionalString(fvv);
          else if (fk == "message") e.failureMessage = readOptionalString(fvv);
        }
      }
    }
  }
  if (e.message.empty()) e.message = e.failureMessage.empty() ? "Query failed" : e.failureMessage;
  return e;
}

std::vector<QueryWarning> parseWarnings(sj::ondemand::value& v) {
  std::vector<QueryWarning> out;
  sj::ondemand::array arr;
  if (v.get_array().get(arr) != sj::SUCCESS) return out;  // null / unexpected shape: no warnings
  for (auto wr : arr) {
    sj::ondemand::object obj;
    check(wr.get_object().get(obj), "warnings[]");
    QueryWarning w;
    for (auto fr : obj) {
      sj::ondemand::field f;
      check(std::move(fr).get(f), "warning field");
      std::string_view k;
      check(f.unescaped_key(false).get(k), "warning key");
      sj::ondemand::value fv = f.value();
      if (k == "message") w.message = readOptionalString(fv);
      else if (k == "warningCode") {
        sj::ondemand::object wc;
        if (fv.get_object().get(wc) == sj::SUCCESS) {
          for (auto cr : wc) {
            sj::ondemand::field cf;
            check(std::move(cr).get(cf), "warningCode field");
            std::string_view ck;
            check(cf.unescaped_key(false).get(ck), "warningCode key");
            sj::ondemand::value cv = cf.value();
            if (ck == "code") w.code = static_cast<int>(readOptionalInt64(cv));
            else if (ck == "name") w.name = readOptionalString(cv);
          }
        }
      }
    }
    out.push_back(std::move(w));
  }
  return out;
}

void parseSegmentMetadata(sj::ondemand::value& v, SpooledSegment& seg) {
  sj::ondemand::object obj;
  if (v.get_object().get(obj) != sj::SUCCESS) return;
  for (auto fr : obj) {
    sj::ondemand::field f;
    check(std::move(fr).get(f), "segment metadata field");
    std::string_view k;
    check(f.unescaped_key(false).get(k), "segment metadata key");
    sj::ondemand::value fv = f.value();
    if (k == "segmentSize") seg.segmentSize = readOptionalInt64(fv);
    else if (k == "rowOffset") seg.rowOffset = readOptionalInt64(fv);
    else if (k == "rowsCount") seg.rowsCount = readOptionalInt64(fv);
    else if (k == "uncompressedSize") seg.uncompressedSize = readOptionalInt64(fv);
  }
}

void parseSegmentHeaders(sj::ondemand::value& v, SpooledSegment& seg) {
  sj::ondemand::object obj;
  if (v.get_object().get(obj) != sj::SUCCESS) return;
  for (auto fr : obj) {
    sj::ondemand::field f;
    check(std::move(fr).get(f), "segment headers field");
    std::string_view name;
    check(f.unescaped_key(false).get(name), "segment headers key");
    sj::ondemand::value fv = f.value();
    json_type t;
    check(fv.type().get(t), "segment header value");
    if (t == json_type::array) {
      sj::ondemand::array values;
      check(fv.get_array().get(values), "segment header values");
      for (auto vr : values) {
        sj::ondemand::value hv;
        check(std::move(vr).get(hv), "segment header value");
        seg.headers.emplace_back(std::string(name), readOptionalString(hv));
      }
    } else if (t == json_type::string) {
      seg.headers.emplace_back(std::string(name), readOptionalString(fv));
    }
  }
}

SpooledData parseSpooledData(sj::ondemand::object& obj) {
  SpooledData out;
  for (auto fr : obj) {
    sj::ondemand::field f;
    check(std::move(fr).get(f), "spooled data field");
    std::string_view k;
    check(f.unescaped_key(false).get(k), "spooled data key");
    sj::ondemand::value fv = f.value();
    if (k == "encoding") {
      out.encoding = readOptionalString(fv);
    } else if (k == "segments") {
      sj::ondemand::array arr;
      check(fv.get_array().get(arr), "segments");
      for (auto sr : arr) {
        sj::ondemand::object so;
        check(sr.get_object().get(so), "segments[]");
        SpooledSegment seg;
        std::string base64;
        for (auto sfr : so) {
          sj::ondemand::field sf;
          check(std::move(sfr).get(sf), "segment field");
          std::string_view sk;
          check(sf.unescaped_key(false).get(sk), "segment key");
          sj::ondemand::value sv = sf.value();
          if (sk == "type") {
            const std::string type = readOptionalString(sv);
            if (iequals(type, "inline")) seg.type = SpooledSegment::Type::Inline;
            else if (iequals(type, "spooled")) seg.type = SpooledSegment::Type::Spooled;
            else throw DriverError(sqlstate::kGeneralError, "Unknown spooled segment type: " + type);
          } else if (sk == "data") {
            base64 = readOptionalString(sv);
          } else if (sk == "uri") {
            seg.uri = readOptionalString(sv);
          } else if (sk == "ackUri") {
            seg.ackUri = readOptionalString(sv);
          } else if (sk == "headers") {
            parseSegmentHeaders(sv, seg);
          } else if (sk == "metadata") {
            parseSegmentMetadata(sv, seg);
          }
        }
        if (seg.type == SpooledSegment::Type::Inline) {
          if (!base64Decode(base64, seg.inlineData))
            throw DriverError(sqlstate::kGeneralError, "Invalid base64 in inline spooled segment");
        } else if (seg.uri.empty()) {
          throw DriverError(sqlstate::kGeneralError, "Spooled segment without uri");
        }
        out.segments.push_back(std::move(seg));
      }
    }
    // "metadata" at the top level carries nothing the driver needs.
  }
  if (out.encoding.empty()) out.encoding = "json";
  return out;
}

}  // namespace

void ensureJsonPadding(std::string& s) {
  if (s.capacity() < s.size() + kJsonPaddingBytes) s.reserve(s.size() + kJsonPaddingBytes);
}

StatementResponse parseStatementResponse(std::string& json, const std::vector<ResultColumn>* knownColumns,
                                         const TypeMappingOptions& opts) {
  ensureJsonPadding(json);
  StatementResponse r;
  sj::ondemand::parser& parser = threadParser();
  sj::ondemand::document doc;
  check(parser.iterate(json.data(), json.size(), json.capacity()).get(doc), "document");
  sj::ondemand::object root;
  check(doc.get_object().get(root), "response object");

  // Rows found before the column list of the same response (never with Trino, but legal JSON):
  // remembered as raw text and decoded once every field has been seen.
  std::string deferredRows;
  const std::vector<ResultColumn>* columns = knownColumns;

  for (auto fieldResult : root) {
    sj::ondemand::field field;
    check(std::move(fieldResult).get(field), "response field");
    std::string_view key;
    check(field.unescaped_key(false).get(key), "response key");
    sj::ondemand::value v = field.value();
    if (key == "id") {
      r.id = readOptionalString(v);
    } else if (key == "infoUri") {
      r.infoUri = readOptionalString(v);
    } else if (key == "nextUri") {
      r.nextUri = readOptionalString(v);
    } else if (key == "partialCancelUri") {
      r.partialCancelUri = readOptionalString(v);
    } else if (key == "columns") {
      json_type t;
      check(v.type().get(t), "columns");
      if (t == json_type::array) {
        r.columns = parseColumns(v, opts);
        r.columnsPresent = true;
        columns = &r.columns;
      }
    } else if (key == "data") {
      json_type t;
      check(v.type().get(t), "data");
      if (t == json_type::array) {
        if (columns != nullptr) {
          sj::ondemand::array rows;
          check(v.get_array().get(rows), "data");
          r.page = decodeRowsArray(rows, *columns);
        } else {
          std::string_view raw;
          check(v.raw_json().get(raw), "data");
          deferredRows.assign(raw.data(), raw.size());
        }
      } else if (t == json_type::object) {
        sj::ondemand::object spooled;
        check(v.get_object().get(spooled), "spooled data");
        r.spooled = parseSpooledData(spooled);
        r.dataIsSpooled = true;
      }
      // null: no rows in this response
    } else if (key == "stats") {
      json_type t;
      check(v.type().get(t), "stats");
      if (t == json_type::object) {
        r.stats = parseStats(v);
        r.statsPresent = true;
      }
    } else if (key == "error") {
      json_type t;
      check(v.type().get(t), "error");
      if (t == json_type::object) r.error = parseError(v);
    } else if (key == "warnings") {
      r.warnings = parseWarnings(v);
    } else if (key == "updateType") {
      r.updateType = readOptionalString(v);
    } else if (key == "updateCount") {
      json_type t;
      check(v.type().get(t), "updateCount");
      int64_t n = 0;
      if (t != json_type::null && readInt64(v, t, &n)) r.updateCount = n;
    }
    // Unknown members are skipped structurally.
  }

  if (!deferredRows.empty()) {
    if (columns == nullptr)
      throw DriverError(sqlstate::kGeneralError, "Server sent data rows before any column metadata");
    // The outer document is finished, so the thread parser is free to be reused for the rows.
    r.page = decodeRowsJson(deferredRows, *columns);
  }
  if (r.page && r.page->rowCount == 0) r.page.reset();

  if (TRINO_LOG_ENABLED(LogLevel::Trace)) {
    TRINO_LOG_TRACE(kLog, "response id=%s state=%s columns=%d rows=%zu spooled=%d next=%s error=%d",
                    r.id.c_str(), r.stats.state.c_str(), r.columnsPresent ? 1 : 0,
                    r.page ? r.page->rowCount : static_cast<size_t>(0), r.dataIsSpooled ? 1 : 0,
                    r.nextUri.empty() ? "-" : "yes", r.error ? 1 : 0);
  }
  return r;
}

std::unique_ptr<Page> decodeRowsJson(std::string& json, const std::vector<ResultColumn>& columns) {
  ensureJsonPadding(json);
  sj::ondemand::parser& parser = threadParser();
  sj::ondemand::document doc;
  check(parser.iterate(json.data(), json.size(), json.capacity()).get(doc), "rows document");
  sj::ondemand::array rows;
  check(doc.get_array().get(rows), "rows array");
  return decodeRowsArray(rows, columns);
}

bool parseOAuthTokenResponse(std::string& json, std::string* token, std::string* nextUri, std::string* error) {
  ensureJsonPadding(json);
  sj::ondemand::parser& parser = threadParser();
  sj::ondemand::document doc;
  if (parser.iterate(json.data(), json.size(), json.capacity()).get(doc) != sj::SUCCESS) return false;
  sj::ondemand::object root;
  if (doc.get_object().get(root) != sj::SUCCESS) return false;
  for (auto fr : root) {
    sj::ondemand::field f;
    if (std::move(fr).get(f) != sj::SUCCESS) return false;
    std::string_view k;
    if (f.unescaped_key(false).get(k) != sj::SUCCESS) return false;
    sj::ondemand::value v = f.value();
    if (k == "token" && token) *token = readOptionalString(v);
    else if (k == "nextUri" && nextUri) *nextUri = readOptionalString(v);
    else if (k == "error" && error) *error = readOptionalString(v);
  }
  return true;
}

}  // namespace trino_odbc
